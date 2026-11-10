# ST's VL53L5CX Ultra Lite Driver 1.3.9 (BSD-3-Clause, see LICENSE.md):
# vl53l5cx_api.c/.h and vl53l5cx_buffers.h unchanged from
# github.com/STMicroelectronics/stm32-vl53l5cx (modules/), with our platform.c/.h
# for Linux i2c-dev. Only built for the ToF feature (DOITDUCK_TOF).
if(NOT DOITDUCK_TOF)
    return()
endif()
hifw_component_register(
    SRCS vl53l5cx_api.c platform.c
    INCLUDE_DIRS .
    COMPILE_OPTIONS -O2)
