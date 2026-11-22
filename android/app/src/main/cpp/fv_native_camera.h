/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FIELDVOX_NATIVE_CAMERA_H
#define FIELDVOX_NATIVE_CAMERA_H

#include <android/native_activity.h>
#include <stdbool.h>
#include <stdint.h>
#include "fv_camera_effects.h"

#define FV_CAMERA_WIDTH 640
#define FV_CAMERA_HEIGHT 360

void fv_android_request_runtime_permissions(ANativeActivity *activity);
void fv_android_request_permissions(ANativeActivity *activity, const char * const *permissions, int count, int request_code);
void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code);
bool fv_android_has_permission(ANativeActivity *activity, const char *permission);
bool fv_camera_start(void);
void fv_camera_stop(void);
bool fv_camera_copy_argb(uint32_t out_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]);
bool fv_camera_is_active(void);
bool fv_camera_is_healthy(uint64_t max_age_ms);
int fv_camera_get_sensor_orientation(void);
int fv_android_get_display_rotation_degrees(ANativeActivity *activity);
int fv_camera_get_display_compensation_degrees(ANativeActivity *activity);
bool fv_camera_set_torch(bool enable);
bool fv_camera_is_torch_on(void);
void fv_android_vibrate(ANativeActivity *activity, int ms);
void fv_android_vibrate_double(ANativeActivity *activity);
void fv_android_open_file(ANativeActivity *activity, const char *path, const char *mime_type);

#endif
