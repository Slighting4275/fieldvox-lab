/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_ui_layout.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

int fv_ui_grain_ms_to_percent(int grain_ms) {
    if (grain_ms <= 65) return 0;
    if (grain_ms >= 1000) return 100;
    return (grain_ms - 65) * 100 / 935;
}

int fv_ui_percent_to_grain_ms(int percent) {
    if (percent <= 0) return 65;
    if (percent >= 100) return 1000;
    return 65 + (percent * 935 + 50) / 100;
}

void fv_ui_place(WidgetRect rects[static W_COUNT], WidgetId id, float x, float y, float w, float h) {
    rects[id] = (WidgetRect){.x = x, .y = y, .w = w, .h = h, .visible = true};
}

int fv_ui_resolve_tap(const WidgetRect rects[static W_COUNT], float x, float y) {
    for (int i = W_COUNT - 1; i >= 0; i--) {
        const WidgetRect *r = &rects[i];
        if (r->visible && x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) {
            return i;
        }
    }
    return -1;
}

bool fv_ui_any_hit(const WidgetRect rects[static W_COUNT], float x, float y) {
    return fv_ui_resolve_tap(rects, x, y) >= 0;
}

bool fv_ui_tap_dismisses_drawer(const WidgetRect rects[static W_COUNT], bool controls_visible,
                                int hit_id, float y_screen, float d) {
    if (hit_id >= 0 || !controls_visible) return false;
    const WidgetRect *close_r = &rects[W_CLOSE_DRAWER];
    if (!close_r->visible) return false;
    const float drawer_top = close_r->y - 6.0f * d;
    return y_screen < drawer_top;
}

static void layout_row(WidgetRect rects[static W_COUNT], float start_x, float y, float total_w,
                       float h, float gap, const WidgetId *ids, size_t count) {
    const float item_w = (total_w - gap * (float)(count - 1u)) / (float)count;
    float px = start_x;
    for (size_t i = 0; i < count; i++) {
        fv_ui_place(rects, ids[i], px, y, item_w, h);
        px += item_w + gap;
    }
}

static void layout_top_header(WidgetRect rects[static W_COUNT], float x, float y, float w, float h, float d) {
    const float row_h = fminf(42.0f * d, h * 0.65f), gap = 4.0f * d;
    const float help_w = 32.0f * d, torch_w = 54.0f * d, mute_w = 56.0f * d;
    const float help_x = x + w - help_w;
    const float torch_x = help_x - gap - torch_w;
    const float mute_x = torch_x - gap - mute_w;
    fv_ui_place(rects, W_MUTE, mute_x, y, mute_w, row_h);
    fv_ui_place(rects, W_TORCH, torch_x, y, torch_w, row_h);
    fv_ui_place(rects, W_HELP, help_x, y, help_w, row_h);
}

static void layout_perimeter_badges(WidgetRect rects[static W_COUNT], float x, float y, float w, float h, float d) {
    const WidgetId badge_ids[2] = {W_CAL, W_REMPOD};
    layout_row(rects, x, y, w, h, 6.0f * d, badge_ids, 2u);
}

static void layout_floating_visualizer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h, float d) {
    const float inset = 40.0f * d;
    const float plot_y = y + 8.0f * d;
    const float footer_h = fminf(30.0f * d, h * 0.42f);
    const float plot_h = fmaxf(12.0f * d, h - footer_h - 16.0f * d);
    const float plot_w = w - inset * 2.0f;
    const float arrow_h = fminf(44.0f * d, h);
    const float footer_y = y + h - footer_h - 2.0f * d;
    fv_ui_place(rects, W_CAM_PREVIEW, x + inset, plot_y, plot_w, plot_h);
    fv_ui_place(rects, W_VIS_PREV, x + 2.0f * d, y + (h - arrow_h) * 0.5f, 34.0f * d, arrow_h);
    fv_ui_place(rects, W_VIS_NEXT, x + w - 36.0f * d, y + (h - arrow_h) * 0.5f, 34.0f * d, arrow_h);
    fv_ui_place(rects, W_VIS_COLOR, x + inset + plot_w * 0.62f, footer_y, plot_w * 0.38f, footer_h);
}

static void layout_bottom_action_bar(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                     float d, bool inline_modes) {
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float r_outer = (h >= 52.0f * d ? 28.0f : 21.0f) * d;
    const float gap = 5.0f * d;
    const float btn_h = (h >= 52.0f * d ? 38.0f : 30.0f) * d;
    const float btn_y = cy - btn_h * 0.5f;
    const float side_clearance = r_outer + 8.0f * d;
    const float avail_w = (w * 0.5f) - side_clearance;
    const float hit_pad = 6.0f * d;

    fv_ui_place(rects, W_RECORD, cx - r_outer - hit_pad, cy - r_outer - hit_pad,
                (r_outer + hit_pad) * 2.0f, (r_outer + hit_pad) * 2.0f);
    const WidgetId left[] = {W_SESS, W_MARK_HIT, W_CAPTURE_CLEAN};
    const WidgetId right[] = {W_CAM_TINT, W_CTRL};
    const WidgetId inline_right[] = {W_CAPTURE_SCREEN, W_CAM_TINT, W_CTRL};
    const size_t count = inline_modes ? 3u : 2u;
    layout_row(rects, x, btn_y, avail_w, btn_h, gap, left, count);
    layout_row(rects, cx + side_clearance, btn_y, avail_w, btn_h, gap,
               inline_modes ? inline_right : right, count);
}

static void layout_vox_drawer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h, float d) {
    fv_ui_place(rects, W_CLOSE_DRAWER, x + w - 42.0f * d, y + 4.0f * d, 36.0f * d, 32.0f * d);
    const bool compact = h < 216.0f * d && w >= 320.0f * d;
    const bool wide = w > 600.0f * d || compact;
    const float row_count = wide ? 4.0f : 6.0f;
    const float rows_h = fmaxf(0.0f, h - 44.0f * d);
    const float equal_row = rows_h / row_count;
    const float action_row = compact ? fmaxf(equal_row, fminf(32.0f * d, rows_h * 0.5f)) : equal_row;
    const float row = (rows_h - action_row) / (row_count - 1.0f);
    const float slider_w = wide ? (w - 24.0f * d) * 0.5f : w - 16.0f * d;
    const float slider_x = x + 8.0f * d, second_x = slider_x + slider_w + 8.0f * d;
    float ry = y + 40.0f * d;

    fv_ui_place(rects, W_ECHO_LEVEL, slider_x, ry, slider_w, row);
    if (!wide) ry += row;
    fv_ui_place(rects, W_REVERB_LEVEL, wide ? second_x : slider_x, ry, slider_w, row);
    ry += row;
    fv_ui_place(rects, W_DISTORTION_LEVEL, slider_x, ry, slider_w, row);
    if (!wide) ry += row;
    fv_ui_place(rects, W_GRAIN_LEVEL, wide ? second_x : slider_x, ry, slider_w, row);
    ry += row;

    const WidgetId row_a[3] = {W_START, W_NOISE, W_VOICE_DUCK};
    const WidgetId row_b[4] = {W_AUTO_GATE, W_RADIO_TUNER, W_ADVANCED, W_SOURCE_MODE};
    const float inset = fminf(4.0f * d, row * 0.15f);
    layout_row(rects, x + 8.0f * d, ry + inset, w - 16.0f * d, row - 2.0f * inset, 4.0f * d, row_a, 3u);
    ry += row;
    const float action_inset = fminf(4.0f * d, action_row * 0.15f);
    layout_row(rects, x + 8.0f * d, ry + action_inset, w - 16.0f * d,
               action_row - 2.0f * action_inset, 4.0f * d, row_b, 4u);
}

static void layout_advanced_drawer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h, float d) {
    const float header_h = 28.0f * d;
    fv_ui_place(rects, W_ADVANCED, x, y, w - 48.0f * d, header_h);
    fv_ui_place(rects, W_CLOSE_DRAWER, x + w - 46.0f * d, y + 4.0f * d, 40.0f * d, 22.0f * d);

    float cur_y = y + header_h + 4.0f * d;
    const float gap = 3.0f * d;
    const float pill_h = fmaxf(0.0f, h - header_h - 28.0f * d) / 5.0f;
    const float row_w = w - 16.0f * d;
    const float row_x = x + 8.0f * d;

    const WidgetId r1[4] = {W_CADENCE, W_DIRECTION, W_PITCH, W_CAL};
    const WidgetId r2[4] = {W_ECHO, W_REVERB, W_NOISE, W_REV};
    const WidgetId r3[6] = {W_SOURCE_MODE, W_VOCAL, W_PAN, W_REMPOD, W_ESTES, W_AUTO_GATE};
    const WidgetId r4[4] = {W_SENS_LOW, W_SENS_MED, W_SENS_HIGH, W_COOL_CYCLE};
    const WidgetId r5[4] = {W_CAMHUD, W_CAM_LENS, W_VIS_LAYOUT, W_MIC};
    const WidgetId *rows[5] = {r1, r2, r3, r4, r5};
    const size_t counts[5] = {4u, 4u, 6u, 4u, 4u};

    for (size_t i = 0; i < 5u; i++) {
        layout_row(rects, row_x, cur_y, row_w, pill_h, gap, rows[i], counts[i]);
        cur_y += pill_h + gap;
    }
}

void fv_ui_layout_main_screen(WidgetRect rects[static W_COUNT], int screen_w, int screen_h, float d,
                              const FvUiState *state, FvUiScreenLayout *out_layout) {
    memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);

    const bool controls_visible = state && state->controls_visible;
    const bool advanced_controls = state && state->advanced_controls;
    const FvVisLayoutPreset vis_layout = state ? state->vis_layout : FV_VIS_LAYOUT_BOTTOM;
    const bool landscape = screen_w > screen_h;
    const float top_inset = (landscape ? 36.0f : 58.0f) * d;
    const float side_inset = (landscape ? 36.0f : 12.0f) * d;
    const float bottom_inset = (landscape ? 10.0f : 32.0f) * d;
    const float ui_x0 = side_inset;
    const float ui_w = (float)screen_w - 2.0f * side_inset;
    const float header_h = (landscape ? 48.0f : 66.0f) * d;
    const float status_y = top_inset + header_h + 3.0f * d;
    const float status_h = (landscape ? 14.0f : 18.0f) * d;
    const float badge_y = status_y + status_h + 3.0f * d;
    const float badge_h = (landscape ? 24.0f : 30.0f) * d;
    const float bottom_bar_h = (landscape ? 46.0f : 66.0f) * d;
    const float bottom_bar_y = (float)screen_h - bottom_inset - bottom_bar_h;
    const bool short_landscape = landscape && (float)screen_h < 300.0f * d;
    const bool inline_modes = short_landscape && ui_w >= 320.0f * d;
    const float capture_h = inline_modes ? 0.0f : 48.0f * d;
    const float capture_y = bottom_bar_y - 8.0f * d - capture_h;
    const float capture_w = fminf(ui_w, 214.0f * d);
    const float vis_bottom = inline_modes ? capture_y : capture_y - 8.0f * d;
    const float vis_min_y = badge_y + badge_h + 6.0f * d;
    const float max_vis_span = vis_bottom - vis_min_y;
    const float base_vis_h = (vis_layout == FV_VIS_LAYOUT_COMPACT)
                                 ? (landscape ? 52.0f : 76.0f) * d
                                 : (landscape ? 74.0f : 130.0f) * d;
    const float vis_h = max_vis_span >= 40.0f * d ? fminf(base_vis_h, max_vis_span) : 0.0f;
    const float vis_top = vis_layout == FV_VIS_LAYOUT_TOP ? vis_min_y : vis_bottom - vis_h;
    const float drawer_bottom = vis_bottom;
    const float drawer_top = short_landscape ? 24.0f * d : top_inset + 12.0f * d;
    const float drawer_h = fmaxf(0.0f, fminf(drawer_bottom - drawer_top,
                                 (advanced_controls ? (landscape ? 216.0f : 280.0f)
                                                    : (landscape ? 260.0f : 380.0f)) * d));
    const float drawer_y = drawer_bottom - drawer_h;

    layout_top_header(rects, ui_x0, top_inset, ui_w, header_h, d);
    layout_perimeter_badges(rects, ui_x0, badge_y, ui_w, badge_h, d);
    layout_bottom_action_bar(rects, ui_x0, bottom_bar_y, ui_w, bottom_bar_h, d, inline_modes);
    if (!inline_modes) {
        const WidgetId capture_ids[] = {W_CAPTURE_CLEAN, W_CAPTURE_SCREEN};
        layout_row(rects, ui_x0 + (ui_w - capture_w) * 0.5f, capture_y, capture_w,
                   capture_h, 4.0f * d, capture_ids, 2u);
    }
    if (!controls_visible) {
        if (vis_h > 0.0f) layout_floating_visualizer(rects, ui_x0, vis_top, ui_w, vis_h, d);
    } else if (advanced_controls) {
        layout_advanced_drawer(rects, ui_x0, drawer_y, ui_w, drawer_h, d);
    } else {
        layout_vox_drawer(rects, ui_x0, drawer_y, ui_w, drawer_h, d);
    }

    if (out_layout) {
        *out_layout = (FvUiScreenLayout){
            .landscape = landscape,
            .top_inset = top_inset,
            .side_inset = side_inset,
            .bottom_inset = bottom_inset,
            .ui_x0 = ui_x0,
            .ui_w = ui_w,
            .header_h = header_h,
            .status_y = status_y,
            .status_h = status_h,
            .badge_y = badge_y,
            .badge_h = badge_h,
            .bottom_bar_h = bottom_bar_h,
            .bottom_bar_y = bottom_bar_y,
            .vis_h = vis_h,
            .vis_top = vis_top,
            .drawer_y = drawer_y,
            .drawer_h = drawer_h,
        };
    }
}

static void layout_help_overlay(WidgetRect rects[static W_COUNT], float ui_x0, float ui_w,
                                float screen_h, float d, float *out_y, float *out_h) {
    memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);
    const float h = fminf(screen_h - 36.0f * d, 360.0f * d);
    const float y = (screen_h - h) * 0.5f;
    const float bw = fminf(150.0f * d, ui_w - 28.0f * d);
    const float bh = 38.0f * d;
    fv_ui_place(rects, W_CLOSE_HELP, ui_x0 + (ui_w - bw) * 0.5f, y + h - bh - 12.0f * d, bw, bh);
    if (out_y) *out_y = y;
    if (out_h) *out_h = h;
}

static void layout_evidence_overlay(WidgetRect rects[static W_COUNT], float ui_x0, float ui_w,
                                    float screen_h, float d, int item_count, const bool is_video[4],
                                    float *out_y, float *out_h, float *out_row_y, float *out_row_h) {
    memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);
    const float h = fminf(screen_h - 92.0f * d, 390.0f * d);
    const float y = (screen_h - h) * 0.5f;
    fv_ui_place(rects, W_CLOSE_EVIDENCE, ui_x0 + ui_w - 48.0f * d, y + 9.0f * d, 38.0f * d, 25.0f * d);
    const float row_x = ui_x0 + 12.0f * d, row_w = ui_w - 24.0f * d;
    const float row_h = fminf(58.0f * d, (h - 132.0f * d) / 4.0f);
    const float start_row_y = y + 62.0f * d;
    const float open_w = 54.0f * d;
    float row_y = start_row_y;
    for (int i = 0; i < item_count && i < 4; i++) {
        if (!is_video[i]) {
            fv_ui_place(rects, (WidgetId)(W_EVIDENCE_BANK_0 + i),
                        row_x + row_w - 2.0f * open_w - 12.0f * d, row_y + 8.0f * d,
                        open_w, row_h - 16.0f * d);
        }
        fv_ui_place(rects, (WidgetId)(W_EVIDENCE_OPEN_0 + i),
                    row_x + row_w - open_w - 8.0f * d, row_y + 8.0f * d,
                    open_w, row_h - 16.0f * d);
        row_y += row_h + 5.0f * d;
    }
    const float footer = y + h - 48.0f * d;
    const WidgetId nav_ids[3] = {W_LIBRARY_PREV, W_LIBRARY_REFRESH, W_LIBRARY_NEXT};
    layout_row(rects, row_x, footer, row_w, 34.0f * d, 6.0f * d, nav_ids, 3u);
    if (out_y) *out_y = y;
    if (out_h) *out_h = h;
    if (out_row_y) *out_row_y = start_row_y;
    if (out_row_h) *out_row_h = row_h;
}

static void layout_tuner_overlay(WidgetRect rects[static W_COUNT], float ui_x0, float ui_w,
                                 float screen_h, float d, int item_count,
                                 float *out_y, float *out_h) {
    memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);
    const float h = fminf(screen_h - 36.0f * d, 430.0f * d);
    const float y = (screen_h - h) * 0.5f;
    fv_ui_place(rects, W_CLOSE_TUNER, ui_x0 + ui_w - 48.0f * d, y + 9.0f * d, 38.0f * d, 25.0f * d);
    const float row_x = ui_x0 + 12.0f * d, row_w = ui_w - 24.0f * d;
    const WidgetId top_ids[3] = {W_TUNER_BANK, W_TUNER_SHUFFLE, W_TUNER_REFRESH};
    layout_row(rects, row_x, y + 42.0f * d, row_w, 30.0f * d, 6.0f * d, top_ids, 3u);
    const float start_row_y = y + 80.0f * d;
    /* Reserve 4 gaps of 4dp, 18dp page caption, and 44dp footer button band. */
    const float row_h = fminf(52.0f * d, (h - 164.0f * d) / 5.0f);
    float row_y = start_row_y;
    for (int i = 0; i < item_count && i < 5; i++) {
        fv_ui_place(rects, (WidgetId)(W_TUNER_PLAY_0 + i), row_x, row_y, row_w, row_h);
        row_y += row_h + 4.0f * d;
    }
    const WidgetId nav_ids[2] = {W_TUNER_PREV, W_TUNER_NEXT};
    layout_row(rects, row_x, y + h - 44.0f * d, row_w, 34.0f * d, 6.0f * d, nav_ids, 2u);
    if (out_y) *out_y = y;
    if (out_h) *out_h = h;
}

static void layout_local_playback(WidgetRect rects[static W_COUNT], float ui_x0, float ui_w,
                                  float width, float height, float d,
                                  const FvUiState *state, FvUiScreenLayout *out_layout) {
    memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);
    const bool landscape = width > height;
    const bool audio_only = state && state->playback_audio_only;
    const bool hide_controls = state && state->playback_controls_hidden && !audio_only;
    const float top = (landscape ? 36.0f : 58.0f) * d;
    const float bottom = (landscape ? 12.0f : 32.0f) * d;
    const float header_end = top + 66.0f * d;
    const float buttons_y = fmaxf(header_end + 62.0f * d, height - bottom - 90.0f * d);
    const float min_wave_h = landscape ? 24.0f * d : 48.0f * d;
    const float desired_wave_h = landscape ? 56.0f * d : 100.0f * d;
    const float max_wave_band = fmaxf(min_wave_h, buttons_y - 42.0f * d - (header_end + 8.0f * d));
    const float wave_h = audio_only ? max_wave_band : fminf(desired_wave_h, max_wave_band);
    const float wave_y = audio_only ? (header_end + 8.0f * d) : (buttons_y - wave_h - 42.0f * d);
    const float footer_start = audio_only ? fmaxf(header_end, buttons_y - 32.0f * d)
                                          : fmaxf(header_end, wave_y - 10.0f * d);
    const float mid_h = fmaxf(40.0f * d, footer_start - header_end);
    if (!hide_controls) {
        fv_ui_place(rects, W_PLAY_BACK, ui_x0, top, 80.0f * d, 34.0f * d);
        fv_ui_place(rects, W_PLAY_MUTE, ui_x0 + ui_w - 64.0f * d, top, 64.0f * d, 34.0f * d);
        fv_ui_place(rects, W_PLAY_SEEK, ui_x0, wave_y, ui_w, wave_h);
        const float gap = 5.0f * d;
        const WidgetId play_ids[4] = {W_PLAY_REWIND_10, W_PLAY_REWIND_5, W_PLAY_TOGGLE, W_PLAY_FORWARD_5};
        const WidgetId mark_ids[3] = {W_PLAY_MARK_PREV, W_PLAY_MARK, W_PLAY_MARK_NEXT};
        layout_row(rects, ui_x0, buttons_y, ui_w, 40.0f * d, gap, play_ids, 4u);
        layout_row(rects, ui_x0, buttons_y + 46.0f * d, ui_w, 40.0f * d, gap, mark_ids, 3u);
    }
    if (out_layout) {
        out_layout->play_top = top;
        out_layout->play_wave_y = wave_y;
        out_layout->play_wave_h = wave_h;
        out_layout->play_buttons_y = buttons_y;
        out_layout->play_header_end = header_end;
        out_layout->play_footer_start = footer_start;
        out_layout->play_mid_h = mid_h;
    }
}

void fv_ui_layout_active(WidgetRect rects[static W_COUNT], int screen_w, int screen_h, float d,
                         const FvUiState *state, int overlay_item_count, const bool evidence_is_video[4],
                         FvUiScreenLayout *out_layout) {
    FvUiScreenLayout layout = {0};
    fv_ui_layout_main_screen(rects, screen_w, screen_h, d, state, &layout);
    if (state) {
        if (state->estes_mode) {
            memset(rects, 0, sizeof(WidgetRect) * (size_t)W_COUNT);
            const float bw = 150.0f * d, bh = 44.0f * d;
            fv_ui_place(rects, W_ESTES_EXIT, (float)screen_w - bw - 16.0f * d, 60.0f * d, bw, bh);
        } else if (state->playback_visible) {
            layout_local_playback(rects, layout.ui_x0, layout.ui_w, (float)screen_w, (float)screen_h, d, state, &layout);
        } else if (state->help_visible) {
            layout_help_overlay(rects, layout.ui_x0, layout.ui_w, (float)screen_h, d,
                                &layout.help_y, &layout.help_h);
        } else if (state->tuner_visible) {
            layout_tuner_overlay(rects, layout.ui_x0, layout.ui_w, (float)screen_h, d,
                                 overlay_item_count, &layout.tuner_y, &layout.tuner_h);
        } else if (state->evidence_visible) {
            static const bool empty_video[4] = {false, false, false, false};
            layout_evidence_overlay(rects, layout.ui_x0, layout.ui_w, (float)screen_h, d,
                                    overlay_item_count, evidence_is_video ? evidence_is_video : empty_video,
                                    &layout.evidence_y, &layout.evidence_h,
                                    &layout.evidence_row_y, &layout.evidence_row_h);
        }
    }
    if (out_layout) *out_layout = layout;
}

int fv_ui_handle_tap(FvUiState *state, const WidgetRect rects[static W_COUNT],
                     float x, float y, float d, bool *out_estes_hit) {
    if (out_estes_hit) *out_estes_hit = false;
    if (!state) return -1;
    const int hit = fv_ui_resolve_tap(rects, x, y);
    if (state->estes_mode) {
        if (hit == W_ESTES_EXIT) {
            state->estes_mode = false;
            return W_ESTES_EXIT;
        }
        if (out_estes_hit) *out_estes_hit = true;
        return -1;
    }
    if (state->help_visible) {
        state->help_visible = false;
        return hit == W_CLOSE_HELP ? W_CLOSE_HELP : -1;
    }
    if (state->evidence_visible && hit < 0) {
        state->evidence_visible = false;
        return -1;
    }
    if (state->tuner_visible && hit < 0) {
        state->tuner_visible = false;
        return -1;
    }
    if (state->playback_visible) {
        if (hit < 0 && !state->playback_audio_only) {
            state->playback_controls_hidden = !state->playback_controls_hidden;
            return -1;
        }
        return hit;
    }
    if (hit < 0 && fv_ui_tap_dismisses_drawer(rects, state->controls_visible, hit, y, d)) {
        state->controls_visible = false;
        return -1;
    }
    return hit;
}

bool fv_ui_back_pressed(FvUiState *state, bool *out_close_playback) {
    if (out_close_playback) *out_close_playback = false;
    if (!state) return false;
    if (state->playback_visible) {
        state->playback_visible = false;
        state->playback_audio_only = false;
        state->playback_controls_hidden = false;
        state->evidence_visible = true;
        if (out_close_playback) *out_close_playback = true;
        return true;
    }
    if (state->evidence_visible) { state->evidence_visible = false; return true; }
    if (state->tuner_visible) { state->tuner_visible = false; return true; }
    if (state->help_visible) { state->help_visible = false; return true; }
    if (state->estes_mode) { state->estes_mode = false; return true; }
    if (state->controls_visible) { state->controls_visible = false; return true; }
    return false;
}
