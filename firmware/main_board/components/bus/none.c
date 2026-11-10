/* bus.h with no servo bus, for a PC: nothing is sent and nothing answers. */
#include "bus.h"

#include <stdio.h>

#include "osal.h"

int bus_open(const struct bus_cfg *cfg)
{
    (void)cfg;
    printf("no servo bus\n");
    return 0;
}

void bus_write(const uint8_t *buf, int len)
{
    (void)buf;
    (void)len;
}

int bus_read(uint8_t *buf, int size, double timeout)
{
    (void)buf;
    (void)size;
    os_sleep(timeout);
    return 0;
}

void bus_flush_input(void)
{
}
