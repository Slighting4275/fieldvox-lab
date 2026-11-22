/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_SENSOR_DSP_H
#define FV_SENSOR_DSP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FV_GATE_AUTO_SCAN = 0,
    FV_GATE_MAGNETOMETER,
    FV_GATE_GYROSCOPE,
    FV_GATE_VIBE,
    FV_GATE_PROXIMITY,
    FV_GATE_REASON_COUNT
} FvGateReason;

const char *fv_gate_reason_name(FvGateReason reason);
bool fv_gate_emf_exceeds_threshold(float delta, float threshold);

float fv_mag3(float x, float y, float z);
int32_t fv_update_barometer(float hpa, float *out_delta);
float fv_get_barometer_baseline(void);
void fv_reset_barometer_baseline(float hpa);
void fv_calc_spatial_pan_gains(float azimuth_deg, float *out_left, float *out_right);
void fv_rem_pod_generate(int16_t *samples, size_t frames, int32_t level, uint32_t sample_rate);
int32_t fv_verify_blob_checksum(const uint8_t *blob, size_t size);

typedef enum {
    FV_VOX_PITCH_LOW = 0,    /* 0.6x (-8.84 semitones) */
    FV_VOX_PITCH_NORMAL = 1, /* 1.0x (0 st) */
    FV_VOX_PITCH_HIGH = 2    /* 1.2x (+3.16 semitones) */
} FvVoxPitch;

typedef enum {
    FV_VOX_SPEED_SLOW = 0,   /* 0.7x */
    FV_VOX_SPEED_NORMAL = 1, /* 1.0x */
    FV_VOX_SPEED_FAST = 2    /* 1.2x */
} FvVoxSpeed;

typedef struct {
    bool passed;             /* True if digits [1:4] <= 50 || >= 950 */
    double magnitude;        /* sqrt(sum of squares) for magnetometer; sum of squares for motion sensors. */
    int gate_code;           /* Digits [1:4] integer (0..999) */
    FvVoxPitch pitch;
    float pitch_factor;      /* 0.6f, 1.0f, 1.2f */
    int pitch_semitones;     /* Rounded telemetry only (-9, 0, +3); playback uses pitch_factor. */
    FvVoxSpeed speed;
    float speed_factor;      /* 0.7f, 1.0f, 1.2f */
    int grain_duration_ms;   /* 400..760 ms */
    int playback_duration_ms;/* Speed-compensated duration: grain_duration_ms / speed_factor */
    float seek_offset;       /* 0.06..0.60 */
    int channel_hop;         /* 0..9 live; 0..99 for larger cached banks */
    bool prefer_live;        /* based on digit 7 */
} FvVoxDecimalTrigger;

/*
 * FieldVox decimal sensor gate evaluator.
 * Evaluates a magnetometer, accelerometer, or gyroscope vector with distortion level (0.0..1.0, default 0.5).
 * Modulates pitch and speed digit thresholds according to distortion level.
 * Calculates speed-compensated playback duration.
 * Returns true if the 10% gate window passed.
 */
bool fv_vox_eval_decimal_trigger(double x, double y, double z, bool is_magnetometer,
                                 float distortion, FvVoxDecimalTrigger *out_trigger);

bool fv_vox_eval_decimal_trigger_ex(double x, double y, double z, bool is_magnetometer,
                                    float distortion, int cache_file_count,
                                    FvVoxDecimalTrigger *out_trigger);

/*
 * Four scheduled echo repetitions: Volume_k = base_vol * 0.2 * (0.3)^k.
 */
enum { FV_VOX_ECHO_TAPS = 4 };
typedef struct {
    uint64_t delay;
    float volume;
} FvVoxEchoTap;

float fv_vox_echo_tap_volume(float base_vol, int tap_index);
uint64_t fv_vox_echo_tap_delay_ms(uint64_t delay_interval_ms, int tap_index);
size_t fv_vox_echo_schedule(float base_vol, uint64_t interval,
                            FvVoxEchoTap out_taps[static FV_VOX_ECHO_TAPS]);

#endif
