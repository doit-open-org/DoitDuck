#ifndef PAD_H
#define PAD_H

/*
 * A gamepad. Implementations: none.c (Bluetooth through BlueZ is reserved).
 */
#include <stdint.h>

enum {
    PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_X = 1 << 2, PAD_Y = 1 << 3,
    PAD_LB = 1 << 4, PAD_RB = 1 << 5, PAD_LT = 1 << 6, PAD_RT = 1 << 7,
    PAD_START = 1 << 8, PAD_SELECT = 1 << 9,
    PAD_UP = 1 << 10, PAD_DOWN = 1 << 11, PAD_LEFT = 1 << 12, PAD_RIGHT = 1 << 13,
};

struct pad_state {
    float lx, ly, rx, ry;   /* -1..1, right and up positive */
    uint32_t buttons;       /* PAD_* */
};

/* Waits up to 20 ms for the pad; 1 with a pad connected (s filled), 0 with none, -1 with no gamepad support. */
int pad_read(struct pad_state *s);

#endif
