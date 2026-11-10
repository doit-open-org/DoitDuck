# Mbed TLS 3.6.4 (Apache-2.0), library/ and include/ from the release tarball,
# default configuration. For the xiaozhi transport: https and wss.
file(GLOB MBEDTLS_SRCS "${CMAKE_CURRENT_LIST_DIR}/library/*.c")
hifw_component_register(
    SRCS ${MBEDTLS_SRCS}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS library
    COMPILE_OPTIONS -O2)
