/* audio.h with no microphone and no speaker. */
#include "audio.h"

void audio_configure(const struct audio_cfg *cfg)
{
    (void)cfg;
}

int audio_read(int16_t *pcm, int samples)
{
    (void)pcm;
    (void)samples;
    return -1;
}

int audio_play(const int16_t *pcm, int samples, int rate)
{
    (void)pcm;
    (void)samples;
    (void)rate;
    return -1;
}

int audio_stream(const int16_t *pcm, int samples)
{
    (void)pcm;
    (void)samples;
    return -1;
}

void audio_flush(void)
{
}

void audio_close(void)
{
}
