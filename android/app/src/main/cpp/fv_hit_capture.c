#define _GNU_SOURCE
#include "fv_hit_capture.h"

#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 256 x 1024-frame blocks cover more than the three-second PCM window. */
#define FV_HIT_BLOCKS 256u
typedef struct { FvHitBlock state; uint32_t frames; } CaptureBlock;
static int16_t dry_ring[FV_HIT_FRAMES], processed_ring[FV_HIT_FRAMES];
static CaptureBlock blocks[FV_HIT_BLOCKS];
static uint32_t write_frame, valid_frames, write_block, valid_blocks;
static uint64_t end_sample;

void fv_hit_capture_reset(void) {
    write_frame = valid_frames = write_block = valid_blocks = 0;
    end_sample = 0;
}

void fv_hit_capture_push(const int16_t *dry, const int16_t *processed,
                         uint32_t frames, const FvHitBlock *block) {
    if (!dry || !processed || !block || !frames || frames > FV_HIT_FRAMES ||
        block->sample_start > UINT64_MAX - frames) return;
    /* Never bridge a pause or a discontinuity with misleading pre-roll. */
    if (valid_frames && block->sample_start != end_sample) fv_hit_capture_reset();
    for (uint32_t i = 0; i < frames; i++) {
        dry_ring[write_frame] = dry[i];
        processed_ring[write_frame] = processed[i];
        write_frame = (write_frame + 1u) % FV_HIT_FRAMES;
    }
    valid_frames = frames > FV_HIT_FRAMES - valid_frames ? FV_HIT_FRAMES : valid_frames + frames;
    blocks[write_block] = (CaptureBlock){*block, frames};
    write_block = (write_block + 1u) % FV_HIT_BLOCKS;
    if (valid_blocks < FV_HIT_BLOCKS) valid_blocks++;
    end_sample = block->sample_start + frames;
}

static FILE *new_file(const char *path) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        const int failure = errno;
        close(fd); unlink(path);
        errno = failure;
    }
    return f;
}

static int finish_file(FILE *f) {
    int failure = ferror(f) ? EIO : 0;
    if (!failure && fflush(f) != 0) failure = errno;
    if (!failure && fsync(fileno(f)) != 0) failure = errno;
    if (fclose(f) != 0 && !failure) failure = errno;
    return failure;
}

static void put_u32(uint8_t *dst, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) dst[i] = (uint8_t)(n >> (8u * i));
}

static bool write_wav(FILE *f, const int16_t *ring) {
    uint8_t header[44] = {0};
    const uint32_t bytes = valid_frames * 2u;
    memcpy(header, "RIFF", 4); put_u32(header + 4, bytes + 36u);
    memcpy(header + 8, "WAVEfmt ", 8); put_u32(header + 16, 16);
    header[20] = 1; header[22] = 1;
    put_u32(header + 24, FV_HIT_RATE); put_u32(header + 28, FV_HIT_RATE * 2u);
    header[32] = 2; header[34] = 16;
    memcpy(header + 36, "data", 4); put_u32(header + 40, bytes);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    const uint32_t start = (write_frame + FV_HIT_FRAMES - valid_frames) % FV_HIT_FRAMES;
    /* Encode explicitly as little endian rather than depending on the host. */
    uint8_t pcm[2048];
    for (uint32_t off = 0; ok && off < valid_frames;) {
        uint32_t count = valid_frames - off;
        if (count > 1024u) count = 1024u;
        for (uint32_t i = 0; i < count; i++) {
            const uint16_t sample = (uint16_t)ring[(start + off + i) % FV_HIT_FRAMES];
            pcm[i * 2u] = (uint8_t)sample;
            pcm[i * 2u + 1u] = (uint8_t)(sample >> 8u);
        }
        ok = fwrite(pcm, 2, count, f) == count;
        off += count;
    }
    return ok;
}

static void json_string(FILE *f, const char *s, size_t capacity) {
    fputc('"', f);
    for (size_t i = 0; i < capacity && s[i]; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 32u || c >= 127u) fprintf(f, "\\u%04x", (unsigned)c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void json_float(FILE *f, float v) {
    if (isfinite(v)) fprintf(f, "%.6f", (double)v);
    else fputs("null", f);
}

static const char *boolean(bool value) { return value ? "true" : "false"; }

static void write_json(FILE *f, uint64_t wall_time_ms, unsigned sequence,
                        const char *dry_name, const char *processed_name) {
    const uint64_t start_sample = end_sample - valid_frames;
    const uint32_t oldest = (write_block + FV_HIT_BLOCKS - valid_blocks) % FV_HIT_BLOCKS;
    const bool complete = valid_blocks && blocks[oldest].state.sample_start <= start_sample;
    fprintf(f, "{\"schema_version\":2,\"wall_time_ms\":%" PRIu64 ",\"sequence\":%u,"
               "\"sample_rate\":%u,\"frames\":%u,\"sample_start\":%" PRIu64 ","
               "\"sample_end\":%" PRIu64 ",\"timeline_complete\":%s,"
               "\"generated_words\":false,\"word_bank\":false,\"tts\":false,"
               "\"clean_stage\":\"pitched_tempo_adjusted_primary_fragment_plus_mic_before_echo_reverb_static_and_alert_tones\","
               "\"processed_stage\":\"mono_recording_bus_before_speaker_mute_and_pan\",\"clean_wav\":",
               wall_time_ms, sequence, FV_HIT_RATE, valid_frames, start_sample, end_sample, boolean(complete));
    json_string(f, dry_name, 96); fputs(",\"processed_wav\":", f);
    json_string(f, processed_name, 96); fputs(",\"timeline\":[", f);
    bool first = true;
    for (uint32_t i = 0; i < valid_blocks; i++) {
        const CaptureBlock *b = &blocks[(oldest + i) % FV_HIT_BLOCKS];
        const FvHitBlock *s = &b->state;
        const uint64_t block_end = s->sample_start + b->frames;
        if (block_end <= start_sample) continue;
        const uint64_t from = s->sample_start < start_sample ? start_sample : s->sample_start;
        if (!first) fputc(',', f);
        first = false;
        fprintf(f, "{\"offset_frames\":%" PRIu64 ",\"frames\":%" PRIu64 ","
                   "\"auto_enabled\":%s,\"microphone_enabled\":%s,\"speaker_muted\":%s,"
                   "\"station_a\":%d,\"station_b\":%d,\"audible_a\":%s,\"audible_b\":%s,\"trigger_a\":",
                   from - start_sample, block_end - from, boolean(s->auto_enabled), boolean(s->microphone),
                   boolean(s->muted), s->station_a, s->station_b, boolean(s->audible_a), boolean(s->audible_b));
        const char *reason = s->audible_a ? fv_gate_reason_name(s->reason_a) : "none";
        json_string(f, reason, 32); fputs(",\"trigger_b\":", f);
        reason = s->audible_b ? fv_gate_reason_name(s->reason_b) : "none";
        json_string(f, reason, 32);
        fprintf(f, ",\"echo_percent\":%d,\"reverb_percent\":%d,\"distortion_percent\":%d,"
                   "\"grain_ms\":%d,\"voice_duck_enabled\":%s,\"speaker_ducked\":%s",
                   s->echo_percent, s->reverb_percent, s->distortion_percent, s->grain_ms,
                   boolean(s->voice_duck_enabled), boolean(s->speaker_ducked));
        fputs(",\"label_a\":", f); json_string(f, s->label_a, sizeof(s->label_a));
        fputs(",\"label_b\":", f); json_string(f, s->label_b, sizeof(s->label_b));
        fputs(",\"url_a\":", f); json_string(f, s->url_a, sizeof(s->url_a));
        fputs(",\"url_b\":", f); json_string(f, s->url_b, sizeof(s->url_b));
        fputs(",\"emf_delta_ut\":", f); json_float(f, s->emf_delta);
        fputs(",\"emf_threshold_ut\":", f); json_float(f, s->emf_threshold);
        fprintf(f, ",\"effects_mask\":%u,\"pitch_semitones\":%d,\"cached_source\":%s,\"active_voices\":%u",
                s->effects, s->pitch_semitones, boolean(s->cached_source), s->active_voices);
        fputs(",\"pitch_factor\":", f); json_float(f, s->pitch_factor);
        fputs(",\"playback_speed\":", f); json_float(f, s->playback_speed);
        fputs(",\"seek_fraction\":", f); json_float(f, s->seek_fraction);
        fputc('}', f);
    }
    fputs("]}\n", f);
}

bool fv_hit_capture_save(const char *directory, uint64_t wall_time_ms, unsigned sequence,
                         char *processed_path, size_t path_capacity) {
    if (processed_path && path_capacity) processed_path[0] = '\0';
    if (!directory || !*directory || !valid_frames) return false;
    char names[3][96], paths[3][640], temporary[3][648];
    const char *suffixes[] = {"clean.wav", "preroll.wav"};
    for (unsigned i = 0; i < 3; i++) {
        if (i < 2) snprintf(names[i], sizeof(names[i]), "hit-%" PRIu64 "-%u-%s", wall_time_ms, sequence, suffixes[i]);
        else snprintf(names[i], sizeof(names[i]), "hit-%" PRIu64 "-%u.json", wall_time_ms, sequence);
        const int n = snprintf(paths[i], sizeof(paths[i]), "%s/%s", directory, names[i]);
        if (n < 0 || (size_t)n >= sizeof(paths[i])) return false;
        memcpy(temporary[i], paths[i], (size_t)n);
        memcpy(temporary[i] + n, ".part", 6);
    }
    if (processed_path && strlen(paths[1]) >= path_capacity) return false;
    unsigned created = 0;
    bool ok = true;
    int failure = 0;
    for (unsigned i = 0; i < 3 && ok; i++) {
        FILE *f = new_file(temporary[i]);
        if (!f) { failure = errno; ok = false; break; }
        if (i < 2) ok = write_wav(f, i == 0 ? dry_ring : processed_ring);
        else write_json(f, wall_time_ms, sequence, names[0], names[1]);
        if (!ok) failure = EIO;
        const int finish_failure = finish_file(f);
        if (finish_failure) { failure = finish_failure; ok = false; }
        /* API 30+ supports atomic no-overwrite rename. Android app SELinux
         * forbids hard links, so publication must use rename, not link. */
        if (ok && renameat2(AT_FDCWD, temporary[i], AT_FDCWD, paths[i], RENAME_NOREPLACE) != 0) {
            failure = errno; ok = false;
        }
        if (ok) created++;
        unlink(temporary[i]);
    }
    if (ok) {
        const int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd < 0) { failure = errno; ok = false; }
        else {
            if (fsync(directory_fd) != 0) { failure = errno; ok = false; }
            if (close(directory_fd) != 0 && ok) { failure = errno; ok = false; }
        }
    }
    if (!ok) {
        for (unsigned i = 0; i < created; i++) unlink(paths[i]);
        errno = failure;
        return false;
    }
    if (processed_path) snprintf(processed_path, path_capacity, "%s", paths[1]);
    return true;
}
