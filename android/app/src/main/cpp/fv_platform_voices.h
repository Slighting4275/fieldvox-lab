/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_PLATFORM_VOICES_H
#define FV_PLATFORM_VOICES_H
#include <jni.h>
#include "fv_fragment_engine.h"
typedef struct FvPlatformVoices FvPlatformVoices;
/* C23 owns all scheduling, descriptors and JNI references. */
FvPlatformVoices *fv_platform_voices_create(JavaVM *vm);
void fv_platform_voices_destroy(FvPlatformVoices *voices);
void fv_platform_voices_control(FvPlatformVoices *voices, bool enabled, bool muted, float reverb);
bool fv_platform_voices_healthy(FvPlatformVoices *voices);
bool fv_platform_voices_submit(void *context, int source_fd, const int16_t *pcm,
    size_t frames, const FvVoxDecimalTrigger *trigger, FvFragmentSettings settings);
#endif
