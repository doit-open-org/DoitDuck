/*
 * The voice feature: a wake word, then command words, each mapped to an api
 * call by robots/<name>.json "voice". After the wake word, command words are
 * accepted until window_s passes with none. Commands marked "walk" switch to
 * walk mode first, so "向前走" works from standing. While the robot moves the
 * window stays open, so "停下来" always works.
 *
 * The wake word is posted as EV_WAKE. An engine with no command words (mipk,
 * the xiaozhi version) also posts EV_WAKE_IDLE at once, which starts a xiaozhi
 * conversation. A command with a keyword model of its own ("model", mipk
 * MicroWakeWord, models/<model>.mipk) is heard at any time, but runs only after
 * the wake word, within window_s, as the others do.
 * With no engine, voice.command from the web page still simulates words.
 */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "asr.h"
#include "duck.h"
#include "event.h"
#include "feature.h"

#define MAX_COMMANDS 48
#define MAX_WAKE 4

struct command {
    char words[32], method[32], model[32];
    int walk;
    cJSON *params;
};

static struct command commands[MAX_COMMANDS];
static char wake_words[MAX_WAKE][32];
static const char *wake_list[MAX_WAKE], *command_list[MAX_COMMANDS], *model_list[MAX_COMMANDS];
static char wake_model[32];
static int ncommands, nwake, engine_ok, engine_tried, enabled = 1;
static float wake_threshold = 0.79f, command_threshold = 0.80f;
static double window_s = 10, awake_until, last_audio;
static char last_words[32];
static const char *last_result = "";
static float last_score;
static os_mutex_t lock = OS_MUTEX_INIT;

static const char *text(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static int configure(const cJSON *section)
{
    const cJSON *e;
    wake_threshold = (float)number(section, "wake_threshold", wake_threshold);
    command_threshold = (float)number(section, "command_threshold", command_threshold);
    window_s = number(section, "window_s", window_s);
    snprintf(wake_model, sizeof wake_model, "%s", text(section, "wake_model"));
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(section, "wake_words")) {
        if (nwake < MAX_WAKE && cJSON_IsString(e)) snprintf(wake_words[nwake++], sizeof wake_words[0], "%s", e->valuestring);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(section, "commands")) {
        if (ncommands == MAX_COMMANDS || !text(e, "words")[0] || !text(e, "call")[0]) continue;
        struct command *c = &commands[ncommands++];
        snprintf(c->words, sizeof c->words, "%s", text(e, "words"));
        snprintf(c->method, sizeof c->method, "%s", text(e, "call"));
        snprintf(c->model, sizeof c->model, "%s", text(e, "model"));
        c->walk = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "walk"));
        c->params = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(e, "params"), 1);
    }
    for (int i = 0; i < nwake; i++) wake_list[i] = wake_words[i];
    for (int i = 0; i < ncommands; i++) {
        command_list[i] = commands[i].words;
        model_list[i] = commands[i].model[0] ? commands[i].model : NULL;
    }
    return 0;
}

static void remember(const char *words, float score, const char *result)
{
    os_lock(&lock);
    snprintf(last_words, sizeof last_words, "%s", words);
    last_result = result;
    last_score = score;
    os_unlock(&lock);
}

static const char *run(const struct command *c)
{
    const char *err = NULL;
    os_lock(&d.lock);
    const int walking = d.mode == MODE_WALK;
    os_unlock(&d.lock);
    if (c->walk && !walking) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "mode", "walk");
        cJSON_Delete(api_call(SRC_VOICE, "robot.set_mode", p, &err));
        cJSON_Delete(p);
        if (err) return err;
    }
    /* A movement carries its own duration, so voice holds no lease. */
    cJSON_Delete(api_call(SRC_VOICE, c->method, c->params, &err));
    return err;
}

static void wake(const char *words, float score)
{
    os_lock(&lock);
    awake_until = os_now() + window_s;
    os_unlock(&lock);
    remember(words, score, "woken");
    event_post(EV_WAKE, words);
    /* No command words to wait for: the wake word is for xiaozhi. */
    if (!(asr_caps() & ASR_CAP_COMMAND)) event_post(EV_WAKE_IDLE, words);
}

static const char *command(const char *words, int ignore_wake, float score)
{
    const double now = os_now();
    for (int i = 0; i < ncommands; i++) {
        if (strcmp(words, commands[i].words)) continue;
        os_lock(&lock);
        const int awake = ignore_wake || now < awake_until;
        if (awake) awake_until = now + window_s;
        os_unlock(&lock);
        if (!awake) return "wake_first";
        const char *err = run(&commands[i]);
        remember(words, score, err ? err : "done");
        event_post(err ? EV_COMMAND_REJECTED : EV_COMMAND_OK, words);
        return err;
    }
    remember(words, score, "unknown_words");
    return "unknown_words";
}

static void on_audio(const int16_t *pcm, int n)
{
    os_lock(&lock);
    last_audio = os_now();
    const int on = enabled;
    os_unlock(&lock);
    if (!on) return;
    /* Opened on the microphone's thread, which then runs it: an NPU model must run on the thread that set it up. */
    if (!engine_tried) {
        engine_tried = 1;
        const struct asr_cfg cfg = {.wake = wake_list, .nwake = nwake, .wake_model = wake_model[0] ? wake_model : NULL,
                                    .commands = command_list, .command_models = model_list, .ncommands = ncommands,
                                    .wake_threshold = wake_threshold, .command_threshold = command_threshold,
                                    .window_s = window_s};
        engine_ok = asr_open(&cfg) == 0;
        if (!engine_ok) printf("voice: no recognition engine; voice.command still simulates words\n");
    }
    if (engine_ok && api_moving()) asr_touch();
    char words[64];
    float score = 0;
    const int ev = engine_ok ? asr_feed(pcm, n, words, sizeof words, &score) : ASR_NONE;
    if (ev == ASR_WAKE) {
        printf("voice: %s (%.2f)\n", words, score);
        wake(words, score);
    } else if (ev == ASR_COMMAND) {
        const char *err = command(words, 0, score);
        printf("voice: %s (%.2f) %s\n", words, score, err ? err : "");
    } else if (ev == ASR_TIMEOUT) {
        event_post(EV_LISTEN_TIMEOUT, "");
        os_lock(&lock);
        awake_until = 0;
        os_unlock(&lock);
        printf("voice: timeout, say the wake word again\n");
    }
}

/* From the web page: a wake word wakes the duck, other words are commands. */
static const char *voice_command(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const char *words = text(p, "words");
    for (int i = 0; i < nwake; i++) {
        if (strcmp(words, wake_words[i])) continue;
        wake(words, 0);
        return NULL;
    }
    return command(words, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "ignore_wake")), 0);
}

/* For robot.status, ten times a second: what the web page's voice bar shows. */
static void status(cJSON *r)
{
    const double now = os_now();
    os_lock(&lock);
    cJSON_AddBoolToObject(r, "on", enabled);
    cJSON_AddBoolToObject(r, "engine", engine_ok);
    cJSON_AddBoolToObject(r, "audio", now - last_audio < 2);
    cJSON_AddNumberToObject(r, "awake_s", now < awake_until ? awake_until - now : 0);
    cJSON_AddStringToObject(r, "last_words", last_words);
    cJSON_AddNumberToObject(r, "last_score", last_score);
    cJSON_AddStringToObject(r, "last_result", last_result);
    os_unlock(&lock);
}

/* {on}: off, nothing is listened for, not even "停下来". */
static const char *voice_enable(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    os_lock(&lock);
    enabled = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "on"));
    if (!enabled) awake_until = 0;
    os_unlock(&lock);
    return NULL;
}

/* As if the wake word was heard: starts a xiaozhi conversation, or interrupts the duck while it speaks. */
static const char *voice_wake(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    (void)r;
    wake(nwake ? wake_words[0] : "", 0);
    return NULL;
}

static const char *voice_status(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    os_lock(&lock);
    cJSON_AddBoolToObject(r, "engine", engine_ok);
    cJSON_AddBoolToObject(r, "audio", os_now() - last_audio < 2);
    cJSON_AddBoolToObject(r, "awake", os_now() < awake_until);
    cJSON_AddStringToObject(r, "last_words", last_words);
    cJSON_AddNumberToObject(r, "last_score", last_score);
    cJSON_AddStringToObject(r, "last_result", last_result);
    os_unlock(&lock);
    cJSON *w = cJSON_AddArrayToObject(r, "wake_words");
    for (int i = 0; i < nwake; i++) cJSON_AddItemToArray(w, cJSON_CreateString(wake_words[i]));
    cJSON *list = cJSON_AddArrayToObject(r, "commands");
    for (int i = 0; i < ncommands; i++) cJSON_AddItemToArray(list, cJSON_CreateString(commands[i].words));
    return NULL;
}

static const struct api_method METHODS[] = {
    {"voice.command", voice_command, 1},
    {"voice.status", voice_status, 1},
    {"voice.enable", voice_enable, 1},
    {"voice.wake", voice_wake, 1},
};

const struct feature voice_feature = {
    .name = "voice",
    .configure = configure,
    .on_audio = on_audio,
    .methods = METHODS,
    .status = status,
    .nmethods = 4,
};
