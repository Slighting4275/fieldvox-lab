/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_CORE_H
#define FV_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FV_SAMPLE_RATE 48000u
#define FV_CHANNELS 1u
#define FV_BLOB_VERSION 1u
#define FV_PI 3.14159265358979323846

typedef struct {
    uint64_t t_ns;
    float mx, my, mz;
    float ax, ay, az;
    float gx, gy, gz;
} FV_SensorSample;

typedef struct {
    float baseline_mag;
    float ema_mag;
    float ema_delta;
    float trigger_threshold;
    uint64_t cooldown_ns;
    uint64_t last_trigger_ns;
    uint64_t trigger_count;
} FV_TriggerState;

typedef struct {
    uint64_t state;
} FV_Rng;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    FILE *fp;
    uint64_t frames_written;
} FV_WavWriter;

typedef struct {
    uint64_t trigger_id;
    uint64_t timestamp_ns;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t frame_count;
    uint32_t payload_bytes;
    uint64_t source_hash;
    float mag_value;
    float mag_delta;
    float freq_hz;
} FV_BlobMeta;

float fv_mag3(float x, float y, float z);
void fv_trigger_init(FV_TriggerState *st, float baseline_mag, float threshold, uint64_t cooldown_ns);
int fv_trigger_update(FV_TriggerState *st, FV_SensorSample sample, float *out_mag, float *out_delta);

void fv_rng_seed(FV_Rng *rng, uint64_t seed);
uint32_t fv_rng_u32(FV_Rng *rng);
float fv_rng_float_signed(FV_Rng *rng);

int fv_mkdir_p(const char *path);
int fv_wav_begin(FV_WavWriter *w, const char *path, uint32_t sample_rate, uint16_t channels);
int fv_wav_write_i16(FV_WavWriter *w, const int16_t *samples, uint32_t frames);
int fv_wav_finish(FV_WavWriter *w);

void fv_make_blob_audio(
    int16_t *dst,
    uint32_t frames,
    float freq_hz,
    float noise_mix,
    float echo_mix,
    float distortion,
    FV_Rng *rng
);

int fv_blob_write(FILE *fp, const FV_BlobMeta *meta, const int16_t *pcm);
uint64_t fv_fnv1a64(const void *data, size_t len, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif
