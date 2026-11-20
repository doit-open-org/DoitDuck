#ifndef OPUS_CODEC_H
#define OPUS_CODEC_H

/*
 * Opus, 16 kHz mono, for xiaozhi: 60 ms frames up, and down decoded at 16 kHz
 * whatever the server's rate. Implementations: libopus.c, none.c.
 */
#include <stdint.h>

int opus_codec_encode(const int16_t *pcm, int samples, uint8_t *out, int size);  /* bytes, or -1 */
/* samples, or -1; data NULL: a lost packet, samples of it made up from the ones before. */
int opus_codec_decode(const uint8_t *data, int len, int16_t *pcm, int samples);

#endif
