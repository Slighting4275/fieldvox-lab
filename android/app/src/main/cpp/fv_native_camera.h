#ifndef FIELDVOX_NATIVE_CAMERA_H
#define FIELDVOX_NATIVE_CAMERA_H

#include <android/native_activity.h>
#include <stdbool.h>
#include <stdint.h>

#define FV_CAMERA_WIDTH 320
#define FV_CAMERA_HEIGHT 180

void fv_android_request_runtime_permissions(ANativeActivity *activity);
void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code);
bool fv_android_has_permission(ANativeActivity *activity, const char *permission);
bool fv_camera_start(void);
void fv_camera_stop(void);
bool fv_camera_copy_luma(uint8_t out_luma[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]);
bool fv_camera_is_active(void);

#endif
