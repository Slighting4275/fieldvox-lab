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
bool fv_camera_copy_argb(uint32_t out_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT],
                        int display_rotation_deg, int *rotation_deg, bool *front_facing,
                        bool *torch_on);
bool fv_camera_is_active(void);
bool fv_camera_is_healthy(uint64_t max_age_ms);
bool fv_camera_has_frame(uint64_t max_age_ms);
bool fv_camera_set_torch(bool enable);
bool fv_camera_is_torch_on(void);
size_t fv_camera_current_lens(void);
void fv_camera_copy_lens_label(size_t index, char *out, size_t cap);
bool fv_camera_cycle_lens(void);
bool fv_camera_is_front_facing(void);
bool fv_camera_needs_reopen(void);

#endif
