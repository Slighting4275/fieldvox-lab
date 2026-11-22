/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_vox_controls.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

int fv_vox_slider_percent(float x, float left, float width) {
    if (!isfinite(x) || !isfinite(left) || !isfinite(width) || width <= 0.0f) return 0;
    const float position = (x - left - width * 0.12f) / (width * 0.76f);
    if (position <= 0.0f) return 0;
    if (position >= 1.0f) return 100;
    return (int)(position * 100.0f + 0.5f);
}

static double packed_bin(const double *re, const double *im, unsigned n, unsigned index) {
    if (index == 0u) return re[0];
    if (index == 1u) return re[n/2u];
    return index & 1u ? im[index/2u] : re[index/2u];
}
float fv_vox_calc_speech_band_energy(const int16_t *mic, int frames, uint32_t sample_rate) {
    if (!mic || frames < 64 || !sample_rate) return 0.0f;
    unsigned n = 64u;
    while (n < 4096u && n*2u <= (unsigned)frames) n *= 2u;
    double re[4096], im[4096];
    static constexpr double pi2 = 6.28318530717958647692;
    for (unsigned i=0;i<n;i++) {
        re[i] = ((double)mic[i]/32768.0)*0.5*(1.0-cos(pi2*(double)i/(double)(n-1u)));
        im[i] = 0.0;
    }
    for (unsigned i=1u,j=0u;i<n;i++) {
        unsigned bit=n/2u;
        while (j & bit) { j ^= bit; bit >>= 1u; }
        j ^= bit;
        if (i<j) { const double value=re[i]; re[i]=re[j]; re[j]=value; }
    }
    for (unsigned length=2u;length<=n;length*=2u) {
        const double ar=cos(-pi2/(double)length), ai=sin(-pi2/(double)length);
        for (unsigned off=0;off<n;off+=length) {
            double wr=1.0,wi=0.0;
            for (unsigned i=0;i<length/2u;i++) {
                const unsigned a=off+i,b=a+length/2u;
                const double tr=wr*re[b]-wi*im[b],ti=wr*im[b]+wi*re[b];
                re[b]=re[a]-tr; im[b]=im[a]-ti; re[a]+=tr; im[a]+=ti;
                const double next=wr*ar-wi*ai; wi=wr*ai+wi*ar; wr=next;
            }
        }
    }
    /* Preserve the packed-real FFT sampling convention and its overlapping
     * real/imaginary pairs; a conventional bin average gives another threshold. */
    const double step=(double)sample_rate/(2.0*(double)n);
    double total=0.0;
    const unsigned count=(unsigned)floor(220.0/step)+1u;
    for (unsigned bin=0;bin<count;bin++) {
        const double frequency=80.0+(double)bin*step;
        const unsigned index=(unsigned)floor(frequency/step);
        if (index+1u<n) {
            const double a=packed_bin(re,im,n,index),b=packed_bin(re,im,n,index+1u);
            total+=hypot(a,b);
        }
    }
    return count ? (float)(total/(double)count) : 0.0f;
}

bool fv_vox_detect_voice(FvVoxDucker *state, const int16_t *mic, int frames,
                         uint64_t clock, uint32_t sample_rate) {
    if (!state || !sample_rate) return false;
    if (mic && frames > 0) {
        /* FieldVox Speech Band Energy Detector */
        const float band_energy = fv_vox_calc_speech_band_energy(mic, frames, sample_rate);
        state->speech_energy = band_energy;

        /* Also track RMS for floor estimation */
        double energy = 0.0;
        for (int i = 0; i < frames; i++) {
            const double value = (double)mic[i] / 32768.0;
            energy += value * value;
        }
        const float rms = (float)sqrt(energy / (double)frames);
        if (!state->initialized) {
            state->initialized = true;
            state->noise_floor = fminf(rms, 0.02f);
        }

        /* FieldVox threshold: speech band energy > 2.0f triggers voice detection */
        const bool speech_hit = band_energy > 2.0f;
        if (speech_hit) {
            /* FieldVox 2000 ms holdoff window */
            const uint64_t hold = (uint64_t)sample_rate * 2000u / 1000u;
            state->hold_until = clock > UINT64_MAX - hold ? UINT64_MAX : clock + hold;
            state->speech_detected = true;
        } else {
            state->noise_floor += 0.01f * (rms - state->noise_floor);
        }
    }
    if (clock >= state->hold_until) {
        state->speech_detected = false;
    }
    return clock < state->hold_until;
}

void fv_hold_begin(FvHoldGesture *g, float x, float y, long now_ms, bool eligible) {
    if (!g) return;
    *g = (FvHoldGesture){0};
    if (!eligible || !isfinite(x) || !isfinite(y)) return;
    g->tracking = true;
    g->start_x = x;
    g->start_y = y;
    g->start_ms = now_ms;
}

bool fv_hold_update(FvHoldGesture *g, float x, float y, float slop_px, long now_ms) {
    if (!g || !g->tracking) return false;
    if (!isfinite(x) || !isfinite(y) || fabsf(x - g->start_x) > slop_px || fabsf(y - g->start_y) > slop_px) {
        /* Stop the burst while keeping an already-triggered hold consumed on release. */
        g->tracking = false;
        return false;
    }
    if (now_ms - g->start_ms >= 350L) g->triggered = true;
    return g->triggered;
}

void fv_hold_cancel(FvHoldGesture *g) {
    if (!g) return;
    g->tracking = false;
    g->triggered = false;
}

bool fv_hold_end(FvHoldGesture *g) {
    if (!g) return false;
    const bool was_triggered = g->triggered;
    g->tracking = false;
    g->triggered = false;
    return was_triggered;
}

static float band_peak(const float spectrum[64], int start, int end) {
    float peak = 0.0f;
    for (int i = start; i < end; i++) {
        if (isfinite(spectrum[i]) && spectrum[i] > peak) peak = spectrum[i];
    }
    return fminf(1.0f, fmaxf(0.0f, peak));
}

void fv_vis_siri_lobes(const float spectrum[64], float time_sec, int columns,
                       float *out_low, float *out_mid, float *out_high) {
    if (!spectrum || columns <= 0 || !out_low || !out_mid || !out_high) return;
    const size_t n = (size_t)columns;
    const float e_low = band_peak(spectrum, 0, 16);
    const float e_mid = band_peak(spectrum, 16, 40);
    const float e_high = band_peak(spectrum, 40, 64);
    if (fmaxf(e_low, fmaxf(e_mid, e_high)) < 0.0005f) {
        memset(out_low, 0, n * sizeof(float));
        memset(out_mid, 0, n * sizeof(float));
        memset(out_high, 0, n * sizeof(float));
        return;
    }
    const float c0 = -0.35f + 0.16f * sinf(time_sec * 1.9f);
    const float c1 = 0.12f * cosf(time_sec * 1.5f);
    const float c2 = 0.35f + 0.16f * sinf(time_sec * 2.3f + 1.0f);
    const float m0 = 0.65f + 0.35f * (0.5f + 0.5f * sinf(time_sec * 3.1f));
    const float m1 = 0.65f + 0.35f * (0.5f + 0.5f * cosf(time_sec * 2.7f));
    const float m2 = 0.65f + 0.35f * (0.5f + 0.5f * sinf(time_sec * 3.7f + 0.7f));
    const float denom = (float)(columns > 1 ? columns - 1 : 1);
    for (int i = 0; i < columns; i++) {
        const float x = -1.0f + 2.0f * (float)i / denom;
        const float taper = fmaxf(0.0f, 1.0f - x * x * 0.42f);
        const float d0 = x - c0, d1 = x - c1, d2 = x - c2;
        out_low[i] = fminf(1.0f, e_low * m0 * expf(-d0 * d0 / 0.18f) * taper);
        out_mid[i] = fminf(1.0f, e_mid * m1 * expf(-d1 * d1 / 0.14f) * taper);
        out_high[i] = fminf(1.0f, e_high * m2 * expf(-d2 * d2 / 0.16f) * taper);
    }
}
