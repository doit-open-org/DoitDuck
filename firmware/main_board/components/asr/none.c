/* asr.h with no recognition engine; the web page can still simulate words. */
#include "asr.h"

int asr_open(const struct asr_cfg *cfg)
{
    (void)cfg;
    return -1;
}

int asr_caps(void)
{
    return 0;
}

int asr_feed(const int16_t *pcm, int samples, char *text, size_t size, float *score)
{
    (void)pcm;
    (void)samples;
    (void)text;
    (void)size;
    (void)score;
    return ASR_NONE;
}

void asr_touch(void)
{
}
