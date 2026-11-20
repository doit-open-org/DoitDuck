/*
 * The BluFi feature: while the hotspot is open (wifi.ap), the duck also takes a
 * network over BLE by Espressif's BluFi (the 四博AI mini program), as DTXZ_ and
 * the Wi-Fi MAC with no colons (xiaozhi's Device-Id), e.g. DTXZ_10b06e94224e:
 * the mini program lists only names that start with DTXZ_.
 * The network goes to wifi.connect as one from the web page does (and the duck
 * reads its address aloud); the phone hears back once the duck is on it, then as
 * custom data {"code": "571480", "ip": "192.168.1.23", "url": "http://192.168.1.23"}:
 * code is xiaozhi's activation code, "" once activated or with no xiaozhi.
 * Then the hotspot closes, and BLE with it, unless the hotspot is to stay open.
 * components/blufi: the protocol, its security, and the BLE side on BlueZ,
 * which the image's ble-service starts.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "blufi_ble.h"
#include "blufi_protocol.h"
#include "duck.h"
#include "event.h"
#include "feature.h"

#define JOIN_S 60   /* how long the duck has to get on the network */
#define RETRY_S 2   /* BLE not up yet (at boot hci0 comes after the program, or the adapter Busy): try again after this */

static os_mutex_t lock = OS_MUTEX_INIT; /* the protocol and the state below */
static blufi_protocol *protocol;
static blufi_ble *ble;
static int hotspot;                     /* the hotspot is opening or up (EV_HOTSPOT_OPENING, EV_HOTSPOT_OPEN) */
static blufi_sta_config wanted;         /* a network from the phone, for task() to join */
static int join_wanted, scan_wanted;

/* The protocol's callbacks, from receive() with lock held. */
static int send_frame(const uint8_t *data, size_t len, void *context)
{
    (void)context;
    return blufi_ble_send(ble, data, len);
}

static void join(const blufi_sta_config *config, void *context)
{
    (void)context;
    wanted = *config;
    join_wanted = 1;
}

/* The phone asks to leave the network: the duck stays on it. */
static void leave(void *context)
{
    (void)context;
}

static void scan(void *context)
{
    (void)context;
    scan_wanted = 1;
}

static void drop_phone(void *context)
{
    (void)context;
    blufi_ble_disconnect(ble);
}

/* The BLE side's callbacks, from its thread. */
static void receive(const uint8_t *data, size_t len, void *context)
{
    (void)context;
    os_lock(&lock);
    if (blufi_protocol_receive(protocol, data, len) != 0) printf("blufi: a frame refused\n");
    os_unlock(&lock);
}

static void connection(bool connected, void *context)
{
    (void)context;
    os_lock(&lock);
    blufi_protocol_reset(protocol);
    os_unlock(&lock);
    printf("blufi: phone %s\n", connected ? "connected" : "gone");
}

/* A wifi.* method's result (free it), NULL on an error. */
static cJSON *wifi(const char *method, const cJSON *params)
{
    const char *err = NULL;
    cJSON *r = api_call(SRC_WEB, method, params, &err);
    if (err) printf("blufi: %s: %s\n", method, err);
    return r;
}

static const char *field(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void start_ble(const char *name)
{
    const blufi_protocol_callbacks pc = {.send = send_frame, .connect = join, .disconnect = leave, .scan = scan,
                                         .disconnect_ble = drop_phone};
    const blufi_ble_callbacks bc = {.receive = receive, .connection = connection};
    protocol = blufi_protocol_create(&pc);
    ble = blufi_ble_create(&bc);
    if (blufi_ble_start(ble, name) == 0) {
        printf("blufi: advertising as %s\n", name);
        return;
    }
    blufi_ble_destroy(ble);
    blufi_protocol_destroy(protocol);
    ble = NULL;
    protocol = NULL;
}

static void stop_ble(void)
{
    blufi_ble_destroy(ble); /* its thread ends here: no callback after */
    blufi_protocol_destroy(protocol);
    ble = NULL;
    protocol = NULL;
    printf("blufi: stopped\n");
}

/* wifi.scan holds the networks found by the scan before; it starts a new one, read a moment later. */
static void report_networks(void)
{
    cJSON_Delete(wifi("wifi.scan", NULL));
    os_sleep(3);
    cJSON *r = wifi("wifi.scan", NULL);
    blufi_scan_result nets[BLUFI_SCAN_RESULT_MAX];
    size_t n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(r, "networks")) {
        const char *ssid = field(e, "ssid");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "signal");
        const double signal = cJSON_IsNumber(v) ? v->valuedouble : -100;
        if (n == BLUFI_SCAN_RESULT_MAX || strlen(ssid) > BLUFI_SSID_MAX_LEN) continue;
        nets[n].ssid_len = strlen(ssid);
        memcpy(nets[n].ssid, ssid, nets[n].ssid_len);
        nets[n++].rssi = (int8_t)(signal < -128 ? -128 : signal > 0 ? 0 : signal);
    }
    cJSON_Delete(r);
    os_lock(&lock);
    blufi_protocol_report_scan_results(protocol, nets, n);
    os_unlock(&lock);
}

/* The network from the phone through wifi.connect; then up to JOIN_S for it and an address, and the phone told:
 * the Wi-Fi status, and once on the network the activation code and address as custom data. */
static void join_network(const blufi_sta_config *c)
{
    char ssid[BLUFI_SSID_MAX_LEN + 1], password[BLUFI_PASSWORD_MAX_LEN + 1];
    snprintf(ssid, sizeof ssid, "%.*s", (int)c->ssid_len, (const char *)c->ssid);
    snprintf(password, sizeof password, "%.*s", (int)c->password_len, (const char *)c->password);
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "ssid", ssid);
    cJSON_AddStringToObject(p, "password", password);
    cJSON *r = wifi("wifi.connect", p);
    cJSON_Delete(p);
    printf("blufi: joining %s\n", ssid);
    blufi_sta_state state = BLUFI_STA_DISCONNECTED;
    char ip[32] = "";
    for (int i = 0; r && i < JOIN_S * 2 && state != BLUFI_STA_CONNECTED; i++) {
        os_sleep(0.5);
        cJSON *s = wifi("wifi.status", NULL);
        const char *now = field(s, "ip");
        if (!strcmp(field(s, "state"), "COMPLETED") && !strcmp(field(s, "ssid"), ssid)) {
            state = now[0] && strcmp(now, "0.0.0.0") ? BLUFI_STA_CONNECTED : BLUFI_STA_NO_IP;
            snprintf(ip, sizeof ip, "%s", now);
        }
        cJSON_Delete(s);
    }
    cJSON_Delete(r);
    printf("blufi: %s %s %s\n", ssid, state == BLUFI_STA_CONNECTED ? "joined" : "not joined", ip);
    os_lock(&lock);
    blufi_protocol_report_wifi_status(protocol, state, (const uint8_t *)ssid, strlen(ssid), NULL, 0);
    os_unlock(&lock);
    if (state != BLUFI_STA_CONNECTED) return;

    /* xiaozhi asks its server again within 10 s of the duck being on a network: up to 20 s for its
     * activation code, or for it to be activated already (no code). */
    char code[16] = "", url[48];
    for (int i = 0; i < 40; i++) {
        const char *err = NULL;
        cJSON *s = api_call(SRC_WEB, "xiaozhi.status", NULL, &err);
        const char *xz = field(s, "state");
        snprintf(code, sizeof code, "%s", field(s, "activation_code"));
        const int done = err || code[0] || (strcmp(xz, "checking") && strcmp(xz, "unavailable"));
        cJSON_Delete(s);
        if (done) break;
        os_sleep(0.5);
    }
    snprintf(url, sizeof url, "http://%s", ip);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "url", url);
    char *text = cJSON_PrintUnformatted(o);
    printf("blufi: to the phone %s\n", text);
    os_lock(&lock);
    blufi_protocol_send_custom(protocol, (const uint8_t *)text, strlen(text));
    os_unlock(&lock);
    cJSON_free(text);
    cJSON_Delete(o);

    /* Then the hotspot closes, as the web page closes it, and BLE with it (unless it stays open);
     * a moment first for the phone to have the data. */
    os_sleep(2);
    cJSON *w = wifi("wifi.status", NULL);
    const int always = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(w, "ap_always"));
    cJSON_Delete(w);
    if (always) return;
    cJSON *off = cJSON_CreateObject();
    cJSON_AddFalseToObject(off, "on");
    cJSON_Delete(wifi("wifi.ap", off));
    cJSON_Delete(off);
}

/* BLE up while the hotspot is, and what the phone asked for. */
static void task(void *arg)
{
    (void)arg;
    double retry_at = 0;
    for (;;) {
        os_sleep(0.5);
        blufi_sta_config c;
        os_lock(&lock);
        const int open = hotspot, join_now = join_wanted, scan_now = scan_wanted;
        c = wanted;
        join_wanted = scan_wanted = 0;
        os_unlock(&lock);
        if (open && !ble && os_now() >= retry_at) {
            char mac[18], name[24] = "DTXZ_";
            os_device_id(mac);
            int n = 5;
            for (int i = 0; mac[i]; i++) {
                if (mac[i] != ':') name[n++] = mac[i];
            }
            name[n] = 0;
            start_ble(name);
            if (!ble) retry_at = os_now() + RETRY_S;
        }
        if (!open && ble) stop_ble();
        if (!ble) continue;
        if (scan_now) report_networks();
        if (join_now) join_network(&c);
    }
}

static void on_event(int ev, const char *text)
{
    (void)text;
    if (ev != EV_HOTSPOT_OPENING && ev != EV_HOTSPOT_OPEN && ev != EV_HOTSPOT_CLOSED) return;
    os_lock(&lock);
    hotspot = ev != EV_HOTSPOT_CLOSED;
    os_unlock(&lock);
}

static int configure(const cJSON *section)
{
    (void)section;
    if (access("/usr/sbin/ble-service", X_OK) == 0) return 0;
    printf("blufi: no ble-service in the image (BR2_PACKAGE_AIC8800D_SDIO_BLE); BluFi off\n");
    return -1;
}

static void start(void)
{
    os_thread(task, NULL, 0, -1);
}

const struct feature blufi_feature = {
    .name = "blufi",
    .configure = configure,
    .start = start,
    .on_event = on_event,
};
