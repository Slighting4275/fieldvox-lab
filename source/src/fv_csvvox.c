/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_core.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FV_CSV_LINE_CAP 4096u
#define FV_SENSOR_ABS_LIMIT 1000000.0f

typedef struct {
    uint64_t rows_seen;
    uint64_t rows_parsed;
    uint64_t rows_skipped;
    uint64_t baseline_samples;
    double baseline_sum;
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
            if (parse_float_arg(argv[++i], threshold) != 0) {
                fprintf(stderr, "invalid threshold value\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--baseline-samples") == 0 && i + 1 < argc) {
            if (parse_u64_arg(argv[++i], 10, UINT64_MAX, baseline_samples) != 0) {
                fprintf(stderr, "invalid baseline-samples value\n");
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
    if (!line || !s) return 0;
    const char *first = line;
    while (*first == ' ' || *first == '\t' || *first == '\r') first++;
    if (*first == '\0' || *first == '\n' || *first == '#') return 0;

    const size_t line_len = strlen(first);
    if (line_len >= FV_CSV_LINE_CAP) return 0;

    char copy[FV_CSV_LINE_CAP];
    memcpy(copy, first, line_len + 1u);

    char *tokens[10];
    size_t token_count = 1;
    tokens[0] = copy;
    for (char *p = copy; *p; p++) {
        if (*p == ',') {
            if (token_count >= 10u) return 0;
            *p = '\0';
            tokens[token_count++] = p + 1;
        }
    }
    if (token_count != 4u && token_count != 7u && token_count != 10u) return 0;

    for (size_t i = 0; i < token_count; i++) {
        char *start = tokens[i];
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'
                || end[-1] == '\r' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (*start == '\0') return 0;
        tokens[i] = start;
    }

    uint64_t t_ns = 0;
    if (parse_u64_arg(tokens[0], 10, UINT64_MAX, &t_ns) != 0) return 0;

    float values[9] = {0};
    for (size_t i = 1; i < token_count; i++) {
        if (parse_float_arg(tokens[i], &values[i - 1u]) != 0
                || fabsf(values[i - 1u]) > FV_SENSOR_ABS_LIMIT) {
            return 0;
        }
    }

    const float mx = values[0];
    const float my = values[1];
    const float mz = values[2];
    if (!isfinite(fv_mag3(mx, my, mz))) return 0;

    memset(s, 0, sizeof(*s));
    s->t_ns = t_ns;
    s->mx = mx;
    s->my = my;
    s->mz = mz;

    if (token_count >= 7u) {
        s->ax = values[3];
        s->ay = values[4];
        s->az = values[5];
    }

    if (token_count == 10u) {
        s->gx = values[6];
        s->gy = values[7];
        s->gz = values[8];
    }

    return 1;
}

static int read_csv_line(FILE *fp, char *line, size_t cap, int *truncated) {
    if (!fp || !line || cap < 2u || cap > (size_t)INT_MAX || !truncated) return -1;
    if (!fgets(line, (int)cap, fp)) return ferror(fp) ? -1 : 0;
    *truncated = 0;
    if (strchr(line, '\n') == NULL && !feof(fp)) {
        int c;
        *truncated = 1;
        while ((c = fgetc(fp)) != '\n' && c != EOF) {
        }
        if (c == EOF && ferror(fp)) return -1;
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

    char line[FV_CSV_LINE_CAP];

    while (stats->baseline_samples < baseline_samples) {
        int truncated = 0;
        const int read_result = read_csv_line(fp, line, sizeof(line), &truncated);
        if (read_result < 0) {
            fprintf(stderr, "failed reading %s: %s\n", path, strerror(errno));
            fclose(fp);
            return -1;
        }
        if (read_result == 0) break;
        FV_SensorSample s;

        stats->rows_seen++;

        if (truncated || !parse_csv_sample(line, &s)) {
            stats->rows_skipped++;
            continue;
        }

        stats->rows_parsed++;
        stats->baseline_sum += (double)fv_mag3(s.mx, s.my, s.mz);
        stats->baseline_samples++;
    }

    fclose(fp);

    if (stats->baseline_samples == 0) {
        fprintf(stderr, "no usable baseline samples found in %s\n", path);
        return -1;
    }

    *out_baseline = (float)(stats->baseline_sum / (double)stats->baseline_samples);
    if (!isfinite(*out_baseline)) {
        fprintf(stderr, "baseline magnitude is not finite\n");
        return -1;
    }
    return 0;
}

static int same_existing_file(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0
        && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
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
    int n = snprintf(blob_path, blob_path_n, "%s.fvblobs", prefix);
    if (n < 0 || (size_t)n >= blob_path_n) return -1;
    n = snprintf(wav_path, wav_path_n, "%s.wav", prefix);
    if (n < 0 || (size_t)n >= wav_path_n) return -1;
    n = snprintf(jsonl_path, jsonl_path_n, "%s.jsonl", prefix);
    if (n < 0 || (size_t)n >= jsonl_path_n) return -1;
    return 0;
}

static int write_json_string(FILE *fp, const char *text) {
    if (fputc('"', fp) == EOF) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '"': if (fputs("\\\"", fp) == EOF) return -1; break;
            case '\\': if (fputs("\\\\", fp) == EOF) return -1; break;
            case '\b': if (fputs("\\b", fp) == EOF) return -1; break;
            case '\f': if (fputs("\\f", fp) == EOF) return -1; break;
            case '\n': if (fputs("\\n", fp) == EOF) return -1; break;
            case '\r': if (fputs("\\r", fp) == EOF) return -1; break;
            case '\t': if (fputs("\\t", fp) == EOF) return -1; break;
            default:
                if (*p < 0x20u) {
                    if (fprintf(fp, "\\u%04x", (unsigned)*p) < 0) return -1;
                } else if (fputc(*p, fp) == EOF) {
                    return -1;
                }
        }
    }
    return fputc('"', fp) == EOF ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *input_csv = NULL;
    const char *out_prefix = NULL;
    float threshold = 1.65f;
    uint64_t baseline_samples = 160;
    uint64_t seed = 0xC5C5C5C5ULL;
    FILE *in = NULL;
    FILE *blob_fp = NULL;
    FILE *log_fp = NULL;
    FV_WavWriter wav;
    int16_t *pcm = NULL;
    uint64_t rows_seen = 0;
    uint64_t rows_parsed = 0;
    uint64_t rows_skipped = 0;
    uint64_t event_count = 0;
    int wav_open = 0;
    int result = 1;

    memset(&wav, 0, sizeof(wav));

    if (parse_args(argc, argv, &input_csv, &out_prefix, &threshold, &baseline_samples, &seed) != 0) {
        return 2;
    }

    char blob_path[4096];
    char wav_path[4096];
    char jsonl_path[4096];

    if (make_out_paths(out_prefix, blob_path, sizeof(blob_path), wav_path, sizeof(wav_path), jsonl_path, sizeof(jsonl_path)) != 0) {
        fprintf(stderr, "output prefix paths too long\n");
        return 1;
    }
    if (strcmp(input_csv, blob_path) == 0
            || strcmp(input_csv, wav_path) == 0
            || strcmp(input_csv, jsonl_path) == 0
            || same_existing_file(input_csv, blob_path)
            || same_existing_file(input_csv, wav_path)
            || same_existing_file(input_csv, jsonl_path)) {
        fprintf(stderr, "input CSV must not be one of the output paths\n");
        return 1;
    }
    if (same_existing_file(blob_path, wav_path)
            || same_existing_file(blob_path, jsonl_path)
            || same_existing_file(wav_path, jsonl_path)) {
        fprintf(stderr, "output paths must refer to different files\n");
        return 1;
    }

    FV_CsvStats stats;
    memset(&stats, 0, sizeof(stats));

    float baseline = 0.0f;
    if (compute_baseline(input_csv, baseline_samples, &baseline, &stats) != 0) {
        return 1;
    }

    in = fopen(input_csv, "rb");
    blob_fp = fopen(blob_path, "wb");
    log_fp = fopen(jsonl_path, "wb");

    if (!in || !blob_fp || !log_fp) {
        fprintf(stderr, "failed to open input or output files\n");
        goto cleanup;
    }

    if (fv_wav_begin(&wav, wav_path, FV_SAMPLE_RATE, FV_CHANNELS) != 0) {
        fprintf(stderr, "failed to open output WAV: %s\n", wav_path);
        goto cleanup;
    }
    wav_open = 1;

    FV_Rng rng;
    fv_rng_seed(&rng, seed);

    FV_TriggerState trig;
    fv_trigger_init(&trig, baseline, threshold, 450000000ULL);

    const uint32_t blob_frames = FV_SAMPLE_RATE / 4u;
    pcm = calloc(blob_frames, sizeof(*pcm));

    int16_t silence[FV_SAMPLE_RATE / 20u];
    memset(silence, 0, sizeof(silence));

    if (!pcm) {
        fprintf(stderr, "allocation failure\n");
        goto cleanup;
    }

    if (fprintf(
        log_fp,
        "{\"type\":\"csv_session_start\",\"engine\":\"fieldvox-csvvox\",\"input_csv\":"
    ) < 0 || write_json_string(log_fp, input_csv) != 0 || fprintf(
        log_fp,
        ",\"sample_rate\":%u,\"threshold\":%.6f,"
        "\"baseline_samples\":%" PRIu64 ",\"baseline_mag\":%.6f,"
        "\"seed\":\"0x%016" PRIx64 "\"}\n",
        FV_SAMPLE_RATE,
        threshold,
        stats.baseline_samples,
        baseline,
        seed
    ) < 0) {
        fprintf(stderr, "failed to write session log header\n");
        goto cleanup;
    }

    char line[FV_CSV_LINE_CAP];
    for (;;) {
        int truncated = 0;
        const int read_result = read_csv_line(in, line, sizeof(line), &truncated);
        if (read_result < 0) {
            fprintf(stderr, "CSV read failed: %s\n", strerror(errno));
            goto cleanup;
        }
        if (read_result == 0) break;
        rows_seen++;

        FV_SensorSample s;
        if (truncated || !parse_csv_sample(line, &s)) {
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

            if (fprintf(
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
            ) < 0) {
                fprintf(stderr, "failed to write trigger log\n");
                goto cleanup;
            }
        }
    }

    if (fprintf(
        log_fp,
        "{\"type\":\"csv_session_end\",\"rows_seen\":%" PRIu64 ",\"rows_parsed\":%" PRIu64 ","
        "\"rows_skipped\":%" PRIu64 ",\"events\":%" PRIu64 "}\n",
        rows_seen,
        rows_parsed,
        rows_skipped,
        event_count
    ) < 0) {
        fprintf(stderr, "failed to write session log footer\n");
        goto cleanup;
    }

    result = event_count == 0 ? 3 : 0;

cleanup:
    free(pcm);
    if (in && fclose(in) != 0) result = 1;
    if (blob_fp && fclose(blob_fp) != 0) result = 1;
    if (log_fp && fclose(log_fp) != 0) result = 1;
    if (wav_open && fv_wav_finish(&wav) != 0) {
        fprintf(stderr, "wav finalize failed\n");
        result = 1;
    }

    if (result != 0 && result != 3) return result;

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

    return result;
}
