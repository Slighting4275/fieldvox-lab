/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fv_native_audio.h"
#include "fv_recording_data.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 3-second Pre-roll Ring Buffer for instant, zero-latency hit marking */
#define FV_PREROLL_SECONDS 3
#define FV_PREROLL_SAMPLES (48000 * FV_PREROLL_SECONDS)
static int16_t s_preroll_ring[FV_PREROLL_SAMPLES];
static uint32_t s_preroll_write = 0;

/* Vocal Formant Bandpass Filter (300Hz - 3400Hz @ 48kHz, Direct Form II Transposed) */
static float s_bp_hp_d1 = 0.0f, s_bp_hp_d2 = 0.0f;
static float s_bp_lp_d1 = 0.0f, s_bp_lp_d2 = 0.0f;

static inline float fv_vocal_bandpass_single(float in_val) {
    float hp_out = 0.972614f * in_val + s_bp_hp_d1;
    s_bp_hp_d1 = -1.945228f * in_val - (-1.944487f * hp_out) + s_bp_hp_d2;
    s_bp_hp_d2 = 0.972614f * in_val - (0.945978f * hp_out);

    float lp_out = 0.037320f * hp_out + s_bp_lp_d1;
    s_bp_lp_d1 = 0.074639f * hp_out - (-1.384079f * lp_out) + s_bp_lp_d2;
    s_bp_lp_d2 = 0.037320f * hp_out - (0.533358f * lp_out);
    return lp_out;
}

void fv_vocal_bandpass(int16_t *samples, int count, uint32_t sample_rate) {
    (void)sample_rate;
    if (!samples || count <= 0) return;
    for (int i = 0; i < count; i++) {
        float f = fv_vocal_bandpass_single((float)samples[i]);
        if (f > 32767.0f) f = 32767.0f;
        if (f < -32768.0f) f = -32768.0f;
        samples[i] = (int16_t)f;
    }
}

/* Real-Time Radix-2 FFT (256 points) for 2D Waterfall Spectrogram */
#define FV_FFT_SIZE 256
#define FV_SPEC_BINS 64
static float s_fft_re[FV_FFT_SIZE];
static float s_fft_im[FV_FFT_SIZE];
static int16_t s_fft_acc[FV_FFT_SIZE];
static int s_fft_acc_count = 0;
static float s_latest_spec_bins[FV_SPEC_BINS];
static float s_hanning[FV_FFT_SIZE];
static int s_hanning_inited = 0;

static void fv_init_hanning(void) {
    if (s_hanning_inited) return;
    for (int i = 0; i < FV_FFT_SIZE; i++) {
        s_hanning[i] = 0.5f * (1.0f - cosf((float)(2.0 * M_PI * i / (FV_FFT_SIZE - 1))));
    }
    s_hanning_inited = 1;
}

static void fv_radix2_fft(float *re, float *im, int n) {
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * (float)M_PI / (float)len;
        float wlen_r = cosf(angle);
        float wlen_i = sinf(angle);
        for (int i = 0; i < n; i += len) {
            float w_r = 1.0f;
            float w_i = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float u_r = re[i + k];
                float u_i = im[i + k];
                float v_r = re[i + k + len / 2] * w_r - im[i + k + len / 2] * w_i;
                float v_i = re[i + k + len / 2] * w_i + im[i + k + len / 2] * w_r;
                re[i + k] = u_r + v_r;
                im[i + k] = u_i + v_i;
                re[i + k + len / 2] = u_r - v_r;
                im[i + k + len / 2] = u_i - v_i;
                float next_w_r = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = next_w_r;
            }
        }
    }
}

void fv_audio_dsp_reset(void) {
    memset(s_preroll_ring, 0, sizeof(s_preroll_ring));
    s_preroll_write = 0;
    memset(s_latest_spec_bins, 0, sizeof(s_latest_spec_bins));
    s_fft_acc_count = 0;
    s_bp_hp_d1 = s_bp_hp_d2 = 0.0f;
    s_bp_lp_d1 = s_bp_lp_d2 = 0.0f;
}

void fv_audio_capture_and_analyze(const int16_t *samples, int count) {
    if (!samples || count <= 0) return;
    fv_init_hanning();
    for (int i = 0; i < count; i++) {
        const int16_t raw = samples[i];
        s_preroll_ring[s_preroll_write] = raw;
        s_preroll_write = (s_preroll_write + 1) % FV_PREROLL_SAMPLES;
        s_fft_acc[s_fft_acc_count++] = raw;
        if (s_fft_acc_count < FV_FFT_SIZE) continue;
        for (int k = 0; k < FV_FFT_SIZE; k++) {
            s_fft_re[k] = (float)s_fft_acc[k] * s_hanning[k];
            s_fft_im[k] = 0.0f;
        }
        fv_radix2_fft(s_fft_re, s_fft_im, FV_FFT_SIZE);
        for (int b = 0; b < FV_SPEC_BINS; b++) {
            const float r = s_fft_re[b];
            const float im = s_fft_im[b];
            const float mag = sqrtf(r * r + im * im) / 32768.0f;
            float log_mag = log1pf(mag * 12.0f) / 2.5f;
            if (log_mag > 1.0f) log_mag = 1.0f;
            s_latest_spec_bins[b] = s_latest_spec_bins[b] * 0.4f + log_mag * 0.6f;
        }
        s_fft_acc_count = 0;
    }
}

void fv_audio_reverse(int16_t *samples, int count) {
    if (!samples || count <= 1) return;
    for (int i = 0; i < count / 2; i++) {
        const int16_t tmp = samples[i];
        samples[i] = samples[count - 1 - i];
        samples[count - 1 - i] = tmp;
    }
}

void fv_audio_get_spectrum(float out_bins[FV_NATIVE_SPECTRUM_BINS]) {
    if (out_bins) memcpy(out_bins, s_latest_spec_bins, sizeof(s_latest_spec_bins));
}

bool fv_audio_export_preroll_wav(const char *path, int pre_seconds) {
    if (!path || !*path) return false;
    const int seconds = pre_seconds <= 0 ? 3 : (pre_seconds > FV_PREROLL_SECONDS ? FV_PREROLL_SECONDS : pre_seconds);
    const uint32_t sample_count = (uint32_t)(seconds * 48000);
    const uint32_t start_idx = (s_preroll_write + FV_PREROLL_SAMPLES - sample_count) % FV_PREROLL_SAMPLES;
    const uint32_t first_count = sample_count < FV_PREROLL_SAMPLES - start_idx
        ? sample_count : FV_PREROLL_SAMPLES - start_idx;
    if (first_count == sample_count) {
        return fv_wav_write_file(path, 48000u, 1u, &s_preroll_ring[start_idx], sample_count);
    }
    int16_t *contiguous = malloc((size_t)sample_count * sizeof(int16_t));
    if (!contiguous) return false;
    memcpy(contiguous, &s_preroll_ring[start_idx], first_count * sizeof(int16_t));
    memcpy(contiguous + first_count, s_preroll_ring, (sample_count - first_count) * sizeof(int16_t));
    const bool ok = fv_wav_write_file(path, 48000u, 1u, contiguous, sample_count);
    free(contiguous);
    return ok;
}
