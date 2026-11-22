/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_ui_layout.h"

#include <math.h>
#include <stddef.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

void fv_ui_place(WidgetRect rects[static W_COUNT], WidgetId id, float x, float y, float w, float h) {
    rects[id].x = x;
    rects[id].y = y;
    rects[id].w = w;
    rects[id].h = h;
    rects[id].visible = true;
}

int fv_ui_resolve_tap(const WidgetRect rects[static W_COUNT], float x, float y) {
    for (int i = W_COUNT - 1; i >= 0; i--) {
        const WidgetRect *r = &rects[i];
        if (!r->visible) continue;
        if (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) {
            return i;
        }
    }
    return -1;
}

bool fv_ui_any_hit(const WidgetRect rects[static W_COUNT], float x, float y) {
    for (int i = 0; i < W_COUNT; i++) {
        const WidgetRect *r = &rects[i];
        if (r->visible && x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) {
            return true;
        }
    }
    return false;
}

bool fv_ui_tap_dismisses_drawer(const WidgetRect rects[static W_COUNT], bool controls_visible,
                                int hit_id, float y_screen, float d) {
    if (hit_id >= 0 || !controls_visible) return false;
    const WidgetRect *close_r = &rects[W_CLOSE_DRAWER];
    if (!close_r->visible) return false;
    const float drawer_top = close_r->y - 6.0f * d;
    return y_screen < drawer_top;
}

FvUiScreenLayout fv_ui_compute_screen(int screen_w, int screen_h, float d) {
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
    const float vis_h = (landscape ? 74.0f : 130.0f) * d;
    const float vis_top = bottom_bar_y - vis_h - 8.0f * d;
    return (FvUiScreenLayout){
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
    };
}

void fv_ui_layout_perimeter_badges(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                   float d, FvUiBadgeGeom *out) {
    const float half_w = (w - 6.0f * d) * 0.5f;
    const float lx = x;
    const float rx = x + half_w + 6.0f * d;
    fv_ui_place(rects, W_CAL, lx, y, half_w, h);
    fv_ui_place(rects, W_REMPOD, rx, y, half_w, h);
    if (out) *out = (FvUiBadgeGeom){.half_w = half_w, .left_x = lx, .right_x = rx};
}

void fv_ui_layout_floating_visualizer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                      float d, FvUiVisualizerGeom *out) {
    const float inset = 40.0f * d;
    const float plot_y = y + 8.0f * d;
    const float plot_h = fmaxf(12.0f * d, h - 46.0f * d);
    const float plot_w = w - inset * 2.0f;
    const float arrow_h = fminf(44.0f * d, h);
    const float footer_h = 30.0f * d;
    const float footer_y = y + h - footer_h - 2.0f * d;
    fv_ui_place(rects, W_CAM_PREVIEW, x + inset, plot_y, plot_w, plot_h);
    fv_ui_place(rects, W_VIS_PREV, x + 2.0f * d, y + (h - arrow_h) * 0.5f, 34.0f * d, arrow_h);
    fv_ui_place(rects, W_VIS_NEXT, x + w - 36.0f * d, y + (h - arrow_h) * 0.5f, 34.0f * d, arrow_h);
    fv_ui_place(rects, W_VIS_COLOR, x + inset + plot_w * 0.62f, footer_y, plot_w * 0.38f, footer_h);
    if (out) {
        *out = (FvUiVisualizerGeom){
            .inset = inset,
            .plot_y = plot_y,
            .plot_w = plot_w,
            .plot_h = plot_h,
            .arrow_h = arrow_h,
            .footer_y = footer_y,
            .footer_h = footer_h,
        };
    }
}

void fv_ui_layout_bottom_action_bar(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                    float d, FvUiDockGeom *out) {
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float r_outer = (h >= 52.0f * d ? 28.0f : 21.0f) * d;
    const float gap = 5.0f * d;
    const float btn_h = (h >= 52.0f * d ? 38.0f : 30.0f) * d;
    const float btn_y = cy - btn_h * 0.5f;
    const float side_clearance = r_outer + 8.0f * d;
    const float avail_w = (w * 0.5f) - side_clearance;
    const float btn_w = (avail_w - gap) * 0.5f;
    const float hit_pad = 6.0f * d;

    const float b1_x = x;
    const float b2_x = b1_x + btn_w + gap;
    const float b3_x = cx + side_clearance;
    const float b4_x = b3_x + btn_w + gap;

    fv_ui_place(rects, W_SESS, b1_x, btn_y, btn_w, btn_h);
    fv_ui_place(rects, W_MARK_HIT, b2_x, btn_y, btn_w, btn_h);
    fv_ui_place(rects, W_VIDEO_REC, cx - r_outer - hit_pad, cy - r_outer - hit_pad,
                (r_outer + hit_pad) * 2.0f, (r_outer + hit_pad) * 2.0f);
    fv_ui_place(rects, W_CAM_TINT, b3_x, btn_y, btn_w, btn_h);
    fv_ui_place(rects, W_CTRL, b4_x, btn_y, btn_w, btn_h);

    if (out) {
        *out = (FvUiDockGeom){
            .cx = cx,
            .cy = cy,
            .r_outer = r_outer,
            .hit_pad = hit_pad,
            .gap = gap,
            .btn_h = btn_h,
            .btn_y = btn_y,
            .btn_w = btn_w,
            .side_clearance = side_clearance,
            .b1_x = b1_x,
            .b2_x = b2_x,
            .b3_x = b3_x,
            .b4_x = b4_x,
        };
    }
}

void fv_ui_layout_advanced_drawer(WidgetRect rects[static W_COUNT], float x, float y, float w, float h,
                                  float d, FvUiAdvDrawerGeom *out) {
    const float header_h = 28.0f * d;
    fv_ui_place(rects, W_ADVANCED, x, y, w - 48.0f * d, header_h);
    const float close_w = 40.0f * d, close_h = 22.0f * d;
    const float close_x = x + w - close_w - 6.0f * d, close_y = y + 4.0f * d;
    fv_ui_place(rects, W_CLOSE_DRAWER, close_x, close_y, close_w, close_h);

    float cur_y = y + header_h + 4.0f * d;
    const float gap = 3.0f * d;
    const float pill_h = (h - header_h - 32.0f * d) / 5.0f;
    const float p4_w = (w - gap * 3.0f - 16.0f * d) / 4.0f;
    const float p5_w = (w - gap * 5.0f - 16.0f * d) / 6.0f;
    const float tools_w = (w - gap * 4.0f - 16.0f * d) / 5.0f;

    /* Row 1: Sweep Controls */
    float px = x + 8.0f * d;
    fv_ui_place(rects, W_CADENCE, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_DIRECTION, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_PITCH, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_CAL, px, cur_y, p4_w, pill_h);
    cur_y += pill_h + gap;

    /* Row 2: FX Toggles */
    px = x + 8.0f * d;
    fv_ui_place(rects, W_ECHO, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_REVERB, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_NOISE, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_REV, px, cur_y, p4_w, pill_h);
    cur_y += pill_h + gap;

    /* Row 3: Specialized Modes */
    px = x + 8.0f * d;
    fv_ui_place(rects, W_SOURCE_MODE, px, cur_y, p5_w, pill_h); px += p5_w + gap;
    fv_ui_place(rects, W_VOCAL, px, cur_y, p5_w, pill_h); px += p5_w + gap;
    fv_ui_place(rects, W_PAN, px, cur_y, p5_w, pill_h); px += p5_w + gap;
    fv_ui_place(rects, W_REMPOD, px, cur_y, p5_w, pill_h); px += p5_w + gap;
    fv_ui_place(rects, W_ESTES, px, cur_y, p5_w, pill_h); px += p5_w + gap;
    fv_ui_place(rects, W_AUTO_GATE, px, cur_y, p5_w, pill_h);
    cur_y += pill_h + gap;

    /* Row 4: Sensitivity & Cooldown */
    px = x + 8.0f * d;
    fv_ui_place(rects, W_SENS_LOW, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_SENS_MED, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_SENS_HIGH, px, cur_y, p4_w, pill_h); px += p4_w + gap;
    fv_ui_place(rects, W_COOL_CYCLE, px, cur_y, p4_w, pill_h);
    cur_y += pill_h + gap;

    /* Row 5: Camera switch, audio session and recording tools */
    px = x + 8.0f * d;
    fv_ui_place(rects, W_CAMHUD, px, cur_y, tools_w, pill_h); px += tools_w + gap;
    fv_ui_place(rects, W_MIC, px, cur_y, tools_w, pill_h); px += tools_w + gap;
    fv_ui_place(rects, W_REC, px, cur_y, tools_w, pill_h); px += tools_w + gap;
    fv_ui_place(rects, W_VIDEO_REC, px, cur_y, tools_w, pill_h); px += tools_w + gap;
    fv_ui_place(rects, W_SESS, px, cur_y, tools_w, pill_h);

    if (out) {
        *out = (FvUiAdvDrawerGeom){
            .drawer_h = h,
            .drawer_y = y,
            .header_h = header_h,
            .close_w = close_w,
            .close_h = close_h,
            .close_x = close_x,
            .close_y = close_y,
            .cur_y = y + header_h + 4.0f * d,
            .gap = gap,
            .pill_h = pill_h,
            .p4_w = p4_w,
            .p5_w = p5_w,
            .tools_w = tools_w,
        };
    }
}
