/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_sensor_dsp.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

/* Barometer state belongs to the sensor thread; tone state to the audio thread. */
static float pressure_baseline, rem_phase;
static uint32_t rem_pulse_counter;
static constexpr float fv_pi = 3.14159265358979323846f;

int32_t fv_update_barometer(float hpa, float *out_delta) {
    if (!isfinite(hpa) || hpa <= 0.0f) {
        if (out_delta) *out_delta = 0.0f;
        return 0;
    }
    if (pressure_baseline <= 0.0f) pressure_baseline = hpa;
    pressure_baseline = pressure_baseline * 0.992f + hpa * 0.008f;
    const float delta = hpa - pressure_baseline;
    if (out_delta) *out_delta = delta;
    return delta <= -0.10f ? 1 : 0;
}

float fv_get_barometer_baseline(void) { return pressure_baseline; }
void fv_reset_barometer_baseline(float hpa) {
    if (isfinite(hpa) && hpa > 0.0f) pressure_baseline = hpa;
}

void fv_calc_spatial_pan_gains(float azimuth_deg, float *left, float *right) {
    if (!left || !right) return;
    if (!isfinite(azimuth_deg)) { *left = *right = 0.70710678f; return; }
    /* fmod avoids an unbounded loop for very large finite input. */
    float angle = fmodf(azimuth_deg, 360.0f);
    if (angle > 180.0f) angle -= 360.0f;
    if (angle < -180.0f) angle += 360.0f;
    const float pan = fmaxf(-1.0f, fminf(1.0f, angle / 90.0f));
    const float theta = (pan + 1.0f) * (fv_pi / 4.0f);
    *left = cosf(theta); *right = sinf(theta);
}

void fv_rem_pod_generate(int16_t *samples, size_t frames, int32_t level, uint32_t sample_rate) {
    if (!samples || !frames || level <= 0 || !sample_rate) return;
    const int tier = level > 4 ? 3 : level - 1;
    static constexpr float frequencies[] = {440.0f, 880.0f, 1320.0f, 1760.0f};
    static constexpr uint32_t pulse_rates[] = {6u, 10u, 16u, 0u};
    const float step = fmodf(frequencies[tier] * 2.0f * fv_pi / (float)sample_rate, 2.0f * fv_pi);
    const uint32_t period = pulse_rates[tier] ? sample_rate / pulse_rates[tier] : 0u;
    for (size_t i = 0; i < frames; i++) {
        const bool on = !period || rem_pulse_counter % period < period / 2u;
        if (period) rem_pulse_counter++;
        if (on) {
            const int32_t tone = (int32_t)(sinf(rem_phase) * (4500.0f + (float)tier * 2000.0f));
            int32_t mixed = (int32_t)samples[i] + tone;
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32767) mixed = -32767;
            samples[i] = (int16_t)mixed;
        }
        rem_phase += step;
        if (rem_phase >= 2.0f * fv_pi) rem_phase -= 2.0f * fv_pi;
    }
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

int32_t fv_verify_blob_checksum(const uint8_t *blob, size_t size) {
    if (!blob || size < 16 || get_u32(blob) != 0x50425646u) return 0;
    const uint32_t version = get_u32(blob + 4);
    const size_t record = version == 1u ? 300u : (version == 2u ? 304u : 0u);
    if (!record) return 0;
    const uint32_t count = get_u32(blob + 8);
    if (!count || count > 4096 || size != 16u + (size_t)count * record) return 0;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 16; i < size; i++) {
        crc ^= blob[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc == get_u32(blob + 12) ? 1 : 0;
}

bool fv_vox_eval_decimal_trigger_ex(double x, double y, double z, bool is_magnetometer,
                                    float distortion, int cache_file_count,
                                    FvVoxDecimalTrigger *out_trigger) {
    if (!out_trigger) return false;
    *out_trigger = (FvVoxDecimalTrigger){0};

    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return false;
    const double val = (x * x) + (y * y) + (z * z);
    if (!isfinite(val) || val <= 0.0) return false;

    const double d_mag = fabs(is_magnetometer ? sqrt(val) : val);
    out_trigger->magnitude = d_mag;

    /* Find a shortest round-tripping decimal, then expand its exponent and
     * pad the fraction. Fixed %.20f exposes binary artifacts absent from
     * BigDecimal(String.valueOf(magnitude)). */
    char buf[64], fraction[21], digits[24];
    for (int precision = 1; precision <= 17; precision++) {
        const int n = snprintf(buf, sizeof(buf), "%.*g", precision, d_mag);
        if (n <= 0 || (size_t)n >= sizeof(buf)) return false;
        if (strtod(buf, NULL) == d_mag) break;
    }
    const char *exponent = strchr(buf, 'e');
    if (!exponent) exponent = strchr(buf, 'E');
    const int power = exponent ? (int)strtol(exponent + 1, NULL, 10) : 0;
    int digit_count = 0, integer_digits = 0;
    bool after_dot = false;
    for (const char *p = buf; *p && p != exponent; p++) {
        if (*p == '.') { after_dot = true; continue; }
        if (*p < '0' || *p > '9' || digit_count >= (int)sizeof(digits)) return false;
        digits[digit_count++] = *p;
        if (!after_dot) integer_digits++;
    }
    memset(fraction, '0', 20); fraction[20] = 0;
    const int decimal_position = integer_digits + power;
    for (int i = 0; i < 20; i++) {
        const int at = decimal_position + i;
        if (at >= 0 && at < digit_count) fraction[i] = digits[at];
    }
    const char *frac = fraction;

    /* Digits [1:4]: 3-digit gate code */
    const int i4 = (frac[1] - '0') * 100 + (frac[2] - '0') * 10 + (frac[3] - '0');
    out_trigger->gate_code = i4;

    /* 10% pass window: <= 50 or >= 950 */
    if (i4 > 50 && i4 < 950) {
        return false;
    }
    out_trigger->passed = true;

    /*
     * FieldVox Distortion-adaptive threshold modulation:
     * v7 = (int)(distortion * 4.0f) in {0, 1, 2, 3, 4}
     * upper_thresh = 9 - v7; lower_thresh = v7 + 1
     */
    const float dist = fmaxf(0.0f, fminf(1.0f, distortion));
    const int v7 = (int)(dist * 4.0f);
    const int upper_thresh = 9 - v7;
    const int lower_thresh = v7 + 1;

    /* Digit [4:5]: Pitch modulation (LOW=0.6f, NORMAL=1.0f, HIGH=1.2f) */
    const int d4 = frac[4] - '0';
    if (d4 > upper_thresh) {
        out_trigger->pitch = FV_VOX_PITCH_HIGH;
        out_trigger->pitch_factor = 1.2f;
        out_trigger->pitch_semitones = 3;
    } else if (d4 < lower_thresh) {
        out_trigger->pitch = FV_VOX_PITCH_LOW;
        out_trigger->pitch_factor = 0.6f;
        out_trigger->pitch_semitones = -9;
    } else {
        out_trigger->pitch = FV_VOX_PITCH_NORMAL;
        out_trigger->pitch_factor = 1.0f;
        out_trigger->pitch_semitones = 0;
    }

    /* Digit [5:6]: Speed modulation (SLOW=0.7f, NORMAL=1.0f, FAST=1.2f) */
    const int d5 = frac[5] - '0';
    if (d5 > upper_thresh) {
        out_trigger->speed = FV_VOX_SPEED_FAST;
        out_trigger->speed_factor = 1.2f;
    } else if (d5 < lower_thresh) {
        out_trigger->speed = FV_VOX_SPEED_SLOW;
        out_trigger->speed_factor = 0.7f;
    } else {
        out_trigger->speed = FV_VOX_SPEED_NORMAL;
        out_trigger->speed_factor = 1.0f;
    }

    /* Digit [6:7]: Grain length: (d * 0.1 * 0.4 + 0.4) sec => 400..760 ms */
    const int d6 = frac[6] - '0';
    const float duration_sec = ((float)d6 * 0.1f * 0.4f) + 0.4f;
    out_trigger->grain_duration_ms = (int)(duration_sec * 1000.0f);

    /*
     * FieldVox Speed-compensated grain duration:
     * playback_duration_ms = grain_duration_ms / playback_speed
     */
    out_trigger->playback_duration_ms = (int)((float)out_trigger->grain_duration_ms / out_trigger->speed_factor);

    /*
     * Digit [7:8]: Live vs local cache decision
     * If d7 >= 9 and 10 < N <= 20 => prefer cached
     * Else if d7 >= 8 and N > 20 => prefer cached
     * Else => prefer live
     */
    const int d7 = frac[7] - '0';
    const int N = cache_file_count;
    bool prefer_cached = false;
    if (d7 >= 9 && N > 10 && N <= 20) {
        prefer_cached = true;
    } else if (d7 >= 8 && N > 20) {
        prefer_cached = true;
    }
    out_trigger->prefer_live = !prefer_cached;

    /* Digit [8:9]: Seek offset: (d + 1) * 0.1 * 0.6 */
    const int d8 = frac[8] - '0';
    out_trigger->seek_offset = ((float)d8 + 1.0f) * 0.1f * 0.6f;

    /* Digit [9:10]: Carrier channel hop */
    const int d9 = cache_file_count >= 10 && prefer_cached ?
        (frac[9] - '0') * 10 + (frac[10] - '0') : frac[9] - '0';
    out_trigger->channel_hop = d9;

    return true;
}

bool fv_vox_eval_decimal_trigger(double x, double y, double z, bool is_magnetometer,
                                 float distortion, FvVoxDecimalTrigger *out_trigger) {
    return fv_vox_eval_decimal_trigger_ex(x, y, z, is_magnetometer, distortion, 0, out_trigger);
}

void fv_vox_clamp_grain_boundary(int total_duration_ms, float seek_fraction,
                                 int *inout_duration_ms, int *out_seek_ms) {
    if (!inout_duration_ms || !out_seek_ms) return;
    if (total_duration_ms <= 0) {
        *inout_duration_ms = 0;
        *out_seek_ms = 0;
        return;
    }
    const float frac = fmaxf(0.0f, fminf(1.0f, seek_fraction));
    int seek = (int)(frac * (float)total_duration_ms);
    int dur = *inout_duration_ms;
    if (dur <= 0) dur = 400;

    /* FieldVox overrun protection */
    if (seek + dur > total_duration_ms) {
        const int shifted = total_duration_ms - dur - 10;
        seek = shifted > 0 ? shifted : 0;
        const int remaining = total_duration_ms - seek;
        if (dur > remaining) dur = remaining > 0 ? remaining : 0;
    }
    *out_seek_ms = seek;
    *inout_duration_ms = dur;
}

float fv_vox_echo_tap_volume(float base_vol, int tap_index) {
    if (tap_index < 1 || tap_index > 4) return 0.0f;
    float atten = 0.2f;
    for (int i = 0; i < tap_index; i++) {
        atten *= 0.3f;
    }
    return base_vol * atten;
}

uint64_t fv_vox_echo_tap_delay_ms(uint64_t delay_interval_ms, int tap_index) {
    if (tap_index < 1) return 0;
    return (uint64_t)tap_index * delay_interval_ms;
}

size_t fv_vox_echo_schedule(float base_vol, uint64_t interval,
                            FvVoxEchoTap out_taps[static FV_VOX_ECHO_TAPS]) {
    for (int tap = 1; tap <= FV_VOX_ECHO_TAPS; tap++) {
        out_taps[tap - 1] = (FvVoxEchoTap){
            .delay = fv_vox_echo_tap_delay_ms(interval, tap),
            .volume = fv_vox_echo_tap_volume(base_vol, tap)
        };
    }
    return FV_VOX_ECHO_TAPS;
}
