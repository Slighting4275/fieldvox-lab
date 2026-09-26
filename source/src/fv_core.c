#include "fv_core.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

float fv_mag3(float x, float y, float z) {
    return hypotf(hypotf(x, y), z);
}

void fv_trigger_init(FV_TriggerState *st, float baseline_mag, float threshold, uint64_t cooldown_ns) {
    memset(st, 0, sizeof(*st));
    st->baseline_mag = baseline_mag;
    st->ema_mag = baseline_mag;
    st->ema_delta = 0.0f;
    st->trigger_threshold = threshold;
    st->cooldown_ns = cooldown_ns;
    st->last_trigger_ns = 0;
    st->trigger_count = 0;
}

int fv_trigger_update(FV_TriggerState *st, FV_SensorSample sample, float *out_mag, float *out_delta) {
    if (!st || !isfinite(sample.mx) || !isfinite(sample.my) || !isfinite(sample.mz)
            || !isfinite(st->baseline_mag) || !isfinite(st->trigger_threshold)) {
        return 0;
    }
    const float mag = fv_mag3(sample.mx, sample.my, sample.mz);
    const float delta = fabsf(mag - st->baseline_mag);

    if (!isfinite(mag) || !isfinite(delta)) {
        return 0;
    }

    st->ema_mag = (st->ema_mag * 0.970f) + (mag * 0.030f);
    st->ema_delta = (st->ema_delta * 0.900f) + (delta * 0.100f);

    if (out_mag) *out_mag = mag;
    if (out_delta) *out_delta = st->ema_delta;

    if (sample.t_ns < st->last_trigger_ns
            || sample.t_ns - st->last_trigger_ns < st->cooldown_ns) {
        return 0;
    }

    if (st->ema_delta >= st->trigger_threshold) {
        st->last_trigger_ns = sample.t_ns;
        st->trigger_count += 1;
        return 1;
    }

    return 0;
}

void fv_rng_seed(FV_Rng *rng, uint64_t seed) {
    rng->state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint32_t fv_rng_u32(FV_Rng *rng) {
    uint64_t x = rng->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng->state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

float fv_rng_float_signed(FV_Rng *rng) {
    const uint32_t v = fv_rng_u32(rng);
    const float f = (float)v / 4294967295.0f;
    return (f * 2.0f) - 1.0f;
}

int fv_mkdir_p(const char *path) {
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return -1;
}

static void write_le16(FILE *fp, uint16_t v) {
    fputc((int)(v & 0xffu), fp);
    fputc((int)((v >> 8) & 0xffu), fp);
}

static void write_le32(FILE *fp, uint32_t v) {
    fputc((int)(v & 0xffu), fp);
    fputc((int)((v >> 8) & 0xffu), fp);
    fputc((int)((v >> 16) & 0xffu), fp);
    fputc((int)((v >> 24) & 0xffu), fp);
}

static void write_le64(FILE *fp, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) {
        fputc((int)((v >> (i * 8u)) & 0xffu), fp);
    }
}

static void write_le_float(FILE *fp, float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    write_le32(fp, u);
}

int fv_wav_begin(FV_WavWriter *w, const char *path, uint32_t sample_rate, uint16_t channels) {
    if (!w || !path || sample_rate == 0 || channels == 0) return -1;
    const uint64_t block_align = (uint64_t)channels * 2ULL;
    const uint64_t byte_rate = (uint64_t)sample_rate * block_align;
    if (block_align > UINT16_MAX || byte_rate > UINT32_MAX) return -1;
    memset(w, 0, sizeof(*w));
    w->fp = fopen(path, "wb");
    if (!w->fp) return -1;
    w->sample_rate = sample_rate;
    w->channels = channels;
    w->frames_written = 0;

    fwrite("RIFF", 1, 4, w->fp);
    write_le32(w->fp, 0);
    fwrite("WAVE", 1, 4, w->fp);
    fwrite("fmt ", 1, 4, w->fp);
    write_le32(w->fp, 16);
    write_le16(w->fp, 1);
    write_le16(w->fp, channels);
    write_le32(w->fp, sample_rate);
    write_le32(w->fp, (uint32_t)byte_rate);
    write_le16(w->fp, (uint16_t)block_align);
    write_le16(w->fp, 16);
    fwrite("data", 1, 4, w->fp);
    write_le32(w->fp, 0);
    if (ferror(w->fp)) {
        fclose(w->fp);
        w->fp = NULL;
        return -1;
    }
    return 0;
}

int fv_wav_write_i16(FV_WavWriter *w, const int16_t *samples, uint32_t frames) {
    if (!w || !w->fp || (!samples && frames > 0)) return -1;
    const uint64_t new_frames = w->frames_written + (uint64_t)frames;
    if (new_frames < w->frames_written
            || new_frames * (uint64_t)w->channels * 2ULL > UINT32_MAX - 36ULL) {
        return -1;
    }
    for (uint32_t i = 0; i < frames * w->channels; i++) {
        write_le16(w->fp, (uint16_t)samples[i]);
    }
    w->frames_written = new_frames;
    return ferror(w->fp) ? -1 : 0;
}

int fv_wav_finish(FV_WavWriter *w) {
    if (!w || !w->fp) return -1;
    const uint32_t data_bytes = (uint32_t)(w->frames_written * w->channels * 2ULL);
    const uint32_t riff_bytes = 36u + data_bytes;
    int rc = 0;
    if (fseek(w->fp, 4, SEEK_SET) != 0) {
        rc = -1;
    } else {
        write_le32(w->fp, riff_bytes);
    }
    if (rc == 0 && fseek(w->fp, 40, SEEK_SET) != 0) {
        rc = -1;
    } else if (rc == 0) {
        write_le32(w->fp, data_bytes);
    }
    if (ferror(w->fp)) rc = -1;
    if (fclose(w->fp) != 0) rc = -1;
    w->fp = NULL;
    return rc;
}

void fv_make_blob_audio(
    int16_t *dst,
    uint32_t frames,
    float freq_hz,
    float noise_mix,
    float echo_mix,
    float distortion,
    FV_Rng *rng
) {
    if (!dst || !rng || frames == 0 || !isfinite(freq_hz)
            || !isfinite(noise_mix) || !isfinite(echo_mix) || !isfinite(distortion)) {
        if (dst && frames > 0) {
            memset(dst, 0, frames * sizeof(*dst));
        }
        return;
    }

    const uint32_t delay = (uint32_t)((float)FV_SAMPLE_RATE * 0.120f);
    float *echo = calloc(delay ? delay : 1u, sizeof(float));
    if (!echo) {
        memset(dst, 0, frames * sizeof(*dst));
        return;
    }

    float phase = 0.0f;
    const float phase_step = (float)((2.0 * FV_PI * (double)freq_hz) / (double)FV_SAMPLE_RATE);

    for (uint32_t i = 0; i < frames; i++) {
        const float env_a = fminf(1.0f, (float)i / (float)(FV_SAMPLE_RATE / 80u));
        const float env_r = fminf(1.0f, (float)(frames - i) / (float)(FV_SAMPLE_RATE / 30u));
        const float env = fminf(env_a, env_r);

        const float tone = sinf(phase) * 0.55f + sinf(phase * 2.01f) * 0.12f;
        const float noise = fv_rng_float_signed(rng) * noise_mix;
        const uint32_t di = i % delay;

        float y = ((tone * (1.0f - noise_mix)) + noise) * env;
        y += echo[di] * echo_mix;
        echo[di] = y * 0.45f;
        y = tanhf(y * (1.0f + distortion * 8.0f));

        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;

        dst[i] = (int16_t)lrintf(y * 30000.0f);

        phase += phase_step;
        if (phase > (float)(2.0 * FV_PI)) phase -= (float)(2.0 * FV_PI);
    }

    free(echo);
}

int fv_blob_write(FILE *fp, const FV_BlobMeta *meta, const int16_t *pcm) {
    if (!fp || !meta || !pcm || meta->channels == 0) return -1;
    const uint64_t sample_count = (uint64_t)meta->frame_count * meta->channels;
    if (sample_count > UINT32_MAX / 2u
            || meta->payload_bytes != sample_count * 2u) {
        return -1;
    }
    fwrite("FVBL", 1, 4, fp);
    write_le16(fp, FV_BLOB_VERSION);
    write_le16(fp, meta->channels);
    write_le32(fp, meta->sample_rate);
    write_le32(fp, meta->frame_count);
    write_le32(fp, meta->payload_bytes);
    write_le64(fp, meta->source_hash);
    write_le64(fp, meta->trigger_id);
    write_le64(fp, meta->timestamp_ns);
    write_le_float(fp, meta->mag_value);
    write_le_float(fp, meta->mag_delta);
    write_le_float(fp, meta->freq_hz);

    for (uint64_t i = 0; i < sample_count; i++) {
        write_le16(fp, (uint16_t)pcm[i]);
    }

    return ferror(fp) ? -1 : 0;
}

uint64_t fv_fnv1a64(const void *data, size_t len, uint64_t seed) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = seed ? seed : 1469598103934665603ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }

    return h;
}
