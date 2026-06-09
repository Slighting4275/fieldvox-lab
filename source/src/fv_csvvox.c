#include "fv_core.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t rows_seen;
    uint64_t rows_parsed;
    uint64_t rows_skipped;
    uint64_t baseline_samples;
    double baseline_sum;
    uint64_t triggers;
} FV_CsvStats;

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s --input-csv FILE --out-prefix PREFIX [--threshold FLOAT] [--baseline-samples N] [--seed HEX_OR_INT]\n\n"
        "CSV format:\n"
        "  t_ns,mx,my,mz,ax,ay,az,gx,gy,gz\n\n"
        "Units expected for Android later:\n"
        "  t_ns = sensor timestamp in nanoseconds\n"
        "  mx,my,mz = magnetic field axes in microtesla\n"
        "  ax,ay,az = accelerometer axes, optional but accepted\n"
        "  gx,gy,gz = gyroscope axes, optional but accepted\n",
        argv0
    );
}

static int parse_args(
    int argc,
    char **argv,
    const char **input_csv,
    const char **out_prefix,
    float *threshold,
    uint64_t *baseline_samples,
    uint64_t *seed
) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input-csv") == 0 && i + 1 < argc) {
            *input_csv = argv[++i];
        } else if (strcmp(argv[i], "--out-prefix") == 0 && i + 1 < argc) {
            *out_prefix = argv[++i];
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            *threshold = strtof(argv[++i], NULL);
        } else if (strcmp(argv[i], "--baseline-samples") == 0 && i + 1 < argc) {
            *baseline_samples = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            *seed = strtoull(argv[++i], NULL, 0);
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    if (!*input_csv || !*out_prefix) {
        usage(argv[0]);
        return -1;
    }

    if (*threshold <= 0.0f || *threshold > 100.0f) {
        fprintf(stderr, "threshold must be 0..100\n");
        return -1;
    }

    if (*baseline_samples < 8 || *baseline_samples > 1000000) {
        fprintf(stderr, "baseline-samples must be 8..1000000\n");
        return -1;
    }

    return 0;
}

static int parse_csv_sample(const char *line, FV_SensorSample *s) {
    unsigned long long t_ns = 0;
    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float gx = 0.0f;
    float gy = 0.0f;
    float gz = 0.0f;

    if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') return 0;

    const int n = sscanf(
        line,
        " %llu , %f , %f , %f , %f , %f , %f , %f , %f , %f",
        &t_ns,
        &mx,
        &my,
        &mz,
        &ax,
        &ay,
        &az,
        &gx,
        &gy,
        &gz
    );

    if (n < 4) return 0;

    memset(s, 0, sizeof(*s));
    s->t_ns = (uint64_t)t_ns;
    s->mx = mx;
    s->my = my;
    s->mz = mz;

    if (n >= 7) {
        s->ax = ax;
        s->ay = ay;
        s->az = az;
    }

    if (n >= 10) {
        s->gx = gx;
        s->gy = gy;
        s->gz = gz;
    }

    return 1;
}

static int compute_baseline(
    const char *path,
    uint64_t baseline_samples,
    float *out_baseline,
    FV_CsvStats *stats
) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[4096];

    while (fgets(line, sizeof(line), fp)) {
        FV_SensorSample s;

        stats->rows_seen++;

        if (!parse_csv_sample(line, &s)) {
            stats->rows_skipped++;
            continue;
        }

        stats->rows_parsed++;

        if (stats->baseline_samples < baseline_samples) {
            stats->baseline_sum += (double)fv_mag3(s.mx, s.my, s.mz);
            stats->baseline_samples++;
        } else {
            break;
        }
    }

    fclose(fp);

    if (stats->baseline_samples == 0) {
        fprintf(stderr, "no usable baseline samples found in %s\n", path);
        return -1;
    }

    *out_baseline = (float)(stats->baseline_sum / (double)stats->baseline_samples);
    return 0;
}

static int make_out_paths(
    const char *prefix,
    char *blob_path,
    size_t blob_path_n,
    char *wav_path,
    size_t wav_path_n,
    char *jsonl_path,
    size_t jsonl_path_n
) {
    if (snprintf(blob_path, blob_path_n, "%s.fvblobs", prefix) >= (int)blob_path_n) return -1;
    if (snprintf(wav_path, wav_path_n, "%s.wav", prefix) >= (int)wav_path_n) return -1;
    if (snprintf(jsonl_path, jsonl_path_n, "%s.jsonl", prefix) >= (int)jsonl_path_n) return -1;
    return 0;
}

int main(int argc, char **argv) {
    const char *input_csv = NULL;
    const char *out_prefix = NULL;
    float threshold = 1.65f;
    uint64_t baseline_samples = 160;
    uint64_t seed = 0xC5C5C5C5ULL;

    if (parse_args(argc, argv, &input_csv, &out_prefix, &threshold, &baseline_samples, &seed) != 0) {
        return 2;
    }

    FV_CsvStats stats;
    memset(&stats, 0, sizeof(stats));

    float baseline = 0.0f;
    if (compute_baseline(input_csv, baseline_samples, &baseline, &stats) != 0) {
        return 1;
    }

    char blob_path[4096];
    char wav_path[4096];
    char jsonl_path[4096];

    if (make_out_paths(out_prefix, blob_path, sizeof(blob_path), wav_path, sizeof(wav_path), jsonl_path, sizeof(jsonl_path)) != 0) {
        fprintf(stderr, "output prefix paths too long\n");
        return 1;
    }

    FILE *in = fopen(input_csv, "rb");
    FILE *blob_fp = fopen(blob_path, "wb");
    FILE *log_fp = fopen(jsonl_path, "wb");

    if (!in || !blob_fp || !log_fp) {
        fprintf(stderr, "failed to open input or output files\n");
        return 1;
    }

    FV_WavWriter wav;
    if (fv_wav_begin(&wav, wav_path, FV_SAMPLE_RATE, FV_CHANNELS) != 0) {
        fprintf(stderr, "failed to open output WAV: %s\n", wav_path);
        return 1;
    }

    FV_Rng rng;
    fv_rng_seed(&rng, seed);

    FV_TriggerState trig;
    fv_trigger_init(&trig, baseline, threshold, 450000000ULL);

    const uint32_t blob_frames = FV_SAMPLE_RATE / 4u;
    int16_t *pcm = calloc(blob_frames, sizeof(*pcm));

    int16_t silence[FV_SAMPLE_RATE / 20u];
    memset(silence, 0, sizeof(silence));

    if (!pcm) {
        fprintf(stderr, "allocation failure\n");
        return 1;
    }

    fprintf(
        log_fp,
        "{\"type\":\"csv_session_start\",\"engine\":\"fieldvox-csvvox\","
        "\"input_csv\":\"%s\",\"sample_rate\":%u,\"threshold\":%.6f,"
        "\"baseline_samples\":%" PRIu64 ",\"baseline_mag\":%.6f,"
        "\"seed\":\"0x%016" PRIx64 "\"}\n",
        input_csv,
        FV_SAMPLE_RATE,
        threshold,
        stats.baseline_samples,
        baseline,
        seed
    );

    char line[4096];
    uint64_t rows_seen = 0;
    uint64_t rows_parsed = 0;
    uint64_t rows_skipped = 0;
    uint64_t event_count = 0;

    while (fgets(line, sizeof(line), in)) {
        rows_seen++;

        FV_SensorSample s;
        if (!parse_csv_sample(line, &s)) {
            rows_skipped++;
            continue;
        }

        rows_parsed++;

        float mag = 0.0f;
        float delta = 0.0f;

        if (fv_trigger_update(&trig, s, &mag, &delta)) {
            event_count++;

            const float motion =
                fabsf(s.ax) + fabsf(s.ay) + fabsf(s.az - 9.8f)
              + fabsf(s.gx) + fabsf(s.gy) + fabsf(s.gz);

            const float freq =
                90.0f + fmodf((mag * 23.0f) + (delta * 43.0f) + (motion * 31.0f) + (float)(event_count * 29u), 920.0f);

            const float noise_mix = fminf(0.88f, 0.18f + delta * 0.040f);
            const float echo_mix = fminf(0.75f, 0.18f + delta * 0.025f);
            const float dist = fminf(0.90f, 0.08f + delta * 0.030f);

            fv_make_blob_audio(pcm, blob_frames, freq, noise_mix, echo_mix, dist, &rng);

            FV_BlobMeta meta;
            memset(&meta, 0, sizeof(meta));

            meta.trigger_id = event_count;
            meta.timestamp_ns = s.t_ns;
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
                return 1;
            }

            if (fv_wav_write_i16(&wav, pcm, blob_frames) != 0) {
                fprintf(stderr, "wav write failed\n");
                return 1;
            }

            fv_wav_write_i16(&wav, silence, (uint32_t)(sizeof(silence) / sizeof(silence[0])));

            fprintf(
                log_fp,
                "{\"type\":\"trigger\",\"id\":%" PRIu64 ",\"row\":%" PRIu64 ","
                "\"t_ns\":%" PRIu64 ",\"mx\":%.6f,\"my\":%.6f,\"mz\":%.6f,"
                "\"mag\":%.6f,\"ema_delta\":%.6f,\"freq_hz\":%.3f,"
                "\"noise_mix\":%.3f,\"echo_mix\":%.3f,\"distortion\":%.3f,"
                "\"source_hash\":\"0x%016" PRIx64 "\",\"frames\":%u}\n",
                event_count,
                rows_seen,
                s.t_ns,
                s.mx,
                s.my,
                s.mz,
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

    fprintf(
        log_fp,
        "{\"type\":\"csv_session_end\",\"rows_seen\":%" PRIu64 ",\"rows_parsed\":%" PRIu64 ","
        "\"rows_skipped\":%" PRIu64 ",\"events\":%" PRIu64 "}\n",
        rows_seen,
        rows_parsed,
        rows_skipped,
        event_count
    );

    free(pcm);
    fclose(in);
    fclose(blob_fp);
    fclose(log_fp);

    if (fv_wav_finish(&wav) != 0) {
        fprintf(stderr, "wav finalize failed\n");
        return 1;
    }

    printf("FieldVox CSV mode complete.\n");
    printf("input_csv=%s\n", input_csv);
    printf("baseline_mag=%.6f\n", baseline);
    printf("rows_seen=%" PRIu64 "\n", rows_seen);
    printf("rows_parsed=%" PRIu64 "\n", rows_parsed);
    printf("rows_skipped=%" PRIu64 "\n", rows_skipped);
    printf("events=%" PRIu64 "\n", event_count);
    printf("outputs:\n");
    printf("  %s\n", blob_path);
    printf("  %s\n", wav_path);
    printf("  %s\n", jsonl_path);

    return event_count == 0 ? 3 : 0;
}
