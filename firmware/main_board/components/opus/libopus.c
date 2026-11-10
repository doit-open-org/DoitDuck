/*
 * opus_codec.h with the SDK's libopus (linked through media_engine). The
 * SDK ships it without its header, so the few functions used are declared
 * here. Up: 16 kHz mono 60 ms frames. Down: decoded at 16 kHz whatever rate
 * the server sends, so it plays without resampling.
 */
#include <stdio.h>

#include "opus_codec.h"

typedef struct OpusEncoder OpusEncoder;
typedef struct OpusDecoder OpusDecoder;
OpusEncoder *opus_encoder_create(int32_t rate, int channels, int application, int *error);
int32_t opus_encode(OpusEncoder *st, const int16_t *pcm, int frame_size, unsigned char *data, int32_t max_bytes);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
OpusDecoder *opus_decoder_create(int32_t rate, int channels, int *error);
int opus_decode(OpusDecoder *st, const unsigned char *data, int32_t len, int16_t *pcm, int frame_size, int fec);

#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SET_COMPLEXITY_REQUEST 4010

static OpusEncoder *enc;
static OpusDecoder *dec;

int opus_codec_encode(const int16_t *pcm, int samples, uint8_t *out, int size)
{
    int err = 0;
    if (!enc) {
        enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
        if (!enc) {
            printf("xiaozhi: opus encoder failed: %d\n", err);
            return -1;
        }
        /* Leaves CPU for the policy loop; speech recognition does not need more. */
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY_REQUEST, 3);
    }
    return opus_encode(enc, pcm, samples, out, size);
}

int opus_codec_decode(const uint8_t *data, int len, int16_t *pcm, int samples)
{
    int err = 0;
    if (!dec) {
        dec = opus_decoder_create(16000, 1, &err);
        if (!dec) {
            printf("xiaozhi: opus decoder failed: %d\n", err);
            return -1;
        }
    }
    return opus_decode(dec, data, len, pcm, samples, 0);
}
