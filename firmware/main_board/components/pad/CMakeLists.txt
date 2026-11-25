# pad.h: a gamepad, from the Linux input devices (BLE through BlueZ).
hifw_component_register(
    SRCS evdev.c
    INCLUDE_DIRS .
    COMPILE_OPTIONS -D_GNU_SOURCE ${DOITDUCK_CFLAGS})
