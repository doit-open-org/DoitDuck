/*
 * The VL53L5CX ULD's platform for Linux i2c-dev: 16-bit register addresses,
 * transfers split into CHUNK bytes (the register address goes up with the
 * data, so a split one simply carries on). Small, because the Cove-610's I2C1
 * driver polls with interrupts off for a whole transfer: 32 bytes hold its
 * CPU some 1 ms.
 */
#define _POSIX_C_SOURCE 200809L /* nanosleep */
#include <errno.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <time.h>

#include "platform.h"

#define CHUNK 32

static int transfer(VL53L5CX_Platform *p, uint16_t reg, uint8_t *data, uint32_t size, int read)
{
    uint8_t buf[2 + CHUNK];
    while (size > 0) {
        const uint16_t n = size > CHUNK ? CHUNK : (uint16_t)size;
        buf[0] = (uint8_t)(reg >> 8);
        buf[1] = (uint8_t)reg;
        struct i2c_msg msgs[2] = {{.addr = p->address >> 1, .flags = 0, .len = 2, .buf = buf}};
        if (read) {
            msgs[1] = (struct i2c_msg){.addr = p->address >> 1, .flags = I2C_M_RD, .len = n, .buf = data};
        } else {
            memcpy(buf + 2, data, n);
            msgs[0].len = (uint16_t)(2 + n);
        }
        struct i2c_rdwr_ioctl_data xfer = {.msgs = msgs, .nmsgs = read ? 2 : 1};
        if (ioctl(p->fd, I2C_RDWR, &xfer) < 0) return 255;
        reg = (uint16_t)(reg + n);
        data += n;
        size -= n;
    }
    return 0;
}

uint8_t RdByte(VL53L5CX_Platform *p, uint16_t reg, uint8_t *value)
{
    return (uint8_t)transfer(p, reg, value, 1, 1);
}

uint8_t WrByte(VL53L5CX_Platform *p, uint16_t reg, uint8_t value)
{
    return (uint8_t)transfer(p, reg, &value, 1, 0);
}

uint8_t RdMulti(VL53L5CX_Platform *p, uint16_t reg, uint8_t *values, uint32_t size)
{
    return (uint8_t)transfer(p, reg, values, size, 1);
}

uint8_t WrMulti(VL53L5CX_Platform *p, uint16_t reg, uint8_t *values, uint32_t size)
{
    return (uint8_t)transfer(p, reg, values, size, 0);
}

/* The sensor's words are big-endian; the host's are little. */
void SwapBuffer(uint8_t *buffer, uint16_t size)
{
    for (uint32_t i = 0; i + 3 < size; i += 4) {
        const uint8_t a = buffer[i], b = buffer[i + 1];
        buffer[i] = buffer[i + 3];
        buffer[i + 1] = buffer[i + 2];
        buffer[i + 2] = b;
        buffer[i + 3] = a;
    }
}

uint8_t WaitMs(VL53L5CX_Platform *p, uint32_t ms)
{
    (void)p;
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
    return 0;
}
