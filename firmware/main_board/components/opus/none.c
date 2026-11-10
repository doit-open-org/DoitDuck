/* opus_codec.h with no codec. */
#include "opus_codec.h"

int opus_codec_encode(const int16_t *pcm, int samples, uint8_t *out, int size)
{
    (void)pcm;
    (void)samples;
    (void)out;
    (void)size;
    return -1;
}

int opus_codec_decode(const uint8_t *data, int len, int16_t *pcm, int samples)
{
    (void)data;
    (void)len;
    (void)pcm;
    (void)samples;
    return -1;
}
