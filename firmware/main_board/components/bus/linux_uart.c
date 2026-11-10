/*
 * bus.h on Linux: the servo bus on a UART (pins routed through /dev/mem).
 * With "tx_enable" the bus buffer sends only while that GPIO is low and always
 * hears the bus: each write drives the pin low, raises it as soon as the last
 * stop bit is out (before the servos answer) and reads back its own echo.
 */
#define _GNU_SOURCE
#include "bus.h"

#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "osal.h"

static const struct bus_cfg *B;
static int uart_fd = -1;
static volatile uint32_t *tx_enable; /* the PL061 data register masked to the pin: 0 sends, ~0 releases */

/* The GPIO as an output, high (not sending), mapped for the life of the program. */
static int open_tx_enable(void)
{
    const int mem = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint8_t *gpio = mem < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, B->tx_enable_base);
    if (mem >= 0) close(mem);
    if (gpio == MAP_FAILED) {
        perror("bus tx enable gpio");
        return -1;
    }
    const uint32_t bit = 1u << B->tx_enable_pin;
    tx_enable = (volatile uint32_t *)(gpio + (bit << 2)); /* PL061: address bits 9..2 select the data bits written */
    *tx_enable = 0xFF;
    *(volatile uint32_t *)(gpio + 0x400) |= bit; /* GPIODIR */
    return 0;
}

static int set_pinmux(void)
{
    if (B->npinmux == 0) return 0;
    const int mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem < 0) {
        perror("/dev/mem");
        return -1;
    }
    for (int i = 0; i < B->npinmux; i++) {
        volatile uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem, B->pinmux[i][0] & ~0xFFFu);
        if (page == MAP_FAILED) {
            perror("mmap pinmux");
            close(mem);
            return -1;
        }
        *(volatile uint32_t *)(page + (B->pinmux[i][0] & 0xFFF)) = B->pinmux[i][1];
        munmap((void *)page, 4096);
    }
    close(mem);
    return 0;
}

int bus_open(const struct bus_cfg *cfg)
{
    B = cfg;
    if (!B->device[0]) {
        printf("no servo bus\n");
        return 0;
    }
    if (set_pinmux() != 0) return -1;
    uart_fd = open(B->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart_fd < 0) {
        fprintf(stderr, "cannot open %s: %m (on the Cove-610 UART1 exists only after 'sdio1-pins sd' and a reboot)\n",
                B->device);
        return -1;
    }
    struct termios t;
    tcgetattr(uart_fd, &t);
    cfmakeraw(&t);
    cfsetispeed(&t, B1000000);
    cfsetospeed(&t, B1000000);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CRTSCTS;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(uart_fd, TCSANOW, &t) != 0) {
        perror("tcsetattr 1 Mbps");
        return -1;
    }
    if (B->tx_enable_pin >= 0 && open_tx_enable() != 0) return -1;
    return 0;
}

void bus_write(const uint8_t *buf, int len)
{
    if (uart_fd < 0) return;
    if (tx_enable) *tx_enable = 0;
    if (write(uart_fd, buf, (size_t)len) != len) perror("servo uart write");
    if (!tx_enable) return;
    /* tcdrain sleeps a whole tick (10 ms); the servos answer within tens of microseconds. Poll the shifter instead. */
    const double give_up = os_now() + len * 10e-6 + 0.002;
    unsigned lsr = 0;
    while (!(lsr & TIOCSER_TEMT) && os_now() < give_up) ioctl(uart_fd, TIOCSERGETLSR, &lsr);
    *tx_enable = 0xFF;
    /* Our own bytes come back first; read exactly those, so the reply behind them stays queued. */
    uint8_t echo[256];
    for (int got = 0; got < len;) {
        const int n = bus_read(echo, len - got < (int)sizeof echo ? len - got : (int)sizeof echo, 0.002);
        if (n <= 0) break;
        got += n;
    }
}

int bus_read(uint8_t *buf, int size, double timeout)
{
    if (uart_fd < 0) {
        os_sleep(timeout);
        return 0;
    }
    struct pollfd p = {uart_fd, POLLIN, 0};
    const struct timespec ts = {(time_t)timeout, (long)((timeout - (double)(time_t)timeout) * 1e9)};
    if (ppoll(&p, 1, &ts, NULL) <= 0) return 0;
    const ssize_t n = read(uart_fd, buf, (size_t)size);
    return n > 0 ? (int)n : 0;
}

void bus_flush_input(void)
{
    if (uart_fd >= 0) tcflush(uart_fd, TCIFLUSH);
}
