/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_LOCAL_PLAYER_H
#define FV_LOCAL_PLAYER_H
#include <jni.h>
#include "fv_recording_data.h"
#include "fv_gfx.h"

typedef struct {
    bool visible, ready, playing, video, muted, error, waveform_ready;
    int64_t position_ms, duration_ms;
    char name[256], status[128];
    float waveform[FV_PLAYBACK_BINS];
    int64_t bookmarks[FV_BOOKMARKS_MAX];
    size_t bookmark_count;
    uint64_t video_frames;
} FvLocalPlayerSnapshot;
bool fv_local_player_init(JavaVM *vm);
void fv_local_player_shutdown(void);
void fv_local_player_open(const char *path, bool muted);
void fv_local_player_close(void);
void fv_local_player_pause(void);
void fv_local_player_toggle(void);
void fv_local_player_mute(void);
void fv_local_player_seek(int64_t ms);
void fv_local_player_seek_delta(int64_t delta_ms);
void fv_local_player_scrub_begin(void);
void fv_local_player_scrub_end(void);
void fv_local_player_bookmark(void);
void fv_local_player_jump_mark(bool forward);
bool fv_local_player_active(void);
void fv_local_player_snapshot(FvLocalPlayerSnapshot *snapshot);
void fv_local_player_draw_video(FvCanvas *canvas);
#endif
