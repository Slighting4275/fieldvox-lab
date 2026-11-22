/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "../../android/app/src/main/cpp/fv_fragment_engine.h"
#include "../../android/app/src/main/cpp/fv_fragment_store.h"
#include "../../android/app/src/main/cpp/fv_vox_controls.h"
#include "../../android/app/src/main/cpp/fv_station_policy.h"
#include "../../android/app/src/main/cpp/fv_radio_playlist.h"
#include "../../android/app/src/main/cpp/fv_environment_reverb.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fill_sine(int16_t *pcm, size_t frames, double freq_hz) {
    for (size_t i = 0; i < frames; i++) {
        pcm[i] = (int16_t)(18000.0 * sin(2.0 * 3.14159265358979323846 * freq_hz *
                                        (double)i / (double)FV_FRAGMENT_RATE));
    }
}

static uint64_t abs_sum_samples(const int16_t *pcm, size_t frames) {
    uint64_t total = 0;
    for (size_t i = 0; i < frames; i++) {
        const int32_t s = pcm[i];
        total += (uint64_t)(s < 0 ? -s : s);
    }
    return total;
}

static void rm_tree_shallow(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    char path[512];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

static void test_pitch_and_tempo(void) {
    static int16_t src[FV_FRAGMENT_RATE];
    static int16_t out_fast[FV_FRAGMENT_RATE];
    static int16_t out_slow[FV_FRAGMENT_RATE];
    fill_sine(src, FV_FRAGMENT_RATE, 220.0);
    const size_t n_norm = fv_fragment_transform(src, 24000u, 1.0f, 1.0f, out_fast, FV_FRAGMENT_RATE);
    assert(n_norm == 24000u);
    const size_t n_fast = fv_fragment_transform(src, 24000u, 1.2f, 1.2f, out_fast, FV_FRAGMENT_RATE);
    assert(n_fast > 0u && n_fast < n_norm);
    const size_t n_slow = fv_fragment_transform(src, 24000u, 0.6f, 0.7f, out_slow, FV_FRAGMENT_RATE);
    assert(n_slow > n_norm);
}

typedef struct {
    int selected_station;
    bool busy;
    bool fail_request;
    int request_calls;
    bool result_ready;
    int result_station;
    size_t result_frames;
    int16_t *result_pcm;
    int result_fd;
    FvVoxDecimalTrigger result_trigger;
} MockLiveSource;

static bool mock_is_busy(void *ctx) {
    return ((const MockLiveSource *)ctx)->busy;
}

static int mock_select_station(void *ctx, int channel_hop) {
    const MockLiveSource *m = (const MockLiveSource *)ctx;
    return m->selected_station >= 0 ? m->selected_station : (channel_hop % FV_FRAGMENT_DECKS);
}

static bool mock_request(void *ctx, int channel_hop, const FvVoxDecimalTrigger *trigger) {
    (void)channel_hop;
    (void)trigger;
    MockLiveSource *m = (MockLiveSource *)ctx;
    m->request_calls++;
    return !m->fail_request;
}

static bool mock_take_result(void *ctx, int16_t **pcm, size_t *frames, int *station,
                             FvVoxDecimalTrigger *trigger, int *source_fd) {
    MockLiveSource *m = (MockLiveSource *)ctx;
    if (!m->result_ready) return false;
    m->result_ready = false;
    *pcm = m->result_pcm;
    *frames = m->result_frames;
    *station = m->result_station;
    *trigger = m->result_trigger;
    *source_fd = m->result_fd;
    m->result_pcm = NULL;
    m->result_fd = -1;
    return true;
}

typedef struct {
    int submit_calls;
    int last_station;
    size_t last_frames;
    int last_fd;
} MockClipStore;

static bool mock_store_submit(void *ctx, const int16_t *pcm, size_t frames,
                              int station, int source_fd) {
    (void)pcm;
    MockClipStore *s = (MockClipStore *)ctx;
    s->submit_calls++;
    s->last_station = station;
    s->last_frames = frames;
    s->last_fd = source_fd;
    return true;
}

typedef struct {
    size_t calls;
    const int16_t *source;
    size_t source_frames;
} MockLocalProvider;

static size_t mock_read_local(void *ctx, float seek_offset, size_t frames,
                              int16_t *output, size_t capacity) {
    (void)seek_offset;
    MockLocalProvider *lp = (MockLocalProvider *)ctx;
    lp->calls++;
    const size_t n = frames < lp->source_frames ? frames : lp->source_frames;
    const size_t copy_n = n < capacity ? n : capacity;
    memcpy(output, lp->source, copy_n * sizeof(int16_t));
    return copy_n;
}

static void test_scheduling_and_live_seams(void) {
    static int16_t clip[FV_FRAGMENT_RATE];
    fill_sine(clip, FV_FRAGMENT_RATE, 330.0);

    FvFragmentEngine *e = fv_fragment_create();
    assert(e != NULL);

    MockLiveSource live = {.selected_station = 7, .result_fd = -1};
    MockClipStore store = {.last_station = -1, .last_fd = -1};
    fv_fragment_set_live_source(e, (FvFragmentLiveSource){
        .context = &live,
        .is_busy = mock_is_busy,
        .select_station = mock_select_station,
        .request = mock_request,
        .take_result = mock_take_result
    });
    fv_fragment_set_clip_store(e, (FvFragmentClipStore){
        .context = &store,
        .submit = mock_store_submit
    });
    fv_fragment_set_settings(e, (FvFragmentSettings){
        .echo = 0.0f, .reverb = 0.0f, .pitch_override = 0.0f,
        .noise = false, .paused = false, .auto_enabled = false, .local_only = false,
        .sweep_ms = 250u
    });

    const FvVoxDecimalTrigger trig = {
        .passed = true, .pitch_factor = 1.0f, .speed_factor = 1.0f,
        .grain_duration_ms = 400, .seek_offset = 0.1f, .channel_hop = 2, .prefer_live = true
    };

    /* Failed live request must roll back live_pending cleanly. */
    live.fail_request = true;
    fv_fragment_submit_trigger(e, FV_GATE_MAGNETOMETER, &trig);
    FvFragmentStepResult res = {0};
    fv_fragment_step(e, 0u, false, NULL, &res);
    assert(!res.request_started);
    assert(!fv_fragment_live_pending(e));

    /* Advance past cooldown (2.0 s @ 48 kHz = 96000 frames) and start live request. */
    live.fail_request = false;
    fv_fragment_submit_trigger(e, FV_GATE_GYROSCOPE, &trig);
    fv_fragment_step(e, 100000u, false, NULL, &res);
    assert(res.request_started);
    assert(fv_fragment_live_pending(e));

    /* Queue a proximity trigger while live fetch is pending; deliver live PCM first. */
    const FvVoxDecimalTrigger prox = {
        .passed = true, .pitch_factor = 1.0f, .speed_factor = 1.0f,
        .grain_duration_ms = 400, .seek_offset = 0.2f, .channel_hop = 0, .prefer_live = false
    };
    fv_fragment_submit_trigger(e, FV_GATE_PROXIMITY, &prox);

    int16_t *downloaded = malloc(sizeof(clip));
    assert(downloaded != NULL);
    memcpy(downloaded, clip, sizeof(clip));
    live.result_ready = true;
    live.result_pcm = downloaded;
    live.result_frames = FV_FRAGMENT_RATE;
    live.result_station = 7;
    live.result_trigger = trig;
    live.result_fd = open("/dev/null", O_RDONLY);

    fv_fragment_step(e, 102400u, false, NULL, &res);
    assert(res.admitted);
    assert(!fv_fragment_live_pending(e));
    assert(fv_fragment_cache_count(e) == 1u);
    assert(store.submit_calls == 1 && store.last_station == 7 && store.last_frames == FV_FRAGMENT_RATE);

    /* Next step immediately honors queued proximity trigger (bypassing cooldown) from cache. */
    fv_fragment_step(e, 103000u, false, NULL, &res);
    assert(res.admitted);
    assert(res.reason == FV_GATE_PROXIMITY);

    /* Regular non-proximity trigger within reverb cooldown is blocked. */
    FvVoxDecimalTrigger cached_trig = trig;
    cached_trig.prefer_live = false;
    cached_trig.channel_hop = 0;
    assert(!fv_fragment_trigger(e, &cached_trig, true, 105000u, false));

    /* Direct tuner audition bypasses cooldown and forwards source_fd. */
    const int audition_fd = open("/dev/null", O_RDONLY);
    assert(fv_fragment_audition_pcm(e, clip, FV_FRAGMENT_RATE, 7, audition_fd, 105000u));
    if (audition_fd >= 0) close(audition_fd);

    int16_t clean[512], proc[512], aux[512];
    FvFragmentStatus status = {0};
    fv_fragment_render(e, clean, proc, aux, 512u, 103000u, &status);
    assert(status.playing && status.voices >= 1u && status.reason == FV_GATE_PROXIMITY);
    assert(abs_sum_samples(clean, 512u) > 0u);

    fv_fragment_destroy(e);
}

typedef struct {
    int calls;
    int last_fd;
    size_t last_frames;
    bool allow;
} MockSpeaker;

static bool mock_speaker_cb(void *ctx, int source_fd, const int16_t *pcm,
                            size_t frames, const FvVoxDecimalTrigger *trigger,
                            FvFragmentSettings settings) {
    (void)pcm;
    (void)trigger;
    (void)settings;
    MockSpeaker *sp = (MockSpeaker *)ctx;
    sp->calls++;
    sp->last_fd = source_fd;
    sp->last_frames = frames;
    return sp->allow;
}

static void test_speaker_local_and_pause(void) {
    static int16_t clip[FV_FRAGMENT_RATE];
    fill_sine(clip, FV_FRAGMENT_RATE, 260.0);

    FvFragmentEngine *e = fv_fragment_create();
    assert(e != NULL);

    MockSpeaker speaker = {.last_fd = -1, .allow = false};
    MockLocalProvider local = {.source = clip, .source_frames = FV_FRAGMENT_RATE};
    fv_fragment_set_speaker(e, mock_speaker_cb, &speaker);
    fv_fragment_set_settings(e, (FvFragmentSettings){
        .echo = 0.0f, .reverb = 0.5f, .pitch_override = 0.6f,
        .noise = true, .paused = false, .auto_enabled = true, .local_only = true,
        .sweep_ms = 200u
    });

    const FvFragmentLocalProvider local_prov = {.context = &local, .read_fragment = mock_read_local};
    /* Speaker returning false prevents voice admission. */
    FvFragmentStepResult res = {0};
    fv_fragment_step(e, 0u, false, &local_prov, &res);
    assert(local.calls == 1u);
    assert(speaker.calls == 1 && !res.admitted);

    /* Allow speaker and advance past auto cooldown -> auto-scan admits from local provider. */
    speaker.allow = true;
    fv_fragment_step(e, 480000u, false, &local_prov, &res);
    assert(res.admitted && res.used_auto_trigger && res.reason == FV_GATE_AUTO_SCAN);
    assert(speaker.last_frames == 400u * 48u);

    /* Pausing suspends fragment playback and static immediately. */
    fv_fragment_set_settings(e, (FvFragmentSettings){
        .paused = true, .noise = true, .reverb = 0.5f, .echo = 0.5f
    });
    int16_t clean[256], proc[256], aux[256];
    FvFragmentStatus status = {0};
    fv_fragment_render(e, clean, proc, aux, 256u, 480000u, &status);
    assert(!status.playing);
    assert(abs_sum_samples(clean, 256u) == 0u);
    assert(abs_sum_samples(proc, 256u) == 0u);
    assert(abs_sum_samples(aux, 256u) == 0u);

    /* Auditioning a station clip while paused still renders non-zero PCM, then returns to silence. */
    assert(fv_fragment_audition_pcm(e, clip, FV_FRAGMENT_RATE, 7, -1, 481000u));
    fv_fragment_render(e, clean, proc, aux, 256u, 481000u, &status);
    assert(status.playing && abs_sum_samples(clean, 256u) > 0u);
    fv_fragment_render(e, clean, proc, aux, 256u, 481000u + 20u * FV_FRAGMENT_RATE, &status);
    assert(!status.playing && abs_sum_samples(clean, 256u) == 0u);

    fv_fragment_destroy(e);
}

static void test_store_roundtrip(void) {
    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/fv_store_test_%d", (int)getpid());
    rm_tree_shallow(dir);
    assert(mkdir(dir, 0700) == 0);

    static int16_t clip[FV_FRAGMENT_RATE / 2u];
    fill_sine(clip, FV_FRAGMENT_RATE / 2u, 440.0);

    FvFragmentEngine *e1 = fv_fragment_create();
    assert(e1 != NULL);
    assert(fv_fragment_store_start(dir, e1));
    FvFragmentClipStore store = fv_fragment_store_adapter();
    assert(store.submit != NULL);
    assert(store.submit(store.context, clip, FV_FRAGMENT_RATE / 2u, 11, -1));
    fv_fragment_store_stop();
    fv_fragment_destroy(e1);

    FvFragmentEngine *e2 = fv_fragment_create();
    assert(e2 != NULL);
    assert(fv_fragment_store_start(dir, e2));
    assert(fv_fragment_cache_count(e2) >= 1u);
    fv_fragment_store_stop();
    fv_fragment_destroy(e2);

    char cache_dir[300];
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", dir);
    rm_tree_shallow(cache_dir);
    rm_tree_shallow(dir);
}

static void test_speech_and_station_reverb(void) {
    int16_t speech[512];
    int16_t high[512];
    fill_sine(speech, 512u, 160.0);
    fill_sine(high, 512u, 1800.0);
    assert(fv_vox_calc_speech_band_energy(speech, 512, FV_FRAGMENT_RATE) >
           fv_vox_calc_speech_band_energy(high, 512, FV_FRAGMENT_RATE));

    const char langs[4][8] = {"en_us", "es_mx", "en_us", "fr_fr"};
    unsigned group[FV_STATION_GROUP_SIZE] = {0};
    const size_t cnt = fv_station_group(langs, 4u, "en_us", 1u, group);
    assert(cnt >= 2u);

    FvEnvironmentReverb *rev = fv_environment_reverb_create();
    assert(rev != NULL);
    fv_environment_reverb_set_mix(rev, 0.5f);
    float send[64];
    float wet[64];
    for (int i = 0; i < 64; i++) send[i] = (i == 0) ? 1.0f : 0.0f;
    fv_environment_reverb_process(rev, send, wet, 64u);
    fv_environment_reverb_destroy(rev);
}

static void test_radio_playlist_resolution(void) {
    assert(fv_radio_is_playlist_url("https://rtsradio-live.morescreens.com/RTS_2_001/audio/chunklist.m3u8", NULL));
    assert(fv_radio_is_playlist_url("https://example.com/stream.M3U?token=abc", NULL));
    assert(fv_radio_is_playlist_url("https://example.com/listen", "application/vnd.apple.mpegurl; charset=utf-8"));
    assert(!fv_radio_is_playlist_url("https://npr-ice.streamguys1.com/live.mp3", "audio/mpeg"));

    const char *m3u8_media =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXTINF:6.0,\n"
        "media_1042.aac\n"
        "#EXTINF:6.0,\n"
        "media_1043.aac\n";
    assert(fv_radio_body_is_playlist(m3u8_media, strlen(m3u8_media)));

    char resolved[FV_RADIO_URL_MAX];
    bool nested = true;
    assert(fv_radio_extract_stream_url(
        "https://rtsradio-live.morescreens.com/RTS_2_001/audio/chunklist.m3u8?foo=bar",
        m3u8_media, strlen(m3u8_media), resolved, sizeof(resolved), &nested));
    assert(!nested);
    assert(strcmp(resolved, "https://rtsradio-live.morescreens.com/RTS_2_001/audio/media_1042.aac") == 0);

    const char *m3u8_master =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\n"
        "/live/audio_128/playlist.m3u8\n";
    assert(fv_radio_extract_stream_url(
        "https://radio.example.org/master.m3u8",
        m3u8_master, strlen(m3u8_master), resolved, sizeof(resolved), &nested));
    assert(nested);
    assert(strcmp(resolved, "https://radio.example.org/live/audio_128/playlist.m3u8") == 0);

    const char *pls =
        "[playlist]\r\n"
        "NumberOfEntries=1\r\n"
        "File1=http://stream.example.net:8000/live\r\n"
        "Title1=Talk\r\n";
    assert(fv_radio_body_is_playlist(pls, strlen(pls)));
    assert(fv_radio_extract_stream_url("https://stream.example.net/listen.pls",
                                       pls, strlen(pls), resolved, sizeof(resolved), &nested));
    assert(!nested);
    assert(strcmp(resolved, "http://stream.example.net:8000/live") == 0);

    assert(!fv_radio_extract_stream_url("https://example.com/a.m3u8",
                                        "#EXTM3U\n#EXT-X-ENDLIST\n", 26u,
                                        resolved, sizeof(resolved), &nested));
}

static void test_station_health_and_dynamic_json(void) {
    assert(strcmp(fv_station_bank_code(FV_RADIO_BANK_EN_US), "en_us") == 0);
    assert(strcmp(fv_station_bank_code(FV_RADIO_BANK_ALL), "all") == 0);
    assert(strcmp(fv_station_bank_label(FV_RADIO_BANK_EN_UK), "UK") == 0);

    assert(!fv_station_url_is_valid(NULL));
    assert(!fv_station_url_is_valid("https://www.radio-browser.info/null"));
    assert(!fv_station_url_is_valid("ftp://example.com/stream"));
    assert(fv_station_url_is_valid("https://npr-ice.streamguys1.com/live.mp3"));

    const char langs[5][8] = {"en_us", "en_us", "en_us", "es", "en_us"};
    const char *urls[5] = {
        "https://www.radio-browser.info/null",
        "https://live1.example.com/a.mp3",
        "https://live2.example.com/b.mp3",
        "https://live3.example.com/es.mp3",
        "https://live4.example.com/c.mp3",
    };
    uint8_t health[5] = {
        FV_STATION_UNTESTED,
        FV_STATION_FAILED,
        FV_STATION_OK,
        FV_STATION_OK,
        FV_STATION_UNTESTED,
    };
    unsigned group[FV_STATION_GROUP_SIZE] = {0};
    const size_t n = fv_station_group_healthy(langs, urls, health, 5u, "en_us", 42u, group);
    assert(n == 4u);
    /* Invalid /null URL must never enter group; healthy stations precede failed. */
    for (size_t i = 0; i < n; i++) assert(group[i] != 0u);
    assert(group[0] != 1u && group[1] != 1u);

    /* "all" includes every valid language */
    unsigned all_group[FV_STATION_GROUP_SIZE] = {0};
    const size_t n_all = fv_station_group_healthy(langs, urls, health, 5u, "all", 7u, all_group);
    assert(n_all == 4u);

    /* Replace failed station inside an active group */
    unsigned active[FV_STATION_GROUP_SIZE] = {2u, 1u};
    health[1] = FV_STATION_FAILED;
    const int rep = fv_station_replace_failed(langs, urls, health, 5u, "en_us", 9u, active, 2u, 1);
    assert(rep == 4);
    assert(active[1] == 4u);

    /* Parse Radio-Browser style JSON array including nested string arrays and URLs >= 160 chars */
    const char *long_url =
        "https://stream-balancer.example.com/radio/live/aac/high_bitrate/channel_zero/"
        "token_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef/"
        "segment_feed_primary/stream.aac";
    assert(strlen(long_url) > 160u);
    char rb_json[1200];
    snprintf(rb_json, sizeof(rb_json),
             "[{\"name\":\"KIRO Newsradio 97.3\",\"tags\":[\"news\",\"talk\"],"
             "\"url_resolved\":\"https:\\/\\/bonneville.cdnstream1.com\\/2697_48.aac\","
             "\"countrycode\":\"US\",\"languagecodes\":\"en\"},"
             "{\"name\":\"Broken\",\"url_resolved\":\"https://www.radio-browser.info/null\",\"countrycode\":\"US\",\"languagecodes\":\"en\"},"
             "{\"name\":\"KOMO Newsradio\",\"tags\":[\"seattle\",\"talk\"],\"url\":\"%s\","
             "\"countrycode\":\"US\",\"languagecodes\":\"en_us\"}]",
             long_url);
    FvDynamicStation dyn[8] = {0};
    const size_t parsed = fv_station_parse_radiobrowser_json(rb_json, strlen(rb_json), dyn, 8u);
    assert(parsed == 2u);
    assert(strcmp(dyn[0].callsign, "KIRO Newsradio 97.3") == 0);
    assert(strcmp(dyn[0].url, "https://bonneville.cdnstream1.com/2697_48.aac") == 0);
    assert(strcmp(dyn[0].country, "US") == 0);
    assert(strcmp(dyn[1].name, "KOMO Newsradio") == 0);
    assert(strcmp(dyn[1].lang, "en_us") == 0);
    assert(strcmp(dyn[1].url, long_url) == 0);
}

int main(void) {
    test_pitch_and_tempo();
    test_scheduling_and_live_seams();
    test_speaker_local_and_pause();
    test_store_roundtrip();
    test_speech_and_station_reverb();
    test_radio_playlist_resolution();
    test_station_health_and_dynamic_json();
    printf("All fragment engine C23 unit tests PASSED.\n");
    return 0;
}
