/*
 * Start-up shared by every platform (app.h). The features built in are listed
 * in FEATURES[] by Kconfig; each one configure accepts is started, its api
 * methods registered, and it receives events and the microphone. The
 * microphone is read here, once, for all of them, and the robot's own events
 * (a fall, a finished skill, torque off, a low battery) are posted from here.
 */
#include "app.h"

#include <signal.h>
#include <stdio.h>

#include "api.h"
#include "audio.h"
#include "bus.h"
#include "calib.h"
#include "config.h"
#include "duck.h"
#include "event.h"
#include "feature.h"
#include "policies.h"
#include "sound.h"

extern const struct feature web_feature, gamepad_feature, voice_feature, xiaozhi_feature, gesture_feature, tof_feature, wifi_feature;

static const struct feature *const FEATURES[] = {
#ifdef CONFIG_DOITDUCK_WEB
    &web_feature,
#endif
#ifdef CONFIG_DOITDUCK_GAMEPAD
    &gamepad_feature,
#endif
#ifdef CONFIG_DOITDUCK_VOICE
    &voice_feature,
#endif
#ifdef CONFIG_DOITDUCK_XIAOZHI
    &xiaozhi_feature,
#endif
#ifdef CONFIG_DOITDUCK_GESTURE
    &gesture_feature,
#endif
#ifdef CONFIG_DOITDUCK_TOF
    &tof_feature,
#endif
#ifdef CONFIG_DOITDUCK_WIFI
    &wifi_feature,
#endif
    NULL,
};
#define NFEATURES (int)(sizeof FEATURES / sizeof FEATURES[0] - 1)

/* The running features; fixed once app_run has started them. */
static const struct feature *running[NFEATURES + 1];
static int nrunning;
static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

void event_post(int ev, const char *text)
{
    sound_event(ev);
    for (int i = 0; i < nrunning; i++) {
        if (running[i]->on_event) running[i]->on_event(ev, text);
    }
}

void app_features(cJSON *r, const char *key)
{
    cJSON *a = cJSON_AddArrayToObject(r, key);
    for (int i = 0; i < nrunning; i++) cJSON_AddItemToArray(a, cJSON_CreateString(running[i]->name));
}

void app_status(cJSON *r)
{
    for (int i = 0; i < nrunning; i++) {
        if (running[i]->status) running[i]->status(cJSON_AddObjectToObject(r, running[i]->name));
    }
}

/* Microphone -> the features that listen. At boot the audio service may not be up yet (sdk_server starts after the app). */
static void mic_task(void *arg)
{
    (void)arg;
    int16_t pcm[160]; /* 10 ms */
    int ok = 0;
    for (;;) {
        const int n = audio_read(pcm, 160);
        if (n < 0) {
            ok = 0;
            os_sleep(5);
            continue;
        }
        if (!ok) printf("microphone open\n");
        ok = 1;
        sound_tick();
        if (n == 0 || sound_muted()) continue;
        for (int i = 0; i < nrunning; i++) {
            if (running[i]->on_audio) running[i]->on_audio(pcm, n);
        }
    }
}

/* Events of the robot itself, which the core does not post: looked for five times a second. */
static void watch(void)
{
    static int mode = -1, phase, skill = -1;
    static double low_at = -1e9;
    os_lock(&d.lock);
    const int m = d.mode, p = d.phase, s = d.skill, on = d.enabled;
    const int low = d.battery_mv > 0 && d.battery_mv < C.low_battery_mv;
    os_unlock(&d.lock);
    if (mode >= 0) {
        if (m == MODE_OFF && (mode == MODE_HOLD || mode == MODE_WALK)) event_post(EV_ESTOP, "");
        if (on && phase == PHASE_WALK && p != PHASE_WALK) event_post(EV_FALL, "");
        if (skill >= 0 && s < 0 && p == PHASE_WALK && m == mode) event_post(EV_SKILL_DONE, "");
    }
    /* The reading wavers around the limit: once in five minutes. */
    if (low && os_now() - low_at > 300) {
        low_at = os_now();
        event_post(EV_LOW_BATTERY, "");
    }
    mode = m;
    phase = p;
    skill = s;
}

int app_run(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* the autostart script logs to a file */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    config_load_board();
    if (config_load_robot(B.robot) != 0) return 1;
    duck_init();
    calib_load();
    audio_configure(&B.audio);
    sound_init();
    policies_load();
    api_register(POLICY_METHODS, NPOLICY_METHODS);
    api_register(SOUND_METHODS, NSOUND_METHODS);
    api_register(CALIB_METHODS, NCALIB_METHODS);
    for (int i = 0; i < NFEATURES; i++) {
        const struct feature *f = FEATURES[i];
        if (f->configure(config_section(f->name)) != 0) {
            printf("%s: not started\n", f->name);
            continue;
        }
        api_register(f->methods, f->nmethods);
        running[nrunning++] = f;
    }
    if (bus_open(&B.bus) != 0) return 1;
    printf("doitduck: board %s, robot %s, %d motors\n", B.name, C.name, C.n);

    /* CPU 0, with the servo UART's interrupt; the ToF's I2C polls on CPU 1 (features/tof). */
    os_thread(bus_task, NULL, 80, 0);
    os_thread(motion_task, NULL, 70, -1);
    os_thread(mic_task, NULL, 0, -1);
    for (int i = 0; i < nrunning; i++) {
        if (running[i]->start) running[i]->start();
    }
    while (!stop) {
        watch();
        calib_watch();
        os_sleep(0.2);
    }

    os_lock(&d.lock);
    arb_estop(SRC_NONE, os_now());
    os_unlock(&d.lock);
    os_sleep(0.05);
    audio_close();
    printf("doitduck: torque off, exit\n");
    return 0;
}
