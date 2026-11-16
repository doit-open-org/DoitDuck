# The program: core/ (bus, arbitration, motion, policy), app/ (start-up, the
# command interface, configuration, policy library, prompts) and the features
# Kconfig turns on.
set(SRCS main.c core/arbiter.c core/bus.c core/motion.c core/policy.c
         app/api.c app/app.c app/calib.c app/config.c app/policies.c app/sound.c)
if(DOITDUCK_WEB)
    list(APPEND SRCS features/web/web.c)
endif()
if(DOITDUCK_GAMEPAD)
    list(APPEND SRCS features/gamepad/gamepad.c)
endif()
if(DOITDUCK_VOICE)
    list(APPEND SRCS features/voice/voice.c)
endif()
if(DOITDUCK_XIAOZHI)
    list(APPEND SRCS features/xiaozhi/xiaozhi.c features/xiaozhi/mcp.c)
endif()
if(DOITDUCK_GESTURE)
    list(APPEND SRCS features/gesture/gesture.c)
endif()
set(REQUIRES osal bus storage media asr net opus httpd pad cjson)
if(DOITDUCK_WIFI)
    list(APPEND SRCS features/wifi/wifi.c)
endif()
if(DOITDUCK_BLUFI)
    list(APPEND SRCS features/blufi/blufi.c)
    list(APPEND REQUIRES blufi)
endif()
if(DOITDUCK_TOF)
    list(APPEND SRCS features/tof/tof.c)
    list(APPEND REQUIRES vl53l5cx)
endif()

hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS core app
    REQUIRES ${REQUIRES}
    LINK_LIBRARIES m
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS} ${DOITDUCK_CONFIG_FLAGS})
