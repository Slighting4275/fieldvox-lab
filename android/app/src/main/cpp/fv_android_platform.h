/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_ANDROID_PLATFORM_H
#define FV_ANDROID_PLATFORM_H

#include <android/native_activity.h>
#include <stdbool.h>

void fv_android_request_runtime_permissions(ANativeActivity *activity);
void fv_android_request_permissions(ANativeActivity *activity, const char * const *permissions,
                                    int count, int request_code);
void fv_android_request_permission(ANativeActivity *activity, const char *permission, int request_code);
bool fv_android_has_permission(ANativeActivity *activity, const char *permission);
void fv_android_vibrate(ANativeActivity *activity, int ms);
void fv_android_vibrate_double(ANativeActivity *activity);
int fv_android_get_display_rotation_degrees(ANativeActivity *activity);

#endif
