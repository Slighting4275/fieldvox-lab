/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_VOX_CONTROLS_H
#define FV_VOX_CONTROLS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float noise_floor;
    uint64_t warmup_frames, hold_until;
    bool initialized;
    /* FieldVox 80-300 Hz Speech VAD */
    float speech_energy;
    bool speech_detected;
} FvVoxDucker;

/* Calculates average magnitude in the human speech frequency band [80 Hz, 300 Hz]. */
float fv_vox_calc_speech_band_energy(const int16_t *mic, int frames, uint32_t sample_rate);

/*
 * FieldVox speech detector.
 * Detects voice in [80 Hz, 300 Hz] band (> 2.0f energy) and applies 2000 ms holdoff.
 */
bool fv_vox_detect_voice(FvVoxDucker *state, const int16_t *mic, int frames,
                         uint64_t clock, uint32_t sample_rate);
/* Shared slider geometry: the first/last 12% of the row are endpoint padding. */
int fv_vox_slider_percent(float x, float left, float width);

#endif
