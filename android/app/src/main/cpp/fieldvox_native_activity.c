/*
 * FIELDVOX LAB - NATIVE ACTIVITY (C + ZIG + BINARY BLOBS)
 *
 * Pure C replacement for the Java MainActivity. The screen layout, the phonetic-grain station
 * sweep and the sensor gates follow the Java version:
 *   - Layout: title, logo glyph, FFT/waterfall panel, EMF meter, channel badge, CAL/cadence/
 *     direction/pitch pills, ECHO/REVERB/NOISE/REV pills, a rounded bottom button row, status
 *     line, a hidden Ctrl panel and the big MARK HIT button; drawn with fv_gfx (Roboto text).
 *   - Sound: one live station at a time plays muted and opens only in short grain packets
 *     (fv_grain_gate), triggered by an auto-scan timer and by magnetometer, gyroscope, vibration
 *     and proximity spikes. The carrier moves to the next station after each packet and every
 *     6 s of dwell. fv_native_radio keeps the next stations buffered so the hop is instant.
 */

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/sensor.h>
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

#include "fv_core.h"
#include "fv_gfx.h"
#include "fv_grain_gate.h"
#include "fv_native_audio.h"
#include "fv_native_camera.h"
#include "fv_native_radio.h"
#include "fv_native_video_recorder.h"
#include "fv_station_blob_data.h"

#define TAG "FieldVoxNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 48000
#define BUFFER_FRAMES 1024
#define CHANNEL_RING ((int)FV_STATION_BLOB_COUNT) /* visible logical channel counter, bounded to 512 verified live stations */
#define DWELL_ADVANCE_MS 6000L     /* Java stage1UNAudioDwellPeriodMs */
#define MIC_MONITOR_GAIN 3.0f      /* Java liveOutputGain */
#define WF_WIDTH 120
#define WF_HEIGHT 64
#define ASENSOR_TYPE_PRESSURE_ID 6
#define ASENSOR_TYPE_PROXIMITY_ID 8

static const int FV_SWEEP_CADENCE_MS[4] = {100, 250, 500, 1000};

// Zig DSP & Telemetry Externs
extern int32_t zig_fv_update_barometer(float hpa, float *out_delta);
extern float zig_fv_get_barometer_baseline(void);
extern void zig_fv_reset_barometer_baseline(float hpa);
extern void zig_fv_calc_spatial_pan_gains(float azimuth_deg, float *out_left, float *out_right);
extern void zig_fv_rem_pod_generate(int16_t *samples, size_t frame_count, int32_t level, uint32_t sample_rate);
extern int32_t zig_fv_verify_blob_checksum(const uint8_t *blob_bytes, size_t blob_size);

/* ---------------------------------------------------------------------------------------------
 * Widgets
 * ------------------------------------------------------------------------------------------- */

typedef enum {
    W_CAL, W_CADENCE, W_DIRECTION, W_PITCH,
    W_ECHO, W_REVERB, W_NOISE, W_REV,
    W_SESS, W_REC, W_START, W_MIC, W_CTRL,
    W_SB11, W_VOCAL, W_REMPOD, W_PAN,
    W_ESTES, W_CAMHUD, W_SENS_DOWN, W_SENS_UP,
    W_COOL_DOWN, W_COOL_UP,
    W_MARK_HIT, W_CAM_PREVIEW,
    W_VIDEO_REC, W_CAM_TINT, W_CLOSE_DRAWER,
    W_COUNT
} WidgetId;

typedef struct {
    float x, y, w, h; /* content coordinates (before scrolling) */
    bool visible;
} WidgetRect;

/* ---------------------------------------------------------------------------------------------
 * App state
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    ANativeActivity *activity;
    ANativeWindow *window;
    AInputQueue *input_queue;
    float density;

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
    unsigned pending_gates; /* bit per FvGateReason */
    float pending_delta[FV_GATE_REASON_COUNT];

    /* Controls */
    bool sweep_active;
    int cadence_idx;
    int direction;           /* 1 FWD, -1 REV, 0 RND */
    int pitch_semitones;     /* 0, -2, +2 */
    bool echo_fx, reverb_fx, noise_fx, reverse_fx;
    bool sb11_dual, vocal_filter, rem_pod_active, spatial_pan;
    bool estes_mode, cam_hud_active, cam_hud_red;
    FvCameraFilter cam_filter;
    bool video_recording_active;
    char video_artifact[512];
    bool recording_active, monitor_active;
    bool controls_visible;
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
static pthread_mutex_t g_window_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_record_mutex = PTHREAD_MUTEX_INITIALIZER;
static ALooper *g_input_looper;
static FILE *g_record_file;
static uint32_t g_record_frames;
static uint32_t g_record_checkpoint_frames;
static char g_last_artifact[256]; /* audio thread only */
static uint8_t g_camera_luma[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];
static uint32_t g_camera_argb[FV_CAMERA_WIDTH * FV_CAMERA_HEIGHT];

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

static const char *cadence_label(int idx) {
    static const char *const labels[4] = {"100ms", "250ms", "500ms", "1.0s"};
    return labels[idx & 3];
}

static const char *direction_label(int direction) {
    return direction > 0 ? "FWD" : (direction < 0 ? "REV" : "RND");
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
} Ui;

static float sp(const Ui *ui, float v) { return v * ui->d; }

/* Largest font size <= size_px that fits `max_w`. */
static float fit_size(int font, float size_px, const char *text, float max_w, bool bold) {
    float s = size_px;
    for (int i = 0; i < 12 && s > 6.0f; i++) {
        if (fvg_text_width(font, s, text, bold) <= max_w) break;
        s *= 0.93f;
    }
    return s;
}

static void place(Ui *ui, WidgetId id, float x, float y, float w, float h) {
    ui->rects[id].x = x;
    ui->rects[id].y = y;
    ui->rects[id].w = w;
    ui->rects[id].h = h;
    ui->rects[id].visible = true;
}

typedef struct {
    uint32_t bg, stroke, text;
} PillStyle;

static const PillStyle PILL_IDLE = {0xFF101622, 0xFF384E6C, 0xFFE8F7FF};
static const PillStyle PILL_ACTIVE = {0xFF082215, 0xFF00E676, 0xFF00E676};

/* Java makePillButton: 34 dp tall, 8 dp radius, 10 sp bold label. */
static void draw_pill(Ui *ui, WidgetId id, float x, float y, float w, float h, const char *label, PillStyle st) {
    place(ui, id, x, y, w, h);
    const float sy = y - ui->scroll;
    fvg_round_rect(ui->c, x, sy, w, h, sp(ui, 8), st.bg, st.stroke, fmaxf(1.0f, sp(ui, 1)));
    const float size = fit_size(FVG_FONT_SANS, sp(ui, 10.5f), label, w - sp(ui, 6), true);
    fvg_text_center(ui->c, FVG_FONT_SANS, size, x, sy, w, h, label, st.text, true);
}

/* Java makeCircleButton: 44 dp tall, 12 dp radius, cyan stroke, 12 sp bold white label. */
static void draw_round_button(Ui *ui, WidgetId id, float x, float y, float w, float h, const char *label,
                              bool active, uint32_t active_color) {
    place(ui, id, x, y, w, h);
    const float sy = y - ui->scroll;
    const uint32_t stroke = active ? active_color : 0xFF6FE7FF;
    const uint32_t bg = active ? ((active_color & 0x00FFFFFFu) | 0x33000000u) : 0xFF151722;
    fvg_round_rect(ui->c, x, sy, w, h, sp(ui, 12), 0xFF151722, 0, 0);
    fvg_round_rect(ui->c, x, sy, w, h, sp(ui, 12), bg, stroke, fmaxf(1.0f, sp(ui, 1)));
    const float size = fit_size(FVG_FONT_SANS, sp(ui, 12), label, w - sp(ui, 8), true);
    fvg_text_center(ui->c, FVG_FONT_SANS, size, x, sy, w, h, label, active ? active_color : 0xFFFFFFFF, true);
}

/* Java LogoGlyphView: sensor ring, compass ticks, waveform core, antenna spine and field dots. */
static void __attribute__((unused)) draw_logo(Ui *ui, float cx, float cy, float size) {
    static const float amps[9] = {0.10f, -0.34f, 0.22f, -0.52f, 0.50f, -0.20f, 0.35f, -0.12f, 0.08f};
    FvCanvas *c = ui->c;
    const float r = size * 0.36f;
    const float stroke = fmaxf(1.5f, sp(ui, 2.0f));
    const float soft = fmaxf(1.0f, sp(ui, 1.0f));
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

/* GhostTube-inspired floating neon sound visualizer (32-bar FFT spectrum with peak needles) */
static void draw_floating_visualizer(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now_ms) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float sy = y - ui->scroll;
    place(ui, W_CAM_PREVIEW, x, y, w, h);

    /* Translucent frosted glass backdrop */
    fvg_round_rect(c, x, sy, w, h, 10.0f * d, 0x88050E18, 0x306FE7FF, fmaxf(1.0f, d));

    const char *vlabel = "SOUND VISUALIZER • 48 kHz FFT SPECTRUM";
    const float vs = fit_size(FVG_FONT_MONO, 8.5f * d, vlabel, w - 16.0f * d, false);
    fvg_text_center(c, FVG_FONT_MONO, vs, x, sy + 3.0f * d, w, 14.0f * d, vlabel, 0x996FE7FF, false);

    const int bars = 32;
    const float pad_x = 8.0f * d;
    const float bar_area_w = w - pad_x * 2.0f;
    const float gap = 2.5f * d;
    const float bar_w = (bar_area_w - gap * (float)(bars - 1)) / (float)bars;
    const float base_y = sy + h - 10.0f * d;
    const float max_bar_h = fmaxf(10.0f * d, h - 28.0f * d);

    const float phase = (float)(now_ms / 60 % 1000);

    for (int i = 0; i < bars; i++) {
        float v = s->spectrum[i % FV_NATIVE_SPECTRUM_BINS];
        if (!s->audio_running) {
            v = 0.08f * (1.0f + sinf((float)i * 0.45f + phase * 0.15f));
        }
        if (v > 1.0f) v = 1.0f;
        if (v < 0.02f) v = 0.02f;

        const float bh = v * max_bar_h;
        const float bx = x + pad_x + (float)i * (bar_w + gap);
        const float by = base_y - bh;

        /* Gradient color based on intensity: Cyan -> Green -> Amber -> Red/Pink */
        uint32_t bar_col = 0xFF00E5FF;
        if (v > 0.75f) {
            bar_col = 0xFFFF1744;
        } else if (v > 0.50f) {
            bar_col = 0xFFFFD600;
        } else if (v > 0.25f) {
            bar_col = 0xFF00FF88;
        }

        /* Anti-aliased vertical bar */
        fvg_round_rect(c, bx, by, bar_w, bh, 1.5f * d, bar_col, 0, 0);

        /* Floating peak needle */
        const float needle_y = by - 3.5f * d;
        if (needle_y > sy + 16.0f * d) {
            fvg_fill_rect(c, bx, needle_y, bar_w, 1.5f * d, (bar_col & 0x00FFFFFFu) | 0xDD000000u);
        }
    }

    /* Baseline glow line */
    fvg_line(c, x + pad_x, base_y + 1.0f, x + w - pad_x, base_y + 1.0f, 1.0f, 0x446FE7FF);
}

static void draw_viewfinder_background(FvCanvas *c, const FieldVoxApp *s, int w, int h, float d, long now) {
    if (s->cam_filter != FV_CAM_FILTER_OFF) {
        const bool frame = fv_camera_copy_luma(g_camera_luma);
        if (frame) {
            int cam_w = FV_CAMERA_WIDTH;
            int cam_h = FV_CAMERA_HEIGHT;
            const int rot_deg = fv_camera_get_display_compensation_degrees(s->activity);
            static int s_view_logged = 0;
            if (s_view_logged++ < 3) {
                LOGI("viewfinder: w=%d, h=%d, rot_deg=%d", w, h, rot_deg);
            }
            fv_camera_luma_to_argb_rotated(g_camera_luma, FV_CAMERA_WIDTH, FV_CAMERA_HEIGHT,
                                          g_camera_argb, &cam_w, &cam_h, rot_deg, s->cam_filter);
            fvg_blit_cover(c, g_camera_argb, cam_w, cam_h);
        } else {
            fvg_clear(c, 0xFF060B11);
        }

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
    FvCanvas *c = ui->c;
    const float d = ui->d;

    /* Frosted glass top banner */
    fvg_round_rect(c, x, y, w, h, 8.0f * d, 0xD808121A, 0x506FE7FF, fmaxf(1.0f, d));

    /* 1. Left Title */
    const char *title = "FIELDVOX LAB";
    const float ts = fit_size(FVG_FONT_SANS, 14.0f * d, title, w * 0.28f, true);
    fvg_text_glow(c, FVG_FONT_SANS, ts, x + 10.0f * d, y + h * 0.68f, title, 0xFF6FE7FF, 2.0f * d, true);
    fvg_text(c, FVG_FONT_SANS, ts, x + 10.0f * d, y + h * 0.68f, title, 0xFFFFFFFF, true);

    /* 2. Right: Recording indicator / filter badge (pure status indicator, no widget collision) */
    const float rec_w = (ui->w > 500.0f * d ? 112.0f : 96.0f) * d;
    const float rec_h = h - 8.0f * d;
    const float rec_x = x + w - rec_w - 6.0f * d;
    const float rec_y = y + 4.0f * d;

    if (s->video_recording_active) {
        const uint32_t dur = fv_video_recorder_get_duration_ms();
        const unsigned mins = (dur / 1000u) / 60u;
        const unsigned secs = (dur / 1000u) % 60u;
        char rec_text[32];
        snprintf(rec_text, sizeof(rec_text), "REC %02u:%02u", mins, secs);

        fvg_round_rect(c, rec_x, rec_y, rec_w, rec_h, 6.0f * d, 0xEE380008, 0xFFFF1744, fmaxf(1.5f, 1.5f * d));
        if ((now / 500L) % 2 == 0) {
            fvg_circle(c, rec_x + 12.0f * d, rec_y + rec_h * 0.5f, 4.0f * d, 0xFFFF1744, 0, 0);
        }
        fvg_text_center(c, FVG_FONT_MONO, 11.0f * d, rec_x + 12.0f * d, rec_y, rec_w - 14.0f * d, rec_h,
                        rec_text, 0xFFFFFFFF, true);
    } else if (s->recording_active) {
        fvg_round_rect(c, rec_x, rec_y, rec_w, rec_h, 6.0f * d, 0xEE332005, 0xFFFF9100, fmaxf(1.0f, d));
        fvg_text_center(c, FVG_FONT_MONO, 10.0f * d, rec_x, rec_y, rec_w, rec_h, "WAV REC", 0xFFFFE6A0, true);
    } else {
        fvg_round_rect(c, rec_x, rec_y, rec_w, rec_h, 6.0f * d, 0xD00A1A28, 0x806FE7FF, fmaxf(1.0f, d));
        fvg_text_center(c, FVG_FONT_MONO, 9.5f * d, rec_x, rec_y, rec_w, rec_h,
                        fv_camera_filter_name(s->cam_filter), 0xFF6FE7FF, true);
    }

    /* 3. Center Channel Badge */
    char badge[96];
    if (!s->sweep_active) {
        snprintf(badge, sizeof(badge), "CH %03d/%03d • SWEEP STOPPED", s->channel_counter, CHANNEL_RING);
    } else if (s->carrier_ready && s->carrier_station >= 0) {
        snprintf(badge, sizeof(badge), "CH %03d/%03d • %s", s->channel_counter, CHANNEL_RING,
                 station_at(s->carrier_station)->callsign);
    } else {
        snprintf(badge, sizeof(badge), "CH %03d/%03d • BUFFERING", s->channel_counter, CHANNEL_RING);
    }

    const float center_x = x + w * 0.28f + 6.0f * d;
    const float center_w = rec_x - center_x - 6.0f * d;
    const float bs = fit_size(FVG_FONT_SANS, 11.5f * d, badge, center_w - 8.0f * d, true);
    fvg_text_center(c, FVG_FONT_SANS, bs, center_x, y, center_w, h, badge, 0xFFE0F7FF, true);
}

static void draw_perimeter_badges(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now_ms) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float half_w = (w - 6.0f * d) * 0.5f;

    /* Left Badge: EMF Meter - tap to zero-calibrate! */
    const float lx = x;
    place(ui, W_CAL, lx, y, half_w, h);
    const bool cal = now_ms < s->cal_flash_until_ms;
    const uint32_t emf_stroke = cal ? 0xFF00E676 : (s->emf_spike ? 0xFFFF1744 :
                                (s->emf_delta >= 1.5f ? 0xFFFFD600 : 0x606FE7FF));
    const uint32_t emf_bg = s->emf_spike ? 0xDD280509 : 0xC00A131C;
    fvg_round_rect(c, lx, y, half_w, h, 6.0f * d, emf_bg, emf_stroke, fmaxf(1.0f, d));

    char emf_str[64];
    snprintf(emf_str, sizeof(emf_str), "EMF %.1f µT%s", s->emf_magnitude,
             cal ? " [CAL: OK]" : (s->emf_spike ? " [SPIKE!]" : ""));
    const uint32_t emf_text_col = s->emf_spike ? 0xFFFF5252 : (cal ? 0xFF00E676 : 0xFFFFFFFF);
    const float es = fit_size(FVG_FONT_SANS, 10.5f * d, emf_str, half_w * 0.65f, true);
    fvg_text(c, FVG_FONT_SANS, es, lx + 8.0f * d, y + h * 0.65f, emf_str, emf_text_col, true);

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
    const float rx = x + half_w + 6.0f * d;
    place(ui, W_REMPOD, rx, y, half_w, h);
    char telem[96];
    uint32_t tel_bg = 0xC00A131C;
    uint32_t tel_stroke = 0x606FE7FF;
    uint32_t tel_text = 0xFFD2F0FF;

    if (s->baro_coldspot) {
        tel_bg = 0xDD042036;
        tel_stroke = 0xFF00E5FF;
        tel_text = 0xFF00E5FF;
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
        tel_stroke = 0xFF00E5FF;
        tel_text = 0xFF80D8FF;
        snprintf(telem, sizeof(telem), "REM-POD ARMED • DIR %.0f°", s->compass_azimuth);
    } else {
        snprintf(telem, sizeof(telem), "BARO %.1f hPa • DIR %.0f°", s->baro_current, s->compass_azimuth);
    }

    fvg_round_rect(c, rx, y, half_w, h, 6.0f * d, tel_bg, tel_stroke, fmaxf(1.0f, d));
    const float ts = fit_size(FVG_FONT_SANS, 10.5f * d, telem, half_w - 12.0f * d, true);
    fvg_text_center(c, FVG_FONT_SANS, ts, rx, y, half_w, h, telem, tel_text, true);
}

/* GhostTube VOX style camera dock with prominent circular record shutter button */
static void draw_bottom_action_bar(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now) {
    FvCanvas *c = ui->c;
    const float d = ui->d;
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;

    /* Outer circular shutter radius (28dp portrait, 22dp landscape) */
    const float r_outer = (h >= 52.0f * d ? 28.0f : 21.0f) * d;
    const float gap = 5.0f * d;
    const float btn_h = (h >= 52.0f * d ? 38.0f : 30.0f) * d;
    const float btn_y = cy - btn_h * 0.5f;

    /* Space available on each flank of the center shutter */
    const float side_clearance = r_outer + 8.0f * d;
    const float avail_w = (w * 0.5f) - side_clearance;
    const float btn_w = (avail_w - gap) * 0.5f;

    /* --- Left Flank: SWEEP (ARM/LIVE) & MARK HIT --- */
    {
        /* Button 1: W_START (Radio Sweep Live Toggle) */
        const float b1_x = x;
        place(ui, W_START, b1_x, btn_y, btn_w, btn_h);
        const uint32_t b1_bg = s->sweep_active ? 0xDD072615 : 0xD00A131C;
        const uint32_t b1_stroke = s->sweep_active ? 0xFF00E676 : 0x806FE7FF;
        const uint32_t b1_txt = s->sweep_active ? 0xFF00E676 : 0xFFD2F0FF;
        fvg_round_rect(c, b1_x, btn_y, btn_w, btn_h, 10.0f * d, b1_bg, b1_stroke, fmaxf(1.0f, 1.2f * d));
        if (s->sweep_active) {
            fvg_circle(c, b1_x + 13.0f * d, btn_y + btn_h * 0.5f, 3.2f * d, 0xFF00E676, 0, 0);
            const float s1 = fit_size(FVG_FONT_SANS, 11.5f * d, "LIVE", btn_w - 20.0f * d, true);
            fvg_text_center(c, FVG_FONT_SANS, s1, b1_x + 6.0f * d, btn_y, btn_w - 6.0f * d, btn_h, "LIVE", b1_txt, true);
        } else {
            const float s1 = fit_size(FVG_FONT_SANS, 11.5f * d, "SWEEP", btn_w - 6.0f * d, true);
            fvg_text_center(c, FVG_FONT_SANS, s1, b1_x, btn_y, btn_w, btn_h, "SWEEP", b1_txt, true);
        }

        /* Button 2: W_MARK_HIT (Pre-Roll Lossless Audio Capture) */
        const float b2_x = b1_x + btn_w + gap;
        place(ui, W_MARK_HIT, b2_x, btn_y, btn_w, btn_h);
        fvg_round_rect(c, b2_x, btn_y, btn_w, btn_h, 10.0f * d, 0xD0241406, 0xFFFF9100, fmaxf(1.0f, 1.2f * d));
        const float s2 = fit_size(FVG_FONT_SANS, 11.0f * d, "MARK HIT", btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s2, b2_x, btn_y, btn_w, btn_h, "MARK HIT", 0xFFFFD180, true);
    }

    /* --- Center: GhostTube Circular Camera Shutter (W_VIDEO_REC) --- */
    {
        /* Generous touch bounding box */
        const float hit_pad = 6.0f * d;
        place(ui, W_VIDEO_REC, cx - r_outer - hit_pad, cy - r_outer - hit_pad,
              (r_outer + hit_pad) * 2.0f, (r_outer + hit_pad) * 2.0f);

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
            /* Centered white text label */
            fvg_text_center(c, FVG_FONT_SANS, 10.5f * d, cx - r_inner, cy - r_inner,
                            r_inner * 2.0f, r_inner * 2.0f, "REC", 0xFFFFFFFF, true);
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
            fvg_text_center(c, FVG_FONT_SANS, 9.5f * d, cx - sq_half, cy - sq_half,
                            sq_size, sq_size, "STOP", 0xFFFFFFFF, true);
        }
    }

    /* --- Right Flank: LENS (Spectral Filter) & SETTINGS (Drawer) --- */
    {
        /* Button 3: W_CAM_TINT (Spectral Lens Switcher) */
        const float b3_x = cx + side_clearance;
        place(ui, W_CAM_TINT, b3_x, btn_y, btn_w, btn_h);
        fvg_round_rect(c, b3_x, btn_y, btn_w, btn_h, 10.0f * d, 0xD0061A28, 0xFF00E5FF, fmaxf(1.0f, 1.2f * d));

        char lens_name[32];
        const char *fname = fv_camera_filter_name(s->cam_filter);
        snprintf(lens_name, sizeof(lens_name), "%s", fname);
        const float s3 = fit_size(FVG_FONT_SANS, 10.5f * d, lens_name, btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s3, b3_x, btn_y, btn_w, btn_h, lens_name, 0xFF80D8FF, true);

        /* Button 4: W_CTRL (DSP & Paranormal Settings Drawer) */
        const float b4_x = b3_x + btn_w + gap;
        place(ui, W_CTRL, b4_x, btn_y, btn_w, btn_h);
        const uint32_t b4_bg = s->controls_visible ? 0xDD280A35 : 0xD01E0A28;
        fvg_round_rect(c, b4_x, btn_y, btn_w, btn_h, 10.0f * d, b4_bg, 0xFFE040FB, fmaxf(1.0f, 1.2f * d));
        const char *b4_label = s->controls_visible ? "CLOSE" : "SETTINGS";
        const float s4 = fit_size(FVG_FONT_SANS, 10.5f * d, b4_label, btn_w - 6.0f * d, true);
        fvg_text_center(c, FVG_FONT_SANS, s4, b4_x, btn_y, btn_w, btn_h, b4_label, 0xFFFF80AB, true);
    }
}

static void draw_slide_up_drawer(Ui *ui, const FieldVoxApp *s, float x, float y, float w, float h, long now) {
    FvCanvas *c = ui->c;
    const float d = ui->d;

    /* Semi-opaque backdrop shadow & card */
    fvg_round_rect(c, x, y, w, h, 14.0f * d, 0xF5081019, 0xFF6FE7FF, fmaxf(1.0f, d * 1.5f));

    /* Drawer Header */
    const float header_h = 28.0f * d;
    const char *header = "DSP & INVESTIGATION SETTINGS";
    fvg_text_center(c, FVG_FONT_SANS, 12.0f * d, x, y + 2.0f * d, w, header_h, header, 0xFF6FE7FF, true);

    /* Close Button in Header */
    const float close_w = 40.0f * d, close_h = 22.0f * d;
    const float close_x = x + w - close_w - 6.0f * d, close_y = y + 4.0f * d;
    draw_pill(ui, W_CLOSE_DRAWER, close_x, close_y, close_w, close_h, "X", PILL_IDLE);

    float cur_y = y + header_h + 4.0f * d;
    const float gap = 3.0f * d;
    const float pill_h = (h - header_h - 32.0f * d) / 5.0f;

    /* Row 1: Sweep Controls (Cadence, Direction, Pitch, Cal) */
    const float p4_w = (w - gap * 3.0f - 16.0f * d) / 4.0f;
    {
        const bool cal = now < s->cal_flash_until_ms;
        PillStyle cal_style = PILL_IDLE;
        if (cal) cal_style.text = 0xFF00E676;
        PillStyle pitch_style = PILL_IDLE;
        const char *pitch_label = "PITCH 0";
        if (s->pitch_semitones < 0) {
            pitch_style = (PillStyle){0xFF0C1F2D, 0xFF6FE7FF, 0xFF6FE7FF};
            pitch_label = "-2st";
        } else if (s->pitch_semitones > 0) {
            pitch_style = (PillStyle){0xFF2E0D1B, 0xFFFF80AB, 0xFFFF80AB};
            pitch_label = "+2st";
        }

        float px = x + 8.0f * d;
        draw_pill(ui, W_CADENCE, px, cur_y, p4_w, pill_h, cadence_label(s->cadence_idx), PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_DIRECTION, px, cur_y, p4_w, pill_h, direction_label(s->direction), PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_PITCH, px, cur_y, p4_w, pill_h, pitch_label, pitch_style);
        px += p4_w + gap;
        draw_pill(ui, W_CAL, px, cur_y, p4_w, pill_h, cal ? "CAL: OK" : "CAL ZERO", cal_style);
        cur_y += pill_h + gap;
    }

    /* Row 2: FX Toggles */
    {
        float px = x + 8.0f * d;
        draw_pill(ui, W_ECHO, px, cur_y, p4_w, pill_h, "ECHO", s->echo_fx ? PILL_ACTIVE : PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_REVERB, px, cur_y, p4_w, pill_h, "REVERB", s->reverb_fx ? PILL_ACTIVE : PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_NOISE, px, cur_y, p4_w, pill_h, "NOISE", s->noise_fx ? PILL_ACTIVE : PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_REV, px, cur_y, p4_w, pill_h, "REV FX", s->reverse_fx ? PILL_ACTIVE : PILL_IDLE);
        cur_y += pill_h + gap;
    }

    /* Row 3: Specialized Modes */
    const float p5_w = (w - gap * 4.0f - 16.0f * d) / 5.0f;
    {
        float px = x + 8.0f * d;
        draw_pill(ui, W_SB11, px, cur_y, p5_w, pill_h, "SB11", s->sb11_dual ? PILL_ACTIVE : PILL_IDLE);
        px += p5_w + gap;
        draw_pill(ui, W_VOCAL, px, cur_y, p5_w, pill_h, "VOCAL", s->vocal_filter ? PILL_ACTIVE : PILL_IDLE);
        px += p5_w + gap;
        draw_pill(ui, W_PAN, px, cur_y, p5_w, pill_h, "3D PAN", s->spatial_pan ? PILL_ACTIVE : PILL_IDLE);
        px += p5_w + gap;
        draw_pill(ui, W_REMPOD, px, cur_y, p5_w, pill_h, "REM-POD", s->rem_pod_active ? PILL_ACTIVE : PILL_IDLE);
        px += p5_w + gap;
        draw_pill(ui, W_ESTES, px, cur_y, p5_w, pill_h, "ESTES", PILL_IDLE);
        cur_y += pill_h + gap;
    }

    /* Row 4: Sensitivity & Cooldown */
    {
        float px = x + 8.0f * d;
        const float step_w = 44.0f * d;
        const float val_w = (w - step_w * 4.0f - gap * 5.0f - 16.0f * d) * 0.5f;

        draw_pill(ui, W_SENS_DOWN, px, cur_y, step_w, pill_h, "-", PILL_IDLE);
        px += step_w + gap;

        char s_str[48];
        snprintf(s_str, sizeof(s_str), "Sens %.2fµT", s->threshold);
        fvg_text_center(c, FVG_FONT_MONO, 10.0f * d, px, cur_y, val_w, pill_h, s_str, 0xFFFFFFFF, false);
        px += val_w + gap;

        draw_pill(ui, W_SENS_UP, px, cur_y, step_w, pill_h, "+", PILL_IDLE);
        px += step_w + gap;

        draw_pill(ui, W_COOL_DOWN, px, cur_y, step_w, pill_h, "-", PILL_IDLE);
        px += step_w + gap;

        char c_str[48];
        snprintf(c_str, sizeof(c_str), "Cool %dms", s->cooldown_ms);
        fvg_text_center(c, FVG_FONT_MONO, 10.0f * d, px, cur_y, val_w, pill_h, c_str, 0xFFFFFFFF, false);
        px += val_w + gap;

        draw_pill(ui, W_COOL_UP, px, cur_y, step_w, pill_h, "+", PILL_IDLE);
        cur_y += pill_h + gap;
    }

    /* Row 5: Audio Session & Recording Tools */
    {
        float px = x + 8.0f * d;
        draw_pill(ui, W_MIC, px, cur_y, p4_w, pill_h, s->monitor_active ? "MIC ON" : "MIC OFF",
                  s->monitor_active ? PILL_ACTIVE : PILL_IDLE);
        px += p4_w + gap;
        draw_pill(ui, W_REC, px, cur_y, p4_w, pill_h, s->recording_active ? "WAV ON" : "SESS WAV",
                  s->recording_active ? PILL_ACTIVE : PILL_IDLE);
        px += p4_w + gap;
        PillStyle rec_st = PILL_IDLE;
        if (s->video_recording_active) rec_st = (PillStyle){0xDD3A050B, 0xFFFF1744, 0xFFFF5252};
        draw_pill(ui, W_VIDEO_REC, px, cur_y, p4_w, pill_h,
                  s->video_recording_active ? "VID STOP" : "VID MP4", rec_st);
        px += p4_w + gap;
        draw_pill(ui, W_SESS, px, cur_y, p4_w, pill_h, "STATS", PILL_IDLE);
    }
}

static void draw_estes(FvCanvas *c, float d, const FieldVoxApp *s, int w, int h) {
    fvg_clear(c, 0xFF000000);
    fvg_text_center(c, FVG_FONT_SANS, 26 * d, 0, (float)h * 0.40f, (float)w, 40 * d, "ESTES PROTOCOL", 0xFF881111, true);
    char hits[64];
    snprintf(hits, sizeof(hits), "Hits logged: %d", s->estes_hit_count);
    fvg_text_center(c, FVG_FONT_SANS, 16 * d, 0, (float)h * 0.40f + 48 * d, (float)w, 30 * d, hits, 0xFF2A6A78, false);
    fvg_text_center(c, FVG_FONT_SANS, 13 * d, 0, (float)h - 90 * d, (float)w, 30 * d, "Tap anywhere to log a hit",
                    0xFF3A3A3A, false);
    const float bw = 150 * d, bh = 44 * d, bx = (float)w - bw - 16 * d, by = 60 * d;
    fvg_round_rect(c, bx, by, bw, bh, 10 * d, 0xFF210707, 0xFF661111, d);
    fvg_text_center(c, FVG_FONT_SANS, 13 * d, bx, by, bw, bh, "EXIT ESTES", 0xFFAA2222, true);
}

/* Estes exit box, in screen coordinates (matches draw_estes). */
static bool estes_exit_hit(float d, int w, float x, float y) {
    const float bw = 150 * d, bh = 44 * d, bx = (float)w - bw - 16 * d, by = 60 * d;
    return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
}

static void render_frame(ANativeWindow_Buffer *buf) {
    const int w = buf->width, h = buf->height;
    FvCanvas canvas;
    fvg_canvas_init(&canvas, (uint32_t *)buf->bits, w, h, buf->stride);

    static FieldVoxApp s;
    pthread_mutex_lock(&g_state_mutex);
    memcpy(&s, &g_app, sizeof(s));
    pthread_mutex_unlock(&g_state_mutex);

    const float d = s.density > 0.5f ? s.density : 3.0f;
    const long now = get_time_ms();
    if (s.estes_mode) {
        draw_estes(&canvas, d, &s, w, h);
        if (fv_video_recorder_is_active()) {
            fv_video_recorder_feed_frame((const uint32_t *)buf->bits, buf->width, buf->height, buf->stride);
        }
        return;
    }

    const bool landscape = w > h;
    const float top_inset = (landscape ? 14.0f : 58.0f) * d;
    const float side_inset = (landscape ? 36.0f : 12.0f) * d;
    const float bottom_inset = (landscape ? 10.0f : 32.0f) * d;

    WidgetRect rects[W_COUNT];
    memset(rects, 0, sizeof(rects));
    Ui ui = {&canvas, d, 0.0f, side_inset, (float)w - 2.0f * side_inset, rects};

    /* 1. Camera Viewfinder Background */
    draw_viewfinder_background(&canvas, &s, w, h, d, now);

    /* 2. Top Status Header HUD */
    const float header_h = (landscape ? 28.0f : 36.0f) * d;
    draw_top_header(&ui, &s, ui.x0, top_inset, ui.w, header_h, now);

    /* 3. Status Line (Amber/Gold) */
    const float status_y = top_inset + header_h + 3.0f * d;
    const float status_h = (landscape ? 14.0f : 18.0f) * d;
    {
        char status_line[160];
        if (now < s.status_until_ms) {
            snprintf(status_line, sizeof(status_line), "%s", s.status_message);
        } else if (!s.sweep_active) {
            snprintf(status_line, sizeof(status_line), "Sweep armed • tap ARM to start live stations");
        } else if (s.carrier_ready) {
            snprintf(status_line, sizeof(status_line), "Auto Live active • %d stations buffered • %s",
                     s.live_decks, s.gate_open ? "gate OPEN" : "listening");
        } else {
            snprintf(status_line, sizeof(status_line), "Auto Live • connecting to live streams…");
        }
        const float ss = fit_size(FVG_FONT_SANS, (landscape ? 9.5f : 11.0f) * d, status_line, ui.w, false);
        fvg_text_center(&canvas, FVG_FONT_SANS, ss, ui.x0, status_y, ui.w, status_h, status_line, 0xFFFFE6A0, false);
    }

    /* 4. Perimeter HUD Badges (EMF & Sensors) */
    const float badge_y = status_y + status_h + 3.0f * d;
    const float badge_h = (landscape ? 24.0f : 30.0f) * d;
    draw_perimeter_badges(&ui, &s, ui.x0, badge_y, ui.w, badge_h, now);

    /* 5. Bottom Action Bar position (GhostTube camera dock) */
    const float bottom_bar_h = (landscape ? 46.0f : 66.0f) * d;
    const float bottom_bar_y = (float)h - bottom_inset - bottom_bar_h;

    /* 6. Centered Floating Sound Visualizer (only rendered and clickable when drawer is closed) */
    const float vis_h = (landscape ? 54.0f : 80.0f) * d;
    const float vis_top = bottom_bar_y - vis_h - 8.0f * d;
    if (!s.controls_visible) {
        draw_floating_visualizer(&ui, &s, ui.x0, vis_top, ui.w, vis_h, now);
    }

    /* 7. Bottom Action Bar (only rendered and clickable when drawer is closed) */
    if (!s.controls_visible) {
        draw_bottom_action_bar(&ui, &s, ui.x0, bottom_bar_y, ui.w, bottom_bar_h, now);
    }

    /* 8. Slide-Up Drawer Overlay (if toggled) */
    if (s.controls_visible) {
        const float drawer_h = (landscape ? 240.0f : 290.0f) * d;
        const float drawer_y = (float)h - bottom_inset - drawer_h;
        draw_slide_up_drawer(&ui, &s, ui.x0, drawer_y, ui.w, drawer_h, now);
    }

    /* Save widget hit rects */
    pthread_mutex_lock(&g_state_mutex);
    memcpy(g_app.widgets, rects, sizeof(rects));
    g_app.max_scroll = 0.0f;
    g_app.scroll_y = 0.0f;
    pthread_mutex_unlock(&g_state_mutex);

    /* Feed video recorder frame if active */
    if (fv_video_recorder_is_active()) {
        fv_video_recorder_feed_frame((const uint32_t *)buf->bits, buf->width, buf->height, buf->stride);
    }
}

static void *render_thread_func(void *arg) {
    (void)arg;
    while (atomic_load(&g_app.running)) {
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
        usleep(33333); // ~30 FPS
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Touch handling: tap on release, drag to scroll (Java ScrollView behaviour)
 * ------------------------------------------------------------------------------------------- */

static int count_saved_files(const char *suffix) {
    if (!g_app.activity) return 0;
    const char *base = g_app.activity->externalDataPath ? g_app.activity->externalDataPath : g_app.activity->internalDataPath;
    if (!base) return 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/fieldvox-recordings", base);
    DIR *dp = opendir(dir);
    if (!dp && g_app.activity->internalDataPath) {
        snprintf(dir, sizeof(dir), "%s/fieldvox-hits", g_app.activity->internalDataPath);
        dp = opendir(dir);
    }
    if (!dp) return 0;
    int count = 0;
    const size_t slen = strlen(suffix);
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n >= slen && strcmp(e->d_name + n - slen, suffix) == 0) count++;
    }
    closedir(dp);
    return count;
}

/* Caller holds g_state_mutex. */
static void handle_widget(WidgetId id) {
    char msg[128];
    switch (id) {
        case W_CAL: {
            /* Java nativeCalibrateSensors: subtract the current magnetic baseline. */
            if (g_app.emf_baseline > 0.0f) g_app.emf_cal_offset += g_app.emf_baseline;
            g_app.emf_baseline = -1.0f;
            zig_fv_reset_barometer_baseline(g_app.baro_current);
            g_app.cal_flash_until_ms = get_time_ms() + 1200L;
            set_status_locked("Sensors zero-calibrated");
            break;
        }
        case W_CADENCE:
            g_app.cadence_idx = (g_app.cadence_idx + 1) % 4;
            snprintf(msg, sizeof(msg), "Channel sweep cadence %s", cadence_label(g_app.cadence_idx));
            set_status_locked(msg);
            break;
        case W_DIRECTION:
            g_app.direction = g_app.direction > 0 ? -1 : (g_app.direction < 0 ? 0 : 1);
            fv_radio_set_direction(g_app.direction);
            break;
        case W_PITCH:
            g_app.pitch_semitones = g_app.pitch_semitones == 0 ? -2 : (g_app.pitch_semitones < 0 ? 2 : 0);
            break;
        case W_ECHO: g_app.echo_fx = !g_app.echo_fx; break;
        case W_REVERB: g_app.reverb_fx = !g_app.reverb_fx; break;
        case W_NOISE: g_app.noise_fx = !g_app.noise_fx; break;
        case W_REV: g_app.reverse_fx = !g_app.reverse_fx; break;
        case W_SESS: {
            const int mp4_count = count_saved_files(".mp4");
            const int total_wav = count_saved_files(".wav");
            const int preroll_hits = count_saved_files("-preroll.wav");
            const int session_wav = total_wav >= preroll_hits ? total_wav - preroll_hits : 0;
            snprintf(msg, sizeof(msg), "Saved: %d MP4 Videos • %d Session WAV • %d Pre-Roll Hits",
                     mp4_count, session_wav, preroll_hits);
            set_status_locked(msg);
            break;
        }
        case W_REC:
            g_app.recording_active = !g_app.recording_active;
            set_status_locked(g_app.recording_active ? "Session WAV recording started" : "Session WAV recording saving…");
            break;
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
        case W_CTRL: g_app.controls_visible = !g_app.controls_visible; break;
        case W_SB11: g_app.sb11_dual = !g_app.sb11_dual; break;
        case W_VOCAL: g_app.vocal_filter = !g_app.vocal_filter; break;
        case W_REMPOD: g_app.rem_pod_active = !g_app.rem_pod_active; break;
        case W_PAN: g_app.spatial_pan = !g_app.spatial_pan; break;
        case W_ESTES:
            g_app.estes_mode = true;
            g_app.vocal_filter = true;
            break;
        case W_CAMHUD:
            if (g_app.cam_filter == FV_CAM_FILTER_OFF) {
                g_app.cam_filter = FV_CAM_FILTER_PHOSPHOR;
                g_app.cam_hud_active = true;
                if (!fv_android_has_permission(g_app.activity, "android.permission.CAMERA")) {
                    fv_android_request_permission(g_app.activity, "android.permission.CAMERA", 61);
                }
                set_status_locked("Camera night-vision ON");
            } else {
                g_app.cam_filter = FV_CAM_FILTER_OFF;
                g_app.cam_hud_active = false;
                set_status_locked("Camera night-vision OFF");
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
            set_status_locked("Capturing 3-second pre-roll hit…");
            break;
        case W_CAM_PREVIEW: {
            g_app.cam_filter = (FvCameraFilter)((g_app.cam_filter + 1) % FV_CAM_FILTER_COUNT);
            g_app.cam_hud_active = (g_app.cam_filter != FV_CAM_FILTER_OFF);
            char tmsg[64];
            snprintf(tmsg, sizeof(tmsg), "Camera filter: %s", fv_camera_filter_name(g_app.cam_filter));
            set_status_locked(tmsg);
            break;
        }
        case W_VIDEO_REC: {
            if (!fv_video_recorder_is_active()) {
                if (!fv_android_has_permission(g_app.activity, "android.permission.CAMERA") ||
                    !fv_android_has_permission(g_app.activity, "android.permission.RECORD_AUDIO")) {
                    fv_android_request_runtime_permissions(g_app.activity);
                }
                char dir[512];
                const char *base = g_app.activity->externalDataPath ? g_app.activity->externalDataPath : g_app.activity->internalDataPath;
                snprintf(dir, sizeof(dir), "%s/fieldvox-recordings", base ? base : "/sdcard");
                mkdir(dir, 0755);
                char path[512];
                snprintf(path, sizeof(path), "%s/session-%lld.mp4", dir, get_wall_time_ms());

                bool land = false;
                pthread_mutex_lock(&g_window_mutex);
                ANativeWindow *win = g_app.window;
                if (win) {
                    ANativeWindow_acquire(win);
                    land = ANativeWindow_getWidth(win) > ANativeWindow_getHeight(win);
                    ANativeWindow_release(win);
                }
                pthread_mutex_unlock(&g_window_mutex);

                const int vw = land ? 1280 : 720;
                const int vh = land ? 720 : 1280;
                if (fv_video_recorder_start(path, vw, vh, SAMPLE_RATE, 1)) {
                    g_app.video_recording_active = true;
                    snprintf(g_app.video_artifact, sizeof(g_app.video_artifact), "%s", path);
                    set_status_locked("Video recording started (H.264 + AAC MP4)");
                } else {
                    set_status_locked("Video recording failed to start");
                }
            } else {
                char saved_path[512];
                fv_video_recorder_stop(saved_path, sizeof(saved_path));
                g_app.video_recording_active = false;
                char vmsg[256];
                if (saved_path[0] != '\0') {
                    snprintf(vmsg, sizeof(vmsg), "Video saved: %s", saved_path);
                } else {
                    snprintf(vmsg, sizeof(vmsg), "Video recording stopped");
                }
                set_status_locked(vmsg);
            }
            break;
        }
        case W_CAM_TINT: {
            g_app.cam_filter = (FvCameraFilter)((g_app.cam_filter + 1) % FV_CAM_FILTER_COUNT);
            g_app.cam_hud_active = (g_app.cam_filter != FV_CAM_FILTER_OFF);
            char tmsg[64];
            snprintf(tmsg, sizeof(tmsg), "Camera filter: %s", fv_camera_filter_name(g_app.cam_filter));
            set_status_locked(tmsg);
            break;
        }
        case W_CLOSE_DRAWER: {
            g_app.controls_visible = false;
            break;
        }
        default: break;
    }
}

static void handle_tap(float x, float y_screen, int screen_w) {
    pthread_mutex_lock(&g_state_mutex);
    if (g_app.estes_mode) {
        if (estes_exit_hit(g_app.density, screen_w, x, y_screen)) {
            g_app.estes_mode = false;
            set_status_locked("Estes protocol ended");
        } else {
            g_app.estes_hit_count++;
            atomic_fetch_add(&g_app.mark_hit_requests, 1u);
        }
        pthread_mutex_unlock(&g_state_mutex);
        return;
    }

    bool hit = false;
    for (int i = W_COUNT - 1; i >= 0; i--) {
        const WidgetRect *r = &g_app.widgets[i];
        if (!r->visible) continue;
        if (x >= r->x && x <= r->x + r->w && y_screen >= r->y && y_screen <= r->y + r->h) {
            LOGI("tap widget %d", i);
            handle_widget((WidgetId)i);
            hit = true;
            break;
        }
    }

    if (!hit && g_app.controls_visible) {
        const float d = g_app.density > 0.5f ? g_app.density : 3.0f;
        const WidgetRect *close_r = &g_app.widgets[W_CLOSE_DRAWER];
        const float drawer_top = close_r->visible ? (close_r->y - 6.0f * d) : 0.0f;
        if (close_r->visible && y_screen < drawer_top) {
            g_app.controls_visible = false;
        }
    }

    pthread_mutex_unlock(&g_state_mutex);
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
    while (atomic_load(&g_app.running)) {
        const int ident = ALooper_pollOnce(50, NULL, NULL, NULL);
        if (ident != 2) continue;
        for (;;) {
            AInputEvent *event = NULL;
            bool tap = false;
            float tap_x = 0, tap_y = 0;
            int screen_w = 0;
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
                } else if (action == AMOTION_EVENT_ACTION_MOVE && tracking) {
                    if (!dragging && fabsf(ey - down_y) > slop) dragging = true;
                    if (dragging && !g_app.estes_mode) {
                        float s = down_scroll - (ey - down_y);
                        g_app.scroll_y = s < 0.0f ? 0.0f : (s > g_app.max_scroll ? g_app.max_scroll : s);
                    }
                } else if (action == AMOTION_EVENT_ACTION_UP && tracking) {
                    tracking = false;
                    if (!dragging && fabsf(ex - down_x) <= slop) {
                        tap = true;
                        tap_x = ex;
                        tap_y = ey;
                    }
                } else if (action == AMOTION_EVENT_ACTION_CANCEL) {
                    tracking = false;
                }
                pthread_mutex_unlock(&g_state_mutex);
                handled = 1;
            }
            pthread_mutex_lock(&g_window_mutex);
            if (g_app.window) screen_w = ANativeWindow_getWidth(g_app.window);
            pthread_mutex_unlock(&g_window_mutex);
            AInputQueue_finishEvent(queue, event, handled);
            pthread_mutex_unlock(&g_input_mutex);
            if (tap) handle_tap(tap_x, tap_y, screen_w);
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
static void raise_gate(FvGateReason reason, float delta) {
    g_app.pending_gates |= 1u << reason;
    g_app.pending_delta[reason] = delta;
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
            pthread_mutex_lock(&g_state_mutex);
            if (ev.type == ASENSOR_TYPE_MAGNETIC_FIELD) {
                const float mag = fv_mag3(ev.magnetic.x, ev.magnetic.y, ev.magnetic.z);
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
                    g_app.emf_spike = g_app.emf_delta >= fmaxf(2.10f, g_app.threshold * 0.90f);
                    g_app.compass_azimuth = atan2f(ev.magnetic.y, ev.magnetic.x) * (180.0f / (float)M_PI);
                    if (g_app.emf_spike) raise_gate(FV_GATE_MAGNETOMETER, g_app.emf_delta);
                }
            } else if (ev.type == ASENSOR_TYPE_ACCELEROMETER) {
                g_app.accel_gx = g_app.accel_gx * 0.92f + ev.acceleration.x * 0.08f;
                g_app.accel_gy = g_app.accel_gy * 0.92f + ev.acceleration.y * 0.08f;
                g_app.accel_gz = g_app.accel_gz * 0.92f + ev.acceleration.z * 0.08f;
                const float vx = ev.acceleration.x - g_app.accel_gx;
                const float vy = ev.acceleration.y - g_app.accel_gy;
                const float vz = ev.acceleration.z - g_app.accel_gz;
                g_app.vibe_magnitude = sqrtf(vx * vx + vy * vy + vz * vz);
                g_app.vibe_spike = g_app.vibe_magnitude >= 0.45f;
                if (g_app.vibe_spike) raise_gate(FV_GATE_VIBE, g_app.vibe_magnitude);
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
                    /* Gyro is movement sensitive: a secondary trigger with a longer cooldown. */
                    if (delta >= 0.18f) raise_gate(FV_GATE_GYROSCOPE, delta);
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
                        raise_gate(FV_GATE_PROXIMITY, fabsf(v - g_app.proximity_last));
                    }
                    g_app.proximity_last = v;
                }
            } else if (ev.type == ASENSOR_TYPE_PRESSURE_ID) {
                float delta = 0.0f;
                const int coldspot = zig_fv_update_barometer(ev.pressure, &delta);
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
            pthread_mutex_unlock(&g_state_mutex);
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
 * Recording
 * ------------------------------------------------------------------------------------------- */

static void write_wav_header(FILE *file, uint32_t frames) {
    if (!file) return;
    const uint32_t pcm_bytes = frames * 2u;
    const uint32_t riff_size = pcm_bytes + 36u;
    const uint32_t sample_rate = SAMPLE_RATE;
    const uint32_t byte_rate = SAMPLE_RATE * 2u;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint16_t block_align = 2;
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &riff_size, 4);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;
    header[20] = 1;
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &pcm_bytes, 4);
    fseek(file, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), file);
}

static void finish_recording(void) {
    pthread_mutex_lock(&g_record_mutex);
    if (g_record_file) {
        write_wav_header(g_record_file, g_record_frames);
        fclose(g_record_file);
        g_record_file = NULL;
        LOGI("Session recording saved: %s (%u frames)", g_last_artifact, g_record_frames);
        set_status("Session WAV saved to app storage");
    }
    g_record_frames = 0;
    g_record_checkpoint_frames = 0;
    pthread_mutex_unlock(&g_record_mutex);
}

static void stop_recording_on_error(const char *message) {
    pthread_mutex_lock(&g_state_mutex);
    g_app.recording_active = false;
    set_status_locked(message);
    pthread_mutex_unlock(&g_state_mutex);
}

static void begin_recording(void) {
    if (g_record_file || !g_app.activity || !g_app.activity->internalDataPath) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/fieldvox-hits", g_app.activity->internalDataPath);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        LOGE("Cannot create recording directory: %s", strerror(errno));
        stop_recording_on_error("Recording directory failed");
        return;
    }
    snprintf(g_last_artifact, sizeof(g_last_artifact), "%s/session-%lld.wav", dir, get_wall_time_ms());
    pthread_mutex_lock(&g_record_mutex);
    g_record_file = fopen(g_last_artifact, "wb+");
    g_record_frames = 0;
    g_record_checkpoint_frames = 0;
    if (g_record_file) write_wav_header(g_record_file, 0);
    pthread_mutex_unlock(&g_record_mutex);
    if (!g_record_file) {
        LOGE("Cannot open recording: %s", strerror(errno));
        stop_recording_on_error("Recording open failed");
    }
}

static void write_recording_block(const int16_t *mono, int frames) {
    pthread_mutex_lock(&g_record_mutex);
    if (g_record_file) {
        const size_t written = fwrite(mono, sizeof(int16_t), (size_t)frames, g_record_file);
        g_record_frames += (uint32_t)written;
        if (written != (size_t)frames) {
            LOGE("Recording write failed: %s", strerror(errno));
            pthread_mutex_unlock(&g_record_mutex);
            stop_recording_on_error("Recording write failed");
            return;
        }
        if (g_record_frames - g_record_checkpoint_frames >= SAMPLE_RATE) {
            write_wav_header(g_record_file, g_record_frames);
            if (fseek(g_record_file, 0, SEEK_END) != 0 || fflush(g_record_file) != 0) {
                LOGE("Recording checkpoint failed: %s", strerror(errno));
                pthread_mutex_unlock(&g_record_mutex);
                stop_recording_on_error("Recording checkpoint failed");
                return;
            }
            g_record_checkpoint_frames = g_record_frames;
        }
    }
    pthread_mutex_unlock(&g_record_mutex);
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
static int next_carrier_deck(const bool ok[FV_RADIO_DECKS], const int station[FV_RADIO_DECKS],
                             int current, int direction, int avoid) {
    const int n = (int)FV_STATION_BLOB_COUNT;
    const int cur_station = current >= 0 ? station[current] : -1;
    int best = -1, best_dist = n + 1;
    int candidates[FV_RADIO_DECKS], count = 0;
    for (int dk = 0; dk < FV_RADIO_DECKS; dk++) {
        if (!ok[dk] || dk == current || dk == avoid) continue;
        candidates[count++] = dk;
        if (cur_station < 0) continue;
        const int s_dk = (station[dk] % n + n) % n;
        const int s_cur = (cur_station % n + n) % n;
        const int dist = direction >= 0 ? (s_dk - s_cur + n) % n : (s_cur - s_dk + n) % n;
        if (dist > 0 && dist < best_dist) {
            best_dist = dist;
            best = dk;
        }
    }
    if (count == 0) return (current >= 0 && ok[current] && current != avoid) ? current : -1;
    if (direction == 0 || best < 0) return candidates[(uint32_t)rand() % (uint32_t)count];
    return best;
}

typedef struct {
    FvGrainGate gate;
    int deck;
    long since_ms;
} Carrier;

/* Keeps a carrier on a ready deck; advances it when the gate asks or its dwell runs out. */
static bool update_carrier(Carrier *c, const bool ok[FV_RADIO_DECKS], const int station[FV_RADIO_DECKS],
                           int direction, int avoid, bool advance, long now_ms, uint64_t clock) {
    const bool dwell_over = c->deck >= 0 && !c->gate.open && now_ms - c->since_ms >= DWELL_ADVANCE_MS;
    if (c->deck < 0 || !ok[c->deck] || c->deck == avoid || advance || dwell_over) {
        const int next = next_carrier_deck(ok, station, c->deck >= 0 && ok[c->deck] ? c->deck : -1, direction, avoid);
        if (next != c->deck) {
            const bool was_idle = c->deck < 0;
            c->deck = next;
            if (next >= 0 && was_idle) fv_gate_carrier_started(&c->gate, clock);
        }
        c->since_ms = now_ms;
    }
    return c->deck >= 0 && ok[c->deck];
}

static void push_waterfall_column(uint32_t *img, const float *bins) {
    for (int y = 0; y < WF_HEIGHT; y++) {
        uint32_t *row = img + y * WF_WIDTH;
        memmove(row, row + 1, (WF_WIDTH - 1) * sizeof(uint32_t));
        const float v = bins[(WF_HEIGHT - 1) - y];
        uint32_t col;
        /* Java WavePanelView.heatmapColor */
        if (v <= 0.02f) col = 0xFF050914;
        else if (v >= 1.0f) col = 0xFFFFFFFF;
        else {
            float r, g, b, t;
            if (v < 0.25f) { t = v / 0.25f; r = 5 + t * 55; g = 9 + t * 6; b = 20 + t * 100; }
            else if (v < 0.50f) { t = (v - 0.25f) / 0.25f; r = 60 - t * 60; g = 15 + t * 185; b = 120 + t * 135; }
            else if (v < 0.75f) { t = (v - 0.50f) / 0.25f; r = t * 255; g = 200 + t * 30; b = 255 - t * 255; }
            else { t = (v - 0.75f) / 0.25f; r = 255; g = 230 + t * 25; b = t * 255; }
            col = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        row[WF_WIDTH - 1] = col;
    }
}

static void *audio_thread_func(void *arg) {
    (void)arg;
    AAudioStream *stream = open_audio_stream(AAUDIO_DIRECTION_OUTPUT, 2);
    if (!stream) {
        LOGE("AAudio output stream failed");
        return NULL;
    }
    AAudioStream *input = NULL;
    long next_input_retry_ms = 0;
    fv_audio_dsp_reset();

    int16_t mono[BUFFER_FRAMES];
    int16_t mic[BUFFER_FRAMES];
    int16_t stereo[BUFFER_FRAMES * 2];
    float gain_a[BUFFER_FRAMES], gain_b[BUFFER_FRAMES];
    static int16_t deck_buf[FV_RADIO_DECKS][BUFFER_FRAMES];
    uint64_t clock = 0;
    Carrier ca = {.deck = -1}, cb = {.deck = -1};
    fv_gate_init(&ca.gate, SAMPLE_RATE, (uint32_t)get_time_ms() | 1u, clock);
    fv_gate_init(&cb.gate, SAMPLE_RATE, (uint32_t)get_time_ms() * 2654435761u | 1u, clock);
    bool was_open = false;
    int wf_divider = 0;
    unsigned diag_packets = 0;
    long diag_last_ms = get_time_ms();

    while (atomic_load(&g_app.running)) {
        const long now = get_time_ms();
        pthread_mutex_lock(&g_state_mutex);
        const bool resumed = atomic_load(&g_app.resumed);
        const bool sweep = g_app.sweep_active && resumed;
        const bool dual = g_app.sb11_dual && sweep;
        const bool monitor = g_app.monitor_active && resumed;
        const bool recording = g_app.recording_active && resumed;
        const bool vocal = g_app.vocal_filter, reverb = g_app.reverb_fx, echo = g_app.echo_fx;
        const bool noise = g_app.noise_fx, reverse = g_app.reverse_fx, rem_pod = g_app.rem_pod_active;
        const bool pan = g_app.spatial_pan;
        const int pitch = g_app.pitch_semitones, direction = g_app.direction, cooldown = g_app.cooldown_ms;
        const int rem_tier = g_app.rem_pod_tier;
        const float azimuth = g_app.compass_azimuth;
        const unsigned gates = sweep ? g_app.pending_gates : 0u;
        float gate_delta[FV_GATE_REASON_COUNT];
        memcpy(gate_delta, g_app.pending_delta, sizeof(gate_delta));
        g_app.pending_gates = 0;
        /* Visible channel counter ticks at the cadence, independent of the audio (Java stage1UL). */
        if (sweep && now - g_app.last_channel_tick_ms >= FV_SWEEP_CADENCE_MS[g_app.cadence_idx]) {
            g_app.channel_counter = direction == 0 ? (int)((uint32_t)rand() % CHANNEL_RING)
                                                   : (g_app.channel_counter + direction + CHANNEL_RING) % CHANNEL_RING;
            g_app.last_channel_tick_ms = now;
        }
        pthread_mutex_unlock(&g_state_mutex);

        /* 1. Pull every deck so all stay at "now"; only the carriers are ever audible. */
        fv_radio_set_enabled(sweep);
        bool ok[FV_RADIO_DECKS];
        int station[FV_RADIO_DECKS];
        int live = 0;
        for (int dk = 0; dk < FV_RADIO_DECKS; dk++) {
            const int got = fv_radio_pull(dk, deck_buf[dk], BUFFER_FRAMES);
            station[dk] = fv_radio_deck_station(dk);
            ok[dk] = sweep && got == BUFFER_FRAMES && station[dk] >= 0 && fv_radio_deck_state(dk) == FV_RADIO_LIVE;
            if (ok[dk]) live++;
        }

        /* 2. Sensor gates go to carrier A (Java routed every sensor to the one carrier). */
        bool ready_a = sweep && ca.deck >= 0 && ok[ca.deck];
        for (int r = 0; r < FV_GATE_REASON_COUNT; r++) {
            if (gates & (1u << r)) fv_gate_request(&ca.gate, clock, (FvGateReason)r, gate_delta[r], cooldown, ready_a);
        }
        bool advance_a = fv_gate_process(&ca.gate, clock, gain_a, BUFFER_FRAMES, ready_a, cooldown);
        if (!sweep) {
            ca.deck = -1;
            cb.deck = -1;
        } else {
            ready_a = update_carrier(&ca, ok, station, direction, -1, advance_a, now, clock);
        }
        bool ready_b = false;
        if (dual) {
            ready_b = cb.deck >= 0 && ok[cb.deck] && cb.deck != ca.deck;
            const bool advance_b = fv_gate_process(&cb.gate, clock, gain_b, BUFFER_FRAMES, ready_b, cooldown);
            /* SB11 dual: the second carrier sweeps the other way. */
            ready_b = update_carrier(&cb, ok, station, direction == 0 ? 0 : -direction, ca.deck, advance_b, now, clock);
        } else {
            cb.deck = -1;
        }

        /* 3. Mix: carrier grains, then the optional mic monitor. */
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            float s = 0.0f;
            if (ready_a) s += (float)deck_buf[ca.deck][i] * gain_a[i];
            if (ready_b) s += (float)deck_buf[cb.deck][i] * gain_b[i];
            mono[i] = (int16_t)(s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s));
        }

        if (!input && monitor && now >= next_input_retry_ms) {
            next_input_retry_ms = now + 1000L;
            if (fv_android_has_permission(g_app.activity, "android.permission.RECORD_AUDIO")) {
                input = open_audio_stream(AAUDIO_DIRECTION_INPUT, 1);
                next_input_retry_ms = now + 3000L;
                LOGI("AAudio microphone input: %s", input ? "ACTIVE" : "RETRY PENDING");
            }
        }
        if (input && !monitor) {
            AAudioStream_requestStop(input);
            AAudioStream_close(input);
            input = NULL;
        }
        if (input) {
            const aaudio_result_t got = AAudioStream_read(input, mic, BUFFER_FRAMES, 0);
            if (got > 0) {
                for (int i = 0; i < got; i++) {
                    const float v = (float)mono[i] + (float)mic[i] * MIC_MONITOR_GAIN;
                    mono[i] = (int16_t)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
                }
            } else if (got < 0) {
                LOGW("AAudio input read failed: %s", AAudio_convertResultToText(got));
                AAudioStream_requestStop(input);
                AAudioStream_close(input);
                input = NULL;
            }
        }

        /* 4. Optional effects (all off by default, so the default sound is the Java grain sound). */
        if (rem_pod && rem_tier > 0) zig_fv_rem_pod_generate(mono, BUFFER_FRAMES, rem_tier, SAMPLE_RATE);
        if (vocal) fv_vocal_bandpass(mono, BUFFER_FRAMES, SAMPLE_RATE);
        if (reverb) fv_apply_schroeder_reverb(mono, BUFFER_FRAMES);
        if (pitch != 0) fv_apply_granular_pitch(mono, BUFFER_FRAMES, pitch);
        if (echo) fv_audio_apply_echo(mono, BUFFER_FRAMES);
        if (noise) fv_audio_apply_noise(mono, BUFFER_FRAMES, 700);
        if (reverse) fv_audio_reverse(mono, BUFFER_FRAMES);

        /* 5. Analysis, pre-roll ring and UI state. */
        fv_audio_capture_and_analyze(mono, BUFFER_FRAMES);
        float spectrum[FV_NATIVE_SPECTRUM_BINS];
        fv_audio_get_spectrum(spectrum);
        const bool open_now = ca.gate.open || (dual && cb.gate.open);
        if (open_now && !was_open) diag_packets++;
        pthread_mutex_lock(&g_state_mutex);
        memcpy(g_app.spectrum, spectrum, sizeof(spectrum));
        if (++wf_divider >= 2) {
            wf_divider = 0;
            push_waterfall_column(g_app.waterfall, spectrum);
        }
        g_app.audio_running = sweep || monitor;
        g_app.live_decks = live;
        g_app.carrier_ready = ready_a;
        g_app.carrier_station = ready_a ? station[ca.deck] : -1;
        g_app.carrier_b_station = ready_b ? station[cb.deck] : -1;
        g_app.gate_open = ca.gate.open;
        g_app.gate_reason = ca.gate.reason;
        if (open_now && !was_open) g_app.packets_opened++;
        pthread_mutex_unlock(&g_state_mutex);
        was_open = open_now;

        if (now - diag_last_ms >= 5000L) {
            LOGI("sweep diag: live_decks=%d carrier=%d station=%d packets=%u gate_open=%d dual=%d",
                 live, ca.deck, ready_a ? station[ca.deck] : -1, diag_packets, ca.gate.open, dual);
            diag_packets = 0;
            diag_last_ms = now;
        }

        const unsigned hits = atomic_exchange(&g_app.mark_hit_requests, 0u);
        for (unsigned k = 0; k < hits && g_app.activity && g_app.activity->internalDataPath; k++) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/fieldvox-hits", g_app.activity->internalDataPath);
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/hit-%lld-%u-preroll.wav", dir, get_wall_time_ms(), k);
            if (fv_audio_export_preroll_wav(path, 3)) {
                LOGI("Pre-roll hit saved: %s", path);
                set_status("3-second pre-roll hit saved");
            } else {
                set_status("Pre-roll hit save failed");
            }
        }

        if (recording && !g_record_file) begin_recording();
        if (!recording && g_record_file) finish_recording();
        write_recording_block(mono, BUFFER_FRAMES);

        if (fv_video_recorder_is_active()) {
            fv_video_recorder_feed_audio(mono, BUFFER_FRAMES);
        }

        float gl = 0.707f, gr = 0.707f;
        if (pan) zig_fv_calc_spatial_pan_gains(azimuth, &gl, &gr);
        for (int i = 0; i < BUFFER_FRAMES; i++) {
            const float v = (float)mono[i];
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
    finish_recording();
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
        const bool cam = (g_app.cam_filter != FV_CAM_FILTER_OFF) || g_app.cam_hud_active;
        pthread_mutex_unlock(&g_state_mutex);
        const bool resumed = atomic_load(&g_app.resumed);
        const bool has_perm = fv_android_has_permission(g_app.activity, "android.permission.CAMERA");

        if (resumed && cam && has_perm) {
            const bool active = fv_camera_is_active();
            const bool healthy = fv_camera_is_healthy(2000);
            if (!active || !healthy) {
                if (active && !healthy) {
                    LOGI("Camera frame watchdog triggered (> 2000 ms stall) - restarting camera pipeline");
                }
                fv_camera_stop();
                if (!fv_camera_start()) {
                    fv_camera_stop();
                    sleep(1);
                }
            }
        } else if (fv_camera_is_active()) {
            fv_camera_stop();
        }
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
    pthread_mutex_lock(&g_state_mutex);
    g_app.density = density;
    pthread_mutex_unlock(&g_state_mutex);
    LOGI("ANativeWindow created: %dx%d density=%.2f", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
         density);
}

static void onNativeWindowDestroyed(ANativeActivity *activity, ANativeWindow *window) {
    (void)activity;
    pthread_mutex_lock(&g_window_mutex);
    if (g_app.window == window) g_app.window = NULL;
    pthread_mutex_unlock(&g_window_mutex);
}

static void onConfigurationChanged(ANativeActivity *activity) {
    const float density = read_density(activity);
    pthread_mutex_lock(&g_state_mutex);
    g_app.density = density;
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
    (void)activity;
    atomic_store(&g_app.resumed, false);
    pthread_mutex_lock(&g_state_mutex);
    g_app.monitor_active = false;
    g_app.recording_active = false;
    if (g_app.video_recording_active) {
        g_app.video_recording_active = false;
        fv_video_recorder_stop(NULL, 0);
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static void onResume(ANativeActivity *activity) {
    atomic_store(&g_app.resumed, true);
    fv_android_request_runtime_permissions(activity);
}

static void onDestroy(ANativeActivity *activity) {
    (void)activity;
    LOGI("ANativeActivity onDestroy");
    atomic_store(&g_app.running, false);
    atomic_store(&g_app.resumed, false);
    pthread_mutex_lock(&g_state_mutex);
    if (g_app.video_recording_active) {
        g_app.video_recording_active = false;
        fv_video_recorder_stop(NULL, 0);
    }
    pthread_mutex_unlock(&g_state_mutex);
    fv_camera_stop();
    if (g_app.render_thread_started) pthread_join(g_app.render_thread, NULL);
    if (g_app.sensor_thread_started) pthread_join(g_app.sensor_thread, NULL);
    if (g_app.audio_thread_started) pthread_join(g_app.audio_thread, NULL);
    if (g_app.input_thread_started) pthread_join(g_app.input_thread, NULL);
    if (g_app.camera_thread_started) pthread_join(g_app.camera_thread, NULL);
}

void ANativeActivity_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    LOGI("FieldVox Lab NativeActivity onCreate (C / Zig)");

    memset(&g_app, 0, sizeof(g_app));
    g_app.activity = activity;
    atomic_init(&g_app.running, true);
    atomic_init(&g_app.resumed, true);
    atomic_init(&g_app.mark_hit_requests, 0u);
    g_app.density = read_density(activity);
    g_app.accel_gz = 9.81f;
    g_app.emf_baseline = -1.0f;
    g_app.gyro_baseline = -1.0f;
    g_app.proximity_last = -1.0f;
    g_app.carrier_station = -1;
    g_app.carrier_b_station = -1;
    /* Java defaults: auto live on launch, 250 ms forward, 1.65 uT, 450 ms cooldown. */
    g_app.sweep_active = true;
    g_app.cadence_idx = 1;
    g_app.direction = 1;
    g_app.threshold = 1.65f;
    g_app.cooldown_ms = 450;
    g_app.cam_filter = FV_CAM_FILTER_PHOSPHOR;
    g_app.cam_hud_active = true;
    for (int i = 0; i < WF_WIDTH * WF_HEIGHT; i++) g_app.waterfall[i] = 0xFF050914;

    if (!fvg_font_load(FVG_FONT_SANS, "/system/fonts/RobotoStatic-Regular.ttf") &&
        !fvg_font_load(FVG_FONT_SANS, "/system/fonts/Roboto-Regular.ttf")) {
        LOGE("No system sans font could be loaded");
    }
    if (!fvg_font_load(FVG_FONT_MONO, "/system/fonts/DroidSansMono.ttf") &&
        !fvg_font_load(FVG_FONT_MONO, "/system/fonts/CutiveMono.ttf")) {
        fvg_font_load(FVG_FONT_MONO, "/system/fonts/RobotoStatic-Regular.ttf");
    }

    const int valid_blob = zig_fv_verify_blob_checksum(FV_STATION_PLAN_BINARY_DATA, sizeof(FV_STATION_PLAN_BINARY_DATA));
    LOGI("Verified Zig Binary Station Blob: %s (%u stations)", valid_blob ? "VALID" : "INVALID",
         (unsigned)FV_STATION_BLOB_COUNT);
    fv_radio_set_direction(g_app.direction);
    if (!fv_radio_start(activity->vm, (int)FV_STATION_BLOB_COUNT)) {
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
    activity->callbacks->onDestroy = onDestroy;

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
