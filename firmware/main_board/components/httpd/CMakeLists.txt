# httpd.h: the web server. POSIX implementation.
hifw_component_register(
    SRCS posix.c
    INCLUDE_DIRS .
    REQUIRES osal
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
