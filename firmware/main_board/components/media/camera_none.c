/* camera.h with no camera. */
#include "camera.h"

int camera_h264_read(uint8_t *buf, int size, int *key_frame)
{
    (void)buf;
    (void)size;
    (void)key_frame;
    return -1;
}

void camera_key_frame(void)
{
}

int camera_jpeg(uint8_t *buf, int size)
{
    (void)buf;
    (void)size;
    return -1;
}

void camera_lines(const float (*seg)[4], int n, uint32_t rgb)
{
    (void)seg;
    (void)n;
    (void)rgb;
}
