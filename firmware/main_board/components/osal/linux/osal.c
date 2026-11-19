/* osal.h on Linux: CLOCK_MONOTONIC, pthreads, SCHED_FIFO for real-time threads, the MAC as device id. */
#define _GNU_SOURCE
#include "osal.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double os_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

void os_sleep(double seconds)
{
    os_sleep_until(os_now() + seconds);
}

void os_sleep_until(double t)
{
    struct timespec ts = {(time_t)t, (long)((t - (double)(time_t)t) * 1e9)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {}
}

struct start {
    void (*fn)(void *);
    void *arg;
};

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int os_thread(void (*fn)(void *), void *arg, int prio, int core)
{
    pthread_t t;
    pthread_attr_t attr;
    struct start *s = malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_init(&attr);
    if (prio > 0) {
        struct sched_param param = {.sched_priority = prio};
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &param);
    }
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        pthread_attr_setaffinity_np(&attr, sizeof set, &set);
    }
    int rc = pthread_create(&t, &attr, trampoline, s);
    if (rc != 0 && prio > 0) {
        printf("SCHED_FIFO refused, running at normal priority\n");
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(&t, &attr, trampoline, s);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) free(s);
    return rc == 0 ? 0 : -1;
}

void os_lock(os_mutex_t *m)
{
    pthread_mutex_lock(m);
}

void os_unlock(os_mutex_t *m)
{
    pthread_mutex_unlock(m);
}

int os_device_id(char id[18])
{
    FILE *f = fopen("/sys/class/net/wlan0/address", "r");
    const int ok = f && fscanf(f, "%17s", id) == 1 && strlen(id) == 17;
    if (f) fclose(f);
    if (!ok) id[0] = 0;
    return ok ? 0 : -1;
}
