cmake_minimum_required(VERSION 3.22.1)

project(fieldvox LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(
    fieldvox_native
    SHARED
    fieldvox_native_activity.c
    fv_native_camera.c
    fv_native_video_recorder.c
    fv_gfx.c
    fv_grain_gate.c
    fv_hit_capture.c
    fv_vox_controls.c
    fv_recording_data.c
    fv_local_player.c
    fv_native_radio.c
    fv_core.c
    fv_source_audit.c
    fv_stage1r_native_audio.c
    fv_stage1u_native_blob_decoder.c
    fv_sensor_dsp.c
    fv_local_bank.c
    fv_fragment_engine.c
    fv_fragment_store.c
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
