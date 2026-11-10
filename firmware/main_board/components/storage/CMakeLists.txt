# storage.h: the program's files and the user's (data/). POSIX implementation.
hifw_component_register(
    SRCS posix.c
    INCLUDE_DIRS .
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
