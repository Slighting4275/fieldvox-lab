/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FIELDVOX_NATIVE_AUDIO_H
#define FIELDVOX_NATIVE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define FV_NATIVE_SPECTRUM_BINS 64

void fv_audio_dsp_reset(void);
void fv_audio_capture_and_analyze(const int16_t *samples, int count);
void fv_audio_reverse(int16_t *samples, int count);
void fv_audio_get_spectrum(float out_bins[FV_NATIVE_SPECTRUM_BINS]);
bool fv_audio_export_preroll_wav(const char *path, int pre_seconds);
void fv_vocal_bandpass(int16_t *samples, int count, uint32_t sample_rate);

#endif
