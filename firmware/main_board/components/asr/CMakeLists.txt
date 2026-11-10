# asr.h: speech recognition, picked by Kconfig (DOITDUCK_ASR_*).
if(DOITDUCK_ASR_MIPK_WAKE)
    set(SRCS mipk_wake.c)
    set(REQUIRES mipk_runtime)
else()
    set(SRCS none.c)
    set(REQUIRES "")
endif()
hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES ${REQUIRES}
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
