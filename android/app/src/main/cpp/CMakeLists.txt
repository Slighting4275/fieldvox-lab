cmake_minimum_required(VERSION 3.22.1)

project(fieldvox LANGUAGES C)

set(FIELDVOX_ZIG_EXECUTABLE "" CACHE FILEPATH "Path to a Zig 0.16 compiler")
if(FIELDVOX_ZIG_EXECUTABLE)
    set(ZIG_EXECUTABLE "${FIELDVOX_ZIG_EXECUTABLE}")
elseif(EXISTS "/usr/bin/zig")
    set(ZIG_EXECUTABLE "/usr/bin/zig")
else()
    find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)
endif()

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(ZIG_TARGET "aarch64-linux-android")
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(ZIG_TARGET "x86_64-linux-android")
else()
    set(ZIG_TARGET "aarch64-linux-android")
endif()

set(ZIG_DSP_OBJ "${CMAKE_CURRENT_BINARY_DIR}/fieldvox_zig_dsp.o")

add_custom_command(
    OUTPUT ${ZIG_DSP_OBJ}
    COMMAND ${ZIG_EXECUTABLE} build-obj
            -target ${ZIG_TARGET}
            -fPIC
            -OReleaseFast
            ${CMAKE_CURRENT_SOURCE_DIR}/fieldvox_zig_dsp.zig
            -femit-bin=${ZIG_DSP_OBJ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fieldvox_zig_dsp.zig
    COMMENT "Compiling FieldVox Zig DSP module for ${ZIG_TARGET}"
)

add_library(
    fieldvox_native
    SHARED
    fieldvox_native_activity.c
    fv_native_camera.c
    fv_native_video_recorder.c
    fv_gfx.c
    fv_grain_gate.c
    fv_native_radio.c
    fv_core.c
    fv_source_audit.c
    fv_stage1r_native_audio.c
    fv_stage1u_native_blob_decoder.c
    ${ZIG_DSP_OBJ}
)

target_compile_options(fieldvox_native PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wformat=2
    -Wno-unused-parameter
)

target_link_libraries(
    fieldvox_native
    android
    log
    aaudio
    camera2ndk
    mediandk
    m
)
