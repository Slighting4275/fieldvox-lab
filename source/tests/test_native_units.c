/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "../../android/app/src/main/cpp/fv_station_blob_data.h"
#include "../../android/app/src/main/cpp/fv_native_audio.h"
#include "../../android/app/src/main/cpp/fv_hit_capture.h"
#include "../../android/app/src/main/cpp/fv_vox_controls.h"
#include "../../android/app/src/main/cpp/fv_sensor_dsp.h"
#include "../../android/app/src/main/cpp/fv_fragment_engine.h"
#include "../../android/app/src/main/cpp/fv_local_bank.h"
#include "../../android/app/src/main/cpp/fv_ui_layout.h"

static void rm_tree_recursive(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[512];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) rm_tree_recursive(path);
        else unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void test_station_blob_and_mag3(void) {
    assert(FV_STATION_BLOB_COUNT == 289U);
    assert(fv_verify_blob_checksum(FV_STATION_PLAN_BINARY_DATA,
                                   sizeof(FV_STATION_PLAN_BINARY_DATA)) == 1);
    assert(fabsf(fv_mag3(3.0f, 4.0f, 12.0f) - 13.0f) < 0.001f);
    assert(fv_gate_emf_exceeds_threshold(1.66f, 1.65f));
    assert(!fv_gate_emf_exceeds_threshold(1.64f, 1.65f));
    assert(strcmp(fv_gate_reason_name(FV_GATE_PROXIMITY), "proximity") == 0);
    assert(strcmp(fv_gate_reason_name(FV_GATE_MAGNETOMETER), "magnetometer") == 0);
    assert(strcmp(fv_gate_reason_name(FV_GATE_AUTO_SCAN), "auto-scan") == 0);
}

static void test_fragment_gating_and_cooldown(void) {
    FvFragmentEngine *e = fv_fragment_create();
    assert(e != NULL);

    static int16_t pcm[48000];
    for (size_t i = 0; i < 48000u; i++) {
        pcm[i] = (int16_t)(15000.0 * sin(2.0 * 3.14159265358979323846 * 200.0 * (double)i / 48000.0));
    }
    assert(fv_fragment_cache_add(e, pcm, 48000u, 3, -1));

    fv_fragment_set_settings(e, (FvFragmentSettings){
        .reverb = 0.5f,
        .pitch_override = 1.2f,
        .auto_enabled = false,
        .local_only = false,
        .sweep_ms = 200u
    });

    /* With auto_enabled=false, stepping with no queued triggers emits nothing. */
    FvFragmentStepResult res = {0};
    fv_fragment_step(e, 0u, false, NULL, &res);
    assert(!res.admitted && !res.request_started && !res.used_auto_trigger);

    /* First cached sensor trigger succeeds with pitch_override applied. */
    const FvVoxDecimalTrigger t_cache = {
        .passed = true, .pitch_factor = 0.6f, .speed_factor = 1.0f,
        .grain_duration_ms = 400, .seek_offset = 0.1f, .channel_hop = 0, .prefer_live = false
    };
    fv_fragment_submit_trigger(e, FV_GATE_VIBE, &t_cache);
    fv_fragment_step(e, 48000u, false, NULL, &res);
    assert(res.admitted && res.reason == FV_GATE_VIBE);

    FvFragmentStatus status = {0};
    int16_t clean[64], proc[64], aux[64];
    fv_fragment_render(e, clean, proc, aux, 64u, 48000u, &status);
    assert(fabsf(status.pitch - 1.2f) < 0.01f);

    /* Non-proximity trigger inside cooldown (2.0 + 8.0 * 0.5 = 6.0 s = 288000 frames) is blocked. */
    fv_fragment_submit_trigger(e, FV_GATE_MAGNETOMETER, &t_cache);
    fv_fragment_step(e, 96000u, false, NULL, &res);
    assert(!res.admitted);

    /* Proximity bypasses cooldown. */
    fv_fragment_submit_trigger(e, FV_GATE_PROXIMITY, &t_cache);
    fv_fragment_step(e, 96000u, false, NULL, &res);
    assert(res.admitted && res.reason == FV_GATE_PROXIMITY);

    fv_fragment_destroy(e);
}

static void test_sensor_dsp_and_vox(void) {
    fv_reset_barometer_baseline(1013.25f);
    assert(fabsf(fv_get_barometer_baseline() - 1013.25f) < 0.01f);
    float delta = 0.0f;
    assert(fv_update_barometer(1013.00f, &delta) == 1);
    assert(delta <= -0.20f);

    float l = 0.0f, r = 0.0f;
    fv_calc_spatial_pan_gains(90.0f, &l, &r);
    assert(r > l);

    int16_t buf[256] = {0};
    fv_rem_pod_generate(buf, 256u, 3, 48000u);
    int peak = 0;
    for (int i = 0; i < 256; i++) {
        if (abs(buf[i]) > peak) peak = abs(buf[i]);
    }
    assert(peak > 0);

    FvVoxEchoTap taps[FV_VOX_ECHO_TAPS];
    const size_t n = fv_vox_echo_schedule(1.0f, 12000u, taps);
    assert(n == FV_VOX_ECHO_TAPS);
    assert(taps[0].delay == 12000u && taps[1].delay == 24000u);
    assert(fv_vox_slider_percent(0.0f, 0.0f, 100.0f) == 0);
    assert(fv_vox_slider_percent(100.0f, 0.0f, 100.0f) == 100);
    assert(fv_vox_slider_percent(50.0f, 0.0f, 100.0f) == 50);

    FvVoxDecimalTrigger trig = {0};
    assert(fv_vox_eval_decimal_trigger_ex(50.005, 0.0, 0.0, true, 0.5f, 15, &trig) ||
           !trig.passed);
}

static void test_hit_capture_and_preroll(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/fv_hits_test_%d", (int)getpid());
    rm_tree_recursive(dir);
    assert(mkdir(dir, 0700) == 0);

    fv_audio_dsp_reset();
    fv_hit_capture_reset();

    int16_t dry[480], proc[480];
    for (int i = 0; i < 480; i++) {
        dry[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979323846 * 400.0 * (double)i / 48000.0));
        proc[i] = (int16_t)(dry[i] / 2);
    }
    const FvHitBlock blk = {
        .sample_start = 0u,
        .echo_percent = 50,
        .reverb_percent = 50,
        .distortion_percent = 50,
        .grain_ms = 200,
        .station_a = 1,
        .station_b = -1,
        .audible_a = true,
        .reason_a = FV_GATE_MAGNETOMETER,
        .pitch_factor = 1.0f,
        .playback_speed = 1.0f
    };
    fv_hit_capture_push(dry, proc, 480u, &blk);
    char saved_path[512];
    assert(fv_hit_capture_save(dir, 1700000000000ULL, 1u, saved_path, sizeof(saved_path)));
    struct stat st;
    assert(stat(saved_path, &st) == 0 && st.st_size > 44);

    fv_audio_capture_and_analyze(dry, 480);
    float bins[FV_NATIVE_SPECTRUM_BINS];
    fv_audio_get_spectrum(bins);

    char wav_path[300];
    snprintf(wav_path, sizeof(wav_path), "%s/preroll.wav", dir);
    assert(fv_audio_export_preroll_wav(wav_path, 1));
    assert(stat(wav_path, &st) == 0 && st.st_size > 44);

    fv_audio_reverse(proc, 480);
    fv_vocal_bandpass(proc, 480, 48000u);

    rm_tree_recursive(dir);
}

static void test_local_bank_cache_cap(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/fv_cap_test_%d", (int)getpid());
    rm_tree_recursive(dir);
    assert(mkdir(dir, 0700) == 0);

    for (int i = 0; i < 6; i++) {
        char path[320];
        snprintf(path, sizeof(path), "%s/clip_%d.wav", dir, i);
        FILE *f = fopen(path, "wb");
        assert(f != NULL);
        fwrite("test", 1u, 4u, f);
        fclose(f);
    }
    const int evicted = fv_local_bank_enforce_cache_cap(dir, 4u);
    assert(evicted == 2);

    rm_tree_recursive(dir);
}

static void check_playback_layout(int w, int h, float d, bool audio_only, const bool is_video[4]) {
    WidgetRect rects[W_COUNT];
    FvUiScreenLayout layout;
    FvUiState st = {
        .playback_visible = true,
        .playback_audio_only = audio_only,
        .playback_controls_hidden = false
    };
    fv_ui_layout_active(rects, w, h, d, &st, 0, is_video, &layout);
    assert(layout.play_header_end < layout.play_footer_start);
    assert(rects[W_PLAY_BACK].visible && rects[W_PLAY_MUTE].visible && rects[W_PLAY_SEEK].visible);
    assert(rects[W_PLAY_TOGGLE].visible && rects[W_PLAY_MARK].visible);
    assert(rects[W_PLAY_BACK].y >= 0.0f && rects[W_PLAY_MARK].y + rects[W_PLAY_MARK].h <= (float)h + 0.01f);
    assert(rects[W_PLAY_BACK].y + rects[W_PLAY_BACK].h < rects[W_PLAY_SEEK].y);
    assert(rects[W_PLAY_SEEK].y + rects[W_PLAY_SEEK].h < rects[W_PLAY_TOGGLE].y);
    assert(rects[W_PLAY_TOGGLE].y + rects[W_PLAY_TOGGLE].h < rects[W_PLAY_MARK].y);
    if (audio_only && h > 2000) assert(rects[W_PLAY_SEEK].h > 1000.0f);
}

static void test_ui_controller_and_layout(void) {
    assert(fv_ui_grain_ms_to_percent(65) == 0);
    assert(fv_ui_grain_ms_to_percent(1000) == 100);
    assert(fv_ui_percent_to_grain_ms(0) == 65);
    assert(fv_ui_percent_to_grain_ms(100) == 1000);
    const int mid_pct = fv_ui_grain_ms_to_percent(532);
    assert(abs(fv_ui_percent_to_grain_ms(mid_pct) - 532) <= 15);

    WidgetRect rects[W_COUNT];
    const bool is_video[4] = {false, true, false, false};
    FvUiScreenLayout layout = {0};

    /* The same shutter and mode choices are reachable in every drawer state. */
    FvUiState capture = {0};
    const int capture_sizes[][2] = {{1080, 2400}, {2400, 1080}};
    for (size_t i = 0; i < sizeof(capture_sizes) / sizeof(capture_sizes[0]); i++) {
        const int width = capture_sizes[i][0], height = capture_sizes[i][1];
        capture = (FvUiState){0};
        fv_ui_layout_active(rects, width, height, 3.0f, &capture, 0, is_video, &layout);
        const WidgetRect shutter = rects[W_RECORD];
        const WidgetRect clean = rects[W_CAPTURE_CLEAN], screen = rects[W_CAPTURE_SCREEN];
        assert(shutter.visible && clean.visible && screen.visible);
        assert(clean.x + clean.w < screen.x && clean.y + clean.h < shutter.y);
        assert(layout.vis_top + layout.vis_h < clean.y);
        assert(fv_ui_resolve_tap(rects, clean.x + clean.w / 2, clean.y + clean.h / 2) == W_CAPTURE_CLEAN);
        assert(fv_ui_resolve_tap(rects, screen.x + screen.w / 2, screen.y + screen.h / 2) == W_CAPTURE_SCREEN);
        for (int advanced = 0; advanced < 2; advanced++) {
            capture.controls_visible = true;
            capture.advanced_controls = advanced != 0;
            fv_ui_layout_active(rects, width, height, 3.0f, &capture, 0, is_video, &layout);
            assert(layout.drawer_y + layout.drawer_h < clean.y);
            assert(rects[W_RECORD].x == shutter.x && rects[W_RECORD].y == shutter.y);
            const float shutter_x = shutter.x + shutter.w / 2;
            const float shutter_y = shutter.y + shutter.h / 2;
            assert(fv_ui_handle_tap(&capture, rects, shutter_x, shutter_y, 3.0f, NULL) == W_RECORD);
            assert(capture.controls_visible);
        }
    }

    /* Short landscape fits the recording dock beside the shutter and leaves the HUD clear. */
    capture = (FvUiState){0};
    fv_ui_layout_active(rects, 1920, 720, 3.0f, &capture, 0, is_video, &layout);
    assert(layout.vis_top >= layout.badge_y + layout.badge_h && rects[W_CAM_PREVIEW].visible);
    assert(rects[W_CAPTURE_CLEAN].x + rects[W_CAPTURE_CLEAN].w < rects[W_RECORD].x);
    assert(rects[W_RECORD].x + rects[W_RECORD].w < rects[W_CAPTURE_SCREEN].x);
    capture.controls_visible = true;
    fv_ui_layout_active(rects, 1920, 720, 3.0f, &capture, 0, is_video, &layout);
    const WidgetRect advanced_button = rects[W_ADVANCED];
    assert(advanced_button.h >= 12.0f * 3.0f);
    assert(fv_ui_resolve_tap(rects, advanced_button.x + advanced_button.w / 2,
                            advanced_button.y + advanced_button.h / 2) == W_ADVANCED);

    /* Landscape with standard VOX drawer & advanced drawer open */
    FvUiState st = {.controls_visible = true, .advanced_controls = false};
    fv_ui_layout_active(rects, 2400, 1080, 3.0f, &st, 0, is_video, &layout);
    assert(layout.landscape);
    assert(rects[W_START].visible && rects[W_ECHO_LEVEL].visible && rects[W_CLOSE_DRAWER].visible);
    assert(rects[W_RADIO_TUNER].visible);

    st.advanced_controls = true;
    fv_ui_layout_active(rects, 2400, 1080, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_CADENCE].visible && rects[W_ADVANCED].visible && rects[W_CLOSE_DRAWER].visible);

    /* Radio Tuner modal overlay in portrait and landscape */
    st = (FvUiState){.tuner_visible = true};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 5, is_video, &layout);
    assert(rects[W_CLOSE_TUNER].visible && rects[W_TUNER_BANK].visible);
    assert(rects[W_TUNER_SHUFFLE].visible && rects[W_TUNER_REFRESH].visible);
    assert(rects[W_TUNER_PREV].visible && rects[W_TUNER_NEXT].visible);
    for (int i = 0; i < 5; i++) {
        const WidgetRect *r = &rects[W_TUNER_PLAY_0 + i];
        assert(r->visible && r->y >= layout.tuner_y && r->y + r->h <= layout.tuner_y + layout.tuner_h);
    }
    fv_ui_layout_active(rects, 2400, 1080, 3.0f, &st, 5, is_video, &layout);
    assert(rects[W_CLOSE_TUNER].visible && rects[W_TUNER_PLAY_4].visible);
    assert(layout.tuner_y >= 0.0f && layout.tuner_y + layout.tuner_h <= 1080.0f);
    assert(rects[W_TUNER_PLAY_4].y + rects[W_TUNER_PLAY_4].h <= rects[W_TUNER_PREV].y - 18.0f * 3.0f);

    bool tuner_estes = false;
    assert(fv_ui_handle_tap(&st, rects, 5.0f, 5.0f, 3.0f, &tuner_estes) == -1 && !st.tuner_visible);
    st.tuner_visible = true;
    bool tuner_close_pb = false;
    assert(fv_ui_back_pressed(&st, &tuner_close_pb) && !st.tuner_visible && !tuner_close_pb);

    /* Help modal overlay + close button + outside tap dismissal */
    st = (FvUiState){.help_visible = true};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_CLOSE_HELP].visible);
    bool estes_hit = false;
    int action = fv_ui_handle_tap(&st, rects,
                                  rects[W_CLOSE_HELP].x + rects[W_CLOSE_HELP].w * 0.5f,
                                  rects[W_CLOSE_HELP].y + rects[W_CLOSE_HELP].h * 0.5f,
                                  3.0f, &estes_hit);
    assert(action == W_CLOSE_HELP && !st.help_visible && !estes_hit);

    st.help_visible = true;
    action = fv_ui_handle_tap(&st, rects, 5.0f, 5.0f, 3.0f, &estes_hit);
    assert(action == -1 && !st.help_visible);

    /* Estes overlay placement, hit marker tap, & exit */
    st = (FvUiState){.estes_mode = true};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_ESTES_EXIT].visible);
    action = fv_ui_handle_tap(&st, rects, 100.0f, 100.0f, 3.0f, &estes_hit);
    assert(action == -1 && st.estes_mode && estes_hit);
    action = fv_ui_handle_tap(&st, rects,
                              rects[W_ESTES_EXIT].x + rects[W_ESTES_EXIT].w * 0.5f,
                              rects[W_ESTES_EXIT].y + rects[W_ESTES_EXIT].h * 0.5f,
                              3.0f, &estes_hit);
    assert(action == W_ESTES_EXIT && !st.estes_mode && !estes_hit);

    /* Android Back button unwinds playback -> evidence -> help -> estes -> controls */
    st = (FvUiState){
        .playback_visible = true, .help_visible = true, .estes_mode = true,
        .controls_visible = true
    };
    bool close_playback = false;
    assert(fv_ui_back_pressed(&st, &close_playback) && !st.playback_visible && st.evidence_visible && close_playback);
    assert(fv_ui_back_pressed(&st, &close_playback) && !st.evidence_visible && !close_playback);
    assert(fv_ui_back_pressed(&st, &close_playback) && !st.help_visible && !close_playback);
    assert(fv_ui_back_pressed(&st, &close_playback) && !st.estes_mode && !close_playback);
    assert(fv_ui_back_pressed(&st, &close_playback) && !st.controls_visible && !close_playback);
    assert(!fv_ui_back_pressed(&st, &close_playback));

    /* Playback layout fills full phone screen height in portrait, landscape, short landscape, and audio-only */
    check_playback_layout(1344, 2992, 3.0f, false, is_video);
    check_playback_layout(2992, 1344, 3.0f, false, is_video);
    check_playback_layout(1920, 720, 2.0f, false, is_video);
    check_playback_layout(2400, 1080, 3.0f, false, is_video);
    check_playback_layout(1344, 2992, 3.0f, true, is_video);

    /* Video playback starts with unobstructed full-bleed video and toggles overlay controls on tap */
    st = (FvUiState){
        .playback_visible = true,
        .playback_audio_only = false,
        .playback_controls_hidden = true
    };
    fv_ui_layout_active(rects, 1344, 2992, 3.0f, &st, 0, is_video, &layout);
    assert(!rects[W_PLAY_BACK].visible && !rects[W_PLAY_SEEK].visible && !rects[W_PLAY_TOGGLE].visible);
    assert(fv_ui_handle_tap(&st, rects, 600.0f, 1500.0f, 3.0f, &estes_hit) == -1);
    assert(!st.playback_controls_hidden);
    fv_ui_layout_active(rects, 1344, 2992, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_PLAY_BACK].visible && rects[W_PLAY_SEEK].visible && rects[W_PLAY_TOGGLE].visible);
    assert(fv_ui_handle_tap(&st, rects, 600.0f, 1500.0f, 3.0f, &estes_hit) == -1);
    assert(st.playback_controls_hidden);

    /* Visualizer press-and-hold burst trigger */
    FvHoldGesture hold = {0};
    fv_hold_begin(&hold, 200.0f, 300.0f, 1000L, true);
    assert(!fv_hold_update(&hold, 204.0f, 302.0f, 30.0f, 1200L)); /* < 350ms */
    assert(fv_hold_update(&hold, 205.0f, 302.0f, 30.0f, 1400L));  /* >= 350ms -> armed */
    assert(fv_hold_end(&hold) && !hold.tracking && !hold.triggered);
    /* Short tap release returns false so tap handler still runs */
    fv_hold_begin(&hold, 200.0f, 300.0f, 2000L, true);
    assert(!fv_hold_update(&hold, 200.0f, 300.0f, 30.0f, 2100L));
    assert(!fv_hold_end(&hold));
    /* Sliding past slop cancels hold */
    fv_hold_begin(&hold, 200.0f, 300.0f, 3000L, true);
    assert(!fv_hold_update(&hold, 250.0f, 300.0f, 30.0f, 3500L));
    assert(!hold.tracking && !fv_hold_end(&hold));

    fv_hold_begin(&hold, 200.0f, 300.0f, 4000L, true);
    assert(fv_hold_update(&hold, 200.0f, 300.0f, 30.0f, 4350L));
    assert(!fv_hold_update(&hold, 250.0f, 300.0f, 30.0f, 4400L));
    assert(!hold.tracking && fv_hold_end(&hold));

    /* Visualizer overlay layout presets: TOP below badges, COMPACT shorter above footer */
    st = (FvUiState){.vis_layout = FV_VIS_LAYOUT_TOP};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_CAM_PREVIEW].visible);
    assert(layout.vis_top >= layout.badge_y + layout.badge_h);
    assert(layout.vis_top + layout.vis_h <= layout.bottom_bar_y);

    FvUiScreenLayout btm_layout = {0};
    st = (FvUiState){.vis_layout = FV_VIS_LAYOUT_BOTTOM};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &btm_layout);
    st = (FvUiState){.vis_layout = FV_VIS_LAYOUT_COMPACT};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &layout);
    assert(layout.vis_h < btm_layout.vis_h && layout.vis_top + layout.vis_h <= layout.bottom_bar_y);

    /* Advanced drawer exposes camera lens and HUD layout pills */
    st = (FvUiState){.controls_visible = true, .advanced_controls = true};
    fv_ui_layout_active(rects, 1080, 2400, 3.0f, &st, 0, is_video, &layout);
    assert(rects[W_CAM_LENS].visible && rects[W_VIS_LAYOUT].visible);

    /* Smooth multi-band Siri frequency wave lobes */
    float zero_spec[64] = {0}, act_spec[64] = {0};
    float low_c[32] = {0}, mid_c[32] = {0}, high_c[32] = {0};
    fv_vis_siri_lobes(zero_spec, 0.5f, 32, low_c, mid_c, high_c);
    for (int i = 0; i < 32; i++) {
        assert(low_c[i] == 0.0f && mid_c[i] == 0.0f && high_c[i] == 0.0f);
    }
    act_spec[6] = 0.9f; act_spec[24] = 0.8f; act_spec[48] = 0.7f;
    fv_vis_siri_lobes(act_spec, 0.25f, 32, low_c, mid_c, high_c);
    float max_l = 0.0f, max_m = 0.0f, max_h = 0.0f;
    for (int i = 0; i < 32; i++) {
        assert(low_c[i] >= 0.0f && low_c[i] <= 1.0f);
        assert(mid_c[i] >= 0.0f && mid_c[i] <= 1.0f);
        assert(high_c[i] >= 0.0f && high_c[i] <= 1.0f);
        if (low_c[i] > max_l) max_l = low_c[i];
        if (mid_c[i] > max_m) max_m = mid_c[i];
        if (high_c[i] > max_h) max_h = high_c[i];
    }
    assert(max_l > 0.2f && max_m > 0.2f && max_h > 0.2f);
}

int main(void) {
    test_station_blob_and_mag3();
    test_fragment_gating_and_cooldown();
    test_sensor_dsp_and_vox();
    test_hit_capture_and_preroll();
    test_local_bank_cache_cap();
    test_ui_controller_and_layout();
    printf("All native C23 unit tests PASSED.\n");
    return 0;
}
