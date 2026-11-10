#ifndef CAMERA_H
#define CAMERA_H

/*
 * The camera. Implementations: media/v610_mpp.c (opened with the audio), none.c.
 */
#include <stdint.h>

/* H.264 (Annex B, 720p 15 fps): one frame into buf; its length, 0 on timeout, -1 with no camera. */
int camera_h264_read(uint8_t *buf, int size, int *key_frame);
/* Asks for a key frame next, for a viewer that has just joined. */
void camera_key_frame(void);
/* One picture as JPEG (720p) into buf; its length, or -1 with no camera. */
int camera_jpeg(uint8_t *buf, int size);
/* Draws segments {x1, y1, x2, y2} (0..1) over the H.264 stream in color 0xRRGGBB, until the next call. */
void camera_lines(const float (*seg)[4], int n, uint32_t rgb);

#endif
