#ifndef BUS_H
#define BUS_H

/*
 * The servo bus: 1 Mbps 8N1 half duplex, also carrying the IMU board.
 * Implementations: linux_uart.c (a UART, with an optional GPIO for the bus
 * buffer's send direction), none.c (no bus, for a PC).
 */
#include <stdint.h>

struct bus_cfg {
    char device[64];         /* empty: no servo bus, for running on a PC */
    int npinmux;
    uint32_t pinmux[8][2];   /* register address, value: routes the board's pins (the bus UART, the ToF's I2C) */
    uint32_t tx_enable_base; /* PL061 GPIO controller driving the bus buffer's TX enable (low = send) */
    int tx_enable_pin;       /* its bit, or -1: a buffer that switches by itself */
};

int bus_open(const struct bus_cfg *cfg);            /* 0 on success */
void bus_write(const uint8_t *buf, int len);
int bus_read(uint8_t *buf, int size, double timeout); /* bytes read, 0 on timeout */
void bus_flush_input(void);

#endif
