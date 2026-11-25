# WebAssembly build of the Velvet UI (run via ../build-web.sh with Emscripten active).
cmake_minimum_required(VERSION 3.16)
project(velvet_web C CXX)
set(CMAKE_CXX_STANDARD 17)

set(VELVET_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL_DIR "" CACHE PATH "LVGL 9.5 source tree")

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE USE_DESKTOP=1)
target_compile_options(lvgl PUBLIC -sUSE_FREETYPE=1 -sUSE_SDL=2 -sUSE_LIBPNG=1)

add_executable(velvet-web
    web_main.cpp
    asset_manager_web.cpp
    ${VELVET_ROOT}/src/view/velvet/velvet_ui.cpp
    ${VELVET_ROOT}/src/view/velvet/device.cpp
    ${VELVET_ROOT}/src/view/velvet/term.cpp
    ${VELVET_ROOT}/src/view/velvet/shell.cpp
    ${VELVET_ROOT}/src/view/velvet/sound.cpp
    ${VELVET_ROOT}/src/view/velvet/sensors.cpp
    ${VELVET_ROOT}/src/view/velvet/browser.cpp
    ${VELVET_ROOT}/src/view/velvet/chrome.cpp
    ${VELVET_ROOT}/src/view/velvet/music.cpp
    ${VELVET_ROOT}/src/view/velvet/netlink.cpp
    ${VELVET_ROOT}/src/view/velvet/pwn.cpp
    ${VELVET_ROOT}/src/view/velvet/wordmark.cpp
    ${VELVET_ROOT}/src/view/velvet/nettools.cpp
    ${VELVET_ROOT}/src/view/velvet/fonts/ter_t14.c
    ${VELVET_ROOT}/src/view/velvet/fonts/ter_t12.c
    ${VELVET_ROOT}/src/view/velvet/fonts/ter_b14.c
    ${VELVET_ROOT}/src/view/velvet/fonts/ter_b16.c
    ${VELVET_ROOT}/src/view/velvet/fonts/ter_b18.c)
target_include_directories(velvet-web PRIVATE ${VELVET_ROOT}/src/app ${VELVET_ROOT}/src/view ${VELVET_ROOT}/third_party/miniaudio ${VELVET_ROOT}/third_party/stb)
target_link_libraries(velvet-web PRIVATE lvgl)
target_link_options(velvet-web PRIVATE
    -sUSE_FREETYPE=1 -sUSE_SDL=2 -sUSE_LIBPNG=1
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432
    -sSINGLE_FILE=1 -sENVIRONMENT=web
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_velvet_key,_velvet_key_layer,_velvet_mod,_velvet_motion,_velvet_camera_frame,_velvet_camera_state,_velvet_snap_done,_velvet_photo_add,_velvet_shell_data,_velvet_shell_exit -sEXPORTED_RUNTIME_METHODS=ccall,HEAPU8 -sFETCH=1
    --embed-file ${VELVET_ROOT}/assets/fonts@/fonts
    -O2)
