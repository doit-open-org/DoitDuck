#ifndef OSAL_PORT_H
#define OSAL_PORT_H

#include <pthread.h>

typedef pthread_mutex_t os_mutex_t;
#define OS_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER

#endif
