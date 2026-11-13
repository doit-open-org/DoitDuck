#ifndef OSAL_H
#define OSAL_H

/*
 * Threads, locks and time: everything core/, app/ and features/ need from the
 * OS. Each implementation supplies osal_port.h (the mutex type).
 */
#include <stddef.h>

#include "osal_port.h"

double os_now(void);                 /* monotonic time, seconds */
void os_sleep(double seconds);
void os_sleep_until(double t);       /* until os_now() >= t */
/* prio 0 is ordinary; higher numbers are real-time, up to 99. core is a CPU hint, -1 for any. */
int os_thread(void (*fn)(void *), void *arg, int prio, int core);
void os_lock(os_mutex_t *m);
void os_unlock(os_mutex_t *m);

#endif
