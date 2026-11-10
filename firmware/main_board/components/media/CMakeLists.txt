# audio.h, camera.h, hand.h: on the Cove-610 one MPP context (v610_mpp.c); none on a PC (DOITDUCK_MEDIA_NONE).
if(DOITDUCK_MEDIA_NONE)
    set(SRCS audio_none.c camera_none.c hand_none.c)
    set(REQUIRES "")
else()
    set(SRCS v610_mpp.c)
    set(REQUIRES osal media_engine mipk_video)
endif()
hifw_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS .
    REQUIRES ${REQUIRES}
    COMPILE_OPTIONS ${DOITDUCK_CFLAGS})
