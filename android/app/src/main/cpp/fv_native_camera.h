/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FIELDVOX_NATIVE_CAMERA_H
#define FIELDVOX_NATIVE_CAMERA_H

#include <stdbool.h>
#include <stdint.h>
#include "fv_camera_effects.h"

#define FV_CAMERA_WIDTH 640
#define FV_CAMERA_HEIGHT 360

bool fv_camera_start(void);
void fv_camera_stop(void);
bool fv_camera_copy_argb(uint32_t out_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT]);
bool fv_camera_is_active(void);
bool fv_camera_is_healthy(uint64_t max_age_ms);
int fv_camera_get_sensor_orientation(void);
int fv_camera_get_compensation_for_display(int display_rotation_deg);
bool fv_camera_set_torch(bool enable);
bool fv_camera_is_torch_on(void);

#endif
