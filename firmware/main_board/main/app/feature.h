#ifndef FEATURE_H
#define FEATURE_H

/*
 * A feature: the web page, the gamepad, voice, xiaozhi, gestures, the ToF, Wi-Fi. Each is built
 * in or left out by Kconfig and listed in app.c's FEATURES[]; at start app.c
 * calls configure and, when that returns 0, start. Features never include each
 * other: they act through api_call and talk through events (event.h).
 */
#include <stdint.h>

#include "cJSON.h"

/* An api method: returns NULL or an error code (api.c ERRORS), filling the result r. */
typedef const char *(*method_fn)(int src, const cJSON *p, cJSON *r);

struct api_method {
    const char *name;
    method_fn fn;
    int web_only;
};

struct feature {
    const char *name;                             /* its section in robots/<name>.json, and its field in robot.status */
    int (*configure)(const cJSON *section);       /* non-zero: do not start (a capability is missing, or the config turns it off) */
    void (*start)(void);                          /* optional: starts its own threads */
    void (*status)(cJSON *r);                     /* optional: live state for robot.status */
    void (*on_event)(int ev, const char *text);   /* optional: see event.h */
    void (*on_audio)(const int16_t *pcm, int n);  /* optional: microphone frames, 16 kHz, none while a prompt plays */
    const struct api_method *methods;             /* optional: its own api methods */
    int nmethods;
};

#endif
