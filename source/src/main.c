/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_core.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--seconds N] [--threshold FLOAT] [--seed HEX_OR_INT]\n", argv0);
}

static int parse_u64_arg(const char *text, int base, uint64_t max_value, uint64_t *out) {
    char *end = NULL;
    if (!text || text[0] == '\0' || text[0] == '-') return -1;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, base);
    if (errno == ERANGE || end == text || *end != '\0' || value > max_value) return -1;
    *out = (uint64_t)value;
    return 0;
}

static int parse_float_arg(const char *text, float *out) {
    char *end = NULL;
    if (!text || text[0] == '\0') return -1;
    errno = 0;
    const float value = strtof(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(value)) return -1;
    *out = value;
    return 0;
}

static int parse_args(int argc, char **argv, uint32_t *seconds, float *threshold, uint64_t *seed) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            uint64_t value = 0;
            if (parse_u64_arg(argv[++i], 10, UINT32_MAX, &value) != 0) {
                fprintf(stderr, "invalid seconds value\n");
                return -1;
            }
            *seconds = (uint32_t)value;
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            if (parse_float_arg(argv[++i], threshold) != 0) {
                fprintf(stderr, "invalid threshold value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], 0, UINT64_MAX, seed) != 0) {
                fprintf(stderr, "invalid seed value\n");
                return -1;
            }
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    if (*seconds < 2 || *seconds > 3600) {
        fprintf(stderr, "seconds must be 2..3600\n");
        return -1;
    }

    if (*threshold <= 0.0f || *threshold > 100.0f) {
        fprintf(stderr, "threshold must be 0..100\n");
        return -1;
    }

    return 0;
}

static FV_SensorSample make_sim_sample(uint64_t t_ns, uint32_t tick, FV_Rng *rng) {
    const float t = (float)t_ns / 1000000000.0f;

    const float base_x = 19.0f;
    const float base_y = 6.0f;
    const float base_z = 46.0f;

    const float drift = sinf(t * 0.21f) * 0.35f;
    const float noise = fv_rng_float_signed(rng) * 0.08f;

    float pulse = 0.0f;
    if ((tick % 137u) > 100u && (tick % 137u) < 112u) pulse += 7.0f;
    if ((tick % 251u) > 190u && (tick % 251u) < 205u) pulse -= 5.0f;

    FV_SensorSample s;
    memset(&s, 0, sizeof(s));

    s.t_ns = t_ns;

    s.mx = base_x + drift + noise + pulse;
    s.my = base_y + noise * 0.5f - pulse * 0.20f;
    s.mz = base_z + drift * 0.25f + noise * 0.25f + pulse * 0.60f;

    s.ax = sinf(t * 0.70f) * 0.03f;
    s.ay = cosf(t * 0.44f) * 0.03f;
    s.az = 9.80f + fv_rng_float_signed(rng) * 0.02f;

    s.gx = fv_rng_float_signed(rng) * 0.01f;
    s.gy = fv_rng_float_signed(rng) * 0.01f;
    s.gz = fv_rng_float_signed(rng) * 0.01f;

    return s;
}

int main(int argc, char **argv) {
    uint32_t seconds = 24;
    float threshold = 1.80f;
    uint64_t seed = 0xBEEFBEEFULL;
    FILE *blob_fp = NULL;
    FILE *log_fp = NULL;
    FV_WavWriter wav;
    int16_t *pcm = NULL;
    uint32_t event_count = 0;
    int wav_open = 0;
    int result = 1;

    memset(&wav, 0, sizeof(wav));

    if (parse_args(argc, argv, &seconds, &threshold, &seed) != 0) return 2;

    if (fv_mkdir_p("out") != 0) {
        fprintf(stderr, "failed to create out/: %s\n", strerror(errno));
        return 1;
    }

    blob_fp = fopen("out/session.fvblobs", "wb");
    log_fp = fopen("out/session.jsonl", "wb");

    if (!blob_fp || !log_fp) {
        fprintf(stderr, "failed to open output files\n");
        goto cleanup;
    }

    if (fv_wav_begin(&wav, "out/session.wav", FV_SAMPLE_RATE, FV_CHANNELS) != 0) {
        fprintf(stderr, "failed to open wav output\n");
        goto cleanup;
    }
    wav_open = 1;

    FV_Rng rng;
    fv_rng_seed(&rng, seed);

    const float baseline = fv_mag3(19.0f, 6.0f, 46.0f);

    FV_TriggerState trig;
    fv_trigger_init(&trig, baseline, threshold, 450000000ULL);

    const uint32_t sensor_hz = 120;
    const uint64_t step_ns = 1000000000ULL / sensor_hz;
    const uint64_t ticks = (uint64_t)seconds * sensor_hz;

    const uint32_t blob_frames = FV_SAMPLE_RATE / 4u;
    pcm = calloc(blob_frames, sizeof(*pcm));

    int16_t silence[FV_SAMPLE_RATE / 20u];
    memset(silence, 0, sizeof(silence));

    if (!pcm) {
        fprintf(stderr, "allocation failure\n");
        goto cleanup;
    }

    fprintf(
        log_fp,
        "{\"type\":\"session_start\",\"engine\":\"fieldvox-c-sim\",\"sample_rate\":%u,"
        "\"sensor_hz\":%u,\"seconds\":%u,\"threshold\":%.3f,\"seed\":\"0x%016" PRIx64 "\"}\n",
        FV_SAMPLE_RATE,
        sensor_hz,
        seconds,
        threshold,
        seed
    );

    for (uint64_t tick = 0; tick < ticks; tick++) {
        const uint64_t t_ns = tick * step_ns;
        FV_SensorSample s = make_sim_sample(t_ns, (uint32_t)tick, &rng);

        float mag = 0.0f;
        float delta = 0.0f;

        if (fv_trigger_update(&trig, s, &mag, &delta)) {
            event_count++;

            const float freq =
                120.0f + fmodf((mag * 19.0f) + (delta * 37.0f) + (float)(event_count * 17u), 720.0f);

            const float noise_mix = fminf(0.82f, 0.15f + delta * 0.035f);
            const float echo_mix = fminf(0.70f, 0.20f + delta * 0.020f);
            const float dist = fminf(0.85f, 0.10f + delta * 0.025f);

            fv_make_blob_audio(pcm, blob_frames, freq, noise_mix, echo_mix, dist, &rng);

            FV_BlobMeta meta;
            memset(&meta, 0, sizeof(meta));

            meta.trigger_id = event_count;
            meta.timestamp_ns = t_ns;
            meta.sample_rate = FV_SAMPLE_RATE;
            meta.channels = FV_CHANNELS;
            meta.frame_count = blob_frames;
            meta.payload_bytes = blob_frames * FV_CHANNELS * 2u;
            meta.source_hash = fv_fnv1a64(pcm, meta.payload_bytes, seed ^ event_count);
            meta.mag_value = mag;
            meta.mag_delta = delta;
            meta.freq_hz = freq;

            if (fv_blob_write(blob_fp, &meta, pcm) != 0) {
                fprintf(stderr, "blob write failed\n");
                goto cleanup;
            }

            if (fv_wav_write_i16(&wav, pcm, blob_frames) != 0) {
                fprintf(stderr, "wav write failed\n");
                goto cleanup;
            }

            if (fv_wav_write_i16(&wav, silence, (uint32_t)(sizeof(silence) / sizeof(silence[0]))) != 0) {
                fprintf(stderr, "wav silence write failed\n");
                goto cleanup;
            }

            fprintf(
                log_fp,
                "{\"type\":\"trigger\",\"id\":%u,\"t_ns\":%" PRIu64 ",\"mag\":%.6f,"
                "\"ema_delta\":%.6f,\"freq_hz\":%.3f,\"noise_mix\":%.3f,"
                "\"echo_mix\":%.3f,\"distortion\":%.3f,\"source_hash\":\"0x%016" PRIx64 "\","
                "\"frames\":%u}\n",
                event_count,
                t_ns,
                mag,
                delta,
                freq,
                noise_mix,
                echo_mix,
                dist,
                meta.source_hash,
                blob_frames
            );
        }
    }

    fprintf(log_fp, "{\"type\":\"session_end\",\"events\":%u}\n", event_count);

    result = event_count == 0 ? 3 : 0;

cleanup:
    free(pcm);
    if (blob_fp && fclose(blob_fp) != 0) result = 1;
    if (log_fp && fclose(log_fp) != 0) result = 1;
    if (wav_open && fv_wav_finish(&wav) != 0) {
        fprintf(stderr, "wav finalize failed\n");
        result = 1;
    }

    if (result != 0 && result != 3) return result;

    printf("FieldVox Lab C simulator complete.\n");
    printf("events=%u\n", event_count);
    printf("outputs:\n");
    printf("  out/session.fvblobs  binary FVBL blob stream\n");
    printf("  out/session.wav      rendered triggered audio\n");
    printf("  out/session.jsonl    audit log\n");

    return result;
}
