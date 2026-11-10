cmake_minimum_required(VERSION 3.16)

project(doitduck LANGUAGES C CXX ASM)

# APP_SDK_PATH is exported by "source <V610-App-SDK>/export.sh".
if(NOT DEFINED ENV{APP_SDK_PATH} OR "$ENV{APP_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "APP_SDK_PATH is not set; run 'source <V610-App-SDK>/export.sh' first")
endif()
get_filename_component(APP_SDK_PATH "$ENV{APP_SDK_PATH}" ABSOLUTE)
set(CMAKE_SKIP_RPATH TRUE)

# The Cove-610 build of DoitDuck: main/ holds the program (core, app,
# features), components/ the adapters and libraries it is built with.
# Features and adapters come from Kconfig (sdkconfig.defaults, Kconfig): each
# CONFIG_DOITDUCK_X=y becomes the CMake variable DOITDUCK_X for main/ and components/.
set(DOITDUCK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
file(STRINGS "${HIFW_CONFIG_HEADER}" _options REGEX "^#define CONFIG_DOITDUCK_[A-Z0-9_]+ 1$")
foreach(_line IN LISTS _options)
    string(REGEX REPLACE "^#define CONFIG_(DOITDUCK_[A-Z0-9_]+) 1$" "\\1" _name "${_line}")
    set(${_name} ON)
endforeach()
set(DOITDUCK_CFLAGS -O2 -Wall -Wextra -Werror)
set(DOITDUCK_CONFIG_FLAGS -include hifw_config.h)

include("${APP_SDK_PATH}/cmake/project.cmake")
hifw_project(doitduck)

# The program runs from a directory holding robots/, web/, sounds/ and the
# user's models/. Copied on every build, so a change to the web page alone also
# reaches out/.
add_custom_target(doitduck_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${HIFW_OUTPUT_DIR}/models"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DOITDUCK_ROOT}/robots" "${HIFW_OUTPUT_DIR}/robots"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DOITDUCK_ROOT}/web" "${HIFW_OUTPUT_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DOITDUCK_ROOT}/sounds" "${HIFW_OUTPUT_DIR}/sounds"
    VERBATIM)
