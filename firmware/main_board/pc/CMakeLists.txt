# DoitDuck on a PC, with no SDK and no servo bus: the web page,
# the command interface, arbitration, the MCP tools and policy import can be
# tried without a board. The same main/ and components/ as the Cove-610 build,
# with no audio, camera or recognition engine; the web page simulates words.
#
#   cmake -S firmware/main_board/pc -B build/pc && cmake --build build/pc
#   cd build/pc && ./doitduck        then open http://localhost:9527
cmake_minimum_required(VERSION 3.16)
project(doitduck C)

set(BOARD ${CMAKE_CURRENT_LIST_DIR}/..)
set(ROOT ${BOARD}/../..)

# What Kconfig would give: every feature but gestures, no bus, audio, camera or recognition engine.
set(DOITDUCK_BUS_NONE ON)
set(DOITDUCK_MEDIA_NONE ON)
set(DOITDUCK_NET_MBEDTLS ON)
foreach(f WEB GAMEPAD VOICE XIAOZHI)
    set(DOITDUCK_${f} ON)
    list(APPEND DOITDUCK_CONFIG_FLAGS -DCONFIG_DOITDUCK_${f}=1)
endforeach()
set(DOITDUCK_CFLAGS -O2 -Wall -Wextra -Werror)

# hifw's component registration, for plain CMake: a library per component, main as objects.
function(hifw_component_register)
    cmake_parse_arguments(C "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;LINK_LIBRARIES;COMPILE_OPTIONS" ${ARGN})
    get_filename_component(name ${CMAKE_CURRENT_SOURCE_DIR} NAME)
    if(name STREQUAL "main")
        add_library(${name} OBJECT ${C_SRCS})
    else()
        add_library(${name} STATIC ${C_SRCS})
    endif()
    target_include_directories(${name} PUBLIC ${C_INCLUDE_DIRS} PRIVATE ${C_PRIV_INCLUDE_DIRS})
    target_link_libraries(${name} PUBLIC ${C_REQUIRES} ${C_LINK_LIBRARIES})
    target_compile_options(${name} PRIVATE ${C_COMPILE_OPTIONS})
endfunction()

foreach(c osal bus storage media asr net opus httpd pad cjson mbedtls)
    add_subdirectory(${BOARD}/components/${c} components/${c})
endforeach()
add_subdirectory(${BOARD}/main main)
add_executable(doitduck)
target_link_libraries(doitduck PRIVATE main)

# The program runs from a directory holding robots/, web/, sounds/ and models/.
foreach(dir robots web sounds models)
    add_custom_command(TARGET doitduck POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink ${ROOT}/${dir} $<TARGET_FILE_DIR:doitduck>/${dir})
endforeach()
