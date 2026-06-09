// SPDX-License-Identifier: GPL-3.0-or-later
#include "fv_core.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t version;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t frame_count;
    uint32_t payload_bytes;
    uint64_t source_hash;
    uint64_t trigger_id;
    uint64_t timestamp_ns;
    float mag_value;
    float mag_delta;
    float freq_hz;
} FV_ReadBlobMeta;

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  %s --input FILE --summary\n"
        "  %s --input FILE --extract-dir DIR\n",
        argv0, argv0
    );
}

static int read_exact(FILE *fp, void *dst, size_t n) {
    return fread(dst, 1, n, fp) == n ? 0 : -1;
}

static int read_le16(FILE *fp, uint16_t *out) {
    unsigned char b[2];
    if (read_exact(fp, b, sizeof(b)) != 0) return -1;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8u);
    return 0;
}

static int read_le32(FILE *fp, uint32_t *out) {
    unsigned char b[4];
    if (read_exact(fp, b, sizeof(b)) != 0) return -1;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8u)
         | ((uint32_t)b[2] << 16u)
         | ((uint32_t)b[3] << 24u);
    return 0;
}

static int read_le64(FILE *fp, uint64_t *out) {
    unsigned char b[8];
    if (read_exact(fp, b, sizeof(b)) != 0) return -1;

    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        v |= ((uint64_t)b[i]) << (i * 8u);
    }

    *out = v;
    return 0;
}

static int read_le_float(FILE *fp, float *out) {
    uint32_t u = 0;
    if (read_le32(fp, &u) != 0) return -1;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int read_le_i16(FILE *fp, int16_t *out) {
    uint16_t u = 0;
    if (read_le16(fp, &u) != 0) return -1;
    *out = (int16_t)u;
    return 0;
}

static int read_blob_header(FILE *fp, FV_ReadBlobMeta *m) {
    char magic[4];

    if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic)) {
        if (feof(fp)) return 1;
        return -1;
    }

    if (memcmp(magic, "FVBL", 4) != 0) {
        fprintf(stderr, "bad blob magic at byte offset %ld\n", ftell(fp) - 4L);
        return -2;
    }

    if (read_le16(fp, &m->version) != 0) return -1;
    if (read_le16(fp, &m->channels) != 0) return -1;
    if (read_le32(fp, &m->sample_rate) != 0) return -1;
    if (read_le32(fp, &m->frame_count) != 0) return -1;
    if (read_le32(fp, &m->payload_bytes) != 0) return -1;
    if (read_le64(fp, &m->source_hash) != 0) return -1;
    if (read_le64(fp, &m->trigger_id) != 0) return -1;
    if (read_le64(fp, &m->timestamp_ns) != 0) return -1;
    if (read_le_float(fp, &m->mag_value) != 0) return -1;
    if (read_le_float(fp, &m->mag_delta) != 0) return -1;
    if (read_le_float(fp, &m->freq_hz) != 0) return -1;

    if (m->version != FV_BLOB_VERSION) {
        fprintf(stderr, "unsupported blob version: %u\n", (unsigned)m->version);
        return -2;
    }

    if (m->channels == 0 || m->channels > 8) {
        fprintf(stderr, "bad channel count: %u\n", (unsigned)m->channels);
        return -2;
    }

    if (m->sample_rate < 8000 || m->sample_rate > 384000) {
        fprintf(stderr, "bad sample rate: %u\n", m->sample_rate);
        return -2;
    }

    const uint64_t expected_payload = (uint64_t)m->frame_count * (uint64_t)m->channels * 2ULL;
    if (expected_payload != (uint64_t)m->payload_bytes) {
        fprintf(stderr,
            "payload mismatch: expected=%" PRIu64 " got=%" PRIu32 "\n",
            expected_payload,
            m->payload_bytes
        );
        return -2;
    }

    if (m->payload_bytes > 128u * 1024u * 1024u) {
        fprintf(stderr, "refusing huge blob payload: %" PRIu32 "\n", m->payload_bytes);
        return -2;
    }

    return 0;
}

static int skip_payload(FILE *fp, uint32_t payload_bytes) {
    if (fseek(fp, (long)payload_bytes, SEEK_CUR) == 0) return 0;

    unsigned char buf[8192];
    uint32_t left = payload_bytes;

    while (left > 0) {
        const size_t take = left < sizeof(buf) ? left : sizeof(buf);
        if (fread(buf, 1, take, fp) != take) return -1;
        left -= (uint32_t)take;
    }

    return 0;
}

static int extract_blob(FILE *fp, const FV_ReadBlobMeta *m, const char *dir) {
    char path[4096];

    snprintf(
        path,
        sizeof(path),
        "%s/blob-%04" PRIu64 "-t-%" PRIu64 ".wav",
        dir,
        m->trigger_id,
        m->timestamp_ns
    );

    int16_t *pcm = calloc((size_t)m->frame_count * (size_t)m->channels, sizeof(*pcm));
    if (!pcm) {
        fprintf(stderr, "allocation failure for blob %" PRIu64 "\n", m->trigger_id);
        return -1;
    }

    for (uint64_t i = 0; i < (uint64_t)m->frame_count * (uint64_t)m->channels; i++) {
        if (read_le_i16(fp, &pcm[i]) != 0) {
            free(pcm);
            return -1;
        }
    }

    FV_WavWriter wav;
    if (fv_wav_begin(&wav, path, m->sample_rate, m->channels) != 0) {
        fprintf(stderr, "failed to open extract WAV: %s\n", path);
        free(pcm);
        return -1;
    }

    if (fv_wav_write_i16(&wav, pcm, m->frame_count) != 0) {
        fprintf(stderr, "failed to write extract WAV: %s\n", path);
        free(pcm);
        return -1;
    }

    if (fv_wav_finish(&wav) != 0) {
        fprintf(stderr, "failed to finish extract WAV: %s\n", path);
        free(pcm);
        return -1;
    }

    free(pcm);
    return 0;
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *extract_dir = NULL;
    int summary = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--summary") == 0) {
            summary = 1;
        } else if (strcmp(argv[i], "--extract-dir") == 0 && i + 1 < argc) {
            extract_dir = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!input_path || (!summary && !extract_dir)) {
        usage(argv[0]);
        return 2;
    }

    if (extract_dir) {
        if (fv_mkdir_p(extract_dir) != 0) {
            fprintf(stderr, "failed to create extract dir %s: %s\n", extract_dir, strerror(errno));
            return 1;
        }
    }

    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        fprintf(stderr, "failed to open %s: %s\n", input_path, strerror(errno));
        return 1;
    }

    uint64_t count = 0;
    uint64_t payload_total = 0;
    float min_mag = INFINITY;
    float max_mag = -INFINITY;
    float min_delta = INFINITY;
    float max_delta = -INFINITY;
    float min_freq = INFINITY;
    float max_freq = -INFINITY;

    printf("FieldVox blobdump: %s\n", input_path);

    for (;;) {
        const long record_offset = ftell(fp);

        FV_ReadBlobMeta m;
        memset(&m, 0, sizeof(m));

        const int hr = read_blob_header(fp, &m);
        if (hr == 1) break;
        if (hr != 0) {
            fclose(fp);
            return 1;
        }

        count++;
        payload_total += m.payload_bytes;

        if (m.mag_value < min_mag) min_mag = m.mag_value;
        if (m.mag_value > max_mag) max_mag = m.mag_value;
        if (m.mag_delta < min_delta) min_delta = m.mag_delta;
        if (m.mag_delta > max_delta) max_delta = m.mag_delta;
        if (m.freq_hz < min_freq) min_freq = m.freq_hz;
        if (m.freq_hz > max_freq) max_freq = m.freq_hz;

        printf(
            "blob=%04" PRIu64 " offset=%ld trigger=%" PRIu64
            " t_ns=%" PRIu64 " sr=%u ch=%u frames=%u payload=%u"
            " mag=%.6f delta=%.6f freq=%.3f hash=0x%016" PRIx64 "\n",
            count,
            record_offset,
            m.trigger_id,
            m.timestamp_ns,
            m.sample_rate,
            (unsigned)m.channels,
            m.frame_count,
            m.payload_bytes,
            m.mag_value,
            m.mag_delta,
            m.freq_hz,
            m.source_hash
        );

        if (extract_dir) {
            if (extract_blob(fp, &m, extract_dir) != 0) {
                fclose(fp);
                return 1;
            }
        } else {
            if (skip_payload(fp, m.payload_bytes) != 0) {
                fprintf(stderr, "failed reading payload for blob %" PRIu64 "\n", m.trigger_id);
                fclose(fp);
                return 1;
            }
        }
    }

    fclose(fp);

    printf("\nsummary:\n");
    printf("  blob_count=%" PRIu64 "\n", count);
    printf("  payload_total=%" PRIu64 "\n", payload_total);

    if (count > 0) {
        printf("  mag_range=%.6f..%.6f\n", min_mag, max_mag);
        printf("  delta_range=%.6f..%.6f\n", min_delta, max_delta);
        printf("  freq_range=%.3f..%.3f\n", min_freq, max_freq);
    }

    if (extract_dir) {
        printf("  extracted_dir=%s\n", extract_dir);
    }

    return count == 0 ? 3 : 0;
}
