/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
/* FieldVox Lab C23 NativeActivity: sensor-derived fragments, on-demand radio
 * clips, persistent offline playback, camera monitoring and aligned recording. */

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <android/window.h>
#include <aaudio/AAudio.h>

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fv_android_platform.h"
#include "fv_gfx.h"
#include "fv_ui_layout.h"
#include "fv_android_text.h"
#include "fv_hit_capture.h"
#include "fv_vox_controls.h"
#include "fv_recording_data.h"
#include "fv_local_player.h"
#include "fv_native_audio.h"
#include "fv_native_camera.h"
#include "fv_native_radio.h"
#include "fv_native_video_recorder.h"
#include "fv_station_blob_data.h"
#include "fv_sensor_dsp.h"
#include "fv_local_bank.h"
#include "fv_fragment_engine.h"
#include "fv_fragment_store.h"
#include "fv_platform_voices.h"

#define TAG "FieldVoxNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 48000
#define BUFFER_FRAMES 1024
#define CHANNEL_RING ((int)FV_STATION_BLOB_COUNT) /* visible logical channel counter, bounded to the embedded station catalog */
#define DWELL_ADVANCE_MS 6000L     /* Java stage1UNAudioDwellPeriodMs */
#define MIC_MONITOR_GAIN 3.0f      /* Java liveOutputGain */
#define WF_WIDTH 120
#define WF_HEIGHT 64
#define ASENSOR_TYPE_PRESSURE_ID 6
#define ASENSOR_TYPE_PROXIMITY_ID 8
#define EVIDENCE_MAX 4
#define TUNER_PAGE_SIZE 5

static const int FV_SWEEP_CADENCE_MS[4] = {100, 250, 500, 1000};

/* ---------------------------------------------------------------------------------------------
 * Widgets & Enums
 * ------------------------------------------------------------------------------------------- */

typedef enum {
    FV_SWEEP_REV = -1,
    FV_SWEEP_RND = 0,
    FV_SWEEP_FWD = 1
} FvSweepDirection;

typedef enum {
    FV_PITCH_OVERRIDE_AUTO = 0,
    FV_PITCH_OVERRIDE_LOW = 1,
    FV_PITCH_OVERRIDE_NORMAL = 2,
    FV_PITCH_OVERRIDE_HIGH = 3
} FvPitchOverride;

typedef struct {
    char path[512];
    char name[256];
    char detail[80];
    char mime[24];
    time_t modified;
} EvidenceEntry;

/* ---------------------------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    ANativeActivity *activity;
    ANativeWindow *window;
    AInputQueue *input_queue;
    float density;
    FvTextScale text_scale;

    pthread_t render_thread, sensor_thread, audio_thread, input_thread, camera_thread;
    bool render_thread_started, sensor_thread_started, audio_thread_started;
    bool input_thread_started, camera_thread_started;
    atomic_bool running;
    atomic_bool resumed;
    atomic_uint mark_hit_requests;

    /* Sensor telemetry (Java nativeUpdateSensorTelemetry model). */
    float emf_magnitude, emf_delta, emf_baseline, emf_cal_offset;
    bool emf_spike;
    float gyro_baseline;
    float vibe_magnitude;
    bool vibe_spike;
    float accel_gx, accel_gy, accel_gz;
    float proximity_last;
    bool proximity_near;
    float baro_current, baro_delta;
    bool baro_coldspot;
    float compass_azimuth;
    int rem_pod_tier;
    long cal_flash_until_ms;

    /* Gate requests raised by the sensor thread, consumed by the audio thread. */
    bool proximity_pending;
    FvVoxDecimalTrigger proximity_trigger;
    FvVoxDecimalTrigger pending_fragment;
    FvGateReason pending_fragment_reason;
    bool fragment_pending, fragment_fetching, local_only, cached_source;
    unsigned fragment_cache_count;

    /* Controls */
    bool sweep_active;
    bool auto_gate;
    FvHoldGesture vis_hold;
    bool voice_duck, voice_detected, voice_input_ready;
    int echo_percent, reverb_percent, distortion_percent, grain_ms;
    int visualizer_style, visualizer_color;
    int cadence_idx;
    FvSweepDirection direction;
    int pitch_semitones;     /* Rounded telemetry of the active pitch ratio. */
    FvPitchOverride pitch_override;
    bool echo_fx, reverb_fx, noise_fx, reverse_fx;
    bool sb11_dual, vocal_filter, rem_pod_active, spatial_pan;
    bool cam_hud_active, cam_hud_red;
    FvCameraFilter cam_filter;
    int display_rotation_deg;
    bool video_recording_active;
    bool window_focused, awaiting_camera_permission;
    long clean_start_deadline_ms;
    FvVideoFrameSource video_source;
    char video_artifact[512];
    bool monitor_active;
    bool torch_active;
    bool audio_muted;
    FvUiState ui;
    int session_hit_count;
    long hit_marker_until_ms;
    long last_spike_vibrate_ms;
    int evidence_count;
    size_t evidence_page, evidence_total;
    int tuner_page;
    int64_t playback_duration_ms;
    EvidenceEntry evidence[EVIDENCE_MAX];
    float threshold;         /* uT, Java default 1.65 */
    int cooldown_ms;         /* Java default 450 */
    int estes_hit_count;

    /* Sweep status for the UI */
    int channel_counter;
    long last_channel_tick_ms;
    int carrier_station;
    int carrier_b_station;
    bool carrier_ready;
    bool gate_open;
    FvGateReason gate_reason;
    int live_decks;
    unsigned packets_opened;

    /* Visuals */
    float spectrum[FV_NATIVE_SPECTRUM_BINS];
    float vis_smooth[FV_NATIVE_SPECTRUM_BINS];
    float waveform[FV_NATIVE_SPECTRUM_BINS];
    uint32_t waterfall[WF_WIDTH * WF_HEIGHT];
    bool audio_running;

    /* Layout and scrolling */
    WidgetRect widgets[W_COUNT];
    float scroll_y;
    float max_scroll;

    char status_message[160];
    long status_until_ms;
} FieldVoxApp;

static FieldVoxApp g_app;
static FvRecordingCatalog g_catalog;
static pthread_mutex_t g_window_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_record_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_render_mutex = PTHREAD_MUTEX_INITIALIZER;
static ALooper *g_input_looper;
static FvMarkerRecorder g_vid_markers;
static long g_vid_rec_start_ms;
static bool g_vid_markers_active;
static uint32_t g_camera_source[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];
static uint32_t g_camera_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];

static void save_recording_markers(const char *path, const FvMarkerRecorder *rec) {
    if (path && path[0] && rec->timeline.count > 0u) {
        if (!fv_timeline_save(path, &rec->timeline)) {
            LOGW("Could not save auto-markers for %s", path);
        } else {
            LOGI("Saved %zu timeline markers for %s", rec->timeline.count, path);
        }
    }
}

static void stop_video_recording_locked(char *out_path, size_t out_cap) {
    pthread_mutex_lock(&g_record_mutex);
    FvMarkerRecorder markers = g_vid_markers;
    g_vid_markers_active = false;
    pthread_mutex_unlock(&g_record_mutex);
    char saved_path[512] = {0};
    fv_video_recorder_stop(saved_path, sizeof(saved_path));
    g_app.video_recording_active = false;
    save_recording_markers(saved_path, &markers);
    if (out_path && out_cap) snprintf(out_path, out_cap, "%s", saved_path);
}

static void record_active_timeline_marker(long now_ms, unsigned kinds) {
    pthread_mutex_lock(&g_record_mutex);
    if (g_vid_markers_active) {
        fv_marker_recorder_record(&g_vid_markers, (int64_t)(now_ms - g_vid_rec_start_ms), kinds);
    }
    pthread_mutex_unlock(&g_record_mutex);
}

static long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static long long get_wall_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static const FvBlobStationEntry *station_at(int index) {
    if (index < 0) index = 0;
    index %= (int)FV_STATION_BLOB_COUNT;
    return (const FvBlobStationEntry *)(FV_STATION_PLAN_BINARY_DATA + 16 + (size_t)index * sizeof(FvBlobStationEntry));
}

static void station_callsign_at(int index, char *out, size_t cap) {
    if (!out || !cap) return;
    if (fv_radio_station_info(index, out, cap, NULL, 0u)) return;
    snprintf(out, cap, "%s", station_at(index)->callsign);
}

/* Caller must hold g_state_mutex. */
static void set_status_locked(const char *message) {
    snprintf(g_app.status_message, sizeof(g_app.status_message), "%s", message ? message : "");
    g_app.status_until_ms = get_time_ms() + 4000L;
}

static void set_status(const char *message) {
    pthread_mutex_lock(&g_state_mutex);
    set_status_locked(message);
    pthread_mutex_unlock(&g_state_mutex);
}

/* Caller holds g_state_mutex. A permission prompt may temporarily pause the activity. */
static void clear_clean_start_locked(void) {
    g_app.clean_start_deadline_ms = 0;
    g_app.awaiting_camera_permission = false;
}

static void start_video_recording_locked(void) {
    clear_clean_start_locked();
    pthread_mutex_lock(&g_window_mutex);
    const bool have_window = g_app.window != NULL;
    const bool landscape = have_window &&
        ANativeWindow_getWidth(g_app.window) > ANativeWindow_getHeight(g_app.window);
    pthread_mutex_unlock(&g_window_mutex);
    if (!have_window || !atomic_load(&g_app.resumed)) {
        set_status_locked("Recording needs a visible viewfinder");
        return;
    }

    const char *base = g_app.activity->externalDataPath ? g_app.activity->externalDataPath
                                                       : g_app.activity->internalDataPath;
    char dir[512], path[512];
    if (!base || snprintf(dir, sizeof(dir), "%s/fieldvox-recordings", base) >= (int)sizeof(dir) ||
        (mkdir(dir, 0755) != 0 && errno != EEXIST) ||
        snprintf(path, sizeof(path), "%s/session-%lld.mp4", dir, get_wall_time_ms()) >= (int)sizeof(path)) {
        set_status_locked("Recording storage is unavailable");
        return;
    }

    const int width = landscape ? 1280 : 720;
    const int height = landscape ? 720 : 1280;
    if (!fv_video_recorder_start(path, width, height, SAMPLE_RATE, 1, g_app.video_source)) {
        set_status_locked("Video recording failed to start");
        return;
    }
    g_app.video_recording_active = true;
    snprintf(g_app.video_artifact, sizeof(g_app.video_artifact), "%s", path);
    pthread_mutex_lock(&g_record_mutex);
    fv_marker_recorder_reset(&g_vid_markers);
    g_vid_rec_start_ms = get_time_ms();
    g_vid_markers_active = true;
    pthread_mutex_unlock(&g_record_mutex);
    set_status_locked(g_app.video_source == FV_VIDEO_CAMERA_ONLY ? "Clean MP4 recording started" : "Screen MP4 recording started");
}

static const char *cadence_label(int idx) {
    static const char *const labels[4] = {"100ms", "250ms", "500ms", "1.0s"};
    return labels[idx & 3];
}

static const char *direction_label(FvSweepDirection direction) {
    return direction == FV_SWEEP_FWD ? "FWD" : (direction == FV_SWEEP_REV ? "REV" : "RND");
}

/* ---------------------------------------------------------------------------------------------
 * Rendering (layout mirrors the Java onCreate view tree)
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    FvCanvas *c;
    float d;        /* px per dp */
    float scroll;   /* content -> screen offset */
    float x0, w;    /* content column */
    WidgetRect *rects;
    const FvTextScale *text_scale;
} Ui;

static float dp(const Ui *ui, float v) { return v * ui->d; }
static float text_sp(const Ui *ui,float v) { return fv_text_sp(ui->text_scale,v); }

/* Largest font size <= size_px that fits `max_w`. */
static float fit_size(int font, float size_px, const char *text, float max_w, bool bold) {
    float s = size_px;
    for (int i = 0; i < 12 && s > 6.0f; i++) {
        if (fvg_text_width(font, s, text, bold) <= max_w) break;
        s *= 0.93f;
    }
    return s;
}

typedef struct {
    uint32_t bg, stroke, text;
} PillStyle;

static const PillStyle PILL_IDLE = {0xE8181818, 0xFF555555, 0xFFEAEAEA};
static const PillStyle PILL_ACTIVE = {0xFFF0F0F0, 0xFFFFFFFF, 0xFF111111};

/* Action labels use platform medium weight at 14 sp; geometry remains dp. */
static void draw_placed_pill(Ui *ui, WidgetId id, const char *label, PillStyle st) {
    const WidgetRect *r = &ui->rects[id];
    const float sy = r->y - ui->scroll;
    fvg_round_rect(ui->c, r->x, sy, r->w, r->h, dp(ui, 8), st.bg, st.stroke, fmaxf(1.0f, dp(ui, 1)));
    const float size = fit_size(FVG_FONT_MEDIUM, text_sp(ui,14.0f), label, r->w - dp(ui, 6), false);
    fvg_text_center(ui->c, FVG_FONT_MEDIUM, size, r->x, sy, r->w, r->h, label, st.text, false);
}

/* Java LogoGlyphView: sensor ring, compass ticks, waveform core, antenna spine and field dots. */
static void __attribute__((unused)) draw_logo(Ui *ui, float cx, float cy, float size) {
    static const float amps[9] = {0.10f, -0.34f, 0.22f, -0.52f, 0.50f, -0.20f, 0.35f, -0.12f, 0.08f};
    FvCanvas *c = ui->c;
    const float r = size * 0.36f;
    const float stroke = fmaxf(1.5f, dp(ui, 2.0f));
    const float soft = fmaxf(1.0f, dp(ui, 1.0f));
    const uint32_t white = 0xFFFFFFFF, soft_col = 0x6EFFFFFF;
    fvg_circle(c, cx, cy, r, 0, white, stroke);
    fvg_circle(c, cx, cy, r * 0.63f, 0, soft_col, soft);
    fvg_line(c, cx, cy - r - size * 0.08f, cx, cy - r + size * 0.06f, stroke, white);
    fvg_line(c, cx, cy + r - size * 0.06f, cx, cy + r + size * 0.08f, stroke, white);
    fvg_line(c, cx - r - size * 0.08f, cy, cx - r + size * 0.06f, cy, stroke, white);
    fvg_line(c, cx + r - size * 0.06f, cy, cx + r + size * 0.08f, cy, stroke, white);
    const float dd = r * 0.73f, e = r * 1.08f;
    fvg_line(c, cx - e, cy - e, cx - dd, cy - dd, soft, soft_col);
    fvg_line(c, cx + dd, cy - dd, cx + e, cy - e, soft, soft_col);
    fvg_line(c, cx - e, cy + e, cx - dd, cy + dd, soft, soft_col);
    fvg_line(c, cx + dd, cy + dd, cx + e, cy + e, soft, soft_col);
    const float left = cx - r * 0.68f, step = (r * 1.36f) / 8.0f;
    for (int i = 1; i < 9; i++) {
        fvg_line(c, left + step * (float)(i - 1), cy + amps[i - 1] * r, left + step * (float)i, cy + amps[i] * r,
                 stroke, white);
    }
    fvg_line(c, cx, cy - r * 0.25f, cx, cy + r * 0.55f, soft, soft_col);
    fvg_circle(c, cx, cy, r * 0.08f, white, 0, 0);
    fvg_circle(c, cx - r * 0.48f, cy - r * 0.42f, r * 0.055f, white, 0, 0);
    fvg_circle(c, cx + r * 0.50f, cy + r * 0.34f, r * 0.055f, white, 0, 0);
    fvg_circle(c, cx + r * 0.38f, cy - r * 0.50f, r * 0.04f, white, 0, 0);
    fvg_circle(c, cx - r * 0.34f, cy + r * 0.48f, r * 0.04f, white, 0, 0);
}

/* Four-style audio visualizer; every trace is measured processed PCM. */
static const char *const visualizer_names[] = {"SPECTRUM", "WAVEFORM", "HISTORY", "WAVE FX"};
static const uint32_t visualizer_colors[] = {0xFFFFFFFFu, 0xFF6FE7FFu, 0xFFFFD166u, 0xFF76E7A2u};
static const char *const visualizer_color_names[] = {"WHITE", "CYAN", "GOLD", "GREEN"};

static void draw_siri_lobe(FvCanvas *c, float plot_x, float mid, float plot_w, float half_h,
                           const float *amps, int cols, uint32_t stroke_col, float d) {
    const float step = plot_w / (float)(cols > 1 ? cols - 1 : 1);
    float prev_x = plot_x;
    float prev_y_top = mid - amps[0] * half_h;
    float prev_y_bot = mid + amps[0] * half_h;
    for (int i = 1; i < cols; i++) {
        const float cx = plot_x + (float)i * step;
        const float y_top = mid - amps[i] * half_h;
        const float y_bot = mid + amps[i] * half_h;
        fvg_line(c, prev_x, prev_y_top, cx, y_top, 1.8f * d, stroke_col);
        fvg_line(c, prev_x, prev_y_bot, cx, y_bot, 1.8f * d, stroke_col);
        prev_x = cx;
        prev_y_top = y_top;
        prev_y_bot = y_bot;
    }
}

static void draw_floating_visualizer(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now_ms) {
    const float d = ui->d;
    FvCanvas *c = ui->c;
    const uint32_t color = visualizer_colors[s->visualizer_color % 4];
    const bool burst = s->vis_hold.tracking && s->vis_hold.triggered;
    const bool compact_clear = s->ui.vis_layout == FV_VIS_LAYOUT_COMPACT && !burst;
    const uint32_t card_fill = compact_clear ? 0u : 0xB8000000u;
    const uint32_t card_stroke = burst ? 0xFF6FE7FFu : (compact_clear ? 0x28FFFFFFu : 0x44FFFFFFu);
    fvg_round_rect(c, x, y, w, h, 14.0f*d, card_fill, card_stroke, burst ? 2.0f * d : d);
    const WidgetRect *pr = &ui->rects[W_CAM_PREVIEW];
    const WidgetRect *cr = &ui->rects[W_VIS_COLOR];
    const float plot_x = pr->x, plot_y = pr->y, plot_w = pr->w, plot_h = pr->h;
    if (s->visualizer_style == 3) {
        enum { SIRI_COLS = 48 };
        float low[SIRI_COLS], mid_l[SIRI_COLS], high[SIRI_COLS];
        static long animation_start_ms;
        if (!animation_start_ms) animation_start_ms = now_ms;
        fv_vis_siri_lobes(s->vis_smooth, (float)(now_ms - animation_start_ms) / 1000.0f,
                          SIRI_COLS, low, mid_l, high);
        const float mid = plot_y + plot_h * 0.5f;
        const float half_h = plot_h * 0.46f;
        fvg_line(c, plot_x, mid, plot_x + plot_w, mid, d, 0x88FFFFFFu);
        draw_siri_lobe(c, plot_x, mid, plot_w, half_h, low, SIRI_COLS, (color & 0x00FFFFFFu) | 0xAA000000u, d);
        draw_siri_lobe(c, plot_x, mid, plot_w, half_h, mid_l, SIRI_COLS, 0xCC6FE7FFu, d);
        draw_siri_lobe(c, plot_x, mid, plot_w, half_h, high, SIRI_COLS, 0xEEFFFFFFu, d);
    } else if (s->visualizer_style == 2) {
        static uint32_t tinted_history[WF_WIDTH*WF_HEIGHT];
        for (int i=0; i<WF_WIDTH*WF_HEIGHT; i++) {
            const uint32_t level = s->waterfall[i]&255u;
            const uint32_t red = ((color>>16)&255u)*level/255u;
            const uint32_t green = ((color>>8)&255u)*level/255u;
            const uint32_t blue = (color&255u)*level/255u;
            tinted_history[i] = 0xFF000000u | (red<<16) | (green<<8) | blue;
        }
        fvg_blit_scaled(c, tinted_history, WF_WIDTH, WF_HEIGHT, plot_x, plot_y, plot_w, plot_h);
    } else if (s->visualizer_style == 1) {
        const float mid = plot_y + plot_h*0.5f;
        fvg_line(c, plot_x, mid, plot_x+plot_w, mid, d, 0x33FFFFFF);
        for (int i=1; i<FV_NATIVE_SPECTRUM_BINS; i++) {
            const float step = plot_w / (float)(FV_NATIVE_SPECTRUM_BINS-1);
            fvg_line(c, plot_x+(float)(i-1)*step, mid-s->waveform[i-1]*plot_h*0.45f,
                     plot_x+(float)i*step, mid-s->waveform[i]*plot_h*0.45f, 1.8f*d, color);
        }
    } else {
        const float step = plot_w/32.0f;
        for (int i=0; i<32; i++) {
            const float v = fmaxf(0.015f, fminf(1.0f, s->spectrum[i*2]));
            const float bh = v*plot_h;
            fvg_round_rect(c, plot_x+(float)i*step, plot_y+plot_h-bh,
                           fmaxf(d, step-2.0f*d), bh, d, color, 0, 0);
        }
    }
    draw_placed_pill(ui, W_VIS_PREV, "<", PILL_IDLE);
    draw_placed_pill(ui, W_VIS_NEXT, ">", PILL_IDLE);
    const char *vis_lbl = burst ? "HOLD BURST" : visualizer_names[s->visualizer_style % 4];
    fvg_text_center(c, FVG_FONT_SANS, text_sp(ui,12.0f), plot_x, cr->y, plot_w*0.60f, cr->h,
                    vis_lbl, burst ? 0xFF6FE7FFu : color, burst);
    draw_placed_pill(ui, W_VIS_COLOR, visualizer_color_names[s->visualizer_color % 4], PILL_IDLE);
}

static void draw_filtered_camera(FvCanvas *canvas, const FieldVoxApp *state) {
    int rotation = 0;
    bool front_facing = false, torch_on = false;
    int width = FV_CAMERA_WIDTH, height = FV_CAMERA_HEIGHT;
    if (fv_camera_copy_argb(g_camera_source, state->display_rotation_deg, &rotation, &front_facing, &torch_on) &&
        fv_camera_filter_frame_ex(g_camera_source, FV_CAMERA_WIDTH, FV_CAMERA_HEIGHT, g_camera_argb,
                                  (size_t)FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT, rotation,
                                  front_facing, state->cam_filter, torch_on, &width, &height)) {
        fvg_blit_cover(canvas, g_camera_argb, width, height);
    } else {
        fvg_clear(canvas, 0xFF060B11);
    }
}

static void draw_viewfinder_background(FvCanvas *c, const FieldVoxApp *s, int w, int h,
                                       float d, long now, bool camera_drawn) {
    if (s->cam_filter != FV_CAM_FILTER_OFF) {
        if (!camera_drawn) draw_filtered_camera(c, s);

        /* Viewfinder reticle and perimeter recording indicator */
        const float rx = (float)w * 0.5f, ry = (float)h * 0.38f, r = 18.0f * d;
        uint32_t ret_col = 0x336FE7FF;
        if (s->video_recording_active) {
            const float rpulse = 0.5f + 0.5f * sinf((float)(now % 1000) * (2.0f * 3.14159265f / 1000.0f));
            ret_col = ((uint32_t)(90.0f + 140.0f * rpulse) << 24) | 0x00FF1744;

            /* Subtle perimeter recording frame */
            fvg_round_rect(c, 4.0f * d, 4.0f * d, (float)w - 8.0f * d, (float)h - 8.0f * d,
                           12.0f * d, 0, ret_col, fmaxf(1.5f, 2.0f * d));
        }

        fvg_circle(c, rx, ry, r, 0, ret_col, fmaxf(1.0f, d));
        fvg_line(c, rx - r * 1.8f, ry, rx - r * 0.6f, ry, fmaxf(1.0f, d), ret_col);
        fvg_line(c, rx + r * 0.6f, ry, rx + r * 1.8f, ry, fmaxf(1.0f, d), ret_col);
        fvg_line(c, rx, ry - r * 1.8f, rx, ry - r * 0.6f, fmaxf(1.0f, d), ret_col);
        fvg_line(c, rx, ry + r * 0.6f, rx, ry + r * 1.8f, fmaxf(1.0f, d), ret_col);

        /* Corner brackets */
        const float bw = 24.0f * d, bh = 24.0f * d, m = 18.0f * d;
        fvg_line(c, m, m, m + bw, m, d, ret_col);
        fvg_line(c, m, m, m, m + bh, d, ret_col);
        fvg_line(c, (float)w - m - bw, m, (float)w - m, m, d, ret_col);
        fvg_line(c, (float)w - m, m, (float)w - m, m + bh, d, ret_col);
    } else {
        fvg_clear(c, 0xFF050910);
        /* Atmospheric radar circles */
        const float rx = (float)w * 0.5f, ry = (float)h * 0.38f;
        for (int ri = 1; ri <= 3; ri++) {
            fvg_circle(c, rx, ry, (float)ri * 45.0f * d, 0, 0x126FE7FF, d);
        }
        const float ang = (float)(now % 4000) / 4000.0f * 6.2831853f;
        fvg_line(c, rx, ry, rx + cosf(ang) * 135.0f * d, ry + sinf(ang) * 135.0f * d, d, 0x226FE7FF);
    }
}

static void draw_top_header(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now) {
    const float d = ui->d;
    FvCanvas *c = ui->c;
    const float row_h = ui->rects[W_MUTE].h;
    fvg_round_rect(c, x, y, w, h, 12.0f*d, 0xBD000000, 0, 0);
    const float title_w = fmaxf(30.0f*d, ui->rects[W_MUTE].x - x - 8.0f*d);
    const float title_size = fit_size(FVG_FONT_SANS, text_sp(ui,16.0f), "FIELDVOX", title_w, true);
    fvg_text_center(c, FVG_FONT_SANS, title_size, x+4.0f*d, y, title_w, row_h, "FIELDVOX", 0xFFFFFFFF, true);
    draw_placed_pill(ui, W_MUTE, s->audio_muted ? "MUTED" : "MUTE",
                     s->audio_muted ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_TORCH, "TORCH", s->torch_active ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_HELP, "?", PILL_IDLE);
    char detail[160];
    char callsign[32] = {0};
    if (s->carrier_station >= 0) station_callsign_at(s->carrier_station, callsign, sizeof(callsign));
    if (s->video_recording_active) {
        const uint32_t seconds = fv_video_recorder_get_duration_ms()/1000u;
        snprintf(detail, sizeof(detail), "REC %02u:%02u  |  %s", seconds/60u, seconds%60u,
                 s->carrier_ready && callsign[0] ? callsign : "CONNECTING");
    } else {
        snprintf(detail, sizeof(detail), "%s  |  %s%s", s->local_only ? "LOCAL" : (s->cached_source ? "CACHED RADIO" : "LIVE RADIO"),
                 !s->sweep_active ? "PAUSED" : (callsign[0] ? callsign : (s->local_only ? "WAV / CACHE" : "CONNECTING")),
                 s->voice_detected ? "  |  SPEAKING MUTE" : "");
    }
    const float size = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), detail, w-16.0f*d, false);
    fvg_text_center(c, FVG_FONT_SANS, size, x+8.0f*d, y+row_h, w-16.0f*d, h-row_h, detail,
                    s->video_recording_active && (now/500L)%2 == 0
                        ? 0xFFFF6677 : 0xFFE0E0E0, false);
}

static void draw_perimeter_badges(Ui *ui, const FieldVoxApp *s, float y, float h, long now_ms) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float gap = 6.0f * d;
    const float half_w = (ui->w - gap) * 0.5f;

    /* Left Badge: EMF Meter - tap to zero-calibrate! */
    const float lx = ui->x0;
    const bool cal = now_ms < s->cal_flash_until_ms;
    const uint32_t emf_stroke = cal ? 0xFF00E676 : (s->emf_spike ? 0xFFFF1744 :
                                (s->emf_delta >= 1.5f ? 0xFFFFD600 : 0x55FFFFFF));
    const uint32_t emf_bg = s->emf_spike ? 0xDD280509 : 0xA8000000;
    fvg_round_rect(c, lx, y, half_w, h, 6.0f * d, emf_bg, emf_stroke, fmaxf(1.0f, d));

    const char *preset_str = "[MED]";
    if (s->threshold >= 2.5f) preset_str = "[LOW]";
    else if (s->threshold < 1.2f) preset_str = "[HIGH]";

    char emf_str[64];
    snprintf(emf_str, sizeof(emf_str), "EMF %.1f µT %s%s", s->emf_magnitude, preset_str,
             cal ? " [OK]" : (s->emf_spike ? " [!]" : ""));
    const uint32_t emf_text_col = s->emf_spike ? 0xFFFF5252 : (cal ? 0xFF00E676 : 0xFFFFFFFF);
    const float es = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), emf_str, half_w * 0.65f, false);
    fvg_text(c, FVG_FONT_SANS, es, lx + 8.0f * d, y + h * 0.65f, emf_str, emf_text_col, false);

    /* 5-LED dynamic indicator inside the EMF badge */
    const int leds = 5;
    const float led_start_x = lx + half_w * 0.67f;
    const float led_area_w = half_w - (half_w * 0.67f) - 6.0f * d;
    const float led_gap = 2.0f * d;
    const float led_w = (led_area_w - led_gap * (float)(leds - 1)) / (float)leds;
    const float led_h = h - 10.0f * d;
    const float led_y = y + 5.0f * d;
    int active_leds = (int)fminf(5.0f, fmaxf(1.0f, s->emf_delta / 2.0f * 5.0f));
    for (int i = 0; i < leds; i++) {
        uint32_t lcol = 0x30182430;
        if (i < active_leds) {
            lcol = i < 2 ? 0xFF00E676 : (i < 4 ? 0xFFFFD600 : 0xFFFF1744);
        }
        fvg_round_rect(c, led_start_x + (float)i * (led_w + led_gap), led_y, led_w, led_h, 1.5f * d, lcol, 0, 0);
    }

    /* Right Badge: Environment Telemetry - tap to toggle REM-POD! */
    const float rx = ui->x0 + half_w + gap;
    char telem[96];
    uint32_t tel_bg = 0xA8000000;
    uint32_t tel_stroke = 0x55FFFFFF;
    uint32_t tel_text = 0xFFE0E0E0;

    if (s->baro_coldspot) {
        tel_bg = 0xDD042036;
        tel_stroke = 0xFFEAEAEA;
        tel_text = 0xFFEAEAEA;
        snprintf(telem, sizeof(telem), "COLD SPOT (%.1f hPa)", s->baro_current);
    } else if (s->rem_pod_active && s->rem_pod_tier > 0) {
        tel_bg = 0xDD300508;
        tel_stroke = 0xFFFF1744;
        tel_text = 0xFFFF5252;
        snprintf(telem, sizeof(telem), "REM-POD TIER %d ALERT", s->rem_pod_tier);
    } else if (s->vibe_spike) {
        tel_bg = 0xDD301805;
        tel_stroke = 0xFFFF9100;
        tel_text = 0xFFFFAB40;
        snprintf(telem, sizeof(telem), "VIBE SPIKE (%.2f m/s²)", s->vibe_magnitude);
    } else if (s->rem_pod_active) {
        tel_bg = 0xDD121C30;
        tel_stroke = 0xFFEAEAEA;
        tel_text = 0xFFFFFFFF;
        snprintf(telem, sizeof(telem), "REM-POD ARMED • DIR %.0f°", s->compass_azimuth);
    } else {
        snprintf(telem, sizeof(telem), "BARO %.1f hPa • DIR %.0f°", s->baro_current, s->compass_azimuth);
    }

    fvg_round_rect(c, rx, y, half_w, h, 6.0f * d, tel_bg, tel_stroke, fmaxf(1.0f, d));
    const float ts = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), telem, half_w - 12.0f * d, false);
    fvg_text_center(c, FVG_FONT_SANS, ts, rx, y, half_w, h, telem, tel_text, false);
}

/* FieldVox style camera dock with prominent circular record shutter button */
static void draw_bottom_action_bar(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float r_outer = (h >= 52.0f * d ? 28.0f : 21.0f) * d;
    const WidgetRect *r1 = &ui->rects[W_SESS];
    const WidgetRect *r2 = &ui->rects[W_MARK_HIT];
    const WidgetRect *r3 = &ui->rects[W_CAM_TINT];
    const WidgetRect *r4 = &ui->rects[W_CTRL];
    const float btn_h = r1->h, btn_y = r1->y, btn_w = r1->w;

    /* --- Left Flank: LIBRARY & MARK HIT --- */
    {
        const float b1_x = r1->x;
        const uint32_t b1_bg = 0xC0181818;
        const uint32_t b1_stroke = 0x77FFFFFF;
        fvg_round_rect(c, b1_x, btn_y, btn_w, btn_h, 10.0f * d, b1_bg, b1_stroke, fmaxf(1.0f, 1.2f * d));
        fvg_text_center(c, FVG_FONT_SANS, fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), "LIBRARY", btn_w-8.0f*d, true),
                        b1_x, btn_y, btn_w, btn_h, "LIBRARY", 0xFFFFFFFF, true);

        /* Button 2: W_MARK_HIT (Pre-Roll Lossless Audio Capture) */
        const float b2_x = r2->x;
        fvg_round_rect(c, b2_x, btn_y, btn_w, btn_h, 10.0f * d, 0xD0241406, 0xFFFF9100, fmaxf(1.0f, 1.2f * d));
        char hit_lbl[32];
        if (s->session_hit_count > 0) {
            snprintf(hit_lbl, sizeof(hit_lbl), "HIT [%02d]", s->session_hit_count);
        } else {
            snprintf(hit_lbl, sizeof(hit_lbl), "MARK HIT");
        }
        const float s2 = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), hit_lbl, btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s2, b2_x, btn_y, btn_w, btn_h, hit_lbl, 0xFFFFD180, true);
    }

    /* --- Center: FieldVox recording shutter --- */
    {

        /* Translucent dark glass disc under the shutter */
        fvg_circle(c, cx, cy, r_outer + 2.0f * d, 0x88000000, 0, 0);
        fvg_circle(c, cx, cy, r_outer, 0xDD081018, 0, 0);

        if (!s->video_recording_active) {
            /* IDLE STATE: Crisp white concentric ring with vivid red inner shutter circle */
            fvg_circle(c, cx, cy, r_outer, 0, 0xFFFFFFFF, fmaxf(2.0f, 3.2f * d));

            const float r_inner = r_outer - 6.0f * d;
            fvg_circle(c, cx, cy, r_inner, 0xFFFF1744, 0x44000000, fmaxf(1.0f, 1.2f * d));
            /* Subtle glossy 3D sheen */
            fvg_circle(c, cx, cy - r_inner * 0.35f, r_inner * 0.45f, 0x22FFFFFF, 0, 0);
            const char *label = s->clean_start_deadline_ms ? "WAIT" : "REC";
            fvg_text_center(c, FVG_FONT_SANS, text_sp(ui,12.0f), cx - r_inner, cy - r_inner,
                            r_inner * 2.0f, r_inner * 2.0f, label, 0xFFFFFFFF, true);
        } else {
            /* RECORDING STATE: Pulsating red aura, morphing into rounded red stop square */
            const float pulse = 0.5f + 0.5f * sinf((float)(now % 900) * (2.0f * 3.14159265f / 900.0f));

            /* Pulsing outer aura ring */
            const float aura_r = r_outer + (3.0f + 4.0f * pulse) * d;
            const uint32_t aura_col = ((uint32_t)(70.0f + 140.0f * pulse) << 24) | 0x00FF1744;
            fvg_circle(c, cx, cy, aura_r, 0, aura_col, 2.0f * d);

            /* Main outer ring pulses between white and bright red */
            const uint8_t gb = (uint8_t)(255.0f * (1.0f - pulse * 0.85f));
            const uint32_t ring_stroke = 0xFF000000 | (0xFF << 16) | (gb << 8) | gb;
            fvg_circle(c, cx, cy, r_outer, 0, ring_stroke, fmaxf(2.0f, 3.2f * d));

            /* Inner Stop square */
            const float sq_size = r_outer * 0.82f;
            const float sq_half = sq_size * 0.5f;
            const float sq_rad = 5.0f * d;
            fvg_round_rect(c, cx - sq_half, cy - sq_half, sq_size, sq_size, sq_rad,
                           0xFFFF1744, 0xFFFFFFFF, fmaxf(1.5f, 2.0f * d));
            fvg_text_center(c, FVG_FONT_SANS, text_sp(ui,12.0f), cx - sq_half, cy - sq_half,
                            sq_size, sq_size, "STOP", 0xFFFFFFFF, true);
        }
    }

    /* --- Right Flank: LENS (Spectral Filter) & SETTINGS (Drawer) --- */
    {
        /* Button 3: W_CAM_TINT (Spectral Lens Switcher) */
        const float b3_x = r3->x;
        fvg_round_rect(c, b3_x, btn_y, btn_w, btn_h, 10.0f * d, 0xC0181818, 0xFFEAEAEA, fmaxf(1.0f, 1.2f * d));

        char lens_name[32];
        const char *fname = fv_camera_filter_name(s->cam_filter);
        snprintf(lens_name, sizeof(lens_name), "%s", fname);
        const float s3 = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), lens_name, btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s3, b3_x, btn_y, btn_w, btn_h, lens_name, 0xFFFFFFFF, true);

        /* Button 4: W_CTRL (DSP & Paranormal Settings Drawer) */
        const float b4_x = r4->x;
        const uint32_t b4_bg = s->ui.controls_visible ? 0xDD280A35 : 0xC0181818;
        fvg_round_rect(c, b4_x, btn_y, btn_w, btn_h, 10.0f * d, b4_bg, 0xFFAAAAAA, fmaxf(1.0f, 1.2f * d));
        const char *b4_label = s->ui.controls_visible ? "CLOSE" : "CONTROLS";
        const float s4 = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), b4_label, btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s4, b4_x, btn_y, btn_w, btn_h, b4_label, 0xFFFFFFFF, false);
    }
    draw_placed_pill(ui, W_CAPTURE_CLEAN, "CLEAN MP4", s->video_source == FV_VIDEO_CAMERA_ONLY ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_CAPTURE_SCREEN, "SCREEN MP4", s->video_source == FV_VIDEO_SCREEN ? PILL_ACTIVE : PILL_IDLE);
}

static void draw_placed_slider(Ui *ui, WidgetId id, const char *label, int level, const char *value) {
    const float d = ui->d;
    const WidgetRect *r = &ui->rects[id];
    const float x = r->x, y = r->y, w = r->w, h = r->h;
    fvg_text(ui->c, FVG_FONT_SANS, text_sp(ui,12.0f), x+8.0f*d, y+h*0.38f, label, 0xFFFFFFFF, false);
    fvg_text_center(ui->c, FVG_FONT_SANS, text_sp(ui,8.0f), x+w*0.70f, y, w*0.30f, h*0.45f, value, 0xFFCCCCCC, false);
    const float left = x+w*0.12f, right = x+w*0.88f, line_y = y+h*0.72f;
    const float knob = left+(right-left)*(float)level/100.0f;
    fvg_line(ui->c, left, line_y, right, line_y, 2.0f*d, 0xFF444444);
    fvg_line(ui->c, left, line_y, knob, line_y, 2.0f*d, 0xFFF0F0F0);
    fvg_circle(ui->c, knob, line_y, 5.0f*d, 0xFFFFFFFF, 0, 0);
}

static void draw_vox_drawer(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h) {
    const float d = ui->d;
    fvg_round_rect(ui->c, x, y, w, h, 16.0f*d, 0xF50D0D0D, 0xFF666666, d);
    fvg_text_center(ui->c, FVG_FONT_SANS, text_sp(ui,14.0f), x, y+4.0f*d, w-44.0f*d, 32.0f*d,
                    "FIELDVOX • AUDIO CONTROLS", 0xFFFFFFFF, false);
    draw_placed_pill(ui, W_CLOSE_DRAWER, "X", PILL_IDLE);
    char value[48];
    snprintf(value, sizeof(value), "%d%%", s->echo_percent);
    draw_placed_slider(ui, W_ECHO_LEVEL, "Echo", s->echo_percent, value);
    snprintf(value, sizeof(value), "%d%%", s->reverb_percent);
    draw_placed_slider(ui, W_REVERB_LEVEL, "Reverb", s->reverb_percent, value);
    snprintf(value, sizeof(value), "%d%%", s->distortion_percent);
    draw_placed_slider(ui, W_DISTORTION_LEVEL, "Distortion", s->distortion_percent, value);
    snprintf(value, sizeof(value), "%d ms", s->grain_ms);
    draw_placed_slider(ui, W_GRAIN_LEVEL, "Radio sweep rate", fv_ui_grain_ms_to_percent(s->grain_ms), value);
    draw_placed_pill(ui, W_START, s->sweep_active ? "RUNNING" : "STOPPED", s->sweep_active ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_NOISE, "WHITE NOISE", s->noise_fx ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_VOICE_DUCK, "SPEAKING MUTE", s->voice_duck ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_AUTO_GATE, s->auto_gate ? "AUTO" : "SENSOR", !s->auto_gate ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_RADIO_TUNER, "TUNER", s->ui.tuner_visible ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_ADVANCED, "ADVANCED", PILL_IDLE);
    draw_placed_pill(ui, W_SOURCE_MODE, s->local_only ? "LOCAL" : "LIVE", s->local_only ? PILL_ACTIVE : PILL_IDLE);
}

static void draw_slide_up_drawer(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now) {
    if (!s->ui.advanced_controls) { draw_vox_drawer(ui, s, x, y, w, h); return; }
    FvCanvas *c = ui->c;
    const float d = ui->d;

    /* Semi-opaque backdrop shadow & card */
    fvg_round_rect(c, x, y, w, h, 14.0f * d, 0xF5081019, 0xFF6FE7FF, fmaxf(1.0f, d * 1.5f));

    /* Drawer Header */
    const float header_h = ui->rects[W_ADVANCED].h;
    const char *header = "ADVANCED • TAP TITLE FOR AUDIO";
    fvg_text_center(c, FVG_FONT_SANS, text_sp(ui,12.0f), x, y + 2.0f * d, w, header_h, header, 0xFF6FE7FF, true);

    /* Close Button in Header */
    draw_placed_pill(ui, W_CLOSE_DRAWER, "X", PILL_IDLE);

    /* Row 1: Sweep Controls (Cadence, Direction, Pitch, Cal) */
    {
        const bool cal = now < s->cal_flash_until_ms;
        PillStyle cal_style = PILL_IDLE;
        if (cal) cal_style.text = 0xFF00E676;
        PillStyle pitch_style = PILL_IDLE;
        const char *pitch_label = s->pitch_override == FV_PITCH_OVERRIDE_AUTO ? "PITCH AUTO" :
            (s->pitch_override == FV_PITCH_OVERRIDE_LOW ? "LOW" :
            (s->pitch_override == FV_PITCH_OVERRIDE_NORMAL ? "NORMAL" : "HIGH"));
        if (s->pitch_semitones < 0) {
            pitch_style = (PillStyle){0xFF0C1F2D, 0xFF6FE7FF, 0xFF6FE7FF};
        } else if (s->pitch_semitones > 0) {
            pitch_style = (PillStyle){0xFF2E0D1B, 0xFFFFFFFF, 0xFFFFFFFF};
        }
        draw_placed_pill(ui, W_CADENCE, cadence_label(s->cadence_idx), PILL_IDLE);
        draw_placed_pill(ui, W_DIRECTION, direction_label(s->direction), PILL_IDLE);
        draw_placed_pill(ui, W_PITCH, pitch_label, pitch_style);
        draw_placed_pill(ui, W_CAL, cal ? "CAL: OK" : "CAL ZERO", cal_style);
    }

    /* Row 2: FX Toggles */
    draw_placed_pill(ui, W_ECHO, "ECHO", s->echo_fx ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_REVERB, "REVERB", s->reverb_fx ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_NOISE, "NOISE", s->noise_fx ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_REV, "REV FX", s->reverse_fx ? PILL_ACTIVE : PILL_IDLE);

    /* Row 3: Specialized Modes */
    draw_placed_pill(ui, W_SOURCE_MODE, s->local_only ? "LOCAL" : "LIVE", s->local_only ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_VOCAL, "VOCAL", s->vocal_filter ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_PAN, "3D PAN", s->spatial_pan ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_REMPOD, "REM-POD", s->rem_pod_active ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui, W_ESTES, "ESTES", PILL_IDLE);
    draw_placed_pill(ui, W_AUTO_GATE, s->auto_gate ? "AUTO" : "SENSOR", s->auto_gate ? PILL_IDLE : PILL_ACTIVE);

    /* Row 4: plain-language sensor presets and one-tap cooldown cycle. */
    {
        const bool low = s->threshold >= 2.5f;
        const bool high = s->threshold < 1.2f;
        const bool med = !low && !high;
        draw_placed_pill(ui, W_SENS_LOW, "SENS LOW", low ? PILL_ACTIVE : PILL_IDLE);
        draw_placed_pill(ui, W_SENS_MED, "SENS MED", med ? PILL_ACTIVE : PILL_IDLE);
        draw_placed_pill(ui, W_SENS_HIGH, "SENS HIGH", high ? PILL_ACTIVE : PILL_IDLE);
        char c_str[48];
        snprintf(c_str, sizeof(c_str), "COOL %dms", s->cooldown_ms);
        draw_placed_pill(ui, W_COOL_CYCLE, c_str, PILL_IDLE);
    }

    /* Row 5: Camera, HUD and monitor controls. */
    {
        draw_placed_pill(ui, W_CAMHUD, s->cam_filter==FV_CAM_FILTER_OFF ? "CAM OFF" : "CAM ON",
                         s->cam_filter==FV_CAM_FILTER_OFF ? PILL_IDLE : PILL_ACTIVE);
        char cam_lbl[24], lens_name[16];
        fv_camera_copy_lens_label(fv_camera_current_lens(), lens_name, sizeof(lens_name));
        snprintf(cam_lbl, sizeof(cam_lbl), "CAM: %s", lens_name);
        draw_placed_pill(ui, W_CAM_LENS, cam_lbl, fv_camera_is_front_facing() ? PILL_ACTIVE : PILL_IDLE);
        static const char *const hud_labels[FV_VIS_LAYOUT_COUNT] = {"HUD: BTM", "HUD: TOP", "HUD: CLR"};
        const FvVisLayoutPreset preset = (unsigned)s->ui.vis_layout < FV_VIS_LAYOUT_COUNT
                                             ? s->ui.vis_layout
                                             : FV_VIS_LAYOUT_BOTTOM;
        draw_placed_pill(ui, W_VIS_LAYOUT, hud_labels[preset],
                         preset == FV_VIS_LAYOUT_BOTTOM ? PILL_IDLE : PILL_ACTIVE);
        draw_placed_pill(ui, W_MIC, s->monitor_active ? "MIC ON" : "MIC OFF",
                         s->monitor_active ? PILL_ACTIVE : PILL_IDLE);
    }
}

static void draw_help_overlay(Ui *ui, const FvUiScreenLayout *layout, float screen_h) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float x = ui->x0;
    const float w = ui->w;
    const float y = layout->help_y, h = layout->help_h;
    fvg_fill_rect(c, 0, 0, (float)c->w, screen_h, 0xB8000000);
    fvg_round_rect(c, x, y, w, h, 14.0f * d, 0xF70A121C, 0xFF6FE7FF, fmaxf(1.0f, 1.5f * d));
    fvg_text_center(c, FVG_FONT_SANS, text_sp(ui,16.0f), x, y + 8.0f * d, w, 30.0f * d,
                    "FIELDVOX QUICK GUIDE", 0xFFFFFFFF, true);
    const char *lines[] = {
        "Red REC: record Clean MP4 or Screen MP4",
        "MARK HIT: save clean + processed WAVs and source timeline",
        "LIBRARY: open saved WAV/MP4 captures in your system viewer",
        "Swipe left/right on camera: next/previous lens filter",
        "TORCH: light the scene without stopping camera or recording",
        "Tap EMF to zero sensors; tap the right badge for REM-POD",
        "CONTROLS: drag echo, reverb, distortion and fragment sliders",
        "SPEAKING MUTE: pauses new fragments during speech + 2 s",
        "Visualizer arrows: spectrum, waveform, history; tap color",
    };
    float ly = y + 52.0f * d;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        fvg_circle(c, x + 16.0f * d, ly - 3.0f * d, 2.3f * d, 0xFFEAEAEA, 0, 0);
        const float fs = fit_size(FVG_FONT_SANS, text_sp(ui,12.0f), lines[i], w - 38.0f * d, false);
        fvg_text(c, FVG_FONT_SANS, fs, x + 26.0f * d, ly, lines[i], 0xFFE0E0E0, false);
        ly += 32.0f * d;
    }
    draw_placed_pill(ui, W_CLOSE_HELP, "GOT IT", PILL_ACTIVE);
}

static void draw_evidence_overlay(Ui *ui, const FieldVoxApp *s, const FvUiScreenLayout *layout,
                                  const bool is_video[EVIDENCE_MAX], float screen_h) {
    FvCanvas *c=ui->c;const float d=ui->d,x=ui->x0,w=ui->w;
    const float y=layout->evidence_y,h=layout->evidence_h,row_h=layout->evidence_row_h;
    float row_y=layout->evidence_row_y;
    fvg_fill_rect(c,0,0,(float)c->w,screen_h,0xD0000000);
    fvg_round_rect(c,x,y,w,h,14.0f*d,0xFA101010,0xFF888888,d);
    fvg_text(c,FVG_FONT_SANS, text_sp(ui,16.0f),x+14.0f*d,y+29.0f*d,"LOCAL RECORDINGS",0xFFFFFFFF,true);
    draw_placed_pill(ui,W_CLOSE_EVIDENCE,"X",PILL_IDLE);
    fvg_text(c,FVG_FONT_SANS, text_sp(ui,12.0f),x+14.0f*d,y+48.0f*d,
              s->video_recording_active ? "Stop recording before replaying saved media" : "Saved on this phone • play without internet",0xFFBBBBBB,false);
    const float row_x=x+12.0f*d,row_w=w-24.0f*d;
    for (int i=0;i<s->evidence_count && i<EVIDENCE_MAX;i++) {
        const EvidenceEntry *entry=&s->evidence[i];const bool video=is_video[i];
        fvg_round_rect(c,row_x,row_y,row_w,row_h,8.0f*d,0xEE191919,0xFF444444,d);
        const float tag_w=44.0f*d,text_x=row_x+58.0f*d,text_w=row_w-188.0f*d;
        fvg_text_center(c,FVG_FONT_SANS, text_sp(ui,12.0f),row_x+6.0f*d,row_y,tag_w,row_h,video ? "MP4" : "WAV",0xFFE0E0E0,true);
        char name[sizeof(entry->name)+4u];
        fvg_text_elide_filename(FVG_FONT_SANS,text_sp(ui,16.0f),entry->name,text_w,name,sizeof(name));
        fvg_text(c,FVG_FONT_SANS,text_sp(ui,16.0f),text_x,row_y+row_h*0.40f,name,0xFFFFFFFF,false);
        char detail[sizeof(entry->detail)+4u];
        fvg_text_elide(FVG_FONT_SANS,text_sp(ui,11.0f),entry->detail,text_w,false,detail,sizeof(detail));
        fvg_text(c,FVG_FONT_SANS,text_sp(ui,11.0f),text_x,row_y+row_h*0.76f,detail,0xFFBBBBBB,false);
        if (!video) draw_placed_pill(ui,(WidgetId)(W_EVIDENCE_BANK_0+i),"BANK",PILL_IDLE);
        draw_placed_pill(ui,(WidgetId)(W_EVIDENCE_OPEN_0+i),"PLAY",PILL_ACTIVE);
        row_y+=row_h+5.0f*d;
    }
    if (!s->evidence_count) fvg_text_center(c,FVG_FONT_SANS, text_sp(ui,12.0f),row_x,row_y+30.0f*d,row_w,45.0f*d,"No saved recordings yet",0xFFBBBBBB,false);
    const float footer=ui->rects[W_LIBRARY_PREV].y;
    draw_placed_pill(ui,W_LIBRARY_PREV,"< PREV",PILL_IDLE);
    draw_placed_pill(ui,W_LIBRARY_REFRESH,"REFRESH",PILL_IDLE);
    draw_placed_pill(ui,W_LIBRARY_NEXT,"NEXT >",PILL_IDLE);
    char count[96];const size_t pages=(s->evidence_total+EVIDENCE_MAX-1)/EVIDENCE_MAX;
    snprintf(count,sizeof(count),"Page %zu / %zu • %zu recordings",pages ? s->evidence_page+1 : 0,pages,s->evidence_total);
    fvg_text_center(c,FVG_FONT_SANS, text_sp(ui,12.0f),row_x,footer-20.0f*d,row_w,18.0f*d,count,0xFFBBBBBB,false);
}

static void draw_tuner_overlay(Ui *ui, const FieldVoxApp *s, const FvUiScreenLayout *layout,
                               const FvRadioTunerSnapshot *tuner, float screen_h) {
    FvCanvas *c = ui->c;
    const float d = ui->d, x = ui->x0, w = ui->w;
    const float y = layout->tuner_y, h = layout->tuner_h;
    fvg_fill_rect(c, 0, 0, (float)c->w, screen_h, 0xD0000000);
    fvg_round_rect(c, x, y, w, h, 14.0f * d, 0xFA101010, 0xFF6FE7FF, d);
    char header[96];
    snprintf(header, sizeof(header), "RADIO TUNER • %zu STATIONS", tuner->total_stations);
    fvg_text(c, FVG_FONT_SANS, text_sp(ui, 14.0f), x + 14.0f * d, y + 27.0f * d, header, 0xFFFFFFFF, true);
    draw_placed_pill(ui, W_CLOSE_TUNER, "X", PILL_IDLE);

    char bank_lbl[32];
    snprintf(bank_lbl, sizeof(bank_lbl), "BANK: %s", fv_station_bank_label(tuner->bank));
    draw_placed_pill(ui, W_TUNER_BANK, bank_lbl, PILL_ACTIVE);
    draw_placed_pill(ui, W_TUNER_SHUFFLE, "SHUFFLE", PILL_IDLE);
    draw_placed_pill(ui, W_TUNER_REFRESH, tuner->refreshing ? "SYNCING…" : "SYNC LIVE",
                     tuner->refreshing ? PILL_ACTIVE : PILL_IDLE);

    const float row_x = ui->rects[W_TUNER_BANK].x;
    const float row_w = ui->rects[W_TUNER_NEXT].x + ui->rects[W_TUNER_NEXT].w - row_x;
    const int start = s->tuner_page * TUNER_PAGE_SIZE;
    for (int i = 0; i < TUNER_PAGE_SIZE && start + i < (int)tuner->group_count; i++) {
        const WidgetRect *r = &ui->rects[W_TUNER_PLAY_0 + i];
        if (!r->visible) continue;
        const int slot = start + i;
        const FvRadioSlotSnapshot *st = &tuner->slots[slot];
        const bool testing_this = tuner->testing && tuner->last_tested_slot == slot;
        fvg_round_rect(c, r->x, r->y, r->w, r->h, 8.0f * d, 0xEE181B22, 0xFF444A58, d);
        char title[96];
        snprintf(title, sizeof(title), "%d. %s [%s]", slot + 1, st->callsign, st->lang);
        char title_elided[100];
        fvg_text_elide(FVG_FONT_SANS, text_sp(ui, 13.0f), title, r->w - 92.0f * d, false,
                       title_elided, sizeof(title_elided));
        fvg_text(c, FVG_FONT_SANS, text_sp(ui, 13.0f), r->x + 10.0f * d, r->y + r->h * 0.43f,
                 title_elided, 0xFFFFFFFF, false);
        char sub[96];
        fvg_text_elide(FVG_FONT_SANS, text_sp(ui, 10.0f), st->name, r->w - 92.0f * d, false,
                       sub, sizeof(sub));
        fvg_text(c, FVG_FONT_SANS, text_sp(ui, 10.0f), r->x + 10.0f * d, r->y + r->h * 0.79f,
                 sub, 0xFF9AA3B2, false);

        const char *badge = "TAP TEST";
        uint32_t bcol = 0xFFBBBBBB;
        if (testing_this) {
            badge = "TESTING…";
            bcol = 0xFF6FE7FF;
        } else if (st->health == FV_STATION_OK) {
            badge = "OK";
            bcol = 0xFF00E676;
        } else if (st->health == FV_STATION_FAILED) {
            badge = "FAILED";
            bcol = 0xFFFF5252;
        }
        fvg_text_center(c, FVG_FONT_SANS, text_sp(ui, 11.0f),
                        r->x + r->w - 78.0f * d, r->y, 70.0f * d, r->h, badge, bcol, true);
    }

    const int pages = (int)((tuner->group_count + TUNER_PAGE_SIZE - 1u) / TUNER_PAGE_SIZE);
    char pstr[64];
    snprintf(pstr, sizeof(pstr), "Page %d / %d • Tap any station to audition",
             pages > 0 ? s->tuner_page + 1 : 0, pages);
    const float footer = ui->rects[W_TUNER_PREV].y;
    fvg_text_center(c, FVG_FONT_SANS, text_sp(ui, 11.0f), row_x, footer - 18.0f * d, row_w, 16.0f * d,
                    pstr, 0xFFBBBBBB, false);
    draw_placed_pill(ui, W_TUNER_PREV, "< PREV", PILL_IDLE);
    draw_placed_pill(ui, W_TUNER_NEXT, "NEXT >", PILL_IDLE);
}

static void playback_time(char *out,size_t capacity,int64_t ms) {
    if (ms<0) ms=0;
    snprintf(out,capacity,"%lld:%02lld.%03lld",(long long)(ms/60000),(long long)(ms/1000%60),(long long)(ms%1000));
}
static void set_window_fullscreen(ANativeActivity *activity, bool fullscreen) {
    if (!activity) return;
    ANativeActivity_setWindowFlags(activity,
                                   fullscreen ? AWINDOW_FLAG_FULLSCREEN : 0u,
                                   fullscreen ? 0u : AWINDOW_FLAG_FULLSCREEN);
}

static const struct { unsigned kind; uint32_t color; const char *label; } timeline_styles[] = {
    {FV_MARK_HIT, 0xFFFFD166u, "Manual hit"},
    {FV_MARK_REM_POD, 0xFFD6A6FFu, "REM-POD alert"},
    {FV_MARK_FRAGMENT, 0xFF80DFC1u, "VOX fragment"},
    {FV_MARK_BOOKMARK, 0xFFCBCDD4u, "Playback bookmark"},
};
static const uint32_t legacy_marker_color = 0xFFCBCDD4u;

static uint32_t timeline_color(unsigned kinds) {
    for (size_t i = 0; i < sizeof(timeline_styles) / sizeof(timeline_styles[0]); i++)
        if (kinds & timeline_styles[i].kind) return timeline_styles[i].color;
    return legacy_marker_color;
}

static void timeline_label(char *out, size_t cap, unsigned kinds) {
    size_t used = 0;
    for (size_t i = 0; i < sizeof(timeline_styles) / sizeof(timeline_styles[0]); i++) {
        if (!(kinds & timeline_styles[i].kind)) continue;
        const int n = snprintf(out + used, cap - used, "%s%s", used ? " + " : "", timeline_styles[i].label);
        if (n < 0 || (size_t)n >= cap - used) return;
        used += (size_t)n;
    }
    if (!used) snprintf(out, cap, "Saved marker");
}

static void draw_local_playback(Ui *ui, const FvLocalPlayerSnapshot *p, const FvUiScreenLayout *layout) {
    FvCanvas *c = ui->c;
    const float d = ui->d, x = ui->x0, w = ui->w;
    const float top = layout->play_top;
    const float wave_y = layout->play_wave_y, wave_h = layout->play_wave_h, buttons_y = layout->play_buttons_y;
    const float header_end = layout->play_header_end;
    const float footer_start = layout->play_footer_start;
    const float mid_h = layout->play_mid_h;
    const bool show_controls = ui->rects[W_PLAY_BACK].visible;
    fvg_clear(c, 0xFF080808);
    if (p->video) {
        fv_local_player_draw_video(c);
        if (!p->video_frames && !p->error) {
            fvg_text_center(c, FVG_FONT_SANS, text_sp(ui, 12.0f), x, header_end, w, mid_h,
                            "Loading local video…", 0xFFAAAAAA, false);
        }
        if (!show_controls) return;
        fvg_fill_rect(c, 0.0f, 0.0f, (float)c->w, header_end, 0x99000000);
        fvg_fill_rect(c, 0.0f, footer_start, (float)c->w, (float)c->h - footer_start, 0xB8000000);
    } else if (p->error) {
        fvg_text_center(c, FVG_FONT_SANS, text_sp(ui, layout->landscape ? 16.0f : 20.0f),
                        x, header_end, w, mid_h, "RECORDING UNAVAILABLE", 0xFFDDDDDD, true);
        if (!show_controls) return;
    } else if (!show_controls) {
        return;
    }
    draw_placed_pill(ui,W_PLAY_BACK,"< LIBRARY",PILL_IDLE);
    draw_placed_pill(ui,W_PLAY_MUTE,p->muted ? "MUTED" : "MUTE",p->muted ? PILL_ACTIVE : PILL_IDLE);
    fvg_text_center(c,FVG_FONT_SANS, text_sp(ui,12.0f),x+84.0f*d,top,w-152.0f*d,34.0f*d,"LOCAL PLAYBACK",0xFFFFFFFF,true);
    fvg_text_center(c,FVG_FONT_SANS,fit_size(FVG_FONT_SANS, text_sp(ui,12.0f),p->name,w,false),x,top+38.0f*d,w,25.0f*d,p->name,0xFFDDDDDD,false);
    fvg_round_rect(c,x,wave_y,w,wave_h,8.0f*d,0xFF141414,0xFF444444,d);
    if (!p->video && !p->error && wave_h > 120.0f * d) {
        fvg_text_center(c, FVG_FONT_SANS, text_sp(ui, 12.0f), x, wave_y + 10.0f * d, w, 20.0f * d,
                        "SAVED AUDIO", 0xFF777777, true);
    }
    const float mid=wave_y+wave_h*0.5f;
    fvg_line(c,x,mid,x+w,mid,d,0xFF555555);
    if (p->waveform_ready) {
        const float step=w/(float)FV_PLAYBACK_BINS;
        const float play_x = p->duration_ms > 0 ? x+w*(float)((double)p->position_ms/(double)p->duration_ms) : x;
        for (int i=0;i<FV_PLAYBACK_BINS;i++) {
            const float bx=x+((float)i+0.5f)*step;
            const float amplitude=p->waveform[i]*wave_h*0.45f;
            const uint32_t bar_col = bx <= play_x ? 0xFF6FE7FFu : 0xFFAAAAAAu;
            fvg_line(c,bx,mid-amplitude,bx,mid+amplitude,fmaxf(d,step*0.65f),bar_col);
        }
    } else if (!p->error) {
        const char *label = p->waveform_unavailable ? "Waveform unavailable" : "Decoding local waveform…";
        fvg_text_center(c,FVG_FONT_SANS,text_sp(ui,12.0f),x,wave_y,w,wave_h,label,0xFFBBBBBB,false);
    }
    if (p->duration_ms>0) {
        for (size_t i=0;i<p->timeline.count;i++) {
            const FvTimelineMarker *marker=&p->timeline.markers[i];
            const float bx=x+w*(float)((double)marker->ms/(double)p->duration_ms);
            fvg_line(c,bx,wave_y,bx,wave_y+wave_h,d,timeline_color(marker->kinds));
        }
        const float cx=x+w*(float)((double)p->position_ms/(double)p->duration_ms);
        fvg_line(c,cx,wave_y,cx,wave_y+wave_h,2.0f*d,0xFF6FE7FF);
    }
    char elapsed[40],duration[40],clock[96];
    playback_time(elapsed,sizeof(elapsed),p->position_ms);playback_time(duration,sizeof(duration),p->duration_ms);
    snprintf(clock,sizeof(clock),"%s / %s",elapsed,duration);
    fvg_text_center(c,FVG_FONT_SANS, text_sp(ui,12.0f),x,wave_y+wave_h,w,20.0f*d,clock,0xFFFFFFFF,false);
    char status[160];
    uint32_t status_color=p->error ? 0xFFFF7777u : 0xFFBBBBBBu;
    if (p->status[0]) {
        snprintf(status,sizeof(status),"%s",p->status);
    } else if (p->cue_type != FV_CUE_NONE) {
        char stamp[32], label[96];
        playback_time(stamp,sizeof(stamp),p->cue.ms);
        timeline_label(label,sizeof(label),p->cue.kinds);
        snprintf(status,sizeof(status),"%s %s  •  %s",
                 p->cue_type == FV_CUE_SELECTED ? "Marker" : (p->cue_type == FV_CUE_LAST ? "Last" : "Next"),
                 stamp,label);
        status_color=timeline_color(p->cue.kinds);
    } else {
        snprintf(status,sizeof(status),"No markers yet • tap MARK to add one");
    }
    fvg_text_center(c,FVG_FONT_SANS,fit_size(FVG_FONT_SANS, text_sp(ui,12.0f),status,w,false),x,buttons_y-21.0f*d,w,20.0f*d,status,status_color,false);
    const WidgetId ids[]={W_PLAY_REWIND_10,W_PLAY_REWIND_5,W_PLAY_TOGGLE,W_PLAY_FORWARD_5};
    const char *labels[]={"-10 SEC","-5 SEC",p->playing ? "PAUSE" : "PLAY","+5 SEC"};
    for (int i=0;i<4;i++) draw_placed_pill(ui,ids[i],labels[i],i==2 ? PILL_ACTIVE : PILL_IDLE);
    draw_placed_pill(ui,W_PLAY_MARK_PREV,"< MARK",PILL_IDLE);
    draw_placed_pill(ui,W_PLAY_MARK,"MARK",PILL_IDLE);
    draw_placed_pill(ui,W_PLAY_MARK_NEXT,"MARK >",PILL_IDLE);
}

static void draw_estes(Ui *ui, const FieldVoxApp *s, int w, int h) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    fvg_clear(c, 0xFF000000);
    const char *title = "ESTES PROTOCOL";
    if (s->video_recording_active)
        title = s->video_source == FV_VIDEO_CAMERA_ONLY ? "ESTES • CLEAN MP4 REC" : "ESTES • SCREEN MP4 REC";
    const float title_size = fit_size(FVG_FONT_SANS, text_sp(ui,20.0f), title, (float)w - 24.0f * d, true);
    fvg_text_center(c, FVG_FONT_SANS, title_size, 0, (float)h * 0.40f, (float)w, 40 * d,
                    title, 0xFF881111, true);
    char hits[64];
    snprintf(hits, sizeof(hits), "Hits logged: %d", s->estes_hit_count);
    fvg_text_center(c, FVG_FONT_SANS, fv_text_sp(&s->text_scale,16), 0, (float)h * 0.40f + 48 * d, (float)w, 30 * d, hits, 0xFF2A6A78, false);
    fvg_text_center(c, FVG_FONT_SANS, fv_text_sp(&s->text_scale,12), 0, (float)h - 90 * d, (float)w, 30 * d, "Tap anywhere to log a hit",
                    0xFF3A3A3A, false);
    const WidgetRect *er = &ui->rects[W_ESTES_EXIT];
    fvg_round_rect(c, er->x, er->y, er->w, er->h, 10 * d, 0xFF210707, 0xFF661111, d);
    fvg_text_center(c, FVG_FONT_SANS, fv_text_sp(&s->text_scale,14), er->x, er->y, er->w, er->h, "EXIT ESTES", 0xFFAA2222, true);
}

static void render_frame(ANativeWindow_Buffer *buf) {
    const int w = buf->width, h = buf->height;
    FvCanvas canvas;
    fvg_canvas_init(&canvas, (uint32_t *)buf->bits, w, h, buf->stride);

    static FieldVoxApp s;
    const long now = get_time_ms();
    pthread_mutex_lock(&g_state_mutex);
    if (g_app.vis_hold.tracking && !g_app.vis_hold.triggered) {
        fv_hold_update(&g_app.vis_hold, g_app.vis_hold.start_x, g_app.vis_hold.start_y, 0.0f, now);
    }
    memcpy(&s, &g_app, sizeof(s));
    pthread_mutex_unlock(&g_state_mutex);

    const float d = s.density > 0.5f ? s.density : 3.0f;
    bool is_video[EVIDENCE_MAX] = {false};
    for (int i = 0; i < s.evidence_count && i < EVIDENCE_MAX; i++) {
        is_video[i] = strcmp(s.evidence[i].mime, "video/mp4") == 0;
    }

    FvUiScreenLayout layout;
    WidgetRect rects[W_COUNT];
    if (s.ui.estes_mode || s.ui.playback_visible) {
        fv_ui_layout_active(rects, w, h, d, &s.ui, s.evidence_count, is_video, &layout);
    } else {
        fv_ui_layout_main_screen(rects, w, h, d, &s.ui, &layout);
    }
    const float top_inset = layout.top_inset;
    Ui ui = {&canvas, d, 0.0f, layout.ui_x0, layout.ui_w, rects, &s.text_scale};

    if (s.ui.playback_visible && !s.ui.estes_mode) {
        FvLocalPlayerSnapshot playback;
        fv_local_player_snapshot(&playback);
        draw_local_playback(&ui, &playback, &layout);
        pthread_mutex_lock(&g_state_mutex);
        memcpy(g_app.widgets, rects, sizeof(rects));
        g_app.playback_duration_ms = playback.duration_ms;
        pthread_mutex_unlock(&g_state_mutex);
        return;
    }

    const bool clean_capture = s.video_recording_active && s.video_source == FV_VIDEO_CAMERA_ONLY;
    if (clean_capture) {
        draw_filtered_camera(&canvas, &s);
        fv_video_recorder_feed_frame(canvas.px, w, h, canvas.stride, FV_VIDEO_CAMERA_ONLY);
    }
    if (s.ui.estes_mode) {
        draw_estes(&ui, &s, w, h);
        goto publish_frame;
    }

    /* 1. Camera Viewfinder Background */
    draw_viewfinder_background(&canvas, &s, w, h, d, now, clean_capture);

    /* 2. Top Status Header HUD */
    draw_top_header(&ui, &s, ui.x0, top_inset, ui.w, layout.header_h, now);

    /* 3. Status Line (Amber/Gold) */
    {
        char status_line[160];
        if (now < s.status_until_ms) {
            fvg_round_rect(&canvas, ui.x0, layout.status_y, ui.w, layout.status_h,
                           5.0f * d, 0xD60A121Cu, 0, 0);
            snprintf(status_line, sizeof(status_line), "%s", s.status_message);
        } else if (!s.sweep_active) {
            snprintf(status_line, sizeof(status_line), "Radio paused • open CONTROLS to start listening");
        } else if (s.carrier_ready) {
            snprintf(status_line, sizeof(status_line), "%s • %s • %u cached • %s",
                     s.local_only ? "LOCAL" : "LIVE", s.auto_gate ? "Auto + sensors" : "Sensor only", s.fragment_cache_count,
                     s.gate_open ? fv_gate_reason_name(s.gate_reason) : "listening");
        } else {
            snprintf(status_line, sizeof(status_line), "%s • %s",
                     s.local_only ? "LOCAL" : "LIVE", s.local_only ? "Choose a WAV with BANK in LIBRARY" : "waiting for a sensor fragment…");
        }
        const float ss = fit_size(FVG_FONT_SANS, text_sp(&ui,12.0f), status_line, ui.w - 12.0f * d, false);
        fvg_text_center(&canvas, FVG_FONT_SANS, ss, ui.x0, layout.status_y, ui.w, layout.status_h, status_line, 0xFFFFE6A0, false);
    }

    /* 4. Perimeter HUD Badges (EMF & Sensors) */
    draw_perimeter_badges(&ui, &s, layout.badge_y, layout.badge_h, now);

    /* 5-7. The shutter dock stays visible while the drawer is open. */
    if (rects[W_CAM_PREVIEW].visible) {
        draw_floating_visualizer(&ui, &s, ui.x0, layout.vis_top, ui.w, layout.vis_h, now);
    }
    draw_bottom_action_bar(&ui, &s, ui.x0, layout.bottom_bar_y, ui.w, layout.bottom_bar_h, now);

    /* 8. Slide-Up Drawer Overlay (if toggled) */
    if (s.ui.controls_visible) {
        draw_slide_up_drawer(&ui, &s, ui.x0, layout.drawer_y, ui.w, layout.drawer_h, now);
    }

    /* Positive evidence feedback remains readable over the camera for two seconds. */
    if (now < s.hit_marker_until_ms) {
        char hit_text[48];
        snprintf(hit_text, sizeof(hit_text), "HIT SAVED  %02d", s.session_hit_count);
        const float hw = fminf(ui.w - 24.0f * d, 190.0f * d);
        const float hh = 42.0f * d;
        const float hx = ((float)w - hw) * 0.5f;
        const float hy = (float)h * 0.56f;
        const float pulse = 0.5f + 0.5f * sinf((float)(now % 650L) * (2.0f * (float)M_PI / 650.0f));
        const uint32_t stroke = ((uint32_t)(150.0f + pulse * 105.0f) << 24) | 0x00FFD600;
        fvg_round_rect(&canvas, hx, hy, hw, hh, 10.0f * d, 0xE82B1D05, stroke, 2.0f * d);
        fvg_text_center(&canvas, FVG_FONT_SANS, text_sp(&ui,14.0f), hx, hy, hw, hh, hit_text, 0xFFFFE082, true);
    }

    /* Modal overlays are drawn last and their touch handlers mask everything beneath. */
    if (s.ui.tuner_visible || s.ui.evidence_visible || s.ui.help_visible) {
        FvRadioTunerSnapshot tuner = {0};
        int overlay_items = s.evidence_count;
        if (s.ui.tuner_visible) {
            fv_radio_tuner_snapshot(&tuner);
            const int rem = (int)tuner.group_count - s.tuner_page * TUNER_PAGE_SIZE;
            overlay_items = rem > 0 ? (rem > TUNER_PAGE_SIZE ? TUNER_PAGE_SIZE : rem) : 0;
        }
        fv_ui_layout_active(rects, w, h, d, &s.ui, overlay_items, is_video, &layout);
        if (s.ui.tuner_visible) draw_tuner_overlay(&ui, &s, &layout, &tuner, (float)h);
        else if (s.ui.evidence_visible) draw_evidence_overlay(&ui, &s, &layout, is_video, (float)h);
        if (s.ui.help_visible) draw_help_overlay(&ui, &layout, (float)h);
    }

publish_frame:
    /* Save widget hit rects */
    pthread_mutex_lock(&g_state_mutex);
    memcpy(g_app.widgets, rects, sizeof(rects));
    g_app.max_scroll = 0.0f;
    g_app.scroll_y = 0.0f;
    pthread_mutex_unlock(&g_state_mutex);

    /* Feed video recorder frame if active */
    if (s.video_recording_active && s.video_source == FV_VIDEO_SCREEN) {
        fv_video_recorder_feed_frame(canvas.px, w, h, canvas.stride, FV_VIDEO_SCREEN);
    }
}

static void *render_thread_func(void *arg) {
    (void)arg;
    while (atomic_load(&g_app.running)) {
        if (!atomic_load(&g_app.resumed)) {
            usleep(50000);
            continue;
        }
        pthread_mutex_lock(&g_render_mutex);
        pthread_mutex_lock(&g_window_mutex);
        ANativeWindow *window = g_app.window;
        if (window) ANativeWindow_acquire(window);
        pthread_mutex_unlock(&g_window_mutex);
        if (window) {
            ANativeWindow_Buffer buffer;
            if (ANativeWindow_lock(window, &buffer, NULL) == 0) {
                render_frame(&buffer);
                ANativeWindow_unlockAndPost(window);
            }
            ANativeWindow_release(window);
        }
        pthread_mutex_unlock(&g_render_mutex);
        usleep(33333); // ~30 FPS
    }
    fvg_text_shutdown();
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Touch handling: tap on release, drag to scroll (Java ScrollView behaviour)
 * ------------------------------------------------------------------------------------------- */

/* Catalog is owned by the UI/input state mutex; the player opens its own local fd. */
static void evidence_page_locked(void) {
    const size_t pages=(g_catalog.count+EVIDENCE_MAX-1)/EVIDENCE_MAX;
    if (g_app.evidence_page>=pages) g_app.evidence_page=pages ? pages-1 : 0;
    g_app.evidence_count=0;g_app.evidence_total=g_catalog.count;
    for (size_t i=g_app.evidence_page*EVIDENCE_MAX;i<g_catalog.count && g_app.evidence_count<EVIDENCE_MAX;i++) {
        const FvRecording *item=&g_catalog.items[i];
        EvidenceEntry *entry=&g_app.evidence[g_app.evidence_count++];
        *entry=(EvidenceEntry){0};
        snprintf(entry->path,sizeof(entry->path),"%s",item->path);
        snprintf(entry->name,sizeof(entry->name),"%s",item->name);
        snprintf(entry->mime,sizeof(entry->mime),"%s",item->video ? "video/mp4" : "audio/wav");
        entry->modified=item->modified;
        struct tm local_tm;char stamp[32]="unknown time";
        if (localtime_r(&item->modified,&local_tm)) strftime(stamp,sizeof(stamp),"%b %d  %I:%M %p",&local_tm);
        const double mib=(double)item->bytes/(1024.0*1024.0);
        snprintf(entry->detail,sizeof(entry->detail),"%s  |  %.2f MiB",stamp,mib);
    }
}
static void refresh_evidence_locked(void) {
    char dir_a[512]={0},dir_b[512]={0};
    if (g_app.activity) {
        const char *base = g_app.activity->externalDataPath ? g_app.activity->externalDataPath
                                                           : g_app.activity->internalDataPath;
        if (base) snprintf(dir_a,sizeof(dir_a),"%s/fieldvox-recordings",base);
        if (g_app.activity->internalDataPath)
            snprintf(dir_b,sizeof(dir_b),"%s/fieldvox-hits",g_app.activity->internalDataPath);
    }
    if (!fv_recording_catalog_load(&g_catalog,dir_a,dir_b,
             g_app.video_recording_active ? g_app.video_artifact : NULL,NULL))
        set_status_locked("Some recordings could not be listed • check storage and refresh");
    evidence_page_locked();
}

/* Caller holds g_state_mutex. */
static void handle_widget(WidgetId id) {
    char msg[128];
    switch (id) {
        case W_CAL: {
            /* Java nativeCalibrateSensors: subtract the current magnetic baseline. */
            if (g_app.emf_baseline > 0.0f) g_app.emf_cal_offset += g_app.emf_baseline;
            g_app.emf_baseline = -1.0f;
            fv_reset_barometer_baseline(g_app.baro_current);
            g_app.cal_flash_until_ms = get_time_ms() + 1200L;
            set_status_locked("Sensors zero-calibrated");
            break;
        }
        case W_CADENCE:
            g_app.cadence_idx = (g_app.cadence_idx + 1) % 4;
            g_app.grain_ms = FV_SWEEP_CADENCE_MS[g_app.cadence_idx];
            snprintf(msg, sizeof(msg), "Radio sweep rate %s", cadence_label(g_app.cadence_idx));
            set_status_locked(msg);
            break;
        case W_DIRECTION:
            g_app.direction = g_app.direction == FV_SWEEP_FWD ? FV_SWEEP_REV :
                              (g_app.direction == FV_SWEEP_REV ? FV_SWEEP_RND : FV_SWEEP_FWD);
            break;
        case W_PITCH:
            g_app.pitch_override = (FvPitchOverride)(((int)g_app.pitch_override + 1) % 4);
            break;
        case W_ECHO:
            g_app.echo_percent = g_app.echo_percent ? 0 : 50;
            g_app.echo_fx = g_app.echo_percent > 0; break;
        case W_REVERB:
            g_app.reverb_percent = g_app.reverb_percent ? 0 : 50;
            g_app.reverb_fx = g_app.reverb_percent > 0; break;
        case W_VIS_PREV: g_app.visualizer_style = (g_app.visualizer_style+3)%4; break;
        case W_VIS_NEXT: g_app.visualizer_style = (g_app.visualizer_style+1)%4; break;
        case W_VIS_COLOR: g_app.visualizer_color = (g_app.visualizer_color+1)%4; break;
        case W_ADVANCED: g_app.ui.advanced_controls = !g_app.ui.advanced_controls; break;
        case W_VOICE_DUCK:
            g_app.voice_duck = !g_app.voice_duck;
            if (g_app.voice_duck && !fv_android_has_permission(g_app.activity, "android.permission.RECORD_AUDIO")) {
                fv_android_request_permission(g_app.activity, "android.permission.RECORD_AUDIO", 60);
            }
            set_status_locked(g_app.voice_duck ? "Speaking mute ON • speech pauses new fragments" : "Speaking mute OFF");
            break;
        case W_SOURCE_MODE:
            g_app.local_only = !g_app.local_only;
            set_status_locked(g_app.local_only ? "LOCAL • cached radio or WAV selected in LIBRARY" : "LIVE • radio fragments with cached fallback");
            break;
        case W_NOISE: g_app.noise_fx = !g_app.noise_fx; break;
        case W_REV: g_app.reverse_fx = !g_app.reverse_fx; break;
        case W_SESS: {
            clear_clean_start_locked();
            g_app.evidence_page=0;
            refresh_evidence_locked();
            g_app.ui.evidence_visible = true;
            g_app.ui.help_visible = false;
            g_app.ui.controls_visible = false;
            break;
        }
        case W_START:
            g_app.sweep_active = !g_app.sweep_active;
            set_status_locked(g_app.sweep_active ? "Auto Live arming • connecting stations" : "Sweep stopped • streams closed");
            break;
        case W_MIC:
            g_app.monitor_active = !g_app.monitor_active;
            if (g_app.monitor_active && !fv_android_has_permission(g_app.activity, "android.permission.RECORD_AUDIO")) {
                fv_android_request_permission(g_app.activity, "android.permission.RECORD_AUDIO", 60);
                set_status_locked("Microphone permission requested");
            } else {
                set_status_locked(g_app.monitor_active ? "Speaker Monitor ON • you may hear mic hiss"
                                                       : "Speaker Monitor OFF");
            }
            break;
        case W_CTRL:
            g_app.ui.controls_visible = !g_app.ui.controls_visible;
            g_app.ui.advanced_controls = false;
            g_app.ui.help_visible = false;
            g_app.ui.evidence_visible = false;
            break;
        case W_SB11: g_app.sb11_dual = !g_app.sb11_dual; break;
        case W_AUTO_GATE:
            g_app.auto_gate = !g_app.auto_gate;
            set_status_locked(g_app.auto_gate ? "Auto + sensor triggers enabled"
                                              : "Sensor only • automatic audio triggers disabled");
            break;
        case W_VOCAL: g_app.vocal_filter = !g_app.vocal_filter; break;
        case W_REMPOD: g_app.rem_pod_active = !g_app.rem_pod_active; break;
        case W_PAN: g_app.spatial_pan = !g_app.spatial_pan; break;
        case W_ESTES:
            clear_clean_start_locked();
            g_app.ui.estes_mode = true;
            g_app.vocal_filter = true;
            break;
        case W_ESTES_EXIT:
            set_status_locked("Estes protocol ended");
            break;
        case W_CAMHUD:
            if (g_app.video_recording_active) {
                set_status_locked("Stop recording before switching the camera preview");
                break;
            }
            if (g_app.cam_filter == FV_CAM_FILTER_OFF) {
                g_app.cam_filter = FV_CAM_FILTER_NORMAL;
                g_app.cam_hud_active = true;
                if (!fv_android_has_permission(g_app.activity, "android.permission.CAMERA")) {
                    fv_android_request_permission(g_app.activity, "android.permission.CAMERA", 61);
                }
                set_status_locked("Camera preview ON");
            } else {
                clear_clean_start_locked();
                g_app.cam_filter = FV_CAM_FILTER_OFF;
                g_app.cam_hud_active = false;
                set_status_locked("Camera preview OFF");
            }
            break;
        case W_SENS_DOWN:
        case W_SENS_UP: {
            /* Java slider: 0.50 .. 10.00 uT. */
            float t = g_app.threshold + (id == W_SENS_UP ? 0.25f : -0.25f);
            g_app.threshold = t < 0.5f ? 0.5f : (t > 10.0f ? 10.0f : t);
            break;
        }
        case W_COOL_DOWN:
        case W_COOL_UP: {
            /* Java slider: 100 .. 2000 ms. */
            int c = g_app.cooldown_ms + (id == W_COOL_UP ? 50 : -50);
            g_app.cooldown_ms = c < 100 ? 100 : (c > 2000 ? 2000 : c);
            break;
        }
        case W_MARK_HIT:
            atomic_fetch_add(&g_app.mark_hit_requests, 1u);
            set_status_locked("Saving clean + processed hit and source timeline…");
            break;
        case W_TORCH:
            g_app.torch_active = !g_app.torch_active;
            if (g_app.torch_active) {
                if (g_app.cam_filter == FV_CAM_FILTER_OFF) g_app.cam_filter = FV_CAM_FILTER_NORMAL;
                g_app.cam_hud_active = true;
                set_status_locked("Torch requested • camera remains live");
            } else {
                set_status_locked("Torch off");
            }
            break;
        case W_HELP:
            g_app.ui.help_visible = !g_app.ui.help_visible;
            g_app.ui.evidence_visible = false;
            g_app.ui.controls_visible = false;
            break;
        case W_MUTE:
            g_app.audio_muted = !g_app.audio_muted;
            set_status_locked(g_app.audio_muted ? "Output muted • sweep and recordings continue"
                                                : "Output restored");
            break;
        case W_SENS_LOW:
            g_app.threshold = 3.0f;
            set_status_locked("EMF sensitivity LOW • noisy indoor spaces");
            break;
        case W_SENS_MED:
            g_app.threshold = 1.65f;
            set_status_locked("EMF sensitivity MED • balanced default");
            break;
        case W_SENS_HIGH:
            g_app.threshold = 0.75f;
            set_status_locked("EMF sensitivity HIGH • quiet environments");
            break;
        case W_COOL_CYCLE:
            g_app.cooldown_ms = g_app.cooldown_ms < 350 ? 450 : (g_app.cooldown_ms < 700 ? 900 : 250);
            snprintf(msg, sizeof(msg), "Sensor cooldown %d ms", g_app.cooldown_ms);
            set_status_locked(msg);
            break;
        case W_CAM_PREVIEW:
            g_app.visualizer_style = (g_app.visualizer_style+1)%4;
            break;
        case W_CAM_LENS: {
            if (g_app.video_recording_active) {
                set_status_locked("Stop video recording before switching camera lens");
                break;
            }
            if (fv_camera_cycle_lens()) {
                if (g_app.cam_filter == FV_CAM_FILTER_OFF) g_app.cam_filter = FV_CAM_FILTER_NORMAL;
                g_app.cam_hud_active = true;
                char lbl[16];
                fv_camera_copy_lens_label(fv_camera_current_lens(), lbl, sizeof(lbl));
                snprintf(msg, sizeof(msg), "Camera lens: %s", lbl);
                set_status_locked(msg);
            } else {
                set_status_locked("Only one camera lens reported by device");
            }
            break;
        }
        case W_VIS_LAYOUT: {
            static const char *const layout_desc[FV_VIS_LAYOUT_COUNT] = {
                "HUD layout: Bottom panel", "HUD layout: Top clear view", "HUD layout: Compact transparent"
            };
            g_app.ui.vis_layout = (FvVisLayoutPreset)(((int)g_app.ui.vis_layout + 1) % FV_VIS_LAYOUT_COUNT);
            set_status_locked(layout_desc[g_app.ui.vis_layout]);
            break;
        }
        case W_CAPTURE_CLEAN:
        case W_CAPTURE_SCREEN: {
            const FvVideoFrameSource source = id == W_CAPTURE_CLEAN ? FV_VIDEO_CAMERA_ONLY : FV_VIDEO_SCREEN;
            if (g_app.video_source == source) break;
            if (g_app.video_recording_active) {
                set_status_locked("Stop recording before changing the capture mode");
                break;
            }
            clear_clean_start_locked();
            g_app.video_source = source;
            set_status_locked(source == FV_VIDEO_CAMERA_ONLY ? "Clean MP4 saves the camera view and session audio"
                                                              : "Screen MP4 includes the FieldVox controls and overlays");
            break;
        }
        case W_RECORD: {
            if (g_app.video_recording_active) {
                char saved_path[512] = {0};
                stop_video_recording_locked(saved_path, sizeof(saved_path));
                set_status_locked(saved_path[0] ? "Video saved to Library" : "Video recording stopped");
            } else if (g_app.clean_start_deadline_ms) {
                clear_clean_start_locked();
                set_status_locked("Recording request canceled");
            } else if (g_app.video_source == FV_VIDEO_SCREEN) {
                start_video_recording_locked();
            } else if (g_app.cam_filter == FV_CAM_FILTER_OFF) {
                set_status_locked("Turn on the camera or choose Screen MP4");
            } else {
                const bool allowed = fv_android_has_permission(g_app.activity, "android.permission.CAMERA");
                if (allowed && fv_camera_has_frame(2000)) {
                    start_video_recording_locked();
                } else {
                    g_app.awaiting_camera_permission = !allowed;
                    g_app.clean_start_deadline_ms = get_time_ms() + (allowed ? 7000L : 30000L);
                    set_status_locked(allowed ? "Camera starting • tap the shutter to cancel"
                                              : "Allow camera access to start Clean MP4");
                    if (!allowed) fv_android_request_permission(g_app.activity, "android.permission.CAMERA", 61);
                }
            }
            break;
        }
        case W_CAM_TINT: {
            if (g_app.video_recording_active) {
                set_status_locked("Stop recording before changing the camera filter");
                break;
            }
            g_app.cam_filter = fv_camera_filter_step(g_app.cam_filter,1);
            g_app.cam_hud_active = true;
            char tmsg[64];
            snprintf(tmsg, sizeof(tmsg), "Camera filter: %s", fv_camera_filter_name(g_app.cam_filter));
            set_status_locked(tmsg);
            LOGI("Camera filter: %d %s (button)",(int)g_app.cam_filter,fv_camera_filter_name(g_app.cam_filter));
            break;
        }
        case W_CLOSE_DRAWER: {
            g_app.ui.controls_visible = false;
            break;
        }
        case W_LIBRARY_PREV:
            if (g_app.evidence_page) g_app.evidence_page--;
            evidence_page_locked();break;
        case W_LIBRARY_NEXT:
            if ((g_app.evidence_page+1)*EVIDENCE_MAX<g_catalog.count) g_app.evidence_page++;
            evidence_page_locked();break;
        case W_LIBRARY_REFRESH: refresh_evidence_locked();break;
        case W_PLAY_BACK:
            fv_local_player_close();
            g_app.ui.playback_visible = false;
            g_app.ui.playback_audio_only = false;
            g_app.ui.playback_controls_hidden = false;
            g_app.ui.evidence_visible = true;
            refresh_evidence_locked();
            break;
        case W_PLAY_TOGGLE: fv_local_player_toggle();break;
        case W_PLAY_REWIND_5: fv_local_player_seek_delta(-5000);break;
        case W_PLAY_REWIND_10: fv_local_player_seek_delta(-10000);break;
        case W_PLAY_FORWARD_5: fv_local_player_seek_delta(5000);break;
        case W_PLAY_MUTE: fv_local_player_mute();break;
        case W_PLAY_MARK: fv_local_player_bookmark();break;
        case W_PLAY_MARK_PREV: fv_local_player_jump_mark(false);break;
        case W_PLAY_MARK_NEXT: fv_local_player_jump_mark(true);break;
        case W_CLOSE_EVIDENCE: g_app.ui.evidence_visible = false; break;
        case W_CLOSE_HELP: g_app.ui.help_visible = false; break;
        case W_RADIO_TUNER:
            g_app.tuner_page = 0;
            g_app.ui.tuner_visible = true;
            g_app.ui.controls_visible = false;
            g_app.ui.help_visible = false;
            g_app.ui.evidence_visible = false;
            break;
        case W_CLOSE_TUNER:
            g_app.ui.tuner_visible = false;
            break;
        case W_TUNER_BANK:
            fv_radio_cycle_bank();
            g_app.tuner_page = 0;
            snprintf(msg, sizeof(msg), "Radio bank: %s", fv_station_bank_label(fv_radio_get_bank()));
            set_status_locked(msg);
            break;
        case W_TUNER_SHUFFLE:
            fv_radio_reshuffle();
            g_app.tuner_page = 0;
            set_status_locked("Radio station bank re-shuffled");
            break;
        case W_TUNER_REFRESH:
            set_status_locked(fv_radio_refresh_catalog() ? "Syncing live news/talk directory…"
                                                         : "Directory sync already in progress");
            break;
        case W_TUNER_PREV:
            if (g_app.tuner_page > 0) g_app.tuner_page--;
            break;
        case W_TUNER_NEXT: {
            FvRadioTunerSnapshot snap = {0};
            fv_radio_tuner_snapshot(&snap);
            if ((g_app.tuner_page + 1) * TUNER_PAGE_SIZE < (int)snap.group_count) {
                g_app.tuner_page++;
            }
            break;
        }
        case W_TUNER_PLAY_0:
        case W_TUNER_PLAY_1:
        case W_TUNER_PLAY_2:
        case W_TUNER_PLAY_3:
        case W_TUNER_PLAY_4: {
            const int slot = g_app.tuner_page * TUNER_PAGE_SIZE + (int)(id - W_TUNER_PLAY_0);
            if (fv_radio_test_slot(slot)) {
                snprintf(msg, sizeof(msg), "Testing radio slot %d…", slot + 1);
            } else {
                snprintf(msg, sizeof(msg), "Radio test busy • wait a moment");
            }
            set_status_locked(msg);
            break;
        }
        default: break;
    }
}

static void handle_tap(float x, float y_screen) {
    ANativeActivity *activity = NULL;
    int vib_ms = 0;
    int fullscreen_cmd = 0;
    char open_path[512] = {0};
    bool open_muted = false;
    pthread_mutex_lock(&g_state_mutex);
    activity = g_app.activity;
    const float d = g_app.density > 0.5f ? g_app.density : 3.0f;
    bool estes_hit = false;
    const int hit = fv_ui_handle_tap(&g_app.ui, g_app.widgets, x, y_screen, d, &estes_hit);
    if (estes_hit) {
        g_app.estes_hit_count++;
        atomic_fetch_add(&g_app.mark_hit_requests, 1u);
        vib_ms = 18;
    } else if (hit >= W_EVIDENCE_BANK_0 && hit < W_EVIDENCE_BANK_0 + EVIDENCE_MAX) {
        const int idx = hit - W_EVIDENCE_BANK_0;
        if (idx < g_app.evidence_count) {
            fv_local_bank_load(g_app.evidence[idx].path);
            g_app.local_only = true;
            g_app.ui.evidence_visible = false;
            set_status_locked("Loading WAV as the LOCAL fragment source");
            vib_ms = 15;
        }
    } else if (hit >= W_EVIDENCE_OPEN_0 && hit < W_EVIDENCE_OPEN_0 + EVIDENCE_MAX) {
        const int idx = hit - W_EVIDENCE_OPEN_0;
        if (idx < g_app.evidence_count) {
            if (g_app.video_recording_active) {
                set_status_locked("Stop recording before local playback");
            } else {
                clear_clean_start_locked();
                const bool is_mp4 = strcmp(g_app.evidence[idx].mime, "video/mp4") == 0;
                snprintf(open_path, sizeof(open_path), "%s", g_app.evidence[idx].path);
                open_muted = g_app.audio_muted;
                g_app.ui.evidence_visible = false;
                g_app.ui.playback_visible = true;
                g_app.ui.playback_audio_only = !is_mp4;
                g_app.ui.playback_controls_hidden = is_mp4;
                fullscreen_cmd = 1;
                set_status_locked("Loading recording on this phone");
            }
            vib_ms = 15;
        }
    } else if (hit >= 0) {
        LOGI("tap widget %d", hit);
        handle_widget((WidgetId)hit);
        if (hit == W_PLAY_BACK) fullscreen_cmd = -1;
        vib_ms = g_app.ui.playback_visible ? 15 : 18;
    } else if (g_app.ui.help_visible || g_app.ui.evidence_visible || g_app.ui.tuner_visible) {
        vib_ms = 15;
    }
    pthread_mutex_unlock(&g_state_mutex);
    if (fullscreen_cmd != 0) set_window_fullscreen(activity, fullscreen_cmd > 0);
    if (vib_ms > 0) fv_android_vibrate(activity, vib_ms);
    if (open_path[0]) fv_local_player_open(open_path, open_muted);
}

/* Caller holds g_state_mutex; overlays suppress slider interaction. */
static int hit_slider_locked(float x, float y) {
    if (g_app.ui.playback_visible) {
        const WidgetRect *r=&g_app.widgets[W_PLAY_SEEK];
        if (r->visible && x>=r->x && x<=r->x+r->w && y>=r->y && y<=r->y+r->h) {
            fv_local_player_scrub_begin();return W_PLAY_SEEK;
        }
        return -1;
    }
    if (!g_app.ui.controls_visible || g_app.ui.advanced_controls || g_app.ui.help_visible ||
        g_app.ui.evidence_visible || g_app.ui.tuner_visible || g_app.ui.estes_mode) return -1;
    for (int id=W_ECHO_LEVEL; id<=W_GRAIN_LEVEL; id++) {
        const WidgetRect *r = &g_app.widgets[id];
        if (r->visible && x>=r->x && x<=r->x+r->w && y>=r->y && y<=r->y+r->h) return id;
    }
    return -1;
}

static void update_slider_locked(int id, float x) {
    const WidgetRect *r = &g_app.widgets[id];
    if (!r->visible) return;
    if (id==W_PLAY_SEEK) {
        if (r->w>0 && isfinite(x)) {
            const double ratio=fmax(0.0,fmin(1.0,(double)(x-r->x)/(double)r->w));
            fv_local_player_seek((int64_t)(ratio*(double)g_app.playback_duration_ms));
        }
        return;
    }
    const int value = fv_vox_slider_percent(x, r->x, r->w);
    if (id==W_ECHO_LEVEL) { g_app.echo_percent=value; g_app.echo_fx=value>0; }
    else if (id==W_REVERB_LEVEL) { g_app.reverb_percent=value; g_app.reverb_fx=value>0; }
    else if (id==W_DISTORTION_LEVEL) g_app.distortion_percent=value;
    else if (id==W_GRAIN_LEVEL) g_app.grain_ms = fv_ui_percent_to_grain_ms(value);
}

/* Caller holds g_state_mutex. Camera swipes never take gestures from overlays. */
static bool camera_swipe_eligible_locked(void) {
    return atomic_load(&g_app.resumed) && g_app.cam_filter!=FV_CAM_FILTER_OFF &&
        !g_app.video_recording_active && !g_app.ui.estes_mode &&
        !g_app.ui.controls_visible && !g_app.ui.help_visible && !g_app.ui.evidence_visible &&
        !g_app.ui.tuner_visible && !g_app.ui.playback_visible && fv_camera_is_healthy(2000);
}
static bool camera_swipe_start_locked(float x, float y) {
    if (!camera_swipe_eligible_locked()) return false;
    return !fv_ui_any_hit(g_app.widgets, x, y);
}
static void *input_thread_func(void *arg) {
    (void)arg;
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    pthread_mutex_lock(&g_input_mutex);
    g_input_looper = looper;
    if (g_app.input_queue) AInputQueue_attachLooper(g_app.input_queue, looper, 2, NULL, NULL);
    pthread_mutex_unlock(&g_input_mutex);

    float down_x = 0, down_y = 0, down_scroll = 0;
    bool tracking = false, dragging = false;
    int active_slider = -1;
    FvCameraGesture camera_gesture={0};
    while (atomic_load(&g_app.running)) {
        const int ident = ALooper_pollOnce(50, NULL, NULL, NULL);
        if (ident != 2) continue;
        for (;;) {
            AInputEvent *event = NULL;
            bool tap = false;
            float tap_x = 0, tap_y = 0;
            pthread_mutex_lock(&g_input_mutex);
            AInputQueue *queue = g_app.input_queue;
            if (!queue || AInputQueue_getEvent(queue, &event) < 0) {
                pthread_mutex_unlock(&g_input_mutex);
                break;
            }
            if (AInputQueue_preDispatchEvent(queue, event)) {
                pthread_mutex_unlock(&g_input_mutex);
                continue;
            }
            int32_t handled = 0;
            if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
                const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
                const float ex = AMotionEvent_getX(event, 0), ey = AMotionEvent_getY(event, 0);
                pthread_mutex_lock(&g_state_mutex);
                const float slop = 10.0f * (g_app.density > 0.5f ? g_app.density : 3.0f);
                if (action == AMOTION_EVENT_ACTION_DOWN) {
                    down_x = ex;
                    down_y = ey;
                    down_scroll = g_app.scroll_y;
                    tracking = true;
                    dragging = false;
                    active_slider = hit_slider_locked(ex, ey);
                    if (active_slider>=0) update_slider_locked(active_slider, ex);
                    const bool on_vis = fv_ui_resolve_tap(g_app.widgets, ex, ey) == W_CAM_PREVIEW;
                    fv_hold_begin(&g_app.vis_hold, ex, ey, AMotionEvent_getEventTime(event) / 1000000, on_vis);
                    fv_camera_gesture_begin(&camera_gesture,ex,ey,AMotionEvent_getEventTime(event),
                                             active_slider<0 && camera_swipe_start_locked(ex,ey));
                } else if (action == AMOTION_EVENT_ACTION_MOVE && tracking) {
                    if (active_slider>=0) update_slider_locked(active_slider, ex);
                    if (g_app.vis_hold.tracking) {
                        fv_hold_update(&g_app.vis_hold, ex, ey, slop,
                                       AMotionEvent_getEventTime(event) / 1000000);
                        if (!g_app.vis_hold.tracking) dragging = true;
                    }
                    fv_camera_gesture_move(&camera_gesture,ex,AMotionEvent_getEventTime(event));
                    if (!dragging && fabsf(ey - down_y) > slop) dragging = true;
                    const bool horizontal = camera_gesture.active && fabsf(ex - down_x) > fabsf(ey - down_y);
                    if (dragging && !g_app.ui.estes_mode && !horizontal) {
                        float s = down_scroll - (ey - down_y);
                        g_app.scroll_y = s < 0.0f ? 0.0f : (s > g_app.max_scroll ? g_app.max_scroll : s);
                    }
                } else if (action == AMOTION_EVENT_ACTION_UP && tracking) {
                    tracking = false;
                    fv_hold_update(&g_app.vis_hold, ex, ey, slop,
                                   AMotionEvent_getEventTime(event) / 1000000);
                    const bool hold_consumed = fv_hold_end(&g_app.vis_hold);
                    if (active_slider>=0) update_slider_locked(active_slider, ex);
                    if (active_slider==W_PLAY_SEEK) fv_local_player_scrub_end();
                    const int swipe=fv_camera_gesture_end(&camera_gesture,ex,ey,AMotionEvent_getEventTime(event),
                                                          camera_swipe_eligible_locked());
                    if (swipe) {
                        g_app.cam_filter=fv_camera_filter_step(g_app.cam_filter,swipe);
                        g_app.cam_hud_active=true;
                        char message[64];
                        snprintf(message,sizeof(message),"Camera filter: %s",fv_camera_filter_name(g_app.cam_filter));
                        set_status_locked(message);
                        LOGI("Camera filter: %d %s (%s swipe)",(int)g_app.cam_filter,
                             fv_camera_filter_name(g_app.cam_filter),swipe>0 ? "left" : "right");
                    } else if (!hold_consumed && active_slider<0 && !dragging &&
                               fabsf(ex - down_x) <= slop && fabsf(ey - down_y) <= slop) {
                        tap = true;
                        tap_x = ex;
                        tap_y = ey;
                    }
                    active_slider = -1;
                } else if (action == AMOTION_EVENT_ACTION_CANCEL || action == AMOTION_EVENT_ACTION_POINTER_DOWN ||
                           action == AMOTION_EVENT_ACTION_POINTER_UP) {
                    if (active_slider==W_PLAY_SEEK) fv_local_player_scrub_end();
                    fv_hold_cancel(&g_app.vis_hold);
                    tracking = false;
                    active_slider = -1;
                    fv_camera_gesture_cancel(&camera_gesture);
                }
                pthread_mutex_unlock(&g_state_mutex);
                handled = 1;
            }
            if (AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event)==AKEYCODE_BACK) {
                ANativeActivity *back_act = NULL;
                bool exited_playback = false;
                pthread_mutex_lock(&g_state_mutex);
                const bool up = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP;
                if (g_app.ui.playback_visible || g_app.ui.evidence_visible || g_app.ui.tuner_visible ||
                    g_app.ui.help_visible || g_app.ui.estes_mode || g_app.ui.controls_visible) {
                    handled = 1;
                    bool close_playback = false;
                    if (up && fv_ui_back_pressed(&g_app.ui, &close_playback) && close_playback) {
                        fv_local_player_close();
                        refresh_evidence_locked();
                        back_act = g_app.activity;
                        exited_playback = true;
                    }
                }
                pthread_mutex_unlock(&g_state_mutex);
                if (exited_playback) set_window_fullscreen(back_act, false);
            }
            AInputQueue_finishEvent(queue, event, handled);
            pthread_mutex_unlock(&g_input_mutex);
            if (tap) handle_tap(tap_x, tap_y);
        }
    }
    pthread_mutex_lock(&g_input_mutex);
    if (g_app.input_queue) AInputQueue_detachLooper(g_app.input_queue);
    g_input_looper = NULL;
    pthread_mutex_unlock(&g_input_mutex);
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Sensors (Java nativeUpdateSensorTelemetry + the carrier's multi-sensor gates)
 * ------------------------------------------------------------------------------------------- */

/* Caller holds g_state_mutex. */
static void raise_proximity_gate(float delta) {
    const int min_grain = g_app.grain_ms < 400 ? 400 : g_app.grain_ms;
    g_app.proximity_trigger = (FvVoxDecimalTrigger){
        .passed = true,
        .pitch_factor = 1.0f,
        .speed_factor = 1.0f,
        .grain_duration_ms = min_grain,
        .seek_offset = 0.25f,
        .prefer_live = true,
        .channel_hop = (int)((uint32_t)(fabsf(delta) * 100.0f) % FV_FRAGMENT_DECKS)
    };
    g_app.proximity_pending = true;
}

static void *sensor_thread_func(void *arg) {
    (void)arg;
    ASensorManager *sm = ASensorManager_getInstanceForPackage("org.fieldvox.lab");
    if (!sm) {
        LOGW("ASensorManager not available");
        return NULL;
    }
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorEventQueue *queue = ASensorManager_createEventQueue(sm, looper, 1, NULL, NULL);
    if (!looper || !queue) {
        LOGW("Sensor event queue is not available");
        return NULL;
    }
    const ASensor *sensors[5] = {
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_MAGNETIC_FIELD),
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER),
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE),
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_PROXIMITY_ID),
        ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_PRESSURE_ID),
    };
    const int32_t rates_us[5] = {60000, 60000, 60000, 200000, 200000}; /* ~SENSOR_DELAY_UI */
    /* The NDK has no ASensor_getRange: track the largest reading seen (proximity is near/far binary on Pixels). */
    float prox_max = -1.0f;
    LOGI("Multi-sensor VOX gates armed: magnetometer=%d gyroscope=%d proximity=%d vibe_geophone=%d barometer=%d",
         sensors[0] != NULL, sensors[2] != NULL, sensors[3] != NULL, sensors[1] != NULL, sensors[4] != NULL);

    bool enabled = false;
    while (atomic_load(&g_app.running)) {
        const bool want = atomic_load(&g_app.resumed);
        if (want != enabled) {
            for (int i = 0; i < 5; i++) {
                if (!sensors[i]) continue;
                if (want) {
                    ASensorEventQueue_enableSensor(queue, sensors[i]);
                    ASensorEventQueue_setEventRate(queue, sensors[i], rates_us[i]);
                } else {
                    ASensorEventQueue_disableSensor(queue, sensors[i]);
                }
            }
            enabled = want;
        }
        if (!enabled) {
            usleep(50000);
            continue;
        }
        if (ALooper_pollOnce(50, NULL, NULL, NULL) != 1) continue;
        ASensorEvent ev;
        while (ASensorEventQueue_getEvents(queue, &ev, 1) > 0) {
            bool alert_haptic = false;
            pthread_mutex_lock(&g_state_mutex);
            const bool was_emf_spike = g_app.emf_spike;
            const bool was_coldspot = g_app.baro_coldspot;
            const bool can_eval = !g_app.voice_detected && !g_app.fragment_pending && !g_app.fragment_fetching;

            if (ev.type == ASENSOR_TYPE_MAGNETIC_FIELD) {
                const double mx = (double)ev.magnetic.x;
                const double my = (double)ev.magnetic.y;
                const double mz = (double)ev.magnetic.z;
                FvVoxDecimalTrigger trig;
                const float dist = (float)g_app.distortion_percent / 100.0f;
                const int cache_cnt = (int)g_app.fragment_cache_count;
                bool vox_pass = false;
                if (can_eval) {
                    vox_pass = fv_vox_eval_decimal_trigger_ex(mx, my, mz, true, dist, cache_cnt, &trig);

                }

                const float mag = (float)(vox_pass ? trig.magnitude : fv_mag3(ev.magnetic.x, ev.magnetic.y, ev.magnetic.z));
                if (isfinite(mag)) {
                    const float net = fmaxf(0.0f, mag - g_app.emf_cal_offset);
                    if (g_app.emf_baseline < 0.0f) {
                        g_app.emf_baseline = net;
                        g_app.emf_delta = 0.0f;
                    } else {
                        g_app.emf_baseline = g_app.emf_baseline * 0.985f + net * 0.015f;
                        g_app.emf_delta = fabsf(net - g_app.emf_baseline);
                    }
                    g_app.emf_magnitude = mag;
                    g_app.compass_azimuth = atan2f(ev.magnetic.y, ev.magnetic.x) * (180.0f / (float)M_PI);

                    const bool threshold_spike = fv_gate_emf_exceeds_threshold(g_app.emf_delta, g_app.threshold);
                    g_app.emf_spike = threshold_spike || vox_pass;

                    if (vox_pass) {
                        g_app.pending_fragment = trig;
                        g_app.pending_fragment_reason = FV_GATE_MAGNETOMETER;
                        g_app.fragment_pending = true;
                    }
                }
            } else if (ev.type == ASENSOR_TYPE_ACCELEROMETER) {
                g_app.accel_gx = g_app.accel_gx * 0.92f + ev.acceleration.x * 0.08f;
                g_app.accel_gy = g_app.accel_gy * 0.92f + ev.acceleration.y * 0.08f;
                g_app.accel_gz = g_app.accel_gz * 0.92f + ev.acceleration.z * 0.08f;
                const float vx = ev.acceleration.x - g_app.accel_gx;
                const float vy = ev.acceleration.y - g_app.accel_gy;
                const float vz = ev.acceleration.z - g_app.accel_gz;
                g_app.vibe_magnitude = sqrtf(vx * vx + vy * vy + vz * vz);

                FvVoxDecimalTrigger trig;
                const float dist = (float)g_app.distortion_percent / 100.0f;
                const int cache_cnt = (int)g_app.fragment_cache_count;
                bool vox_pass = false;
                if (can_eval) {
                    vox_pass = fv_vox_eval_decimal_trigger_ex((double)ev.acceleration.x,
                                                             (double)ev.acceleration.y,
                                                             (double)ev.acceleration.z,
                                                             false, dist, cache_cnt, &trig);

                }
                const bool vibe_spike = g_app.vibe_magnitude >= 0.45f;
                g_app.vibe_spike = vibe_spike || vox_pass;
                if (vox_pass) {
                    g_app.pending_fragment = trig;
                    g_app.pending_fragment_reason = FV_GATE_VIBE;
                    g_app.fragment_pending = true;
                }
            } else if (ev.type == ASENSOR_TYPE_GYROSCOPE) {
                const float mag = fv_mag3(ev.vector.x, ev.vector.y, ev.vector.z);
                if (isfinite(mag)) {
                    float delta = 0.0f;
                    if (g_app.gyro_baseline < 0.0f) {
                        g_app.gyro_baseline = mag;
                    } else {
                        g_app.gyro_baseline = g_app.gyro_baseline * 0.965f + mag * 0.035f;
                        delta = fabsf(mag - g_app.gyro_baseline);
                    }
                    /* Gyroscope values participate in the same decimal gate as
                     * the other vector sensors, without a baseline threshold. */
                    (void)delta;
                    if (can_eval) {
                        FvVoxDecimalTrigger trig;
                        if (fv_vox_eval_decimal_trigger_ex((double)ev.vector.x, (double)ev.vector.y,
                                (double)ev.vector.z, false, (float)g_app.distortion_percent/100.0f,
                                (int)g_app.fragment_cache_count, &trig)) {
                            g_app.pending_fragment=trig;
                            g_app.pending_fragment_reason=FV_GATE_GYROSCOPE;
                            g_app.fragment_pending=true;
                        }
                    }
                }
            } else if (ev.type == ASENSOR_TYPE_PROXIMITY_ID) {
                const float v = ev.distance;
                if (isfinite(v)) {
                    if (v > prox_max) prox_max = v;
                    const bool near = v <= 0.0f || v < prox_max;
                    if (g_app.proximity_last < 0.0f) {
                        g_app.proximity_near = near;
                    } else if (near != g_app.proximity_near) {
                        g_app.proximity_near = near;
                        raise_proximity_gate(fabsf(v - g_app.proximity_last));
                    }
                    g_app.proximity_last = v;
                }
            } else if (ev.type == ASENSOR_TYPE_PRESSURE_ID) {
                float delta = 0.0f;
                const int coldspot = fv_update_barometer(ev.pressure, &delta);
                g_app.baro_current = ev.pressure;
                g_app.baro_delta = delta;
                g_app.baro_coldspot = coldspot != 0;
            }
            int tier = 0;
            if (g_app.emf_delta >= 10.0f) tier = 4;
            else if (g_app.emf_delta >= 5.0f) tier = 3;
            else if (g_app.emf_delta >= 2.5f) tier = 2;
            else if (g_app.emf_delta >= 1.0f) tier = 1;
            if (g_app.vibe_magnitude >= 2.0f || g_app.baro_delta <= -0.20f) tier = 4;
            else if ((g_app.vibe_magnitude >= 1.0f || g_app.baro_delta <= -0.10f) && tier < 3) tier = 3;
            else if ((g_app.vibe_magnitude >= 0.45f || g_app.baro_delta <= -0.05f) && tier < 2) tier = 2;
            g_app.rem_pod_tier = tier;
            const long alert_now = get_time_ms();
            if (((!was_emf_spike && g_app.emf_spike) || (!was_coldspot && g_app.baro_coldspot)) &&
                alert_now - g_app.last_spike_vibrate_ms >= 2500L) {
                g_app.last_spike_vibrate_ms = alert_now;
                alert_haptic = true;
            }
            pthread_mutex_unlock(&g_state_mutex);
            if (alert_haptic) fv_android_vibrate_double(g_app.activity);
        }
    }
    if (enabled) {
        for (int i = 0; i < 5; i++) {
            if (sensors[i]) ASensorEventQueue_disableSensor(queue, sensors[i]);
        }
    }
    ASensorManager_destroyEventQueue(sm, queue);
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Audio: live carrier + grain gate + mic monitor + DSP
 * ------------------------------------------------------------------------------------------- */

static AAudioStream *open_audio_stream(aaudio_direction_t direction, int channels) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDirection(builder, direction);
    if (direction == AAUDIO_DIRECTION_INPUT) {
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    } else {
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_SPEECH);
    }
    AAudioStream *stream = NULL;
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || !stream) return NULL;
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        return NULL;
    }
    return stream;
}

static aaudio_result_t write_all_audio_frames(AAudioStream *stream, const int16_t *stereo, int32_t frames) {
    int32_t offset = 0;
    int no_progress = 0;
    while (offset < frames && atomic_load(&g_app.running)) {
        const aaudio_result_t written = AAudioStream_write(
            stream, stereo + (size_t)offset * 2u, frames - offset, 50000000L);
        if (written < 0) return written;
        if (written == 0) {
            if (++no_progress >= 3) return AAUDIO_ERROR_TIMEOUT;
            continue;
        }
        no_progress = 0;
        offset += written;
    }
    return offset;
}

/*
 * Next carrier deck: the live deck whose station follows the current one in the sweep direction
 * (random picks any other live deck). Never returns `avoid`. -1 when no deck qualifies.
 */
static void push_waterfall_column(uint32_t *img, const float *bins) {
    for (int y = 0; y < WF_HEIGHT; y++) {
        uint32_t *row = img + y * WF_WIDTH;
        memmove(row, row + 1, (WF_WIDTH - 1) * sizeof(uint32_t));
        const float v = bins[(WF_HEIGHT - 1) - y];
        const float intensity = isfinite(v) ? fmaxf(0.0f, fminf(1.0f, v)) : 0.0f;
        const uint32_t level = (uint32_t)(intensity*255.0f);
        const uint32_t col = 0xFF000000u | (level<<16) | (level<<8) | level;
        row[WF_WIDTH - 1] = col;
    }
}

static size_t local_bank_read(void *context, float seek_offset, size_t frames,
                              int16_t *output, size_t capacity) {
    (void)context;
    return fv_local_bank_fragment(seek_offset, frames, output, capacity);
}

static void *audio_thread_func(void *arg) {
    (void)arg;
    FvFragmentEngine *fragments = fv_fragment_create();
    if (!fragments) { LOGE("Fragment engine allocation failed"); return NULL; }
    if (!fv_fragment_store_start(g_app.activity->internalDataPath, fragments)) LOGW("Radio cache persistence unavailable");
    fv_fragment_set_live_source(fragments, fv_radio_live_source());
    fv_fragment_set_clip_store(fragments, fv_fragment_store_adapter());
    FvPlatformVoices *platform=fv_platform_voices_create(g_app.activity->vm);
    if (!platform) LOGW("Platform voices unavailable; native PCM speaker fallback");
    AAudioStream *stream = open_audio_stream(AAUDIO_DIRECTION_OUTPUT, 2);
    if (!stream) {
        LOGE("AAudio output stream failed");
        fv_platform_voices_destroy(platform);
        fv_fragment_store_stop(); fv_fragment_destroy(fragments);
        return NULL;
    }
    AAudioStream *input = NULL;
    long next_input_retry_ms = 0;
    long audition_until_ms = 0;
    fv_audio_dsp_reset();
    fv_hit_capture_reset();

    int16_t mono[BUFFER_FRAMES];
    int16_t clean[BUFFER_FRAMES];
    int16_t aux[BUFFER_FRAMES];
    int16_t mic[BUFFER_FRAMES];
    int16_t stereo[BUFFER_FRAMES * 2];
    uint64_t clock = 0;
    FvVoxDucker ducker = {0};
    float duck_gain = 1.0f;
    int wf_divider = 0;
    int previous_rem_tier = 0;
    long diag_last_ms = get_time_ms();

    while (atomic_load(&g_app.running)) {
        const long now = get_time_ms();
        const bool playback = fv_local_player_active();
        pthread_mutex_lock(&g_state_mutex);
        const bool resumed = atomic_load(&g_app.resumed);
        const bool burst_hold = g_app.vis_hold.tracking && g_app.vis_hold.triggered;
        const bool sweep = (g_app.sweep_active || burst_hold) && resumed && !playback;
        const bool local_only = g_app.local_only;
        const bool auto_gate = g_app.auto_gate || burst_hold;
        const bool monitor = g_app.monitor_active && resumed && !playback;
        const bool voice_duck = g_app.voice_duck && resumed && !playback;
        const bool need_input = monitor || voice_duck;
        const int echo_level = g_app.echo_percent, reverb_level = g_app.reverb_percent;
        const int distortion_level = g_app.distortion_percent, grain_ms = g_app.grain_ms;
        const bool vocal = g_app.vocal_filter, reverb = g_app.reverb_fx, echo = g_app.echo_fx;
        const bool noise = g_app.noise_fx, reverse = g_app.reverse_fx, rem_pod = g_app.rem_pod_active;
        const bool pan = g_app.spatial_pan;
        const bool muted = g_app.audio_muted;
        const FvSweepDirection direction = g_app.direction;
        const FvPitchOverride pitch_override = g_app.pitch_override;
        const int rem_tier = resumed && rem_pod ? g_app.rem_pod_tier : 0;
        const bool rem_alert = rem_tier > previous_rem_tier;
        previous_rem_tier = rem_tier;
        const float azimuth = g_app.compass_azimuth;
        const float emf_delta = g_app.emf_delta, emf_threshold = g_app.threshold;
        const bool requested = sweep && g_app.fragment_pending;
        const FvVoxDecimalTrigger fragment = g_app.pending_fragment;
        const FvGateReason request_reason = g_app.pending_fragment_reason;
        const bool prox_requested = sweep && g_app.proximity_pending;
        const FvVoxDecimalTrigger prox_fragment = g_app.proximity_trigger;
        g_app.fragment_pending = false;
        g_app.proximity_pending = false;
        /* Visible channel counter ticks at the cadence, independent of the audio. */
        if (sweep && now - g_app.last_channel_tick_ms >= FV_SWEEP_CADENCE_MS[g_app.cadence_idx]) {
            g_app.channel_counter = direction == FV_SWEEP_RND ? (int)((uint32_t)rand() % CHANNEL_RING)
                                                              : (g_app.channel_counter + (int)direction + CHANNEL_RING) % CHANNEL_RING;
            g_app.last_channel_tick_ms = now;
        }
        pthread_mutex_unlock(&g_state_mutex);

        const float pitch_override_f =
            pitch_override == FV_PITCH_OVERRIDE_LOW ? 0.6f :
            (pitch_override == FV_PITCH_OVERRIDE_NORMAL ? 1.0f :
             (pitch_override == FV_PITCH_OVERRIDE_HIGH ? 1.2f : 0.0f));
        fv_fragment_set_settings(fragments, (FvFragmentSettings){
            .echo = (float)echo_level / 100.0f,
            .reverb = (float)reverb_level / 100.0f,
            .pitch_override = pitch_override_f,
            .noise = noise,
            .paused = !sweep,
            .auto_enabled = auto_gate && sweep,
            .local_only = local_only,
            .sweep_ms = (unsigned)grain_ms});
        fv_radio_set_enabled(sweep && !local_only);
        int16_t *test_pcm = NULL;
        size_t test_frames = 0u;
        int test_station = -1, test_fd = -1;
        const bool has_test = fv_radio_take_test_result(&test_pcm, &test_frames, &test_station, &test_fd);
        if (has_test && test_pcm && test_frames > 0u) {
            audition_until_ms = now + (long)(test_frames * 1000u / SAMPLE_RATE) + 500L;
        }
        const int live = fv_radio_busy() ? 1 : 0;
        memset(mono, 0, sizeof(mono));
        if (!input && need_input && now >= next_input_retry_ms) {
            next_input_retry_ms = now + 1000L;
            if (fv_android_has_permission(g_app.activity, "android.permission.RECORD_AUDIO")) {
                input = open_audio_stream(AAUDIO_DIRECTION_INPUT, 1);
                next_input_retry_ms = now + 3000L;
                LOGI("AAudio microphone input: %s", input ? "ACTIVE" : "RETRY PENDING");
            }
        }
        if (input && !need_input) {
            AAudioStream_requestStop(input);
            AAudioStream_close(input);
            input = NULL;
        }
        bool microphone_present = false;
        int microphone_frames = 0;
        if (input) {
            const aaudio_result_t got = AAudioStream_read(input, mic, BUFFER_FRAMES, 0);
            if (got > 0) {
                microphone_present = true;
                microphone_frames = got;

            } else if (got < 0) {
                LOGW("AAudio input read failed: %s", AAudio_convertResultToText(got));
                AAudioStream_requestStop(input);
                AAudioStream_close(input);
                input = NULL;
            }
        }

        if (!voice_duck) ducker = (FvVoxDucker){0};
        const bool ducked = voice_duck && fv_vox_detect_voice(&ducker, microphone_frames ? mic : NULL,
                                                            microphone_frames, clock, SAMPLE_RATE);
        /* Optional PCM tools select the native speaker path. Recordings always
         * retain the native reconstruction; platform playback is not loopback. */
        const bool platform_speaker=fv_platform_voices_healthy(platform) && !vocal && !reverse && !pan && !rem_pod;
        const bool audition_active = now < audition_until_ms;
        fv_platform_voices_control(platform,platform_speaker && (sweep || audition_active),
                                   muted || ducked || playback || !resumed,(float)reverb_level/100.0f);
        fv_fragment_set_speaker(fragments,platform_speaker ? fv_platform_voices_submit : NULL,platform);
        if (has_test) {
            if (test_pcm && test_frames > 0u) {
                if (fv_fragment_audition_pcm(fragments, test_pcm, test_frames, test_station, test_fd, clock)) {
                    char cname[32] = {0};
                    station_callsign_at(test_station, cname, sizeof(cname));
                    char smsg[96];
                    snprintf(smsg, sizeof(smsg), "Playing %s (%.1fs)", cname[0] ? cname : "station",
                             (double)test_frames / 48000.0);
                    set_status(smsg);
                } else {
                    audition_until_ms = 0;
                    set_status("Audition playback unavailable");
                }
            }
            free(test_pcm);
            if (test_fd >= 0) close(test_fd);
        }
        if (sweep) {
            if (prox_requested) fv_fragment_submit_trigger(fragments, FV_GATE_PROXIMITY, &prox_fragment);
            if (requested) fv_fragment_submit_trigger(fragments, request_reason, &fragment);
        }
        const FvFragmentLocalProvider local_prov = {.read_fragment = local_bank_read};
        FvFragmentStepResult step_res = {0};
        fv_fragment_step(fragments, clock, ducked, &local_prov, &step_res);
        const bool admitted = step_res.admitted;
        FvFragmentStatus fragment_status = {0};
        fv_fragment_render(fragments, clean, mono, aux, BUFFER_FRAMES, clock, &fragment_status);
        for (int i = 0; monitor && i < microphone_frames; i++) {
            const float input_sample = (float)mic[i]*MIC_MONITOR_GAIN;
            const float dry = (float)clean[i]+input_sample, wet = (float)mono[i]+input_sample;
            clean[i] = (int16_t)fmaxf(-32768.0f,fminf(32767.0f,dry));
            mono[i] = (int16_t)fmaxf(-32768.0f,fminf(32767.0f,wet));
            aux[i] = (int16_t)fmaxf(-32768.0f,fminf(32767.0f,(float)aux[i]+input_sample));
        }
        /* Optional investigation tools remain available; the Distortion slider
         * now selects pitch/speed bins, rather than adding unrelated clipping. */
        if (rem_pod && rem_tier > 0) fv_rem_pod_generate(mono, BUFFER_FRAMES, rem_tier, SAMPLE_RATE);
        if (vocal) fv_vocal_bandpass(mono, BUFFER_FRAMES, SAMPLE_RATE);
        if (reverse) fv_audio_reverse(mono, BUFFER_FRAMES);

        FvHitBlock hit_block = {
            .sample_start = clock, .auto_enabled = auto_gate, .microphone = microphone_present && monitor,
            .voice_duck_enabled = voice_duck, .speaker_ducked = ducked,
            .echo_percent = echo_level, .reverb_percent = reverb_level,
            .distortion_percent = distortion_level, .grain_ms = fragment_status.grain_ms,
            .pitch_factor = fragment_status.pitch, .playback_speed = fragment_status.speed,
            .seek_fraction = fragment_status.seek_fraction, .cached_source = fragment_status.cached,
            .active_voices = fragment_status.voices,
            .muted = muted, .station_a = fragment_status.station, .station_b = -1,
            .reason_a = fragment_status.reason, .reason_b = FV_GATE_AUTO_SCAN,
            .emf_delta = emf_delta, .emf_threshold = emf_threshold,
            .pitch_semitones = (int)lrintf(12.0f*log2f(fragment_status.pitch > 0.0f ? fragment_status.pitch : 1.0f)),
            .effects = (vocal ? FV_HIT_FX_VOCAL : 0u) | (echo ? FV_HIT_FX_ECHO : 0u) |
                       (reverb ? FV_HIT_FX_REVERB : 0u) | (noise ? FV_HIT_FX_NOISE : 0u) |
                       (reverse ? FV_HIT_FX_REVERSE : 0u) | (rem_pod ? FV_HIT_FX_REMPOD : 0u) |
                       (pan ? FV_HIT_FX_PAN : 0u)
        };
        hit_block.audible_a = fragment_status.playing;
        if (fragment_status.station >= 0) {
            if (!fv_radio_station_info(fragment_status.station, hit_block.label_a,
                                       sizeof(hit_block.label_a), hit_block.url_a,
                                       sizeof(hit_block.url_a))) {
                const FvBlobStationEntry *st = station_at(fragment_status.station);
                memcpy(hit_block.label_a, st->callsign, sizeof(hit_block.label_a));
                memcpy(hit_block.url_a, st->url, sizeof(hit_block.url_a));
            }
        } else if (fragment_status.playing) {
            snprintf(hit_block.label_a, sizeof(hit_block.label_a), "LOCAL WAV");
            snprintf(hit_block.url_a, sizeof(hit_block.url_a), "local:selected-wav");
        }
        if (resumed && !playback) fv_hit_capture_push(clean, mono, BUFFER_FRAMES, &hit_block);
        else fv_hit_capture_reset();

        /* 5. Analysis, pre-roll ring and UI state. */
        fv_audio_capture_and_analyze(mono, BUFFER_FRAMES);
        float spectrum[FV_NATIVE_SPECTRUM_BINS];
        fv_audio_get_spectrum(spectrum);
        pthread_mutex_lock(&g_state_mutex);
        memcpy(g_app.spectrum, spectrum, sizeof(spectrum));
        for (int i = 0; i < FV_NATIVE_SPECTRUM_BINS; i++) {
            const float target = isfinite(spectrum[i]) ? fmaxf(0.0f, fminf(1.0f, spectrum[i])) : 0.0f;
            const float rate = target >= g_app.vis_smooth[i] ? 0.45f : 0.14f;
            g_app.vis_smooth[i] += rate * (target - g_app.vis_smooth[i]);
        }
        static_assert(BUFFER_FRAMES % FV_NATIVE_SPECTRUM_BINS == 0, "Waveform stride must be exact");
        for (int i = 0; i < FV_NATIVE_SPECTRUM_BINS; i++) {
            const int waveform_index = i * BUFFER_FRAMES / FV_NATIVE_SPECTRUM_BINS;
            g_app.waveform[i] = (float)mono[waveform_index]/32768.0f;
        }
        g_app.voice_detected = ducked;
        g_app.voice_input_ready = voice_duck && input != NULL;
        if (++wf_divider >= 2) {
            wf_divider = 0;
            push_waterfall_column(g_app.waterfall, spectrum);
        }
        g_app.audio_running = sweep || monitor;
        g_app.live_decks = live;
        g_app.carrier_ready = live > 0 || fragment_status.cache_count > 0 || local_only;
        g_app.carrier_station = fragment_status.station;
        g_app.carrier_b_station = -1;
        g_app.gate_open = fragment_status.playing;
        g_app.gate_reason = fragment_status.reason;
        g_app.fragment_cache_count = fragment_status.cache_count;
        g_app.fragment_fetching = fragment_status.live_pending;
        g_app.cached_source = fragment_status.cached;
        g_app.packets_opened = fragment_status.admitted;
        if (admitted) {
            g_app.pitch_semitones = hit_block.pitch_semitones;
        }
        pthread_mutex_unlock(&g_state_mutex);
        if (admitted) LOGI("VOX fragment: source=%s station=%d pitch=%.2f speed=%.2f grain=%dms voices=%u cache=%u",
            fragment_status.cached ? "CACHE" : (local_only ? "LOCAL" : "LIVE"), fragment_status.station,
            (double)fragment_status.pitch, (double)fragment_status.speed, fragment_status.grain_ms,
            fragment_status.voices, fragment_status.cache_count);
        if (now - diag_last_ms >= 5000L) {
            LOGI("fragment diag: live=%d cache=%u voices=%u admitted=%u playing=%d local=%d speech=%d",
                live, fragment_status.cache_count, fragment_status.voices, fragment_status.admitted,
                fragment_status.playing, local_only, ducked);
            diag_last_ms = now;
        }

        const unsigned hits = atomic_exchange(&g_app.mark_hit_requests, 0u);
        const unsigned kinds = (admitted ? FV_MARK_FRAGMENT : 0u) |
                               (rem_alert ? FV_MARK_REM_POD : 0u) |
                               (hits > 0u ? FV_MARK_HIT : 0u);
        if (kinds) record_active_timeline_marker(now, kinds);
        for (unsigned k = 0; k < hits && g_app.activity && g_app.activity->internalDataPath; k++) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/fieldvox-hits", g_app.activity->internalDataPath);
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) continue;
            char path[512];
            if (fv_hit_capture_save(dir, (uint64_t)get_wall_time_ms(), k, path, sizeof(path))) {
                LOGI("Paired hit and source timeline saved: %s", path);
                pthread_mutex_lock(&g_state_mutex);
                g_app.session_hit_count++;
                g_app.hit_marker_until_ms = get_time_ms() + 2200L;
                set_status_locked("Clean + processed hit and source timeline saved");
                pthread_mutex_unlock(&g_state_mutex);
                fv_android_vibrate_double(g_app.activity);
            } else {
                LOGE("Paired hit save failed: errno=%d (%s)", errno, strerror(errno));
                set_status("Hit save failed • check storage and retry");
            }
        }

        if (fv_video_recorder_is_active()) {
            fv_video_recorder_feed_audio(mono, BUFFER_FRAMES);
        }

        float gl = 0.707f, gr = 0.707f;
        if (pan) fv_calc_spatial_pan_gains(azimuth, &gl, &gr);
        const bool platform_monitor=platform_speaker && fv_platform_voices_healthy(platform);
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            const float target_gain = ducked ? 0.0f : 1.0f;
            duck_gain += (target_gain-duck_gain)*(ducked ? 0.01f : 0.0005f);
            const float v = muted || playback || !resumed ? 0.0f : (float)(platform_monitor ? aux[i] : mono[i])*duck_gain;
            const float l = v * gl, r = v * gr;
            stereo[i * 2] = (int16_t)(l > 32767.0f ? 32767.0f : (l < -32767.0f ? -32767.0f : l));
            stereo[i * 2 + 1] = (int16_t)(r > 32767.0f ? 32767.0f : (r < -32767.0f ? -32767.0f : r));
        }
        const aaudio_result_t written = write_all_audio_frames(stream, stereo, BUFFER_FRAMES);
        if (written < 0) {
            LOGE("AAudio output failed: %s", AAudio_convertResultToText(written));
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = NULL;
            while (atomic_load(&g_app.running) && !stream) {
                usleep(250000);
                stream = open_audio_stream(AAUDIO_DIRECTION_OUTPUT, 2);
            }
            if (!stream) break;
            LOGI("AAudio output stream recovered");
        }
        clock += BUFFER_FRAMES;
    }

    fv_radio_set_enabled(false);
    fv_platform_voices_destroy(platform);
    fv_fragment_store_stop();
    fv_fragment_destroy(fragments);
    if (input) {
        AAudioStream_requestStop(input);
        AAudioStream_close(input);
    }
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Camera and lifecycle
 * ------------------------------------------------------------------------------------------- */

static void *camera_thread_func(void *arg) {
    (void)arg;
    while (atomic_load(&g_app.running)) {
        pthread_mutex_lock(&g_state_mutex);
        const bool torch = g_app.torch_active;
        const bool cam = (g_app.cam_filter != FV_CAM_FILTER_OFF) || g_app.cam_hud_active || torch;
        pthread_mutex_unlock(&g_state_mutex);
        const bool resumed = atomic_load(&g_app.resumed);
        const bool has_perm = fv_android_has_permission(g_app.activity, "android.permission.CAMERA");

        if (resumed && cam && has_perm) {
            const bool active = fv_camera_is_active();
            const bool healthy = fv_camera_is_healthy(2000);
            const bool reopen = fv_camera_needs_reopen();
            if (!active || !healthy || reopen) {
                if (active && !healthy) {
                    LOGI("Camera frame watchdog triggered (> 2000 ms stall) - restarting camera pipeline");
                } else if (reopen) {
                    LOGI("Switching active camera lens");
                }
                fv_camera_stop();
                if (!fv_camera_start()) {
                    sleep(1);
                }
            }
            if (fv_camera_is_active() && fv_camera_is_torch_on() != torch) {
                if (!fv_camera_set_torch(torch)) {
                    LOGW("Could not apply requested torch state: %s", torch ? "ON" : "OFF");
                }
            }
        } else if (fv_camera_is_active()) {
            fv_camera_stop();
        }
        pthread_mutex_lock(&g_state_mutex);
        if (g_app.clean_start_deadline_ms) {
            if (get_time_ms() >= g_app.clean_start_deadline_ms) {
                set_status_locked(g_app.awaiting_camera_permission ? "Clean MP4 needs camera permission"
                                                                   : "Camera did not start • try Screen MP4");
                clear_clean_start_locked();
            } else if (atomic_load(&g_app.resumed) && g_app.window_focused &&
                       !g_app.awaiting_camera_permission && has_perm && fv_camera_has_frame(2000)) {
                start_video_recording_locked();
            }
        }
        pthread_mutex_unlock(&g_state_mutex);
        usleep(250000);
    }
    fv_camera_stop();
    return NULL;
}

static float read_density(ANativeActivity *activity) {
    float density = 3.0f;
    AConfiguration *config = AConfiguration_new();
    if (config && activity && activity->assetManager) {
        AConfiguration_fromAssetManager(config, activity->assetManager);
        const int32_t dpi = AConfiguration_getDensity(config);
        if (dpi > 0 && dpi != ACONFIGURATION_DENSITY_ANY && dpi != ACONFIGURATION_DENSITY_NONE) density = (float)dpi / 160.0f;
    }
    if (config) AConfiguration_delete(config);
    return density;
}

static void onNativeWindowCreated(ANativeActivity *activity, ANativeWindow *window) {
    pthread_mutex_lock(&g_window_mutex);
    g_app.window = window;
    pthread_mutex_unlock(&g_window_mutex);
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
    const float density = read_density(activity);
    const int disp_rot = fv_android_get_display_rotation_degrees(activity);
    pthread_mutex_lock(&g_state_mutex);
    g_app.density = density;
    g_app.text_scale = fv_android_text_scale(activity->clazz, density);
    g_app.display_rotation_deg = disp_rot;
    pthread_mutex_unlock(&g_state_mutex);
    LOGI("ANativeWindow created: %dx%d density=%.2f", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
         density);
}

static void onNativeWindowDestroyed(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    pthread_mutex_lock(&g_window_mutex);
    if (g_app.window == window) g_app.window = NULL;
    pthread_mutex_unlock(&g_window_mutex);
    pthread_mutex_lock(&g_render_mutex);
    /* Window is now guaranteed not to be locked by render thread. */
    pthread_mutex_unlock(&g_render_mutex);
}

static void onConfigurationChanged(ANativeActivity *activity) {
    const float density = read_density(activity);
    const int disp_rot = fv_android_get_display_rotation_degrees(activity);
    pthread_mutex_lock(&g_state_mutex);
    g_app.density = density;
    g_app.text_scale = fv_android_text_scale(activity->clazz, density);
    g_app.display_rotation_deg = disp_rot;
    g_app.scroll_y = 0.0f;
    pthread_mutex_unlock(&g_state_mutex);
}

static void onInputQueueCreated(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    pthread_mutex_lock(&g_input_mutex);
    g_app.input_queue = queue;
    if (g_input_looper) AInputQueue_attachLooper(queue, g_input_looper, 2, NULL, NULL);
    pthread_mutex_unlock(&g_input_mutex);
}

static void onInputQueueDestroyed(ANativeActivity *activity, AInputQueue *queue) {
    (void)activity;
    pthread_mutex_lock(&g_input_mutex);
    if (g_input_looper) AInputQueue_detachLooper(queue);
    if (g_app.input_queue == queue) g_app.input_queue = NULL;
    pthread_mutex_unlock(&g_input_mutex);
}

static void onPause(ANativeActivity *activity) {
    ANativeActivity_setWindowFlags(activity, 0u, AWINDOW_FLAG_KEEP_SCREEN_ON);
    fv_local_player_pause();
    atomic_store(&g_app.resumed, false);
    pthread_mutex_lock(&g_state_mutex);
    g_app.monitor_active = false;
    g_app.torch_active = false;
    fv_hold_cancel(&g_app.vis_hold);
    if (!g_app.awaiting_camera_permission) clear_clean_start_locked();
    if (g_app.video_recording_active) {
        stop_video_recording_locked(NULL, 0);
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static void onResume(ANativeActivity *activity) {
    ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0u);
    atomic_store(&g_app.resumed, true);
}

static void onStop(ANativeActivity *activity) {
    (void)activity;
    pthread_mutex_lock(&g_state_mutex);
    clear_clean_start_locked();
    pthread_mutex_unlock(&g_state_mutex);
}

static void onWindowFocusChanged(ANativeActivity *activity, int focused) {
    pthread_mutex_lock(&g_state_mutex);
    g_app.window_focused = focused != 0;
    if (focused && g_app.awaiting_camera_permission) {
        g_app.awaiting_camera_permission = false;
        if (fv_android_has_permission(activity, "android.permission.CAMERA")) {
            g_app.clean_start_deadline_ms = get_time_ms() + 7000L;
            set_status_locked("Camera starting • tap the shutter to cancel");
        } else {
            clear_clean_start_locked();
            set_status_locked("Clean MP4 needs camera permission");
        }
    } else if (!focused && !g_app.awaiting_camera_permission) {
        clear_clean_start_locked();
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static void onDestroy(ANativeActivity *activity) {
    ANativeActivity_setWindowFlags(activity, 0u, AWINDOW_FLAG_KEEP_SCREEN_ON);
    LOGI("ANativeActivity onDestroy");
    atomic_store(&g_app.running, false);
    atomic_store(&g_app.resumed, false);
    pthread_mutex_lock(&g_state_mutex);
    if (g_app.video_recording_active) {
        stop_video_recording_locked(NULL, 0);
    }
    pthread_mutex_unlock(&g_state_mutex);
    /* The camera worker exclusively owns camera start/stop.  Joining it first
     * prevents lifecycle teardown from racing its watchdog restart path. */
    if (g_app.camera_thread_started) {
        pthread_join(g_app.camera_thread, NULL);
        g_app.camera_thread_started = false;
    }
    if (g_app.render_thread_started) {
        pthread_join(g_app.render_thread, NULL);
        g_app.render_thread_started = false;
    }
    if (g_app.sensor_thread_started) {
        pthread_join(g_app.sensor_thread, NULL);
        g_app.sensor_thread_started = false;
    }
    if (g_app.audio_thread_started) {
        pthread_join(g_app.audio_thread, NULL);
        g_app.audio_thread_started = false;
    }
    if (g_app.input_thread_started) {
        pthread_join(g_app.input_thread, NULL);
        g_app.input_thread_started = false;
    }
    fv_local_player_shutdown();
    fv_local_bank_shutdown();
    fv_recording_catalog_free(&g_catalog);
}

void ANativeActivity_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    LOGI("FieldVox Lab NativeActivity onCreate (C23)");

    if (atomic_load(&g_app.running)) {
        LOGW("ANativeActivity_onCreate: tearing down existing instance before re-creating");
        onDestroy(g_app.activity ? g_app.activity : activity);
    }

    memset(&g_app, 0, sizeof(g_app));
    g_app.activity = activity;
    /* Keep the foreground workstation visible without changing device settings. */
    ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0u);
    atomic_init(&g_app.running, true);
    atomic_init(&g_app.resumed, true);
    atomic_init(&g_app.mark_hit_requests, 0u);
    fv_android_text_set_vm(activity->vm);
    g_app.density = read_density(activity);
    g_app.text_scale = fv_android_text_scale(activity->clazz,g_app.density);
    g_app.display_rotation_deg = fv_android_get_display_rotation_degrees(activity);
    g_app.accel_gz = 9.81f;
    g_app.emf_baseline = -1.0f;
    g_app.gyro_baseline = -1.0f;
    g_app.proximity_last = -1.0f;
    g_app.carrier_station = -1;
    g_app.carrier_b_station = -1;
    /* Java defaults: auto live on launch, 250 ms forward, 1.65 uT, 450 ms cooldown. */
    g_app.sweep_active = true;
    g_app.auto_gate = false;
    g_app.cadence_idx = 1;
    g_app.grain_ms = 200;
    g_app.echo_percent = g_app.reverb_percent = g_app.distortion_percent = 50;
    g_app.echo_fx = g_app.reverb_fx = true;
    g_app.direction = FV_SWEEP_FWD;
    g_app.threshold = 1.65f;
    g_app.cooldown_ms = 450;
    g_app.cam_filter = FV_CAM_FILTER_NORMAL;
    g_app.cam_hud_active = true;
    g_app.video_source = FV_VIDEO_CAMERA_ONLY;
    for (int i = 0; i < WF_WIDTH * WF_HEIGHT; i++) g_app.waterfall[i] = 0xFF000000;

    if (!fvg_font_load(FVG_FONT_SANS, "/system/fonts/RobotoStatic-Regular.ttf") &&
        !fvg_font_load(FVG_FONT_SANS, "/system/fonts/Roboto-Regular.ttf")) {
        LOGE("No system sans font could be loaded");
    }
    if (!fvg_font_load(FVG_FONT_MONO, "/system/fonts/DroidSansMono.ttf") &&
        !fvg_font_load(FVG_FONT_MONO, "/system/fonts/CutiveMono.ttf")) {
        fvg_font_load(FVG_FONT_MONO, "/system/fonts/RobotoStatic-Regular.ttf");
    }

    if (!fvg_font_load(FVG_FONT_MEDIUM,"/system/fonts/Roboto-Medium.ttf"))
        fvg_font_load(FVG_FONT_MEDIUM,"/system/fonts/RobotoStatic-Regular.ttf");
    if (!fvg_font_load(FVG_FONT_BOLD,"/system/fonts/Roboto-Bold.ttf"))
        fvg_font_load(FVG_FONT_BOLD,"/system/fonts/RobotoStatic-Regular.ttf");
    const int valid_blob = fv_verify_blob_checksum(FV_STATION_PLAN_BINARY_DATA, sizeof(FV_STATION_PLAN_BINARY_DATA));
    LOGI("Verified C23 Binary Station Blob: %s (%u stations)", valid_blob ? "VALID" : "INVALID",
         (unsigned)FV_STATION_BLOB_COUNT);
    /* Explicit station profile; regional English groups have different voices. */
    const char *radio_language = "en_us";
    if (!fv_radio_start(activity->vm, activity->internalDataPath, radio_language, (int)FV_STATION_BLOB_COUNT)) {
        LOGE("Live radio engine failed to start");
    }

    fv_android_request_runtime_permissions(activity);

    activity->callbacks->onNativeWindowCreated = onNativeWindowCreated;
    activity->callbacks->onNativeWindowDestroyed = onNativeWindowDestroyed;
    activity->callbacks->onInputQueueCreated = onInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = onInputQueueDestroyed;
    activity->callbacks->onConfigurationChanged = onConfigurationChanged;
    activity->callbacks->onPause = onPause;
    activity->callbacks->onResume = onResume;
    activity->callbacks->onStop = onStop;
    activity->callbacks->onWindowFocusChanged = onWindowFocusChanged;
    activity->callbacks->onDestroy = onDestroy;

    fv_local_player_init(activity->vm);
    fv_local_bank_init(activity->internalDataPath);
    g_app.render_thread_started = pthread_create(&g_app.render_thread, NULL, render_thread_func, NULL) == 0;
    g_app.sensor_thread_started = pthread_create(&g_app.sensor_thread, NULL, sensor_thread_func, NULL) == 0;
    g_app.audio_thread_started = pthread_create(&g_app.audio_thread, NULL, audio_thread_func, NULL) == 0;
    g_app.input_thread_started = pthread_create(&g_app.input_thread, NULL, input_thread_func, NULL) == 0;
    g_app.camera_thread_started = pthread_create(&g_app.camera_thread, NULL, camera_thread_func, NULL) == 0;
    if (!g_app.render_thread_started || !g_app.sensor_thread_started || !g_app.audio_thread_started ||
        !g_app.input_thread_started || !g_app.camera_thread_started) {
        LOGE("One or more native worker threads failed to start");
    }
}
