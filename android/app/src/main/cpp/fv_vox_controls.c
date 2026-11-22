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
