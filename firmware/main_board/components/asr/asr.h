#ifndef ASR_H
#define ASR_H

/*
 * Speech recognition, text in and text out. Implementations:
 *   mipk_wake.c  keyword models (mipk MicroWakeWord, models/<name>.mipk): the
 *                wake word, for xiaozhi, and command words heard at any time
 *   none.c       nothing
 * After a wake word an engine with ASR_CAP_COMMAND listens for command words
 * until window_s passes with none, reports ASR_TIMEOUT and listens for the wake
 * word again. Command words with a model of their own are reported at any time
 * (voice runs them only after the wake word).
 */
#include <stddef.h>
#include <stdint.h>

struct asr_cfg {
    const char *const *wake;
    int nwake;
    const char *wake_model;               /* models/<wake_model>.mipk, or NULL for the engine's own */
    const char *const *commands;          /* robots/<name>.json voice.commands */
    const char *const *command_models;    /* each command's model, or NULL: a word the web page simulates */
    int ncommands;
    float wake_threshold, command_threshold;
    double window_s;
};

enum { ASR_NONE, ASR_WAKE, ASR_COMMAND, ASR_TIMEOUT };
enum { ASR_CAP_WAKE = 1, ASR_CAP_COMMAND = 2 };

int asr_open(const struct asr_cfg *cfg);   /* 0 on success */
int asr_caps(void);                        /* ASR_CAP_* */
/* Feeds 16 kHz PCM; returns ASR_* and, for a wake or command word, its text and score. */
int asr_feed(const int16_t *pcm, int samples, char *text, size_t size, float *score);
/* Restarts the command window, so "停下来" is still heard while the robot moves. */
void asr_touch(void);

#endif
