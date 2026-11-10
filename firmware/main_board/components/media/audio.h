#ifndef AUDIO_H
#define AUDIO_H

/*
 * Microphone and speaker, 16-bit mono at 16 kHz. Implementations:
 * media/v610_mpp.c (Cove-610, shared with the camera), none.c.
 * Every function returns -1 when there is no audio (yet).
 */
#include <stdint.h>

struct audio_cfg {
    int mic_channel;         /* 0 = ADCL, 1 = ADCR */
    int mic_gain_db;
    int speaker_gain_db;     /* -121..6 */
};

/* Before the first use; the audio opens on first use. */
void audio_configure(const struct audio_cfg *cfg);
int audio_read(int16_t *pcm, int samples);              /* samples read, 0 on timeout */
/* Starts playing a clip in the background, replacing one still playing. */
int audio_play(const int16_t *pcm, int samples, int rate);
/* Queues speech (16 kHz) behind what is still playing; blocks while the queue is full. */
int audio_stream(const int16_t *pcm, int samples);
/* Drops the queued speech at once (the user talks over the duck). */
void audio_flush(void);
/* Releases the audio, and the camera opened with it, before exit; left open, the next run cannot open the camera. */
void audio_close(void);

#endif
