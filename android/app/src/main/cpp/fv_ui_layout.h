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
    W_COUNT
} WidgetId;

typedef struct {
    float x, y, w, h; /* content coordinates (before scrolling) */
    bool visible;
} WidgetRect;

typedef struct {
    bool landscape;
    float top_inset, side_inset, bottom_inset;
    float ui_x0, ui_w;
    float header_h;
    float status_y, status_h;
    float badge_y, badge_h;
    float bottom_bar_h, bottom_bar_y;
    float vis_h, vis_top;
} FvUiScreenLayout;

typedef struct {
    float half_w;
    float left_x, right_x;
} FvUiBadgeGeom;

typedef struct {
    float inset;
    float plot_y, plot_w, plot_h;
    float arrow_h;
    float footer_y, footer_h;
} FvUiVisualizerGeom;

typedef struct {
    float cx, cy, r_outer, hit_pad;
    float gap, btn_h, btn_y, btn_w;
    float side_clearance;
    float b1_x, b2_x, b3_x, b4_x;
} FvUiDockGeom;

typedef struct {
    float drawer_h, drawer_y;
    float header_h;
    float close_w, close_h, close_x, close_y;
    float cur_y, gap, pill_h;
    float p4_w, p5_w, tools_w;
} FvUiAdvDrawerGeom;

void fv_ui_place(WidgetRect rects[static W_COUNT], WidgetId id, float x, float y, float w, float h);
int fv_ui_resolve_tap(const WidgetRect rects[static W_COUNT], float x, float y);
bool fv_ui_any_hit(const WidgetRect rects[static W_COUNT], float x, float y);
bool fv_ui_tap_dismisses_drawer(const WidgetRect rects[static W_COUNT], bool controls_visible,
                                int hit_id, float y_screen, float d);

FvUiScreenLayout fv_ui_compute_screen(int screen_w, int screen_h, float d);
void fv_ui_layout_perimeter_badges(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                   float d, FvUiBadgeGeom *out);
void fv_ui_layout_floating_visualizer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                      float d, FvUiVisualizerGeom *out);
void fv_ui_layout_bottom_action_bar(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                    float d, FvUiDockGeom *out);
void fv_ui_layout_advanced_drawer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                  float d, FvUiAdvDrawerGeom *out);

#endif
