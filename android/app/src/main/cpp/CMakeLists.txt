cmake_minimum_required(VERSION 3.22.1)

project(fieldvox LANGUAGES C CXX)

add_library(
    fieldvox
    SHARED
    fieldvox_jni.cpp
    fv_core.c
    fv_source_audit.c
)

target_compile_features(fieldvox PRIVATE cxx_std_20)

target_compile_options(fieldvox PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

target_link_libraries(
    fieldvox
    android
    log
    aaudio
    m
)

# FIELDVOX_STAGE1R_NATIVE_AUDIO_WINDOW
add_library(fieldvox_stage1r_native SHARED
    fv_stage1r_native_audio.c
)

target_link_libraries(fieldvox_stage1r_native
    android
    log
    aaudio
)
