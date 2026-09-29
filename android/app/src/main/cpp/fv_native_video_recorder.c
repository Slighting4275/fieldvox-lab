#include "fv_native_video_recorder.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define TAG "FieldVoxVideoRec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_PENDING_PACKETS 128
#define MAX_PACKET_SIZE (512 * 1024)

typedef struct {
    uint8_t *data;
    size_t size;
    AMediaCodecBufferInfo info;
    bool is_video;
} PendingPacket;

typedef struct {
    pthread_mutex_t mutex;
    bool active;
    bool muxer_started;
    int fd;
    char path[512];

    int width;
    int height;
    int sample_rate;
    int channels;

    AMediaCodec *video_codec;
    AMediaCodec *audio_codec;
    AMediaMuxer *muxer;

    ssize_t video_track;
    ssize_t audio_track;

    uint64_t start_time_ms;
    uint64_t last_frame_time_ms;
    uint64_t video_frame_count;
    uint64_t audio_sample_count;

    PendingPacket pending[MAX_PENDING_PACKETS];
    int pending_count;

    /* Scaled conversion staging buffer and encoder geometry */
    int input_stride;
    int input_slice_height;
    uint8_t *nv12_buffer;
    size_t nv12_size;
} VideoRecorderState;

static VideoRecorderState s_rec = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .active = false,
    .fd = -1,
    .video_track = -1,
    .audio_track = -1,
};

static uint64_t current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/*
 * Convert native RGBA_8888 buffer to NV12 format with BT.709 colorimetry.
 * Bilinearly scales src_w x src_h into dst_w x dst_h with hardware encoder
 * stride and slice-height alignment.
 * Note: On Android little-endian RGBA_8888, pixel bits are:
 *   [0..7] = Red, [8..15] = Green, [16..23] = Blue, [24..31] = Alpha.
 */
static void argb_to_nv12_bt709(const uint32_t *src, int src_w, int src_h, int src_stride_px,
                              uint8_t *dst_nv12, int dst_w, int dst_h, int dst_stride, int dst_slice_h) {
    if (dst_stride < dst_w) dst_stride = dst_w;
    if (dst_slice_h < dst_h) dst_slice_h = dst_h;

    uint8_t *y_plane = dst_nv12;
    uint8_t *uv_plane = dst_nv12 + (size_t)dst_stride * (size_t)dst_slice_h;

    /* Initialize margins to standard studio black (Y=16, UV=128) if padded */
    if (dst_stride > dst_w || dst_slice_h > dst_h) {
        memset(y_plane, 16, (size_t)dst_stride * (size_t)dst_slice_h);
        memset(uv_plane, 128, (size_t)dst_stride * (size_t)(dst_slice_h / 2));
    }

    const float sx_scale = (float)src_w / (float)dst_w;
    const float sy_scale = (float)src_h / (float)dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        const int sy = (int)((float)dy * sy_scale);
        const int sy_clamped = sy < 0 ? 0 : (sy < src_h ? sy : src_h - 1);
        const uint32_t *src_row = src + (size_t)sy_clamped * (size_t)src_stride_px;
        uint8_t *y_row = y_plane + (size_t)dy * (size_t)dst_stride;

        for (int dx = 0; dx < dst_w; dx++) {
            const int sx = (int)((float)dx * sx_scale);
            const int sx_clamped = sx < 0 ? 0 : (sx < src_w ? sx : src_w - 1);
            const uint32_t pixel = src_row[sx_clamped];

            const int r = (int)(pixel & 0xFF);
            const int g = (int)((pixel >> 8) & 0xFF);
            const int b = (int)((pixel >> 16) & 0xFF);

            /* BT.709 Studio Swing coefficients */
            const int y = ((54 * r + 183 * g + 18 * b + 128) >> 8) + 16;
            y_row[dx] = clamp_u8(y);

            /* Subsample UV on even (dx, dy) */
            if ((dy & 1) == 0 && (dx & 1) == 0) {
                const int cb = ((-29 * r - 99 * g + 128 * b + 128) >> 8) + 128;
                const int cr = ((128 * r - 116 * g - 12 * b + 128) >> 8) + 128;

                const int uv_row_idx = dy / 2;
                uint8_t *uv_row = uv_plane + (size_t)uv_row_idx * (size_t)dst_stride;
                uv_row[dx] = clamp_u8(cb);
                uv_row[dx + 1] = clamp_u8(cr);
            }
        }
    }
}

static void flush_pending_packets_locked(void) {
    if (!s_rec.muxer_started || !s_rec.muxer) return;
    for (int i = 0; i < s_rec.pending_count; i++) {
        PendingPacket *p = &s_rec.pending[i];
        const ssize_t track = p->is_video ? s_rec.video_track : s_rec.audio_track;
        if (track >= 0 && p->data && p->size > 0) {
            AMediaMuxer_writeSampleData(s_rec.muxer, (size_t)track, p->data, &p->info);
        }
        if (p->data) {
            free(p->data);
            p->data = NULL;
        }
    }
    s_rec.pending_count = 0;
}

static void queue_or_write_packet_locked(bool is_video, const uint8_t *data, const AMediaCodecBufferInfo *info) {
    if (!data || info->size <= 0) return;

    if (s_rec.muxer_started && s_rec.muxer) {
        flush_pending_packets_locked();
        const ssize_t track = is_video ? s_rec.video_track : s_rec.audio_track;
        if (track >= 0) {
            /* AMediaMuxer_writeSampleData adds info->offset internally to data */
            AMediaMuxer_writeSampleData(s_rec.muxer, (size_t)track, data, info);
        }
        return;
    }

    if (s_rec.pending_count < MAX_PENDING_PACKETS) {
        PendingPacket *p = &s_rec.pending[s_rec.pending_count];
        p->data = (uint8_t *)malloc((size_t)info->size);
        if (p->data) {
            memcpy(p->data, data + info->offset, (size_t)info->size);
            p->is_video = is_video;
            p->info = *info;
            /* p->data starts at offset 0, so reset info.offset to prevent double addition */
            p->info.offset = 0;
            p->size = (size_t)info->size;
            s_rec.pending_count++;
        }
    }
}

static void drain_encoder_locked(bool is_video, int64_t timeout_us, bool wait_for_eos) {
    AMediaCodec *codec = is_video ? s_rec.video_codec : s_rec.audio_codec;
    if (!codec || !s_rec.muxer) return;

    AMediaCodecBufferInfo info;
    int iterations = 0;
    while (iterations++ < 100) {
        const ssize_t status = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout_us);
        if (status == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(codec);
            if (is_video) {
                s_rec.video_track = AMediaMuxer_addTrack(s_rec.muxer, fmt);
                LOGI("Video track added to muxer: index %zd", s_rec.video_track);
            } else {
                s_rec.audio_track = AMediaMuxer_addTrack(s_rec.muxer, fmt);
                LOGI("Audio track added to muxer: index %zd", s_rec.audio_track);
            }
            AMediaFormat_delete(fmt);

            if (s_rec.video_track >= 0 && s_rec.audio_track >= 0 && !s_rec.muxer_started) {
                if (AMediaMuxer_start(s_rec.muxer) == AMEDIA_OK) {
                    s_rec.muxer_started = true;
                    LOGI("AMediaMuxer started successfully");
                    flush_pending_packets_locked();
                } else {
                    LOGE("AMediaMuxer_start failed");
                }
            }
        } else if (status >= 0) {
            size_t out_size = 0;
            const uint8_t *out = AMediaCodec_getOutputBuffer(codec, (size_t)status, &out_size);
            if (out && info.size > 0) {
                queue_or_write_packet_locked(is_video, out, &info);
            }
            AMediaCodec_releaseOutputBuffer(codec, (size_t)status, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                break;
            }
        } else {
            if (!wait_for_eos) break;
            if (status == AMEDIACODEC_INFO_TRY_AGAIN_LATER) break;
        }
    }
}

bool fv_video_recorder_start(const char *output_mp4_path, int width, int height, int sample_rate, int channels) {
    if (!output_mp4_path || width <= 0 || height <= 0 || sample_rate <= 0) return false;

    pthread_mutex_lock(&s_rec.mutex);
    if (s_rec.active) {
        pthread_mutex_unlock(&s_rec.mutex);
        return true;
    }

    width = (width / 16) * 16;
    height = (height / 16) * 16;
    if (width < 320) width = 320;
    if (height < 240) height = 240;

    const int fd = open(output_mp4_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        LOGE("Cannot open MP4 destination '%s': %s", output_mp4_path, strerror(errno));
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }

    AMediaMuxer *muxer = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer) {
        close(fd);
        LOGE("AMediaMuxer_new failed");
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }

    /* Video Encoder (H.264 AVC) */
    AMediaCodec *vcodec = AMediaCodec_createEncoderByType("video/avc");
    if (!vcodec) {
        AMediaMuxer_delete(muxer);
        close(fd);
        LOGE("Failed to create video/avc encoder");
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }

    AMediaFormat *vfmt = AMediaFormat_new();
    AMediaFormat_setString(vfmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); /* COLOR_FormatYUV420SemiPlanar (NV12) */
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_BIT_RATE, 3500000); /* 3.5 Mbps */
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
    AMediaFormat_setInt32(vfmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    AMediaFormat_setInt32(vfmt, "color-standard", 1); /* BT.709 */
    AMediaFormat_setInt32(vfmt, "color-range", 2);    /* Limited studio range */
    AMediaFormat_setInt32(vfmt, "color-transfer", 3); /* SDR */

    if (AMediaCodec_configure(vcodec, vfmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        AMediaCodec_start(vcodec) != AMEDIA_OK) {
        AMediaFormat_delete(vfmt);
        AMediaCodec_delete(vcodec);
        AMediaMuxer_delete(muxer);
        close(fd);
        LOGE("Failed to configure/start video encoder");
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }
    AMediaFormat_delete(vfmt);

    /* Audio Encoder (AAC-LC) */
    AMediaCodec *acodec = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!acodec) {
        AMediaCodec_stop(vcodec);
        AMediaCodec_delete(vcodec);
        AMediaMuxer_delete(muxer);
        close(fd);
        LOGE("Failed to create audio/mp4a-latm encoder");
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }

    AMediaFormat *afmt = AMediaFormat_new();
    AMediaFormat_setString(afmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, sample_rate);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_BIT_RATE, 128000);
    AMediaFormat_setInt32(afmt, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); /* LC */

    if (AMediaCodec_configure(acodec, afmt, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        AMediaCodec_start(acodec) != AMEDIA_OK) {
        AMediaFormat_delete(afmt);
        AMediaCodec_delete(acodec);
        AMediaCodec_stop(vcodec);
        AMediaCodec_delete(vcodec);
        AMediaMuxer_delete(muxer);
        close(fd);
        LOGE("Failed to configure/start audio encoder");
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }
    AMediaFormat_delete(afmt);

    const size_t nv12_bytes = (size_t)width * (size_t)height * 3 / 2;
    uint8_t *nv12 = (uint8_t *)malloc(nv12_bytes);
    if (!nv12) {
        AMediaCodec_stop(acodec);
        AMediaCodec_delete(acodec);
        AMediaCodec_stop(vcodec);
        AMediaCodec_delete(vcodec);
        AMediaMuxer_delete(muxer);
        close(fd);
        pthread_mutex_unlock(&s_rec.mutex);
        return false;
    }

    s_rec.active = true;
    s_rec.muxer_started = false;
    s_rec.fd = fd;
    snprintf(s_rec.path, sizeof(s_rec.path), "%s", output_mp4_path);
    s_rec.width = width;
    s_rec.height = height;
    s_rec.sample_rate = sample_rate;
    s_rec.channels = channels;
    s_rec.video_codec = vcodec;
    s_rec.audio_codec = acodec;
    s_rec.muxer = muxer;
    s_rec.video_track = -1;
    s_rec.audio_track = -1;

    int input_stride = width;
    int input_slice_h = height;
    AMediaFormat *in_fmt = AMediaCodec_getInputFormat(vcodec);
    if (in_fmt) {
        if (!AMediaFormat_getInt32(in_fmt, AMEDIAFORMAT_KEY_STRIDE, &input_stride) || input_stride <= 0) {
            AMediaFormat_getInt32(in_fmt, "stride", &input_stride);
        }
        if (!AMediaFormat_getInt32(in_fmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &input_slice_h) || input_slice_h <= 0) {
            AMediaFormat_getInt32(in_fmt, "slice-height", &input_slice_h);
        }
        const char *fmt_str = AMediaFormat_toString(in_fmt);
        LOGI("Video encoder initial input format: %s (stride=%d, slice_h=%d)",
             fmt_str ? fmt_str : "null", input_stride, input_slice_h);
        AMediaFormat_delete(in_fmt);
    }
    s_rec.input_stride = input_stride;
    s_rec.input_slice_height = input_slice_h;
    s_rec.start_time_ms = current_time_ms();
    s_rec.last_frame_time_ms = 0;
    s_rec.video_frame_count = 0;
    s_rec.audio_sample_count = 0;
    for (int i = 0; i < MAX_PENDING_PACKETS; i++) {
        if (s_rec.pending[i].data) {
            free(s_rec.pending[i].data);
            s_rec.pending[i].data = NULL;
        }
    }
    s_rec.pending_count = 0;
    s_rec.nv12_buffer = nv12;
    s_rec.nv12_size = nv12_bytes;

    LOGI("FieldVox video recorder started: %dx%d @ 30fps -> %s", width, height, output_mp4_path);
    pthread_mutex_unlock(&s_rec.mutex);
    return true;
}

void fv_video_recorder_feed_audio(const int16_t *pcm, int frames) {
    if (!pcm || frames <= 0) return;

    pthread_mutex_lock(&s_rec.mutex);
    if (!s_rec.active || !s_rec.audio_codec) {
        pthread_mutex_unlock(&s_rec.mutex);
        return;
    }

    const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(s_rec.audio_codec, 0);
    if (in_idx >= 0) {
        size_t in_size = 0;
        uint8_t *buf = AMediaCodec_getInputBuffer(s_rec.audio_codec, (size_t)in_idx, &in_size);
        const size_t bytes_to_copy = (size_t)frames * sizeof(int16_t) * (size_t)s_rec.channels;
        if (buf && in_size >= bytes_to_copy) {
            memcpy(buf, pcm, bytes_to_copy);
            const uint64_t pts_us = s_rec.audio_sample_count * 1000000ULL / (uint64_t)s_rec.sample_rate;
            s_rec.audio_sample_count += (uint64_t)frames;
            AMediaCodec_queueInputBuffer(s_rec.audio_codec, (size_t)in_idx, 0, bytes_to_copy, pts_us, 0);
        } else if (buf) {
            AMediaCodec_queueInputBuffer(s_rec.audio_codec, (size_t)in_idx, 0, 0, 0, 0);
        }
    }

    drain_encoder_locked(false, 0, false);
    pthread_mutex_unlock(&s_rec.mutex);
}

void fv_video_recorder_feed_frame(const uint32_t *argb, int src_w, int src_h, int stride_px) {
    if (!argb || src_w <= 0 || src_h <= 0) return;

    const uint64_t now_ms = current_time_ms();

    pthread_mutex_lock(&s_rec.mutex);
    if (!s_rec.active || !s_rec.video_codec) {
        pthread_mutex_unlock(&s_rec.mutex);
        return;
    }

    /* Target 30 fps (throttle frame rate) */
    if (now_ms - s_rec.last_frame_time_ms < 32) {
        pthread_mutex_unlock(&s_rec.mutex);
        return;
    }

    const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(s_rec.video_codec, 0);
    if (in_idx >= 0) {
        size_t in_size = 0;
        uint8_t *buf = AMediaCodec_getInputBuffer(s_rec.video_codec, (size_t)in_idx, &in_size);
        if (buf && in_size >= s_rec.nv12_size) {
            if (s_rec.input_stride < s_rec.width || s_rec.input_slice_height < s_rec.height) {
                int stride = 0, slice_h = 0;
                AMediaFormat *infmt = AMediaCodec_getInputFormat(s_rec.video_codec);
                if (infmt) {
                    if (!AMediaFormat_getInt32(infmt, AMEDIAFORMAT_KEY_STRIDE, &stride) || stride <= 0) {
                        AMediaFormat_getInt32(infmt, "stride", &stride);
                    }
                    if (!AMediaFormat_getInt32(infmt, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice_h) || slice_h <= 0) {
                        AMediaFormat_getInt32(infmt, "slice-height", &slice_h);
                    }
                    const char *str = AMediaFormat_toString(infmt);
                    LOGI("Video encoder dequeued input format: %s (stride=%d, slice_h=%d, in_size=%zu)",
                         str ? str : "null", stride, slice_h, in_size);
                    AMediaFormat_delete(infmt);
                }

                if (stride < s_rec.width) {
                    if (s_rec.height > 0 && in_size >= (size_t)s_rec.width * (size_t)s_rec.height * 3 / 2) {
                        const int derived = (int)((in_size * 2) / (size_t)(s_rec.height * 3));
                        if (derived >= s_rec.width && (derived % 16 == 0)) {
                            stride = derived;
                        } else {
                            stride = (s_rec.width + 63) & ~63;
                        }
                    } else {
                        stride = (s_rec.width + 63) & ~63;
                    }
                }
                if (slice_h < s_rec.height) {
                    slice_h = s_rec.height;
                }
                s_rec.input_stride = stride;
                s_rec.input_slice_height = slice_h;
                LOGI("Resolved video encoder geometry: target=%dx%d, stride=%d, slice_height=%d, in_size=%zu",
                     s_rec.width, s_rec.height, stride, slice_h, in_size);
            }

            argb_to_nv12_bt709(argb, src_w, src_h, stride_px, buf,
                               s_rec.width, s_rec.height,
                               s_rec.input_stride, s_rec.input_slice_height);

            const uint64_t pts_us = s_rec.video_frame_count * 1000000ULL / 30ULL;
            s_rec.video_frame_count++;
            s_rec.last_frame_time_ms = now_ms;

            size_t bytes_to_queue = (size_t)s_rec.input_stride * (size_t)s_rec.input_slice_height * 3 / 2;
            if (bytes_to_queue > in_size) bytes_to_queue = in_size;
            AMediaCodec_queueInputBuffer(s_rec.video_codec, (size_t)in_idx, 0, bytes_to_queue, pts_us, 0);
        } else if (buf) {
            AMediaCodec_queueInputBuffer(s_rec.video_codec, (size_t)in_idx, 0, 0, 0, 0);
        }
    }

    drain_encoder_locked(true, 0, false);
    pthread_mutex_unlock(&s_rec.mutex);
}

void fv_video_recorder_stop(char *out_saved_path, size_t max_len) {
    pthread_mutex_lock(&s_rec.mutex);
    if (!s_rec.active) {
        pthread_mutex_unlock(&s_rec.mutex);
        if (out_saved_path && max_len > 0) out_saved_path[0] = '\0';
        return;
    }

    LOGI("Stopping video recording (%llu frames, %llu audio samples)...",
         (unsigned long long)s_rec.video_frame_count, (unsigned long long)s_rec.audio_sample_count);

    /* Signal EOS to video encoder with retries and draining */
    if (s_rec.video_codec) {
        ssize_t v_idx = -1;
        for (int retry = 0; retry < 10 && v_idx < 0; retry++) {
            v_idx = AMediaCodec_dequeueInputBuffer(s_rec.video_codec, 10000);
            if (v_idx < 0) drain_encoder_locked(true, 5000, false);
        }
        if (v_idx >= 0) {
            AMediaCodec_queueInputBuffer(s_rec.video_codec, (size_t)v_idx, 0, 0,
                                         s_rec.video_frame_count * 1000000ULL / 30ULL,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
        drain_encoder_locked(true, 10000, true);
    }

    /* Signal EOS to audio encoder with retries and draining */
    if (s_rec.audio_codec) {
        ssize_t a_idx = -1;
        for (int retry = 0; retry < 10 && a_idx < 0; retry++) {
            a_idx = AMediaCodec_dequeueInputBuffer(s_rec.audio_codec, 10000);
            if (a_idx < 0) drain_encoder_locked(false, 5000, false);
        }
        if (a_idx >= 0) {
            AMediaCodec_queueInputBuffer(s_rec.audio_codec, (size_t)a_idx, 0, 0,
                                         s_rec.audio_sample_count * 1000000ULL / (uint64_t)s_rec.sample_rate,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        }
        drain_encoder_locked(false, 10000, true);
    }

    /* Finalize pending packets if any */
    flush_pending_packets_locked();
    for (int i = 0; i < s_rec.pending_count; i++) {
        if (s_rec.pending[i].data) {
            free(s_rec.pending[i].data);
            s_rec.pending[i].data = NULL;
        }
    }
    s_rec.pending_count = 0;

    const bool saved_ok = s_rec.muxer_started;
    if (!saved_ok) {
        LOGW("Muxer was never started; deleting incomplete output file '%s'", s_rec.path);
    }

    if (s_rec.muxer) {
        if (s_rec.muxer_started) {
            AMediaMuxer_stop(s_rec.muxer);
        }
        AMediaMuxer_delete(s_rec.muxer);
        s_rec.muxer = NULL;
    }

    if (s_rec.video_codec) {
        AMediaCodec_stop(s_rec.video_codec);
        AMediaCodec_delete(s_rec.video_codec);
        s_rec.video_codec = NULL;
    }

    if (s_rec.audio_codec) {
        AMediaCodec_stop(s_rec.audio_codec);
        AMediaCodec_delete(s_rec.audio_codec);
        s_rec.audio_codec = NULL;
    }

    if (s_rec.fd >= 0) {
        close(s_rec.fd);
        s_rec.fd = -1;
    }

    if (!saved_ok) {
        unlink(s_rec.path);
    }

    if (s_rec.nv12_buffer) {
        free(s_rec.nv12_buffer);
        s_rec.nv12_buffer = NULL;
    }

    if (out_saved_path && max_len > 0) {
        if (saved_ok) {
            snprintf(out_saved_path, max_len, "%s", s_rec.path);
        } else {
            out_saved_path[0] = '\0';
        }
    }

    s_rec.active = false;
    s_rec.muxer_started = false;
    LOGI("FieldVox video recording %s: %s", saved_ok ? "finished" : "discarded", s_rec.path);
    pthread_mutex_unlock(&s_rec.mutex);
}

bool fv_video_recorder_is_active(void) {
    pthread_mutex_lock(&s_rec.mutex);
    const bool active = s_rec.active;
    pthread_mutex_unlock(&s_rec.mutex);
    return active;
}

uint32_t fv_video_recorder_get_duration_ms(void) {
    pthread_mutex_lock(&s_rec.mutex);
    uint32_t dur = 0;
    if (s_rec.active) {
        dur = (uint32_t)(current_time_ms() - s_rec.start_time_ms);
    }
    pthread_mutex_unlock(&s_rec.mutex);
    return dur;
}
