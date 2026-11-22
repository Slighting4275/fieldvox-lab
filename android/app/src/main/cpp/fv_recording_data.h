/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_RECORDING_DATA_H
#define FV_RECORDING_DATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

enum { FV_PLAYBACK_BINS = 256, FV_BOOKMARKS_MAX = 256 };
typedef struct {
    char path[512], name[256];
    int64_t bytes;
    time_t modified;
    bool video;
} FvRecording;
typedef struct { FvRecording *items; size_t count; } FvRecordingCatalog;
bool fv_recording_catalog_load(FvRecordingCatalog *catalog, const char *dir_a, const char *dir_b,
                               const char *exclude_a, const char *exclude_b);
void fv_recording_catalog_free(FvRecordingCatalog *catalog);

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint64_t data_offset, frames;
} FvWavInfo;
bool fv_wav_read_info(const char *path, FvWavInfo *info);
bool fv_wav_waveform(const char *path, float peaks[FV_PLAYBACK_BINS], const atomic_bool *cancel);
bool fv_wav_load_mono_48k(const char *path, int16_t **samples, size_t *frames);
void fv_wav_build_header(uint8_t header[static 44], uint32_t sample_rate,
                         uint16_t channels, uint32_t pcm_bytes);
bool fv_wav_write_fd(int fd, uint32_t sample_rate, uint16_t channels,
                     const int16_t *pcm, size_t frames);
bool fv_wav_write_file(const char *path, uint32_t sample_rate, uint16_t channels,
                       const int16_t *pcm, size_t frames);
int64_t fv_playback_clamp_seek(int64_t target_ms, int64_t duration_ms);
size_t fv_adts_sync_offset(const uint8_t *buf, size_t len);
bool fv_bookmark_add(int64_t marks[FV_BOOKMARKS_MAX], size_t *count, int64_t ms);
int64_t fv_bookmark_next(const int64_t *marks, size_t count, int64_t ms, bool forward);
bool fv_bookmarks_load(const char *recording, int64_t marks[FV_BOOKMARKS_MAX], size_t *count);
bool fv_bookmarks_save(const char *recording, const int64_t *marks, size_t count);
#endif
