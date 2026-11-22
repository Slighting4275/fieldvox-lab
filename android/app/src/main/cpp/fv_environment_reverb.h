/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_ENVIRONMENT_REVERB_H
#define FV_ENVIRONMENT_REVERB_H
#include <stddef.h>
typedef struct FvEnvironmentReverb FvEnvironmentReverb;
FvEnvironmentReverb *fv_environment_reverb_create(void);
void fv_environment_reverb_destroy(FvEnvironmentReverb *effect);
void fv_environment_reverb_reset(FvEnvironmentReverb *effect);
void fv_environment_reverb_set_mix(FvEnvironmentReverb *effect, float mix);
void fv_environment_reverb_process(FvEnvironmentReverb *effect, const float *send,
                                    float *wet, size_t frames);
#endif
