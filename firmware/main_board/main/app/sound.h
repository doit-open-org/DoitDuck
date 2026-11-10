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
void sound_speech(const int16_t *pcm, int samples);
void sound_speech_stop(void);      /* drops the queued speech at once */

extern const struct api_method SOUND_METHODS[];
extern const int NSOUND_METHODS;

#endif
