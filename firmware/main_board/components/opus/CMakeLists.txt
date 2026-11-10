# opus_codec.h, picked by Kconfig (DOITDUCK_OPUS_*): the SDK's libopus comes with media_engine.
if(DOITDUCK_OPUS_LIBOPUS)
    set(SRCS libopus.c)
    set(REQUIRES media_engine)
else()
    set(SRCS none.c)
    set(REQUIRES "")
endif()
hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES ${REQUIRES}
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
