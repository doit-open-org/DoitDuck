/*
 * The xiaozhi feature: the duck as a xiaozhi device (xiaozhi.me), with OTA
 * check and activation, then a WebSocket conversation with Opus audio both ways
 * and MCP tool calls from the server's LLM (mcp.c). Protocol:
 * github.com/78/xiaozhi-esp32 docs/websocket.md.
 *
 * Start-up: POST the OTA endpoint with Device-Id and
 * Client-Id; the reply carries either an activation code (shown on the web
 * page; the user binds the device at xiaozhi.me, then /activate answers 200) or
 * the WebSocket url and token. EV_WAKE_IDLE (the wake word, voice has no command words) opens the
 * WebSocket, sends hello, then listen/start; EV_WAKE interrupts the duck while
 * it speaks; the server answers with stt, tts, Opus audio and MCP calls.
 * The connection is made on its own thread; what the mic hears meanwhile is
 * kept and sent once listening starts, so the first words are not lost.
 * A conversation with nothing heard or said for window_s ends with the falling
 * tone; it stays open while the duck moves.
 * This board has no eFuse key, so activation is by code ("Activation-Version: 1").
 */
#include "xiaozhi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "config.h"
#include "duck.h"
#include "event.h"
#include "feature.h"
#include "mcp.h"
#include "net.h"
#include "opus_codec.h"
#include "sound.h"
#include "storage.h"

#define FRAME_SAMPLES 960 /* 60 ms at 16 kHz */
#define BACKLOG 50        /* 3 s of packets while connecting */
#define OPUS_MAX 512
#define LISTEN_START ",\"state\":\"start\",\"mode\":\"auto\""
#define ECHO_TAIL_S 0.3   /* after the speaker stops, before the mic is sent again */

enum { XZ_UNAVAILABLE, XZ_CHECKING, XZ_ACTIVATING, XZ_READY, XZ_CONNECTING, XZ_LISTENING, XZ_SPEAKING };
static const char *const STATE_NAMES[] = {"unavailable", "checking", "activating",
                                          "ready", "connecting", "listening", "speaking"};

static os_mutex_t lock = OS_MUTEX_INIT;
static int state = XZ_CHECKING;
static char ota_url[160] = "https://api.tenclass.net/xiaozhi/ota/";
static char device_id[32], client_id[40], ws_url[160], token[160], session[64];
static char reason[128], code[16], code_message[160], last_stt[256], last_tts[256], emotion[24];
static int16_t frame[FRAME_SAMPLES];
static int frame_fill, nbacklog;
static uint8_t backlog[BACKLOG][OPUS_MAX];
static int backlog_len[BACKLOG];
static double last_activity, play_until, window_s = 8;

static void on_text(const char *text);
static void on_binary(const uint8_t *data, size_t len);
static void on_closed(void);
static const struct net_ws_handler WS = {on_text, on_binary, on_closed};

static int configure(const cJSON *section)
{
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(section, "enabled");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(section, "ota_url");
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(section, "window_s");
    if (cJSON_IsString(u)) snprintf(ota_url, sizeof ota_url, "%s", u->valuestring);
    if (cJSON_IsNumber(w)) window_s = w->valuedouble;
    return cJSON_IsFalse(e) ? -1 : 0;
}

static void set_state(int s, const char *why)
{
    os_lock(&lock);
    state = s;
    snprintf(reason, sizeof reason, "%s", why ? why : "");
    os_unlock(&lock);
}

/*
 * Device-Id: a random MAC (locally administered), and Client-Id: a random UUID,
 * each made once and kept in data/. xiaozhi.me knows a device by its Device-Id,
 * so the factory reset, which removes them, makes the duck a new device to activate.
 */
static void load_ids(void)
{
    uint8_t r[22];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(r, 1, sizeof r, f) != sizeof r) printf("xiaozhi: /dev/urandom not read\n");
    if (f) fclose(f);
    if (store_kv_get("xz_device.txt", device_id, sizeof device_id) != 17) {
        snprintf(device_id, sizeof device_id, "%02x:%02x:%02x:%02x:%02x:%02x", (r[0] & 0xFC) | 0x02, r[1], r[2], r[3], r[4], r[5]);
        store_kv_set("xz_device.txt", device_id);
    }
    if (store_kv_get("xz_client.txt", client_id, sizeof client_id) == 36) return;
    /* UUID version 4: r[6..21], with the version and variant bits set. */
    r[12] = (r[12] & 0x0F) | 0x40;
    r[14] = (r[14] & 0x3F) | 0x80;
    char *s = client_id;
    for (int i = 0; i < 16; i++) s += sprintf(s, "%s%02x", i == 4 || i == 6 || i == 8 || i == 10 ? "-" : "", r[6 + i]);
    store_kv_set("xz_client.txt", client_id);
}

static int post(const char *url, const char *body, char *resp, size_t size)
{
    char h_dev[64], h_cli[64];
    snprintf(h_dev, sizeof h_dev, "Device-Id: %s", device_id);
    snprintf(h_cli, sizeof h_cli, "Client-Id: %s", client_id);
    const char *const headers[] = {h_dev, h_cli, "Activation-Version: 1", "User-Agent: doitduck/" DOITDUCK_VERSION,
                                   "Accept-Language: zh-CN", "Content-Type: application/json", NULL};
    return net_https_post(url, headers, body, strlen(body), resp, size);
}

int xiaozhi_explain(const char *url, const char *token, const char *question, const uint8_t *jpeg, size_t len,
                    char *resp, size_t size)
{
    /* multipart/form-data as the ESP32 firmware sends it: "question", then the photo as "file". */
    static const char boundary[] = "----DOITDUCK_CAMERA_BOUNDARY";
    char head[1024], tail[64], h_auth[200], h_dev[64], h_cli[64], h_type[96];
    const int nh = snprintf(head, sizeof head,
                            "--%s\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n%s\r\n"
                            "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
                            "Content-Type: image/jpeg\r\n\r\n",
                            boundary, question, boundary);
    const int nt = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
    if (nh >= (int)sizeof head) return -1;
    uint8_t *body = malloc((size_t)nh + len + (size_t)nt);
    if (!body) return -1;
    memcpy(body, head, (size_t)nh);
    memcpy(body + nh, jpeg, len);
    memcpy(body + nh + len, tail, (size_t)nt);
    snprintf(h_auth, sizeof h_auth, "Authorization: Bearer %s", token);
    snprintf(h_dev, sizeof h_dev, "Device-Id: %s", device_id);
    snprintf(h_cli, sizeof h_cli, "Client-Id: %s", client_id);
    snprintf(h_type, sizeof h_type, "Content-Type: multipart/form-data; boundary=%s", boundary);
    const char *const headers[] = {h_auth, h_dev, h_cli, h_type, NULL};
    const int status = net_https_post(url, headers, body, (size_t)nh + len + (size_t)nt, resp, size);
    free(body);
    return status;
}

/* POST the OTA endpoint and keep what it says; returns 0 when it answered. */
static int check_version(void)
{
    char body[512], resp[4096];
    snprintf(body, sizeof body,
             "{\"version\":2,\"language\":\"zh-CN\",\"uuid\":\"%s\",\"mac_address\":\"%s\","
             "\"chip_model_name\":\"hi3516cv610\",\"application\":{\"name\":\"doitduck\",\"version\":\"%s\"},"
             "\"board\":{\"type\":\"doitduck\",\"name\":\"%s\",\"mac\":\"%s\"}}",
             client_id, device_id, DOITDUCK_VERSION, B.name, device_id);
    const int status = post(ota_url, body, resp, sizeof resp);
    if (status != 200) {
        if (status >= 0) printf("xiaozhi: OTA answered %d\n", status);
        set_state(XZ_UNAVAILABLE, status < 0 ? "no_server" : "ota_failed");
        return -1;
    }
    cJSON *root = cJSON_Parse(resp);
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "activation");
    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(act, "code");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(act, "message");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(ws, "url");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(ws, "token");
    os_lock(&lock);
    snprintf(code, sizeof code, "%s", cJSON_IsString(c) ? c->valuestring : "");
    snprintf(code_message, sizeof code_message, "%s", cJSON_IsString(m) ? m->valuestring : "");
    if (cJSON_IsString(u)) snprintf(ws_url, sizeof ws_url, "%s", u->valuestring);
    if (cJSON_IsString(t)) snprintf(token, sizeof token, "%s", t->valuestring);
    os_unlock(&lock);
    cJSON_Delete(root);
    return 0;
}

static void task(void *arg)
{
    (void)arg;
    load_ids();
    for (;;) {
        const char *why = net_unavailable();
        if (why) {
            set_state(XZ_UNAVAILABLE, why);
            os_sleep(60);
            continue;
        }
        set_state(XZ_CHECKING, NULL);
        /* At boot the Wi-Fi joins after the app starts, so retry soon. */
        if (check_version() != 0) {
            os_sleep(10);
            continue;
        }
        /* An activation code: wait until the user has bound it at xiaozhi.me. */
        while (code[0]) {
            set_state(XZ_ACTIVATING, "activate");
            char url[176], resp[512];
            snprintf(url, sizeof url, "%s%sactivate", ota_url, ota_url[strlen(ota_url) - 1] == '/' ? "" : "/");
            const int status = post(url, "{}", resp, sizeof resp);
            if (status == 200) {
                code[0] = 0;
                check_version();
            } else {
                os_sleep(5);
            }
        }
        set_state(ws_url[0] ? XZ_READY : XZ_UNAVAILABLE, ws_url[0] ? NULL : "no_websocket");
        /* Recheck once an hour; the token may change. */
        os_sleep(3600);
    }
}

static void send_json(const char *type, const char *extra)
{
    char msg[512];
    snprintf(msg, sizeof msg, "{\"session_id\":\"%s\",\"type\":\"%s\"%s}", session, type, extra);
    net_ws_send_text(msg);
}

static void connect_task(void *arg)
{
    (void)arg;
    char h_auth[200], h_dev[64], h_cli[64];
    snprintf(h_auth, sizeof h_auth, "Authorization: Bearer %s", token);
    snprintf(h_dev, sizeof h_dev, "Device-Id: %s", device_id);
    snprintf(h_cli, sizeof h_cli, "Client-Id: %s", client_id);
    const char *const headers[] = {h_auth, "Protocol-Version: 1", h_dev, h_cli, NULL};
    if (net_ws_connect(ws_url, headers, &WS) != 0) {
        set_state(XZ_READY, "ws_failed");
        return;
    }
    net_ws_send_text("{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},\"transport\":\"websocket\","
                     "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
    /* listen/start follows the server's hello, in on_text. */
}

static void on_event(int ev, const char *text)
{
    (void)text;
    if (ev != EV_WAKE && ev != EV_WAKE_IDLE) return;
    os_lock(&lock);
    const int s = state;
    last_activity = os_now();
    const int start = ev == EV_WAKE_IDLE && s == XZ_READY;
    const int interrupt = ev == EV_WAKE && (s == XZ_LISTENING || s == XZ_SPEAKING);
    if (start) {
        state = XZ_CONNECTING;
        reason[0] = 0;
    }
    if (interrupt) play_until = 0; /* the queued speech is dropped below */
    os_unlock(&lock);
    if (interrupt) {
        /* The wake word interrupts the duck, as on the ESP32 devices. */
        sound_speech_stop();
        send_json("abort", ",\"reason\":\"wake_word_detected\"");
        send_json("listen", LISTEN_START);
        set_state(XZ_LISTENING, NULL);
    }
    if (start && os_thread(connect_task, NULL, 0, -1) != 0) set_state(XZ_READY, "no_thread");
}

/* Ends a conversation with nothing heard or said for window_s (a moving duck keeps it open); 1 when it did. */
static int tick(void)
{
    const int moving = api_moving();
    os_lock(&lock);
    const int live = state == XZ_CONNECTING || state == XZ_LISTENING || state == XZ_SPEAKING;
    if (moving || state == XZ_SPEAKING) last_activity = os_now();
    const int expired = live && os_now() - last_activity > window_s;
    os_unlock(&lock);
    if (!expired) return 0;
    net_ws_close();
    set_state(XZ_READY, NULL);
    return 1;
}

static void on_audio(const int16_t *pcm, int samples)
{
    if (tick()) {
        event_post(EV_LISTEN_TIMEOUT, "");
        printf("xiaozhi: nothing said for %.0f s, say the wake word again\n", window_s);
    }
    os_lock(&lock);
    /* The speaker may still be playing queued speech after tts/stop; the mic would send the duck's own voice. */
    const int s = os_now() < play_until + ECHO_TAIL_S ? XZ_SPEAKING : state;
    os_unlock(&lock);
    if (s != XZ_LISTENING && s != XZ_CONNECTING) {
        frame_fill = 0;
        nbacklog = 0;
        return;
    }
    if (s == XZ_LISTENING) {
        for (int i = 0; i < nbacklog; i++) net_ws_send_binary(backlog[i], (size_t)backlog_len[i]);
        nbacklog = 0;
    }
    while (samples > 0) {
        const int n = samples < FRAME_SAMPLES - frame_fill ? samples : FRAME_SAMPLES - frame_fill;
        memcpy(frame + frame_fill, pcm, (size_t)n * sizeof *pcm);
        frame_fill += n;
        pcm += n;
        samples -= n;
        if (frame_fill < FRAME_SAMPLES) break;
        frame_fill = 0;
        uint8_t opus[OPUS_MAX];
        const int len = opus_codec_encode(frame, FRAME_SAMPLES, opus, sizeof opus);
        if (len <= 0) continue;
        if (s == XZ_LISTENING) {
            net_ws_send_binary(opus, (size_t)len);
        } else if (nbacklog < BACKLOG) {
            memcpy(backlog[nbacklog], opus, (size_t)len);
            backlog_len[nbacklog++] = len;
        }
    }
}

static void copy_text(char *dst, size_t size, const cJSON *root, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(v)) snprintf(dst, size, "%s", v->valuestring);
}

static void on_text(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "state"));
    os_lock(&lock);
    last_activity = os_now();
    if (!strcmp(t, "hello")) {
        copy_text(session, sizeof session, root, "session_id");
        state = XZ_LISTENING;
        os_unlock(&lock);
        send_json("listen", LISTEN_START);
        os_lock(&lock);
    } else if (!strcmp(t, "stt")) {
        copy_text(last_stt, sizeof last_stt, root, "text");
    } else if (!strcmp(t, "llm")) {
        copy_text(emotion, sizeof emotion, root, "emotion");
    } else if (!strcmp(t, "tts") && st) {
        if (!strcmp(st, "start")) state = XZ_SPEAKING;
        if (!strcmp(st, "sentence_start")) copy_text(last_tts, sizeof last_tts, root, "text");
        if (!strcmp(st, "stop")) {
            state = XZ_LISTENING;
            os_unlock(&lock);
            send_json("listen", LISTEN_START);
            os_lock(&lock);
        }
    }
    os_unlock(&lock);
    if (!strcmp(t, "mcp")) {
        cJSON *resp = mcp_handle(cJSON_GetObjectItemCaseSensitive(root, "payload"));
        if (resp) {
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "session_id", session);
            cJSON_AddStringToObject(msg, "type", "mcp");
            cJSON_AddItemToObject(msg, "payload", resp);
            char *out = cJSON_PrintUnformatted(msg);
            net_ws_send_text(out);
            cJSON_free(out);
            cJSON_Delete(msg);
        }
    }
    cJSON_Delete(root);
}

static void on_binary(const uint8_t *data, size_t len)
{
    int16_t pcm[2880]; /* up to 180 ms at 16 kHz */
    const int n = opus_codec_decode(data, (int)len, pcm, 2880);
    const double now = os_now();
    os_lock(&lock);
    last_activity = now;
    if (n > 0) play_until = (play_until > now ? play_until : now) + n / 16000.0;
    os_unlock(&lock);
    if (n > 0) sound_speech(pcm, n);
}

static void on_closed(void)
{
    os_lock(&lock);
    if (state == XZ_CONNECTING || state == XZ_LISTENING || state == XZ_SPEAKING) state = XZ_READY;
    os_unlock(&lock);
}

/* For robot.status: the state and the last words both ways, for the voice bar's subtitles. */
static void status(cJSON *r)
{
    os_lock(&lock);
    cJSON_AddStringToObject(r, "state", STATE_NAMES[state]);
    cJSON_AddStringToObject(r, "stt", last_stt);
    cJSON_AddStringToObject(r, "tts", last_tts);
    cJSON_AddStringToObject(r, "emotion", emotion);
    os_unlock(&lock);
}

static const char *xiaozhi_status(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    os_lock(&lock);
    cJSON_AddStringToObject(r, "state", STATE_NAMES[state]);
    cJSON_AddStringToObject(r, "reason", reason);
    cJSON_AddStringToObject(r, "ota_url", ota_url);
    cJSON_AddStringToObject(r, "device_id", device_id);
    cJSON_AddStringToObject(r, "client_id", client_id);
    cJSON_AddStringToObject(r, "activation_code", code);
    cJSON_AddStringToObject(r, "activation_message", code_message);
    cJSON_AddStringToObject(r, "last_stt", last_stt);
    cJSON_AddStringToObject(r, "last_tts", last_tts);
    cJSON_AddStringToObject(r, "emotion", emotion);
    os_unlock(&lock);
    return NULL;
}

/* Runs an MCP request as the server would send it, to test the LLM's tools without the cloud. */
static const char *xiaozhi_mcp(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    cJSON *resp = mcp_handle(cJSON_GetObjectItemCaseSensitive(p, "payload"));
    if (resp) cJSON_AddItemToObject(r, "payload", resp);
    return NULL;
}

static void start(void)
{
    os_thread(task, NULL, 0, -1);
}

static const struct api_method METHODS[] = {
    {"xiaozhi.status", xiaozhi_status, 1},
    {"xiaozhi.mcp", xiaozhi_mcp, 1},
};

const struct feature xiaozhi_feature = {
    .name = "xiaozhi",
    .configure = configure,
    .start = start,
    .status = status,
    .on_event = on_event,
    .on_audio = on_audio,
    .methods = METHODS,
    .nmethods = 2,
};
