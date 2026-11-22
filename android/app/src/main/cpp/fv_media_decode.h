/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_MEDIA_DECODE_H
#define FV_MEDIA_DECODE_H

#include <media/NdkImage.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

bool fv_media_image_to_argb(AImage *image, uint32_t *dst, int dst_w, int dst_h);
bool fv_media_open_audio_track(int fd, off_t byte_size, bool force_pcm16,
                               AMediaExtractor **out_extractor, AMediaCodec **out_codec,
                               int32_t *out_rate, int32_t *out_channels, int64_t *out_duration_us);

#endif
