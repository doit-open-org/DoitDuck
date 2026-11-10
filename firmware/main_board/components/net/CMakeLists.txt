# net.h: HTTPS and a WebSocket for xiaozhi, picked by Kconfig (DOITDUCK_NET_*).
if(DOITDUCK_NET_MBEDTLS)
    set(SRCS mbedtls.c)
    set(REQUIRES osal mbedtls)
else()
    set(SRCS none.c)
    set(REQUIRES "")
endif()
hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES ${REQUIRES}
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
