#ifndef SOUND_H
#define SOUND_H

/*
 * The duck's sounds (sound.c): the quack library, playback by priority, the
 * mouth moving with the sound, and xiaozhi's speech.
 */
#include <stdint.h>

#include "cJSON.h"
#include "feature.h"

void sound_init(void);
void sound_event(int ev);          /* the sound robots/<name>.json gives this event, if any */
int sound_muted(void);             /* a sound is playing, so the mic would hear the duck itself */
void sound_tick(void);             /* every microphone frame: the mouth, quacks when idle */
/* xiaozhi's speech (16 kHz): queued behind what is still playing; drops other sounds while it plays. */
int sound_speech(const int16_t *pcm, int samples);  /* -1: the speaker is not open */
void sound_speech_stop(void);      /* drops the queued speech at once */
/* The speaker's volume, 0..100, on everything played (speech and sounds); kept across restarts. */
int sound_volume(void);
void sound_set_volume(int v);
void sound_say_ap(void);            /* "开始配网", queued as speech (at boot, once the speaker opens) */
void sound_say_ip(const char *ip);  /* "配网成功，请使用浏览器访问" and the address, queued as speech */

extern const struct api_method SOUND_METHODS[];
extern const int NSOUND_METHODS;

#endif
