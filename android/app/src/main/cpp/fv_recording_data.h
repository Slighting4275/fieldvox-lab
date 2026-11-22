/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_RECORDING_DATA_H
#define FV_RECORDING_DATA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

enum { FV_PLAYBACK_BINS = 256, FV_TIMELINE_MAX = 256 };
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
/* These flag values are stored in version 2 of the .fvmarks sidecar. */
enum {
    FV_MARK_FRAGMENT = 1u << 0,
    FV_MARK_REM_POD = 1u << 1,
    FV_MARK_HIT = 1u << 2,
    FV_MARK_BOOKMARK = 1u << 3,
    FV_MARK_LEGACY = 1u << 4,
};
typedef struct {
    int64_t ms;
    unsigned kinds;
} FvTimelineMarker;
typedef struct {
    FvTimelineMarker markers[FV_TIMELINE_MAX];
    size_t count;
} FvTimeline;

bool fv_timeline_add(FvTimeline *timeline, int64_t ms, unsigned kinds);
const FvTimelineMarker *fv_timeline_step(const FvTimeline *timeline, int64_t ms, bool forward);
bool fv_timeline_load(const char *recording, FvTimeline *timeline);
bool fv_timeline_save(const char *recording, const FvTimeline *timeline);

typedef struct {
    FvTimeline timeline;
    int64_t last_fragment_ms, last_rem_pod_ms;
} FvMarkerRecorder;
void fv_marker_recorder_reset(FvMarkerRecorder *rec);
bool fv_marker_recorder_record(FvMarkerRecorder *rec, int64_t elapsed_ms, unsigned kinds);

void fv_waveform_accumulate_sample(float peaks[FV_PLAYBACK_BINS], int64_t pts_us,
                                   int64_t duration_us, float abs_level);
void fv_waveform_normalize(float peaks[FV_PLAYBACK_BINS]);
#endif
