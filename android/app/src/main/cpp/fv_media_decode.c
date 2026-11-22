/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_media_decode.h"
#include "fv_camera_effects.h"

#include <stdio.h>
#include <string.h>

bool fv_media_image_to_argb(AImage *image, uint32_t *dst, int dst_w, int dst_h) {
    if (!image || !dst || dst_w <= 0 || dst_h <= 0) return false;
    int32_t width = 0, height = 0;
    AImageCropRect crop = {0};
    const uint8_t *planes[3] = {0};
    size_t lengths[3] = {0};
    int strides[3] = {0}, pixel_strides[3] = {0};
    bool ok = AImage_getWidth(image, &width) == AMEDIA_OK &&
              AImage_getHeight(image, &height) == AMEDIA_OK &&
              AImage_getCropRect(image, &crop) == AMEDIA_OK;
    for (int i = 0; ok && i < 3; i++) {
        uint8_t *data = NULL;
        int length = 0;
        int32_t stride = 0, pixel = 0;
        ok = AImage_getPlaneData(image, i, &data, &length) == AMEDIA_OK && data && length > 0 &&
             AImage_getPlaneRowStride(image, i, &stride) == AMEDIA_OK &&
             AImage_getPlanePixelStride(image, i, &pixel) == AMEDIA_OK;
        planes[i] = data;
        lengths[i] = length > 0 ? (size_t)length : 0u;
        strides[i] = stride;
        pixel_strides[i] = pixel;
    }
    return ok && fv_yuv420_to_argb(dst, dst_w, dst_h, width, height,
                                   crop.left, crop.top, crop.right, crop.bottom,
                                   planes, lengths, strides, pixel_strides);
}

bool fv_media_open_audio_track(int fd, off_t byte_size, bool force_pcm16,
                               AMediaExtractor **out_extractor, AMediaCodec **out_codec,
                               int32_t *out_rate, int32_t *out_channels, int64_t *out_duration_us) {
    if (fd < 0 || byte_size <= 0 || !out_extractor || !out_codec || !out_rate || !out_channels) return false;
    *out_extractor = NULL;
    *out_codec = NULL;
    AMediaExtractor *extractor = AMediaExtractor_new();
    AMediaCodec *codec = NULL;
    AMediaFormat *format = NULL;
    if (!extractor || AMediaExtractor_setDataSourceFd(extractor, fd, 0, byte_size) != AMEDIA_OK) goto fail;

    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    for (size_t i = 0; i < tracks; i++) {
        AMediaFormat *candidate = AMediaExtractor_getTrackFormat(extractor, i);
        const char *mime = NULL;
        if (candidate && AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &mime) &&
            mime && strncmp(mime, "audio/", 6) == 0) {
            int32_t rate = 48000, channels = 1;
            int64_t duration_us = 0;
            AMediaFormat_getInt32(candidate, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
            AMediaFormat_getInt32(candidate, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
            AMediaFormat_getInt64(candidate, AMEDIAFORMAT_KEY_DURATION, &duration_us);
            codec = AMediaCodec_createDecoderByType(mime);
            if (!codec || AMediaExtractor_selectTrack(extractor, i) != AMEDIA_OK) {
                AMediaFormat_delete(candidate);
                goto fail;
            }
            format = candidate;
            if (force_pcm16) AMediaFormat_setInt32(format, "pcm-encoding", 2);
            if (AMediaCodec_configure(codec, format, NULL, NULL, 0) != AMEDIA_OK ||
                AMediaCodec_start(codec) != AMEDIA_OK) {
                goto fail;
            }
            AMediaFormat_delete(format);
            *out_extractor = extractor;
            *out_codec = codec;
            *out_rate = rate;
            *out_channels = channels;
            if (out_duration_us) *out_duration_us = duration_us;
            return true;
        }
        if (candidate) AMediaFormat_delete(candidate);
    }

fail:
    if (codec) AMediaCodec_delete(codec);
    if (format) AMediaFormat_delete(format);
    if (extractor) AMediaExtractor_delete(extractor);
    return false;
}
