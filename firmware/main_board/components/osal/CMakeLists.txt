# osal.h: threads, locks, time, device id. Linux implementation.
hifw_component_register(
    SRCS linux/osal.c
    INCLUDE_DIRS . linux
    LINK_LIBRARIES pthread
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
