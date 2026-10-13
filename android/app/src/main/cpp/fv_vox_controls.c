#include "fv_vox_controls.h"
#include "fv_native_audio.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static int percent(int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); }

int fv_vox_slider_percent(float x, float left, float width) {
    if (!isfinite(x) || !isfinite(left) || !isfinite(width) || width <= 0.0f) return 0;
    const float position = (x - left - width * 0.12f) / (width * 0.76f);
    if (position <= 0.0f) return 0;
    if (position >= 1.0f) return 100;
    return (int)(position * 100.0f + 0.5f);
}

float fv_vox_calc_speech_band_energy(const int16_t *mic, int frames, uint32_t sample_rate) {
    if (!mic || frames < 64 || !sample_rate) return 0.0f;
    const int n = frames;
    const double delta_f = (double)sample_rate / (double)n;
    int k_start = (int)ceil(80.0 / delta_f);
    int k_end = (int)floor(300.0 / delta_f);
    if (k_start < 1) k_start = 1;
    if (k_end >= n / 2) k_end = n / 2 - 1;
    if (k_start > k_end) return 0.0f;

    static constexpr double pi2 = 6.28318530717958647692;
    double total_mag = 0.0;
    int bin_count = 0;
    for (int k = k_start; k <= k_end; k++) {
        double re = 0.0, im = 0.0;
        const double omega = (pi2 * (double)k) / (double)n;
        for (int i = 0; i < n; i++) {
            const double w = 0.5 * (1.0 - cos((pi2 * (double)i) / (double)(n - 1)));
            const double s = ((double)mic[i] / 32768.0) * w;
            const double angle = omega * (double)i;
            re += s * cos(angle);
            im -= s * sin(angle);
        }
        total_mag += sqrt(re * re + im * im);
        bin_count++;
    }
    return bin_count > 0 ? (float)(total_mag / (double)bin_count) : 0.0f;
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

static void blend(int16_t *pcm, const int16_t *dry, int frames, int amount) {
    for (int i = 0; i < frames; i++) {
        const int32_t value = ((int32_t)pcm[i] * amount + (int32_t)dry[i] * (100 - amount)) / 100;
        pcm[i] = (int16_t)value;
    }
}

void fv_vox_apply_effects(int16_t *pcm, int frames, int echo, int reverb, int distortion) {
    if (!pcm || frames <= 0) return;
    echo = percent(echo); reverb = percent(reverb); distortion = percent(distortion);
    int16_t dry[256];
    const int echo_delay_ms = echo > 0 ? (echo * 200 / 100) : 150;
    const float reverb_mix = reverb > 0 ? ((float)reverb / 100.0f) : 0.75f;
    for (int off = 0; off < frames;) {
        const int count = frames - off > 256 ? 256 : frames - off;
        int16_t *block = pcm + off;
        memcpy(dry, block, (size_t)count * sizeof(*dry));
        /* Advance delay state even at 0%, so enabling cannot replay an old session. */
        fv_audio_apply_echo_ex(block, count, echo_delay_ms, 1.0f);
        blend(block, dry, count, echo);
        memcpy(dry, block, (size_t)count * sizeof(*dry));
        fv_apply_schroeder_reverb_ex(block, count, reverb_mix);
        blend(block, dry, count, reverb);
        if (distortion) {
            const float drive = 1.0f + (float)distortion * 0.07f;
            const float normalization = tanhf(drive);
            for (int i = 0; i < count; i++) {
                const float clean = (float)block[i];
                const float wet = tanhf(clean / 32768.0f * drive) / normalization * 32767.0f;
                const float value = clean + (wet - clean) * ((float)distortion / 100.0f);
                block[i] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, value));
            }
        }
        off += count;
    }
}
