cmake_minimum_required(VERSION 3.22.1)

project(fieldvox LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

file(GLOB FV_LVM_SOURCES CONFIGURE_DEPENDS
    third_party/lvm/Reverb/src/*.c third_party/lvm/Common/src/*.c)
add_library(fieldvox_audio_algorithms STATIC ${FV_LVM_SOURCES} third_party/sonic/sonic.c)
set_target_properties(fieldvox_audio_algorithms PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_definitions(fieldvox_audio_algorithms PUBLIC BUILD_FLOAT HIGHER_FS)
target_include_directories(fieldvox_audio_algorithms PUBLIC
    third_party/lvm/Reverb/lib third_party/lvm/Reverb/src
    third_party/lvm/Common/lib third_party/lvm/Common/src)

add_library(
    fieldvox_native
    SHARED
    fieldvox_native_activity.c
    fv_android_platform.c
    fv_media_decode.c
    fv_native_camera.c
    fv_camera_effects.c
    fv_native_video_recorder.c
    fv_gfx.c
    fv_ui_layout.c
    fv_android_text.c
    fv_hit_capture.c
    fv_vox_controls.c
    fv_recording_data.c
    fv_local_player.c
    fv_native_radio.c
    fv_platform_voices.c
    fv_stage1r_native_audio.c
    fv_sensor_dsp.c
    fv_local_bank.c
    fv_fragment_engine.c
    fv_fragment_store.c
    fv_station_policy.c
    fv_radio_playlist.c
    fv_environment_reverb.c
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
    jnigraphics
    aaudio
    camera2ndk
    mediandk
    m
    fieldvox_audio_algorithms
)
