/*
 * The duck's sounds (docs/doitduck-design.md §11). The quacks are WAVs, 16 kHz
 * mono 16-bit: the built-in ones in sounds/ (tools/gen_sounds.py), and the
 * user's in data/sounds/, which replace one of the same name or add a new one.
 * robots/<name>.json "sounds" says which one an event plays and when an idle
 * duck quacks by itself; the web page can change the volume and the idle quacks.
 *
 * One sound at a time, by priority: xiaozhi's speech drops every quack while
 * it plays; an event's quack interrupts a quack of the web page or an idle one,
 * which are dropped while anything plays. The mic is muted for a quack and
 * 0.15 s after it, so the duck does not hear itself. The mouth opens with the
 * loudness of every 20 ms, of a quack or of the speech, while the duck stands
 * with torque on, runs no skill and nobody else has opened its mouth.
 */
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "audio.h"
#include "config.h"
#include "duck.h"
#include "event.h"
#include "storage.h"

#define RATE 16000
#define MAX_SOUNDS 24
#define MAX_SAMPLES (3 * RATE)
#define ENV_N 320                   /* 20 ms */
#define ENV_S ((double)ENV_N / RATE)
#define MUTE_TAIL_S 0.15            /* after a quack, for the room to go quiet */
#define SPEECH_ENV 1024             /* 20 s of speech loudness */
#define PER_SOURCE_S 1.0            /* robot.sound: once a second from each source */

enum { PRI_LOW, PRI_EVENT };

/* gen_sounds.py makes these. */
static const char *const BUILTIN[] = {"quack", "quack_double", "quack_happy", "quack_question", "quack_angry",
                                      "quack_scared", "quack_sad", "quack_sleepy", "tick"};
static const char *const EVENTS[EV_COUNT] = {
    [EV_WAKE] = "wake", [EV_COMMAND_OK] = "command_ok", [EV_COMMAND_REJECTED] = "command_rejected",
    [EV_LISTEN_TIMEOUT] = "listen_timeout", [EV_SKILL_DONE] = "skill_done", [EV_FALL] = "fall",
    [EV_LOW_BATTERY] = "low_battery", [EV_ESTOP] = "estop",
};

struct sound {
    char name[24];
    const int16_t *pcm;             /* built in, or NULL */
    int n;
    void *user_file;                /* the user's WAV, which replaces it */
    const int16_t *user;
    int user_n;
    uint8_t env[MAX_SAMPLES / ENV_N]; /* loudness of each 20 ms of the one in use */
};

static struct sound sounds[MAX_SOUNDS];
static int nsounds;
static char event_sound[EV_COUNT][24];
static float volume_db;
static int idle_on;
static double idle_after = 120, idle_min = 60, idle_max = 180;
static char idle_pick[4][24];
static int nidle_pick;
static os_mutex_t lock = OS_MUTEX_INIT;

/* Playing. */
static int16_t buf[MAX_SAMPLES];
static const uint8_t *clip_env;
static int clip_nenv, clip_pri;
static double clip_start, clip_end;
static uint8_t speech_env[SPEECH_ENV];
static int speech_count, speech_acc_n;
static double speech_start, speech_end, speech_acc;
/* The mouth angle we last set, or -1 while the mouth is not ours. Only the microphone thread touches it. */
static float mouth_set = -1;
static double last_busy, next_idle, next_idle_check, last_by[SRC_COUNT];

static uint8_t loudness(double sum_squares, int n)
{
    const double db = 10 * log10(sum_squares / n / (32768.0 * 32768.0) + 1e-12);
    const double l = (db + 45) / 35; /* -45 dBFS closed, -10 dBFS wide open */
    return l <= 0 ? 0 : l >= 1 ? 255 : (uint8_t)(l * 255);
}

/* The samples of a 16 kHz mono 16-bit WAV, or NULL. */
static const int16_t *wav_pcm(const uint8_t *w, size_t size, int *n)
{
    int fmt_ok = 0;
    if (size < 12 || memcmp(w, "RIFF", 4) || memcmp(w + 8, "WAVE", 4)) return NULL;
    for (size_t i = 12; i + 8 <= size;) {
        const uint32_t len = w[i + 4] | w[i + 5] << 8 | w[i + 6] << 16 | (uint32_t)w[i + 7] << 24;
        const uint8_t *c = w + i + 8;
        if (len > size - i - 8) return NULL;
        if (!memcmp(w + i, "fmt ", 4) && len >= 16)
            fmt_ok = (c[0] | c[1] << 8) == 1 && (c[2] | c[3] << 8) == 1 &&
                     (c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24) == RATE && (c[14] | c[15] << 8) == 16;
        if (!memcmp(w + i, "data", 4)) {
            /* RIFF chunks start on even offsets, so the samples are aligned in the malloc'd file. */
            if (!fmt_ok || len / 2 > MAX_SAMPLES || len < 2) return NULL;
            *n = (int)(len / 2);
            return (const int16_t *)c;
        }
        i += 8 + len + (len & 1);
    }
    return NULL;
}

static void make_env(struct sound *s)
{
    const int16_t *p = s->user ? s->user : s->pcm;
    const int n = s->user ? s->user_n : s->n;
    for (int k = 0; k * ENV_N < n; k++) {
        double sum = 0;
        int m = 0;
        for (int i = k * ENV_N; i < n && i < (k + 1) * ENV_N; i++, m++) sum += (double)p[i] * p[i];
        s->env[k] = loudness(sum, m);
    }
}

static int find(const char *name)
{
    for (int i = 0; i < nsounds; i++) {
        if (!strcmp(sounds[i].name, name)) return i;
    }
    return -1;
}

/* The user's WAV for sound name, replacing or adding; with lock held. NULL or an error code. */
static const char *add_user(const char *name, void *file, size_t size)
{
    int n = 0;
    const int16_t *pcm = wav_pcm(file, size, &n);
    if (!pcm) return "sound_format";
    int k = find(name);
    if (k < 0 && nsounds == MAX_SOUNDS) return "sound_full";
    if (k < 0) {
        k = nsounds++;
        memset(&sounds[k], 0, sizeof sounds[k]);
        snprintf(sounds[k].name, sizeof sounds[k].name, "%s", name);
    }
    if (clip_env == sounds[k].env) clip_end = 0; /* replaced while playing */
    free(sounds[k].user_file);
    sounds[k].user_file = file;
    sounds[k].user = pcm;
    sounds[k].user_n = n;
    make_env(&sounds[k]);
    return NULL;
}

static void load_settings(const cJSON *s)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, "volume_db");
    if (cJSON_IsNumber(v)) volume_db = (float)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(s, "idle");
    if (cJSON_IsBool(v)) idle_on = cJSON_IsTrue(v);
}

void sound_init(void)
{
    const cJSON *cfg = config_section("sounds"), *e;
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++) {
        char path[64];
        size_t size = 0;
        snprintf(path, sizeof path, "sounds/%s.wav", BUILTIN[i]);
        const uint8_t *w = store_blob(path, &size);
        struct sound *s = &sounds[nsounds];
        s->pcm = w ? wav_pcm(w, size, &s->n) : NULL;
        if (!s->pcm) {
            printf("sound: cannot read %s\n", path);
            continue;
        }
        snprintf(s->name, sizeof s->name, "%s", BUILTIN[i]);
        make_env(s);
        nsounds++;
    }
    char names[MAX_SOUNDS][32];
    const int n = store_list("sounds", names, MAX_SOUNDS);
    for (int i = 0; i < n; i++) {
        const size_t len = strlen(names[i]);
        if (len < 5 || strcmp(names[i] + len - 4, ".wav")) continue;
        char path[64], name[24];
        size_t size = 0;
        snprintf(path, sizeof path, "sounds/%.31s", names[i]);
        snprintf(name, sizeof name, "%.*s", (int)(len - 4), names[i]);
        void *file = store_get(path, &size);
        const char *err = file ? add_user(name, file, size) : "sound_storage";
        if (err) {
            printf("sound: %s: %s\n", path, err);
            free(file);
        }
    }

    for (int k = 0; k < EV_COUNT; k++) {
        const cJSON *v = EVENTS[k] ? cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cfg, "events"), EVENTS[k]) : NULL;
        if (cJSON_IsString(v)) snprintf(event_sound[k], sizeof event_sound[k], "%s", v->valuestring);
    }
    const cJSON *idle = cJSON_GetObjectItemCaseSensitive(cfg, "idle");
    const cJSON *every = cJSON_GetObjectItemCaseSensitive(idle, "every_s");
    idle_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(idle, "enabled"));
    const cJSON *after = cJSON_GetObjectItemCaseSensitive(idle, "after_s");
    if (cJSON_IsNumber(after)) idle_after = after->valuedouble;
    if (cJSON_GetArraySize(every) == 2) {
        idle_min = cJSON_GetArrayItem(every, 0)->valuedouble;
        idle_max = cJSON_GetArrayItem(every, 1)->valuedouble;
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(idle, "pick")) {
        if (nidle_pick < 4 && cJSON_IsString(e)) snprintf(idle_pick[nidle_pick++], sizeof idle_pick[0], "%s", e->valuestring);
    }
    load_settings(cfg);
    /* The web page's settings, kept across restarts. */
    char text[128];
    cJSON *saved = store_kv_get("sound.json", text, sizeof text) > 0 ? cJSON_Parse(text) : NULL;
    load_settings(saved);
    cJSON_Delete(saved);
    printf("sound: %d sounds, volume %.0f dB, idle quacks %s\n", nsounds, volume_db, idle_on ? "on" : "off");
}

/* Starts sound k at priority pri, with lock held; NULL or an error code. */
static const char *play(int k, int pri)
{
    const double now = os_now();
    if (now < speech_end || (pri == PRI_LOW && now < clip_end)) return "sound_busy";
    const struct sound *s = &sounds[k];
    const int16_t *p = s->user ? s->user : s->pcm;
    const int n = s->user ? s->user_n : s->n;
    const float gain = powf(10, volume_db / 20);
    for (int i = 0; i < n; i++) {
        const float v = p[i] * gain;
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (audio_play(buf, n, RATE) != 0) return "no_audio";
    clip_env = s->env;
    clip_nenv = (n + ENV_N - 1) / ENV_N;
    clip_start = now;
    clip_end = now + (double)n / RATE;
    clip_pri = pri;
    return NULL;
}

void sound_event(int ev)
{
    os_lock(&lock);
    last_busy = os_now();
    const int k = event_sound[ev][0] ? find(event_sound[ev]) : -1;
    if (k >= 0) play(k, PRI_EVENT);
    os_unlock(&lock);
}

int sound_muted(void)
{
    os_lock(&lock);
    const int m = os_now() < clip_end + MUTE_TAIL_S;
    os_unlock(&lock);
    return m;
}

void sound_speech(const int16_t *pcm, int samples)
{
    const double now = os_now();
    os_lock(&lock);
    if (now >= speech_end) {
        speech_start = speech_end = now;
        speech_count = speech_acc_n = 0;
        speech_acc = 0;
    }
    for (int i = 0; i < samples; i++) {
        speech_acc += (double)pcm[i] * pcm[i];
        if (++speech_acc_n < ENV_N) continue;
        speech_env[speech_count++ % SPEECH_ENV] = loudness(speech_acc, ENV_N);
        speech_acc = 0;
        speech_acc_n = 0;
    }
    speech_end += (double)samples / RATE;
    os_unlock(&lock);
    audio_stream(pcm, samples);
}

void sound_speech_stop(void)
{
    os_lock(&lock);
    speech_end = 0;
    os_unlock(&lock);
    audio_flush();
}

/* The mouth follows level (0..255), or closes when nothing plays (-1). */
static void mouth(int level)
{
    if (level < 0 && mouth_set < 0) return;
    const float deg = level > 8 ? level / 255.0f * C.mouth_max : 0;
    if (level >= 0 && mouth_set >= 0 && fabsf(deg - mouth_set) < 2) return;
    os_lock(&d.lock);
    if (mouth_set < 0) {
        /* Only a standing duck with no skill, whose mouth nobody has opened. */
        if (deg > 0 && C.mouth >= 0 && d.enabled && d.skill < 0 && d.mouth == 0) d.mouth = mouth_set = deg;
    } else if (d.mouth != mouth_set || d.skill >= 0 || !d.enabled) {
        mouth_set = -1; /* someone else has it now */
    } else {
        d.mouth = deg;
        mouth_set = level < 0 ? -1 : deg;
    }
    os_unlock(&d.lock);
}

/* A standing duck nobody has touched for idle_after quacks now and then. */
static void idle(double now)
{
    if (now < next_idle_check) return;
    next_idle_check = now + 1;
    os_lock(&d.lock);
    const int busy = d.owner != SRC_NONE || d.skill >= 0 || !d.enabled;
    os_unlock(&d.lock);
    os_lock(&lock);
    if (busy || !idle_on) last_busy = now;
    if (now - last_busy < idle_after) {
        next_idle = 0;
    } else if (next_idle == 0) {
        next_idle = now + idle_min + (idle_max - idle_min) * rand() / RAND_MAX;
    } else if (now >= next_idle && nidle_pick) {
        const int k = find(idle_pick[rand() % nidle_pick]);
        if (k >= 0) play(k, PRI_LOW);
        next_idle = now + idle_min + (idle_max - idle_min) * rand() / RAND_MAX;
    }
    os_unlock(&lock);
}

void sound_tick(void)
{
    const double now = os_now();
    int level = -1;
    os_lock(&lock);
    if (now < speech_end) {
        const int i = (int)((now - speech_start) / ENV_S);
        level = i < speech_count && speech_count - i <= SPEECH_ENV ? speech_env[i % SPEECH_ENV] : 0;
    } else if (now < clip_end) {
        const int i = (int)((now - clip_start) / ENV_S);
        level = i < clip_nenv ? clip_env[i] : 0;
    }
    os_unlock(&lock);
    mouth(level);
    idle(now);
}

/* {name}: from any source and without control, as it moves nothing but the mouth; once a second from each. */
static const char *robot_sound(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
    const double now = os_now();
    os_lock(&lock);
    const int k = cJSON_IsString(name) ? find(name->valuestring) : -1;
    const char *err = k < 0 ? "no_such_sound" : now - last_by[src] < PER_SOURCE_S ? "sound_too_often" : play(k, PRI_LOW);
    if (!err) {
        last_by[src] = now;
        last_busy = now;
    }
    os_unlock(&lock);
    return err;
}

static const char *sound_list(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    cJSON *list = cJSON_AddArrayToObject(r, "sounds");
    os_lock(&lock);
    for (int i = 0; i < nsounds; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", sounds[i].name);
        cJSON_AddStringToObject(o, "source", sounds[i].user ? "user" : "builtin");
        cJSON_AddBoolToObject(o, "builtin", sounds[i].pcm != NULL);
        cJSON_AddNumberToObject(o, "seconds", (double)(sounds[i].user ? sounds[i].user_n : sounds[i].n) / RATE);
        cJSON_AddItemToArray(list, o);
    }
    cJSON *events = cJSON_AddObjectToObject(r, "events");
    for (int k = 0; k < EV_COUNT; k++) {
        if (EVENTS[k]) cJSON_AddStringToObject(events, EVENTS[k], event_sound[k]);
    }
    cJSON_AddNumberToObject(r, "volume_db", volume_db);
    cJSON_AddBoolToObject(r, "idle", idle_on);
    os_unlock(&lock);
    return NULL;
}

/* {name, wav}: a 16 kHz mono 16-bit WAV, base64, at most 3 s; the web page converts the user's recording. */
static const char *sound_upload(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
    const cJSON *wav = cJSON_GetObjectItemCaseSensitive(p, "wav");
    if (!cJSON_IsString(name) || !api_good_name(name->valuestring, sizeof sounds[0].name)) return "sound_name";
    if (!cJSON_IsString(wav)) return "sound_format";
    const size_t cap = strlen(wav->valuestring) / 4 * 3 + 3;
    uint8_t *file = malloc(cap);
    const int n = file ? api_base64_decode(wav->valuestring, file, cap) : -1;
    int samples;
    char path[64];
    snprintf(path, sizeof path, "sounds/%s.wav", name->valuestring);
    const char *err = n < 0 || !wav_pcm(file, (size_t)n, &samples) ? "sound_format"
                    : store_put(path, file, (size_t)n) != 0 ? "sound_storage" : NULL;
    if (!err) {
        os_lock(&lock);
        err = add_user(name->valuestring, file, (size_t)n);
        os_unlock(&lock);
    }
    if (err) free(file);
    return err;
}

static const char *sound_remove(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
    const char *err = NULL;
    os_lock(&lock);
    const int k = cJSON_IsString(name) ? find(name->valuestring) : -1;
    if (k < 0 || !sounds[k].user) {
        err = k < 0 ? "no_such_sound" : "sound_builtin";
    } else {
        if (clip_env == sounds[k].env) clip_end = 0;
        free(sounds[k].user_file);
        sounds[k].user_file = NULL;
        sounds[k].user = NULL;
        if (sounds[k].pcm) make_env(&sounds[k]);
        else sounds[k] = sounds[--nsounds];
    }
    os_unlock(&lock);
    if (err) return err;
    char path[64];
    snprintf(path, sizeof path, "sounds/%s.wav", name->valuestring);
    store_remove(path);
    return NULL;
}

/* {volume_db, idle}, each optional; kept across restarts. */
static const char *sound_settings(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, "volume_db");
    if (v && !(cJSON_IsNumber(v) && v->valuedouble >= -40 && v->valuedouble <= 10)) return "bad_volume";
    char text[96];
    os_lock(&lock);
    load_settings(p);
    snprintf(text, sizeof text, "{\"volume_db\": %.1f, \"idle\": %s}\n", volume_db, idle_on ? "true" : "false");
    cJSON_AddNumberToObject(r, "volume_db", volume_db);
    cJSON_AddBoolToObject(r, "idle", idle_on);
    os_unlock(&lock);
    store_kv_set("sound.json", text);
    return NULL;
}

const struct api_method SOUND_METHODS[] = {
    {"robot.sound", robot_sound, 0},
    {"sound.list", sound_list, 0},
    {"sound.upload", sound_upload, 1},
    {"sound.remove", sound_remove, 1},
    {"sound.settings", sound_settings, 1},
};
const int NSOUND_METHODS = sizeof SOUND_METHODS / sizeof SOUND_METHODS[0];
