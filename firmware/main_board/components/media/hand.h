#ifndef HAND_H
#define HAND_H

/*
 * Hands in the camera picture (MediaPipe hand model). Implementations:
 * media/v610_mpp.c (models/mediapipe_hand.mipk), none.c.
 */
#define HAND_POINTS 21

/* 21 landmarks, x and y as 0..1 of the picture. */
struct hand {
    float x[HAND_POINTS], y[HAND_POINTS];
};

/* Loads the model; before the audio opens, which opens the camera with a stream sized for it. 0 on success. */
int hand_open(void);
/* Waits for the next recognition frame (about 10 a second); hands found, up to max, or -1 with no camera or model. */
int hand_read(struct hand *hands, int max);

#endif
