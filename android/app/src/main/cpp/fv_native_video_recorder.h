/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FIELDVOX_NATIVE_VIDEO_RECORDER_H
#define FIELDVOX_NATIVE_VIDEO_RECORDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FV_VIDEO_VIEWFINDER,
    FV_VIDEO_CAMERA_ONLY,
} FvVideoFrameSource;

/**
 * Initialize and start MP4 video recording using AMediaCodec (H.264 + AAC) and AMediaMuxer.
 * @param output_mp4_path Absolute path to target .mp4 file.
 * @param width Target video width (must be multiple of 16, e.g. 720 or 1280).
 * @param height Target video height (must be multiple of 16, e.g. 1280 or 720).
 * @param sample_rate Audio sample rate (e.g. 44100).
 * @param channels Audio channels (1 = mono, 2 = stereo).
 * @param source Frame source to accept for this recording.
 * @return true if encoder started successfully.
 */
bool fv_video_recorder_start(const char *output_mp4_path, int width, int height, int sample_rate,
                             int channels, FvVideoFrameSource source);

/**
 * Feed PCM 16-bit audio samples into the AAC encoder.
 */
void fv_video_recorder_feed_audio(const int16_t *pcm, int frames);

/**
 * Feed an RGBA canvas frame to the H.264 encoder. Camera-only frames are center
 * cropped to preserve their aspect ratio. Both sources use BT.709 NV12.
 */
void fv_video_recorder_feed_frame(const uint32_t *rgba, int src_w, int src_h, int stride_px,
                                  FvVideoFrameSource source);

/**
 * Stop recording, drain encoders, finalize MP4 moov atom, and close file.
 * @param out_saved_path Optional buffer to receive final file path.
 * @param max_len Size of out_saved_path buffer.
 */
void fv_video_recorder_stop(char *out_saved_path, size_t max_len);

/**
 * Check if video recording is actively running.
 */
bool fv_video_recorder_is_active(void);

/**
 * Get elapsed recording time in milliseconds.
 */
uint32_t fv_video_recorder_get_duration_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* FIELDVOX_NATIVE_VIDEO_RECORDER_H */
