#ifndef FIELDVOX_NATIVE_AUDIO_H
#define FIELDVOX_NATIVE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#define FV_NATIVE_SPECTRUM_BINS 64

void fv_audio_dsp_reset(void);
void fv_audio_capture_and_analyze(const int16_t *samples, int count);
void fv_audio_apply_echo(int16_t *samples, int count);
void fv_audio_apply_noise(int16_t *samples, int count, int amplitude);
void fv_audio_reverse(int16_t *samples, int count);
void fv_audio_get_spectrum(float out_bins[FV_NATIVE_SPECTRUM_BINS]);
bool fv_audio_export_preroll_wav(const char *path, int pre_seconds);

void fv_vocal_bandpass(int16_t *samples, int count, uint32_t sample_rate);
void fv_apply_schroeder_reverb(int16_t *samples, int count);
void fv_apply_granular_pitch(int16_t *samples, int count, int semitones);

#endif
