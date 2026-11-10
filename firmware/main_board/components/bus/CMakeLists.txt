# bus.h: the servo bus. A UART on the board, none on a PC (DOITDUCK_BUS_NONE).
if(DOITDUCK_BUS_NONE)
    set(SRCS none.c)
else()
    set(SRCS linux_uart.c)
endif()
hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES osal
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
