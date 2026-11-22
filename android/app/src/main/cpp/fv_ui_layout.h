/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_UI_LAYOUT_H
#define FV_UI_LAYOUT_H

#include <stdbool.h>

typedef enum {
    W_CAL, W_CADENCE, W_DIRECTION, W_PITCH,
    W_ECHO, W_REVERB, W_NOISE, W_REV,
    W_SESS, W_REC, W_START, W_MIC, W_CTRL,
    W_SB11, W_VOCAL, W_REMPOD, W_PAN,
    W_ESTES, W_CAMHUD, W_SENS_DOWN, W_SENS_UP,
    W_COOL_DOWN, W_COOL_UP,
    W_MARK_HIT, W_CAM_PREVIEW,
    W_VIDEO_REC, W_CAM_TINT, W_CLOSE_DRAWER,
    W_TORCH, W_HELP, W_MUTE,
    W_SENS_LOW, W_SENS_MED, W_SENS_HIGH, W_COOL_CYCLE,
    W_EVIDENCE_OPEN_0, W_EVIDENCE_OPEN_1, W_EVIDENCE_OPEN_2, W_EVIDENCE_OPEN_3,
    W_CLOSE_EVIDENCE, W_CLOSE_HELP, W_AUTO_GATE,
    W_VIS_PREV, W_VIS_NEXT, W_VIS_COLOR, W_VOICE_DUCK, W_ADVANCED,
    W_ECHO_LEVEL, W_REVERB_LEVEL, W_DISTORTION_LEVEL, W_GRAIN_LEVEL,
    W_LIBRARY_PREV, W_LIBRARY_NEXT, W_LIBRARY_REFRESH,
    W_PLAY_BACK, W_PLAY_TOGGLE, W_PLAY_REWIND_5, W_PLAY_REWIND_10, W_PLAY_FORWARD_5,
    W_PLAY_SEEK, W_PLAY_MUTE, W_PLAY_MARK, W_PLAY_MARK_PREV, W_PLAY_MARK_NEXT,
    W_SOURCE_MODE, W_EVIDENCE_BANK_0, W_EVIDENCE_BANK_1, W_EVIDENCE_BANK_2, W_EVIDENCE_BANK_3,
    W_RADIO_TUNER, W_CLOSE_TUNER, W_TUNER_BANK, W_TUNER_SHUFFLE, W_TUNER_REFRESH,
    W_TUNER_PREV, W_TUNER_NEXT,
    W_TUNER_PLAY_0, W_TUNER_PLAY_1, W_TUNER_PLAY_2, W_TUNER_PLAY_3, W_TUNER_PLAY_4,
    W_CAM_LENS, W_VIS_LAYOUT, W_VIDEO_SOURCE,
    W_ESTES_EXIT,
    W_COUNT
} WidgetId;

typedef enum {
    FV_VIS_LAYOUT_BOTTOM = 0,
    FV_VIS_LAYOUT_TOP,
    FV_VIS_LAYOUT_COMPACT,
    FV_VIS_LAYOUT_COUNT
} FvVisLayoutPreset;

typedef struct {
    float x, y, w, h; /* content coordinates (before scrolling) */
    bool visible;
} WidgetRect;

typedef struct {
    bool controls_visible;
    bool advanced_controls;
    bool help_visible;
    bool evidence_visible;
    bool tuner_visible;
    bool playback_visible;
    bool playback_audio_only;
    bool playback_controls_hidden;
    bool estes_mode;
    FvVisLayoutPreset vis_layout;
} FvUiState;

typedef struct {
    bool landscape;
    float top_inset, side_inset, bottom_inset;
    float ui_x0, ui_w;
    float header_h;
    float status_y, status_h;
    float badge_y, badge_h;
    float bottom_bar_h, bottom_bar_y;
    float vis_h, vis_top;
    float drawer_y, drawer_h;
    float help_y, help_h;
    float evidence_y, evidence_h, evidence_row_y, evidence_row_h;
    float tuner_y, tuner_h;
    float play_top, play_wave_y, play_wave_h, play_buttons_y;
    float play_header_end, play_footer_start, play_mid_h;
} FvUiScreenLayout;

int fv_ui_grain_ms_to_percent(int grain_ms);
int fv_ui_percent_to_grain_ms(int percent);

void fv_ui_place(WidgetRect rects[static W_COUNT], WidgetId id, float x, float y, float w, float h);
int fv_ui_resolve_tap(const WidgetRect rects[static W_COUNT], float x, float y);
bool fv_ui_any_hit(const WidgetRect rects[static W_COUNT], float x, float y);
bool fv_ui_tap_dismisses_drawer(const WidgetRect rects[static W_COUNT], bool controls_visible,
                                int hit_id, float y_screen, float d);

void fv_ui_layout_main_screen(WidgetRect rects[static W_COUNT], int screen_w, int screen_h, float d,
                              const FvUiState *state, FvUiScreenLayout *out_layout);
void fv_ui_layout_active(WidgetRect rects[static W_COUNT], int screen_w, int screen_h, float d,
                         const FvUiState *state, int overlay_item_count, const bool evidence_is_video[4],
                         FvUiScreenLayout *out_layout);

/* Handles modal/drawer dismissal and returns the triggered WidgetId (>= 0), or -1 if already handled. */
int fv_ui_handle_tap(FvUiState *state, const WidgetRect rects[static W_COUNT],
                     float x, float y, float d, bool *out_estes_hit);
/* Handles Android Back button across modals/drawers. Returns true if consumed.
 * Sets *out_close_playback when transitioning from local playback back to the library. */
bool fv_ui_back_pressed(FvUiState *state, bool *out_close_playback);

#endif
