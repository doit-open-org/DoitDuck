#ifndef PAD_H
#define PAD_H

/*
 * A BLE gamepad, paired through BlueZ (the image's ble-service starts
 * bluetoothd) and read from the Linux input devices (evdev.c).
 */
#include <stddef.h>
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

/* Waits up to 20 ms for the pad; 1 with a pad connected (s filled), 0 with none. */
int pad_read(struct pad_state *s);

/*
 * Pairs a BLE gamepad in pairing mode, in place of the one paired before; takes up to a
 * minute. 0 with one paired (its name in name), -1 with none found.
 */
int pad_pair(char *name, size_t size);

/* The paired gamepad: 0 with one paired (its address and name in name, "03:2E:10:15:57:F3 (Q34B)"), -1 with none. */
int pad_paired(char *name, size_t size);

/* Forgets the paired gamepads; 0 when done. */
int pad_unpair(void);

/*
 * What the image lacks for a BLE gamepad: NULL with nothing, else "no_ble" (no BlueZ),
 * "no_uhid" (no uhid module) or "no_hog" (bluetoothd without its HoG plugin).
 */
const char *pad_missing(void);

#endif
