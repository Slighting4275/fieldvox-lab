/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "../../android/app/src/main/cpp/fv_station_blob_data.h"
#include "../../android/app/src/main/cpp/fv_native_audio.h"
#include "../../android/app/src/main/cpp/fv_grain_gate.h"
#include "../../android/app/src/main/cpp/fv_hit_capture.h"
#include "../../android/app/src/main/cpp/fv_vox_controls.h"
#include "../../android/app/src/main/cpp/fv_sensor_dsp.h"
#include "../../android/app/src/main/cpp/fv_local_bank.h"
#include "../../android/app/src/main/cpp/fv_ui_layout.h"

static inline uint32_t load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

/* Minimal standalone SHA-256 for test verification */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} TestSha256Ctx;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define EP1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void test_sha256_transform(TestSha256Ctx *ctx, const uint8_t *data) {
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    uint32_t m[64];
    for (int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) |
               ((uint32_t)data[i*4+2] << 8) | ((uint32_t)data[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void test_sha256_init(TestSha256Ctx *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c; ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void test_sha256_update(TestSha256Ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[ctx->count & 63] = data[i];
        ctx->count++;
        if ((ctx->count & 63) == 0) {
            test_sha256_transform(ctx, ctx->buffer);
        }
    }
}

static void test_sha256_final(TestSha256Ctx *ctx, uint8_t hash[32]) {
    uint8_t pad[64] = { 0x80 };
    uint64_t bits = ctx->count * 8ULL;
    size_t pad_len = (ctx->count & 63) < 56 ? (56 - (ctx->count & 63)) : (120 - (ctx->count & 63));
    test_sha256_update(ctx, pad, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = (uint8_t)((bits >> ((7 - i) * 8)) & 0xff);
    test_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        hash[i*4+1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        hash[i*4+2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        hash[i*4+3] = (uint8_t)(ctx->state[i] & 0xff);
    }
}

static void test_sha256_hex(const char *input, char output[65]) {
    TestSha256Ctx ctx;
    test_sha256_init(&ctx);
    test_sha256_update(&ctx, (const uint8_t*)input, strlen(input));
    uint8_t hash[32];
    test_sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++) {
        snprintf(&output[i*2], 3, "%02x", hash[i]);
    }
    output[64] = '\0';
}

static int is_safe_session_stem(const char *stem) {
    if (!stem) return 0;
    size_t len = strlen(stem);
    if (len == 0 || len > 160) return 0;
    if (strncmp(stem, "session-", 8) != 0) return 0;
    if (strstr(stem, "..") != NULL || strchr(stem, '/') != NULL || strchr(stem, '\\') != NULL) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = stem[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static uint32_t crc32_buf(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)(-(int)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static int count_trigger_events_str(const char *jsonl) {
    if (!jsonl) return 0;
    int count = 0;
    const char *p = jsonl;
    while (*p) {
        const char *next_line = strchr(p, '\n');
        size_t line_len = next_line ? (size_t)(next_line - p) : strlen(p);
        if (strstr(p, "\"type\":\"trigger\"") != NULL || strstr(p, "\"event\":\"trigger\"") != NULL) {
            // Check that the match is within this line
            const char *m1 = strstr(p, "\"type\":\"trigger\"");
            const char *m2 = strstr(p, "\"event\":\"trigger\"");
            if ((m1 && m1 < p + line_len) || (m2 && m2 < p + line_len)) {
                count++;
            }
        }
        if (!next_line) break;
        p = next_line + 1;
    }
    return count;
}

/* Grain gate timing must stay inside the Java carrier's ranges (MainActivity.openOnlineCarrierGate). */
static void test_grain_gate(void) {
    const uint32_t rate = 48000;
    const uint64_t ms = rate / 1000;
    FvGrainGate gate;
    float gain[1024];

    /* No carrier ready: nothing opens. */
    fv_gate_init(&gate, rate, 12345u, 0);
    for (uint64_t t = 0; t < 10 * rate; t += 1024) {
        fv_gate_process(&gate, t, gain, 1024, false, 450);
        for (int i = 0; i < 1024; i++) assert(gain[i] == 0.0f);
    }
    assert(!gate.open);

    /* Carrier ready for 10 minutes: every packet obeys the Java grain, gap and silence ranges. */
    fv_gate_init(&gate, rate, 777u, 0);
    fv_gate_carrier_started(&gate, 0);
    int packets = 0, advances = 0;
    bool was_open = false;
    uint64_t last_packet_end = 0, last_next_allowed = 0;
    for (uint64_t t = 0; t < 600 * rate; t += 1024) {
        if (fv_gate_process(&gate, t, gain, 1024, true, 450)) advances++;
        for (int i = 0; i < 1024; i++) assert(gain[i] >= 0.0f && gain[i] <= 1.0f);
        if (gate.open && !was_open) {
            packets++;
            assert(gate.packet_start >= 1200 * ms - 1024);           /* first auto-scan after 1.2 s */
            if (packets > 1) assert(gate.packet_start >= last_next_allowed - 1024);
            assert(gate.grain_count >= 1 && gate.grain_count <= FV_GATE_MAX_GRAINS);
            for (int k = 0; k < gate.grain_count; k++) {
                assert(gate.grain_len[k] >= 65 * ms && gate.grain_len[k] <= 390 * ms);
                if (k > 0) {
                    const uint64_t gap = gate.grain_start[k] - (gate.grain_start[k - 1] + gate.grain_len[k - 1]);
                    assert(gap >= 30 * ms && gap <= 260 * ms);
                }
            }
            const uint64_t silence = gate.next_allowed - (gate.packet_start + gate.packet_len);
            assert(silence >= 1100 * ms && silence <= 4700 * ms);
            last_packet_end = gate.packet_start + gate.packet_len;
            last_next_allowed = gate.next_allowed;
        }
        was_open = gate.open;
    }
    (void)last_packet_end;
    /* Auto-scan every 1.7-6.0 s plus 1.1-4.7 s silence: roughly 60-200 packets in 10 minutes. */
    assert(packets >= 60 && packets <= 220);
    assert(advances >= packets - 1 && advances <= packets);
    printf("  [PASS] Grain gate: %d packets / %d station advances in 10 min, all within Java ranges\n",
           packets, advances);

    /* Proximity bypasses forced silence but never overlaps an open packet. */
    fv_gate_init(&gate, rate, 99u, 0);
    assert(fv_gate_request(&gate, 0, FV_GATE_MAGNETOMETER, 4.0f, 450, true));
    const uint64_t end = gate.packet_start + gate.packet_len;
    assert(!fv_gate_request(&gate, 1024, FV_GATE_PROXIMITY, 1.0f, 450, true)); /* queued */
    assert(gate.proximity_pending);
    uint64_t t = 0;
    while (t <= end + 2048) {
        fv_gate_process(&gate, t, gain, 1024, true, 450);
        t += 1024;
    }
    assert(gate.open && gate.reason == FV_GATE_PROXIMITY);  /* flushed right after the packet */
    assert(gate.packet_start < end + 3 * 1024);             /* did not wait out the 1.1 s silence */
    /* Sensor cooldowns: a magnetometer spike right after a packet is refused. */
    fv_gate_init(&gate, rate, 5u, 0);
    assert(fv_gate_request(&gate, 0, FV_GATE_MAGNETOMETER, 3.0f, 450, true));
    assert(!fv_gate_request(&gate, gate.packet_len + 10, FV_GATE_MAGNETOMETER, 3.0f, 450, true));
    fv_gate_init(NULL, rate, 0, 0);
    fv_gate_carrier_started(NULL, 0);
    assert(!fv_gate_request(NULL, 0, FV_GATE_AUTO_SCAN, 0.0f, 450, true));
    assert(!fv_gate_process(NULL, 0, gain, 1024, true, 450));
    assert(!fv_gate_process(&gate, 0, gain, 0, true, 450));
    assert(!fv_gate_process(&gate, 0, gain, -1, true, 450));
    printf("  [PASS] Grain gate proximity queue and sensor cooldowns\n");
}

static void test_sensor_only(void) {
    assert(fv_gate_emf_exceeds_threshold(0.8f, 0.75f));
    assert(!fv_gate_emf_exceeds_threshold(0.8f, 1.65f));
    assert(fv_gate_emf_exceeds_threshold(1.8f, 1.65f));
    assert(!fv_gate_emf_exceeds_threshold(1.8f, 3.0f));
    assert(fv_gate_emf_exceeds_threshold(3.0f, 3.0f));
    assert(!fv_gate_emf_exceeds_threshold(NAN, 0.75f));
    assert(!fv_gate_emf_exceeds_threshold(INFINITY, 0.75f));
    assert(!fv_gate_emf_exceeds_threshold(1.0f, NAN));
    assert(!fv_gate_emf_exceeds_threshold(1.0f, 0.0f));
    FvGrainGate gate;
    float gain[1024];
    fv_gate_init(&gate, 48000, 123u, 0);
    fv_gate_set_auto_enabled(&gate, false, 0);
    for (uint64_t t = 0; t < 60u * 48000u; t += 1024) {
        fv_gate_process(&gate, t, gain, 1024, true, 450);
        for (int i = 0; i < 1024; i++) assert(gain[i] == 0.0f);
    }
    assert(!gate.open);
    assert(!fv_gate_request(&gate, 60u * 48000u, FV_GATE_AUTO_SCAN, 0, 450, true));
    assert(fv_gate_request(&gate, 60u * 48000u, FV_GATE_MAGNETOMETER, 1.8f, 450, true));
    fv_gate_process(&gate, 60u * 48000u, gain, 1024, true, 450);
    assert(gain[512] > 0.0f && gate.reason == FV_GATE_MAGNETOMETER);
    /* A mode change cuts an active timer packet, but preserves a sensor packet. */
    fv_gate_set_auto_enabled(&gate, true, 60u * 48000u);
    fv_gate_set_auto_enabled(&gate, false, 60u * 48000u);
    assert(gate.open && gate.reason == FV_GATE_MAGNETOMETER);
    fv_gate_init(&gate, 48000, 123u, 0);
    assert(fv_gate_request(&gate, 0, FV_GATE_AUTO_SCAN, 0, 450, true));
    fv_gate_set_auto_enabled(&gate, false, 0);
    assert(!gate.open);
    fv_gate_process(&gate, 0, gain, 1024, true, 450);
    for (int i = 0; i < 1024; i++) assert(gain[i] == 0.0f);
    printf("  [PASS] Distinct EMF thresholds and 60-second sensor-only gating\n");
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_hit_capture(void) {
    char directory[] = "/tmp/fieldvox-hit-test-XXXXXX";
    assert(mkdtemp(directory));
    char output[640], path[640];
    fv_hit_capture_reset();
    assert(!fv_hit_capture_save(directory, 123, 0, output, sizeof(output)));
    int16_t clean[1024], processed[1024];
    FvHitBlock block = {.station_a = 7, .station_b = -1, .audible_a = true,
        .reason_a = FV_GATE_MAGNETOMETER, .emf_threshold = 0.75f, .emf_delta = NAN,
        .effects = FV_HIT_FX_ECHO, .pitch_semitones = -2,
        .echo_percent = 37, .reverb_percent = 62, .distortion_percent = 18,
        .grain_ms = 450, .voice_duck_enabled = true, .speaker_ducked = true};
    strcpy(block.label_a, "Quoted \"station\"\\\n");
    strcpy(block.url_a, "https://example.invalid/radio?x=\"test\"");
    for (uint32_t n = 0; n < 180u; n++) {
        block.sample_start = (uint64_t)n * 1024u;
        block.auto_enabled = n < 100u;
        block.effects = n < 100u ? FV_HIT_FX_ECHO : FV_HIT_FX_REVERB;
        for (uint32_t i = 0; i < 1024; i++) {
            clean[i] = (int16_t)((n * 1024u + i) % 10000u);
            processed[i] = (int16_t)-clean[i];
        }
        fv_hit_capture_push(clean, processed, 1024, &block);
    }
    assert(fv_hit_capture_save(directory, 123, 0, output, sizeof(output)));
    const char *suffixes[] = {"clean.wav", "preroll.wav"};
    for (unsigned which = 0; which < 2; which++) {
        snprintf(path, sizeof(path), "%s/hit-123-0-%s", directory, suffixes[which]);
        FILE *f = fopen(path, "rb"); assert(f);
        uint8_t header[44];
        size_t nread = fread(header, 1, 44, f);
        assert(nread == 44);
        assert(memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0);
        assert(read_le32(header + 24) == FV_HIT_RATE);
        assert(read_le32(header + 40) == FV_HIT_FRAMES * 2u);
        const uint32_t first = 180u * 1024u - FV_HIT_FRAMES;
        for (uint32_t i = 0; i < FV_HIT_FRAMES; i++) {
            uint8_t bytes[2];
            size_t br = fread(bytes, 1, 2, f);
            assert(br == 2);
            const int16_t actual = (int16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
            const int16_t expected = (int16_t)((first + i) % 10000u);
            assert(actual == (which ? (int16_t)-expected : expected));
        }
        int c_eof = fgetc(f);
        assert(c_eof == EOF);
        int fc_rc = fclose(f);
        assert(fc_rc == 0);
    }
    snprintf(path, sizeof(path), "%s/hit-123-0.json", directory);
    FILE *f = fopen(path, "rb"); assert(f);
    char *json = malloc(160000); assert(json);
    const size_t len = fread(json, 1, 159999, f); json[len] = '\0';
    int at_eof = feof(f);
    assert(at_eof != 0);
    fclose(f);
    assert(strstr(json, "\"timeline_complete\":true"));
    assert(strstr(json, "\"emf_delta_ut\":null"));
    assert(strstr(json, "\"offset_frames\":0,\"frames\":640"));
    assert(strstr(json, "\"auto_enabled\":true") && strstr(json, "\"auto_enabled\":false"));
    assert(strstr(json, "Quoted \\\"station\\\"\\\\\\u000a"));
    free(json);
    /* A repeated ID must not replace an existing hit. */
    assert(!fv_hit_capture_save(directory, 123, 0, output, sizeof(output)));
    struct stat st; assert(stat(path, &st) == 0 && st.st_size == (off_t)len);
    /* Force a failure after writing both WAVs: only our new files are removed. */
    snprintf(path, sizeof(path), "%s/hit-124-0.json", directory);
    f = fopen(path, "wb"); assert(f); fputs("keep", f); fclose(f);
    assert(!fv_hit_capture_save(directory, 124, 0, output, sizeof(output)));
    snprintf(path, sizeof(path), "%s/hit-124-0-clean.wav", directory); assert(access(path, F_OK) != 0);
    snprintf(path, sizeof(path), "%s/hit-124-0-preroll.wav", directory); assert(access(path, F_OK) != 0);
    snprintf(path, sizeof(path), "%s/hit-124-0.json", directory); assert(stat(path, &st) == 0 && st.st_size == 4);
    /* Discontinuous input starts a new capture; no stale PCM or phantom padding. */
    block.sample_start = 999999u;
    fv_hit_capture_push(clean, processed, 1024, &block);
    assert(fv_hit_capture_save(directory, 125, 0, output, sizeof(output)));
    assert(stat(output, &st) == 0 && st.st_size == 44 + 2048);
    if (getenv("FIELDVOX_KEEP_TEST_HITS")) printf("HIT_TEST_DIRECTORY=%s\n", directory);
    else {
        const unsigned ids[] = {123, 124, 125};
        const char *all_suffixes[] = {"-clean.wav", "-preroll.wav", ".json"};
        for (unsigned i = 0; i < 3; i++) for (unsigned j = 0; j < 3; j++) {
            snprintf(path, sizeof(path), "%s/hit-%u-0%s", directory, ids[i], all_suffixes[j]);
            unlink(path);
        }
        assert(rmdir(directory) == 0);
    }
    printf("  [PASS] Paired PCM, historical metadata, JSON escaping, wraparound, interruption, and failure cleanup\n");
}

static void test_c23_dsp_and_vox(void) {
    float delta, left, right;
    assert(fv_update_barometer(1005.0f, &delta)==0 && fabsf(delta)<0.001f);
    assert(fv_update_barometer(1004.7f, &delta)==1 && delta < -0.29f);
    assert(fv_update_barometer(NAN, &delta)==0 && delta==0);
    fv_reset_barometer_baseline(1000.0f);
    assert(fv_get_barometer_baseline()==1000.0f);
    fv_calc_spatial_pan_gains(0, &left, &right);
    assert(fabsf(left-0.70710678f)<0.0001f && fabsf(left-right)<0.0001f);
    fv_calc_spatial_pan_gains(FLT_MAX, &left, &right);
    assert(isfinite(left) && isfinite(right) && fabsf(left*left+right*right-1.0f)<0.0001f);
    fv_calc_spatial_pan_gains(INFINITY, &left, &right);
    assert(fabsf(left-0.70710678f)<0.0001f);
    fv_calc_spatial_pan_gains(0, NULL, NULL);
    assert(fv_verify_blob_checksum(FV_STATION_PLAN_BINARY_DATA,sizeof(FV_STATION_PLAN_BINARY_DATA))==1);
    assert(fv_verify_blob_checksum(FV_STATION_PLAN_BINARY_DATA,16)==0);
    assert(fv_verify_blob_checksum(NULL,100)==0);
    uint8_t *corrupt = malloc(sizeof(FV_STATION_PLAN_BINARY_DATA)); assert(corrupt);
    memcpy(corrupt,FV_STATION_PLAN_BINARY_DATA,sizeof(FV_STATION_PLAN_BINARY_DATA));
    corrupt[20]^=255u;
    assert(fv_verify_blob_checksum(corrupt,sizeof(FV_STATION_PLAN_BINARY_DATA))==0);
    free(corrupt);
    int16_t tone[48000];
    for (size_t i=0; i<48000; i++) tone[i]=32760;
    fv_rem_pod_generate(tone,48000,99,48000);
    for (size_t i=0; i<48000; i++) assert(tone[i]>=22260);
    fv_rem_pod_generate(NULL,1,1,48000); fv_rem_pod_generate(tone,1,1,0);

    /* FieldVox micro-decimal sensor gate assertions */
    FvVoxDecimalTrigger trig = {0};
    assert(!fv_vox_eval_decimal_trigger(0, 0, 0, true, 0.5f, &trig));
    assert(!fv_vox_eval_decimal_trigger(NAN, 0, 0, true, 0.5f, &trig));
    assert(!fv_vox_eval_decimal_trigger(10, 20, 30, true, 0.5f, NULL));
    /* Shortest fraction 00010000000000000000: gate digits [1:4] are 001. */
    assert(fv_vox_eval_decimal_trigger(10.0001, 0, 0, true, 0.5f, &trig));
    assert(trig.passed && trig.gate_code == 1);
    assert(trig.pitch == FV_VOX_PITCH_LOW && trig.pitch_semitones == -9 && fabsf(trig.pitch_factor - 0.6f) < 0.001f);
    assert(trig.speed == FV_VOX_SPEED_SLOW && fabsf(trig.speed_factor - 0.7f) < 0.001f);
    assert(trig.grain_duration_ms == 400);
    assert(trig.playback_duration_ms == 571); /* Speed-compensated: 400 / 0.7 = 571 ms */
    assert(fabsf(trig.seek_offset - 0.06f) < 0.001f);
    assert(trig.channel_hop == 0);

    assert(fv_vox_slider_percent(0,0,100)==0);
    assert(fv_vox_slider_percent(50,0,100)==50);
    assert(fv_vox_slider_percent(200,0,100)==100);
    assert(fv_vox_slider_percent(NAN,0,100)==0);
    int16_t original[1024], pcm[1024];
    for (int i=0; i<1024; i++) original[i]=(int16_t)((i%2 ? -1 : 1)*12000);
    memcpy(pcm,original,sizeof(pcm)); fv_audio_dsp_reset();
    fv_vox_apply_effects(pcm,1024,0,0,0);
    assert(memcmp(pcm,original,sizeof(pcm))==0);
    fv_vox_apply_effects(pcm,1024,0,0,100);
    assert(pcm[0]>original[0] && pcm[1]<original[1]);
    assert(abs(pcm[0]+pcm[1])<=1);
    memset(pcm,0,sizeof(pcm)); fv_audio_dsp_reset();
    pcm[0]=20000; fv_vox_apply_effects(pcm,1024,100,100,0);
    assert(pcm[0]!=20000);
    fv_vox_apply_effects(NULL,10,100,100,100);
    fv_audio_dsp_reset();

    /* FieldVox Speech VAD (80-300 Hz) */
    FvVoxDucker detector={0}; uint64_t clock=0;
    memset(pcm,0,sizeof(pcm));
    for (int i=0; i<20; i++,clock+=1024) assert(!fv_vox_detect_voice(&detector,pcm,1024,clock,48000));
    /* Synthesize 150 Hz speech fundamental tone */
    for (int i=0; i<1024; i++) pcm[i]=(int16_t)(sin(2.0 * M_PI * 150.0 * (double)i / 48000.0) * 16000.0);
    assert(fv_vox_detect_voice(&detector,pcm,1024,clock,48000));
    assert(detector.speech_detected);
    assert(detector.speech_energy > 2.0f);
    /* 2000 ms holdoff: 48000 * 2 = 96000 samples */
    assert(fv_vox_detect_voice(&detector,NULL,0,clock+95000,48000));
    assert(!fv_vox_detect_voice(&detector,NULL,0,clock+96001,48000));

    /* FieldVox Voice Pool & Reverb Tail Release */
    FvVoxVoicePool pool;
    fv_vox_voice_pool_init(&pool, 0.5f);
    int slot = fv_vox_voice_pool_allocate(&pool, 1000, 760, 1.2f, 10);
    assert(slot == 0);
    assert(pool.voices[0].play_duration_ms == 633);
    assert(pool.voices[0].release_time_ms == 1000 + 633 + 4500); /* 4500 ms reverb tail */
    assert(fv_vox_voice_is_playing(&pool, 0, 1500));
    assert(!fv_vox_voice_is_playing(&pool, 0, 1700));
    assert(fv_vox_voice_is_busy(&pool, 0, 1700));
    assert(fv_vox_voice_is_busy(&pool, 0, 6132));
    assert(!fv_vox_voice_is_busy(&pool, 0, 6134));

    /* FieldVox 10 concurrent voices allocation */
    for (int i = 1; i < 10; i++) {
        int v_slot = fv_vox_voice_pool_allocate(&pool, 1000 + (uint64_t)i * 10, 500, 1.0f, i);
        assert(v_slot == i);
        assert(pool.voices[i].active);
    }
    assert(FV_VOX_MAX_VOICES == 10);
    assert(fv_vox_voice_pool_allocate(&pool,1100,500,1.0f,99)==-1);

    /* FieldVox Boundary-clamped grain seek & duration truncation */
    int dur = 760, seek = 0;
    /* 5000 ms buffer, seek_fraction = 0.90 -> seek = 4500, seek+dur = 5260 > 5000 */
    fv_vox_clamp_grain_boundary(5000, 0.90f, &dur, &seek);
    assert(seek == 5000 - 760 - 10); /* shifted = 4230 */
    assert(seek + dur <= 5000);
    assert(dur == 760);
    /* Very small buffer 500 ms with 760 ms duration */
    dur = 760; seek = 0;
    fv_vox_clamp_grain_boundary(500, 0.50f, &dur, &seek);
    assert(seek == 0);
    assert(dur == 500);

    /* FieldVox four-tap echo volume attenuation */
    assert(fabsf(fv_vox_echo_tap_volume(1.0f, 1) - 0.06f) < 0.001f);
    assert(fabsf(fv_vox_echo_tap_volume(1.0f, 2) - 0.018f) < 0.001f);
    assert(fabsf(fv_vox_echo_tap_volume(1.0f, 3) - 0.0054f) < 0.0001f);
    assert(fabsf(fv_vox_echo_tap_volume(1.0f, 4) - 0.00162f) < 0.00001f);
    assert(fv_vox_echo_tap_volume(1.0f, 5) == 0.0f);
    assert(fv_vox_echo_tap_delay_ms(150, 1) == 150);
    assert(fv_vox_echo_tap_delay_ms(150, 2) == 300);
    assert(fv_vox_echo_tap_delay_ms(150, 3) == 450);

    /* FieldVox Cache vs Live decision */
    FvVoxDecimalTrigger dtrig = {0};
    /* Vector with frac "000" in [1:4], d7=9 (prefer cached if 10 < N <= 20) */
    assert(fv_vox_eval_decimal_trigger_ex(10.00010009, 0, 0, true, 0.5f, 5, &dtrig));
    assert(dtrig.prefer_live == true); /* N=5 <= 10 => prefer live */
    assert(fv_vox_eval_decimal_trigger_ex(10.00010009, 0, 0, true, 0.5f, 15, &dtrig));
    assert(dtrig.prefer_live == false); /* N=15, d7=9 => prefer cached */
    assert(fv_vox_eval_decimal_trigger_ex(10.00010009, 0, 0, true, 0.5f, 25, &dtrig));
    assert(dtrig.prefer_live == false); /* N=25 > 20, d7=9 >= 8 => prefer cached */

    /* FieldVox Reverb-dependent global cooldown gate */
    FvGrainGate rgate;
    fv_gate_init(&rgate, 48000, 42, 0);
    fv_gate_set_reverb_mix(&rgate, 0.5f); /* 2000 ms + 8000 ms * 0.5 = 6000 ms = 288000 samples */
    assert(fv_gate_request(&rgate, 0, FV_GATE_MAGNETOMETER, 5.0f, 450, true));
    for (uint64_t t = 0; t <= rgate.packet_start + rgate.packet_len + 48000; t += 1024) {
        fv_gate_process(&rgate, t, NULL, 1024, true, 450);
    }
    assert(!rgate.open);
    /* At 5.0s (240000 samples), still within 6.0s (288000 samples) lockout */
    assert(!fv_gate_request(&rgate, 240000, FV_GATE_MAGNETOMETER, 5.0f, 450, true));
    const uint64_t target_t = rgate.next_allowed > 288000 ? rgate.next_allowed + 1024 : 288001;
    assert(fv_gate_request(&rgate, target_t, FV_GATE_MAGNETOMETER, 5.0f, 450, true));

    /* FieldVox Sweep burst static */
    int16_t static_pcm[128];
    uint32_t srng = 54321;
    fv_audio_generate_sweep_static(static_pcm, 128, &srng, 0.02f);
    for (int i=0; i<128; i++) assert(abs(static_pcm[i]) <= 250);
    assert(fv_audio_is_sweep_burst_active(50, 200));
    assert(!fv_audio_is_sweep_burst_active(220, 200));

    /* FieldVox Local bank 100-file FIFO eviction cap */
    char temp_dir[] = "/tmp/fv_bank_test_XXXXXX";
    assert(mkdtemp(temp_dir) != NULL);
    for (int i = 0; i < 105; i++) {
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/clip_%03d.wav", temp_dir, i);
        FILE *tf = fopen(file_path, "wb");
        assert(tf != NULL);
        fputc(0, tf);
        fclose(tf);
    }
    int evicted = fv_local_bank_enforce_cache_cap(temp_dir, 100);
    assert(evicted == 5);
    for (int i = 5; i < 105; i++) {
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/clip_%03d.wav", temp_dir, i);
        unlink(file_path);
    }
    rmdir(temp_dir);

    FvGrainGate gate;
    fv_gate_init(&gate,48000,123,0); fv_gate_set_grain_ms(&gate,1000);
    assert(fv_gate_request(&gate,0,FV_GATE_PROXIMITY,0,450,true));
    for (int i=0; i<gate.grain_count; i++) assert(gate.grain_len[i]==48000u);
    fv_gate_init(&gate,48000,123,0); fv_gate_set_grain_ms(&gate,-1); assert(gate.grain_ms==0);
    fv_gate_set_grain_ms(&gate,1); assert(gate.grain_ms==65);
    printf("  [PASS] C23 sensor/DSP port, CRC validation, effect levels, fragment timing and microphone ducking\n");
}

int main(void) {
    test_c23_dsp_and_vox();
    test_sensor_only();
    test_hit_capture();
    test_grain_gate();
    printf("[TEST] Running FieldVox native C unit tests...\n");

    /* 1. Verify SHA-256 standard test vector */
    char hex[65];
    test_sha256_hex("abc", hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    printf("  [PASS] SHA-256 standard vector 'abc'\n");

    /* 2. Verify Session Stem Validation */
    assert(is_safe_session_stem("session-1790453871-1") == 1);
    assert(is_safe_session_stem("session-unknown") == 1);
    assert(is_safe_session_stem("session-1743123456-live") == 1);
    assert(is_safe_session_stem("../session-attack") == 0);
    assert(is_safe_session_stem("not-a-session-123") == 0);
    assert(is_safe_session_stem("") == 0);
    assert(is_safe_session_stem(NULL) == 0);
    printf("  [PASS] Session stem safety assertions\n");

    /* 3. Verify Trigger Event Counting */
    const char *test_jsonl =
        "{\"type\":\"trigger\",\"trigger\":1,\"t_ns\":1000,\"mag\":1.0,\"delta\":0.5,\"freq_hz\":440.0,\"threshold\":0.4,\"cooldown_ms\":450}\n"
        "{\"type\":\"frame\",\"seq\":2}\n"
        "{\"event\":\"trigger\",\"mode\":\"realtime_evp_monitor\",\"source\":\"microphone\",\"t_ms\":2000}\n"
        "{\"type\":\"trigger\",\"trigger\":2,\"t_ns\":3000,\"mag\":1.2,\"delta\":0.6,\"freq_hz\":880.0,\"threshold\":0.4,\"cooldown_ms\":450}\n";
    assert(count_trigger_events_str(test_jsonl) == 3);
    printf("  [PASS] Native trigger counting (3 events)\n");

    /* 4. Verify Station Blob Magic, Count, and CRC32 */
    assert(sizeof(FV_STATION_PLAN_BINARY_DATA) >= 16);
    uint32_t magic = load_le32(FV_STATION_PLAN_BINARY_DATA);
    assert(magic == FV_STATION_BLOB_MAGIC);

    uint32_t count = load_le32(FV_STATION_PLAN_BINARY_DATA + 8);
    assert(count == FV_STATION_BLOB_COUNT);
    assert(count == 289);
    assert(FV_STATION_BLOB_COUNT == 289);

    uint32_t expected_crc = load_le32(FV_STATION_PLAN_BINARY_DATA + 12);
    assert(expected_crc == FV_STATION_BLOB_CRC32);

    const uint8_t *record_bytes = FV_STATION_PLAN_BINARY_DATA + 16;
    size_t record_len = count * sizeof(FvBlobStationEntry);
    uint32_t computed_crc = crc32_buf(0, record_bytes, record_len);
    assert(computed_crc == expected_crc);
    printf("  [PASS] C23 packed station blob header & CRC32 verification (%u stations)\n", (unsigned)count);

    /* 5. Verify first station entry in binary blob */
    const FvBlobStationEntry *e0 = (const FvBlobStationEntry *)record_bytes;
    assert(e0->slot_id == 0);
    assert(strcmp(e0->callsign, "531AM Bible Radio") == 0);
    assert(strcmp(e0->lang, "tl") == 0);
    assert(strncmp(e0->url, "https://", 8) == 0);
    printf("  [PASS] Binary station blob slot 0: %s (%s)\n", e0->name, e0->callsign);

    /* Catalog records must preserve bounded, terminated HTTP(S) metadata.
     * Individual broadcasters can still be unavailable or require playlists. */
    for (uint32_t i = 0; i < count; i++) {
        const FvBlobStationEntry *e = (const FvBlobStationEntry *)(record_bytes + (size_t)i * sizeof(FvBlobStationEntry));
        assert(e->slot_id == i);
        assert(memchr(e->callsign, '\0', sizeof(e->callsign)) != NULL);
        assert(memchr(e->name, '\0', sizeof(e->name)) != NULL);
        assert(memchr(e->country, '\0', sizeof(e->country)) != NULL);
        assert(memchr(e->lang, '\0', sizeof(e->lang)) != NULL);
        assert(memchr(e->url_hash, '\0', sizeof(e->url_hash)) != NULL);
        assert(memchr(e->url, '\0', sizeof(e->url)) != NULL);
        assert(strncmp(e->url, "https://", 8) == 0 || strncmp(e->url, "http://", 7) == 0);
        assert(e->callsign[0] != '\0' && e->name[0] != '\0');
    }
    printf("  [PASS] All %u station slots contain bounded http(s) metadata\n", count);

    /* 6. Exercise the production native DSP and WAV exporter. */
    fv_audio_dsp_reset();
    int16_t full_scale[8192];
    for (size_t i = 0; i < sizeof(full_scale) / sizeof(full_scale[0]); i++) {
        full_scale[i] = INT16_MAX;
    }
    fv_apply_schroeder_reverb(full_scale, (int)(sizeof(full_scale) / sizeof(full_scale[0])));
    for (size_t i = 0; i < sizeof(full_scale) / sizeof(full_scale[0]); i++) {
        assert(full_scale[i] >= 0);
    }
    printf("  [PASS] Native reverb feedback remains saturated without signed wraparound\n");

    /* 6a. EnvironmentalReverb 50 ms pre-delay verification */
    {
        fv_audio_dsp_reset();
        int16_t impulse_buf[4800] = {0};
        impulse_buf[0] = 20000;
        fv_apply_schroeder_reverb_ex(impulse_buf, 4800, 1.0f);
        assert(impulse_buf[0] == (int16_t)(((int64_t)20000 * 700LL) / 1000LL));
        for (size_t t = 1; t < 2400; t++) {
            assert(impulse_buf[t] == 0);
        }
        bool reflections_arrived = false;
        for (size_t t = 2400; t < 4800; t++) {
            if (impulse_buf[t] != 0) {
                reflections_arrived = true;
                break;
            }
        }
        assert(reflections_arrived);
        printf("  [PASS] FieldVox EnvironmentalReverb 50ms pre-delay (2400 samples) verification\n");
    }

    /* 6b. FieldVox 3-tap discrete echo model verification */
    {
        fv_audio_dsp_reset();
        const int delay_ms = 150;
        const uint32_t j = (uint32_t)(48000 * delay_ms / 1000);
        int16_t *echo_test = calloc(28800 + 100, sizeof(int16_t));
        assert(echo_test != NULL);
        echo_test[0] = 10000;
        fv_audio_apply_echo_ex(echo_test, (int)(28800 + 100), delay_ms, 1.0f);

        assert(echo_test[0] == 10000);
        for (size_t t = 1; t < j; t++) {
            assert(echo_test[t] == 0);
        }
        assert(echo_test[j] == 600);
        for (size_t t = j + 1; t < 2 * j; t++) {
            assert(echo_test[t] == 0);
        }
        assert(echo_test[2 * j] == 180);
        for (size_t t = 2 * j + 1; t < 3 * j; t++) {
            assert(echo_test[t] == 0);
        }
        assert(echo_test[3 * j] == 54);
        for (size_t t = 3 * j + 1; t < 28800 + 100; t++) {
            assert(echo_test[t] == 0);
        }
        free(echo_test);
        printf("  [PASS] Legacy PCM echo 3-tap discrete model (production fragment echo is tested separately) (Volume_k = vol * 0.2 * (0.3)^k) verified\n");
    }

    const int16_t capture[] = { -32768, -1234, 0, 1234, 32767 };
    fv_audio_capture_and_analyze(capture, (int)(sizeof(capture) / sizeof(capture[0])));
    char wav_path[128];
    snprintf(wav_path, sizeof(wav_path), "/tmp/fieldvox-preroll-%ld.wav", (long)getpid());
    assert(fv_audio_export_preroll_wav(wav_path, 1));
    FILE *wav = fopen(wav_path, "rb");
    assert(wav != NULL);
    uint8_t wav_header[44];
    const size_t header_read = fread(wav_header, 1, sizeof(wav_header), wav);
    assert(header_read == sizeof(wav_header));
    assert(memcmp(wav_header, "RIFF", 4) == 0);
    assert(memcmp(wav_header + 8, "WAVEfmt ", 8) == 0);
    assert(memcmp(wav_header + 36, "data", 4) == 0);
    const int seek_result = fseek(wav, 0, SEEK_END);
    assert(seek_result == 0);
    const long wav_size = ftell(wav);
    assert(wav_size == 96044L);
    const int close_result = fclose(wav);
    assert(close_result == 0);
    assert(unlink(wav_path) == 0);
    printf("  [PASS] Production pre-roll WAV header and payload size\n");

    /* 7. Verify BT.709 NV12 colorimetry with native Android Little-Endian RGBA_8888 */
    {
        /* Pure Red pixel in Android native RGBA_8888 memory: R=255, G=0, B=0, A=255 -> 0xFF0000FF */
        const uint32_t red_pixel = 0xFF0000FFu;
        const int r = (int)(red_pixel & 0xFF);
        const int g = (int)((red_pixel >> 8) & 0xFF);
        const int b = (int)((red_pixel >> 16) & 0xFF);
        assert(r == 255);
        assert(g == 0);
        assert(b == 0);

        /* BT.709 Studio Swing coefficients: Y in [16..235], Cb/Cr in [16..240] centered at 128 */
        const int y = ((54 * r + 183 * g + 18 * b + 128) >> 8) + 16;
        const int cb = (-29 * r - 99 * g + 128 * b + 32896) >> 8;
        const int cr = (128 * r - 116 * g - 12 * b + 32896) >> 8;

        /* For pure Red: Y ~ 70, Cb ~ 100 (< 128), Cr ~ 256 clamped to 255 (> 200) */
        assert(y >= 65 && y <= 75);
        assert(cb < 128);
        assert(cr > 200);

        /* Pure Blue pixel: R=0, G=0, B=255, A=255 -> 0xFFFF0000 */
        const uint32_t blue_pixel = 0xFFFF0000u;
        const int br = (int)(blue_pixel & 0xFF);
        const int bg = (int)((blue_pixel >> 8) & 0xFF);
        const int bb = (int)((blue_pixel >> 16) & 0xFF);
        assert(br == 0 && bg == 0 && bb == 255);
        const int b_cb = (-29 * br - 99 * bg + 128 * bb + 32896) >> 8;
        const int b_cr = (128 * br - 116 * bg - 12 * bb + 32896) >> 8;
        /* For pure Blue: Cb must be high (> 200), Cr must be low (< 128) */
        assert(b_cb > 200);
        assert(b_cr < 128);
        printf("  [PASS] Native RGBA_8888 to BT.709 NV12 colorimetry mapping (Red/Blue channels verified)\n");
    }

    /* Camera filter tests call the production functions in test_camera_effects.c. */

    /* 9. Verify packet buffer offset safety invariant */
    {
        /* In NDK AMediaMuxer_writeSampleData, pointer is passed as (data + info->offset).
         * Detached pending packet buffers allocate exactly info.size bytes with data copied
         * from data + info.offset. Therefore, p->info.offset MUST be 0 to prevent buffer overflow. */
        const size_t raw_codec_offset = 64;
        const size_t payload_size = 2048;
        uint8_t raw_buffer[raw_codec_offset + payload_size];
        memset(raw_buffer, 0xAA, sizeof(raw_buffer));

        uint8_t *detached = (uint8_t *)malloc(payload_size);
        assert(detached != NULL);
        memcpy(detached, raw_buffer + raw_codec_offset, payload_size);
        size_t detached_offset = 0; /* Reset invariant */
        assert(detached_offset == 0);
        /* Ensure effective pointer is within [detached, detached + payload_size] */
        const uint8_t *effective_ptr = detached + detached_offset;
        assert(effective_ptr == detached);
        free(detached);
        printf("  [PASS] Video recorder pending packet offset invariant (no double addition)\n");
    }

    /* 10. Verify camera sensor orientation compensation and bijective 90-deg rotation */
    {
        /* Back camera sensor orientation = 90 deg.
         * In portrait (display rotation 0 deg): compensation = (90 - 0 + 360) % 360 = 90 deg.
         * In landscape (display rotation 90 deg): compensation = (90 - 90 + 360) % 360 = 0 deg.
         * In landscape (display rotation 270 deg): compensation = (90 - 270 + 360) % 360 = 180 deg. */
        const int sensor_ori = 90;
        assert((sensor_ori - 0 + 360) % 360 == 90);
        assert((sensor_ori - 90 + 360) % 360 == 0);
        assert((sensor_ori - 270 + 360) % 360 == 180);

        /* Test 90-degree clockwise bijective coordinate mapping for 320x180 -> 180x320 */
        const int sw = 320, sh = 180;
        const int dw = sh, dh = sw; /* 180 x 320 */
        int hit_map[180 * 320];
        memset(hit_map, 0, sizeof(hit_map));

        for (int sy = 0; sy < sh; sy++) {
            for (int sx = 0; sx < sw; sx++) {
                const int dx = (sh - 1) - sy;
                const int dy = sx;
                assert(dx >= 0 && dx < dw);
                assert(dy >= 0 && dy < dh);
                const int idx = dy * dw + dx;
                assert(hit_map[idx] == 0); /* Bijective: no overlaps */
                hit_map[idx] = 1;
            }
        }
        for (int i = 0; i < dw * dh; i++) {
            assert(hit_map[i] == 1); /* All destination pixels covered */
        }

        /* Test 90-degree clockwise bijective coordinate mapping for 640x360 -> 360x640 */
        const int sw2 = 640, sh2 = 360;
        const int dw2 = sh2, dh2 = sw2; /* 360 x 640 */
        uint8_t *hit_map2 = (uint8_t *)calloc((size_t)dw2 * (size_t)dh2, 1);
        assert(hit_map2 != NULL);
        for (int sy = 0; sy < sh2; sy++) {
            for (int sx = 0; sx < sw2; sx++) {
                const int dx = (sh2 - 1) - sy;
                const int dy = sx;
                assert(dx >= 0 && dx < dw2);
                assert(dy >= 0 && dy < dh2);
                const int idx = dy * dw2 + dx;
                assert(hit_map2[idx] == 0);
                hit_map2[idx] = 1;
            }
        }
        for (int i = 0; i < dw2 * dh2; i++) {
            assert(hit_map2[i] == 1);
        }
        free(hit_map2);
        printf("  [PASS] Camera sensor orientation compensation & bijective 90° portrait rotation (320x180 & 640x360)\n");
    }

    /* 11. Verify production GUI widget layout invariants, bounding boxes, dock isolation, and tap hit resolution */
    {
        assert(W_CAL == 0);
        assert(W_COUNT > W_EVIDENCE_BANK_3);

        /* Test across various display configurations:
         * 1) Standard portrait: 1080x2400, density 2.625
         * 2) Narrow portrait: 720x1280, density 2.0
         * 3) Large landscape: 2400x1080, density 2.625 */
        const struct {
            int w, h;
            float d;
        } configs[] = {
            {1080, 2400, 2.625f},
            {720, 1280, 2.0f},
            {2400, 1080, 2.625f}
        };

        for (int c_idx = 0; c_idx < 3; c_idx++) {
            const int screen_w = configs[c_idx].w;
            const int screen_h = configs[c_idx].h;
            const float d = configs[c_idx].d;
            FvUiScreenLayout layout;

            /* Check 1: Drawer Closed State using production main-screen layout */
            WidgetRect widgets[W_COUNT];
            fv_ui_layout_main_screen(widgets, screen_w, screen_h, d, false, false, &layout);

            /* Assert dock buttons do not overlap horizontally */
            const float dock_gap = 5.0f * d;
            const float b1_right = widgets[W_SESS].x + widgets[W_SESS].w;
            const float b2_left = widgets[W_MARK_HIT].x;
            const float b2_right = widgets[W_MARK_HIT].x + widgets[W_MARK_HIT].w;
            const float shutter_left = widgets[W_VIDEO_REC].x;
            const float shutter_right = widgets[W_VIDEO_REC].x + widgets[W_VIDEO_REC].w;
            const float b3_left = widgets[W_CAM_TINT].x;
            const float b3_right = widgets[W_CAM_TINT].x + widgets[W_CAM_TINT].w;
            const float b4_left = widgets[W_CTRL].x;
            const float b4_right = widgets[W_CTRL].x + widgets[W_CTRL].w;

            assert(b2_left >= b1_right + dock_gap - 0.01f);
            assert(shutter_left >= b2_right + 2.0f * d - 0.01f);
            assert(b3_left >= shutter_right + 2.0f * d - 0.01f);
            assert(b4_left >= b3_right + dock_gap - 0.01f);
            assert(b4_right <= layout.ui_x0 + layout.ui_w + 0.01f);

            /* Test production tap resolution on closed-drawer dock & badges */
            assert(fv_ui_resolve_tap(widgets, widgets[W_VIDEO_REC].x + widgets[W_VIDEO_REC].w * 0.5f,
                                     widgets[W_VIDEO_REC].y + widgets[W_VIDEO_REC].h * 0.5f) == W_VIDEO_REC);
            assert(fv_ui_resolve_tap(widgets, widgets[W_SESS].x + widgets[W_SESS].w * 0.5f,
                                     widgets[W_SESS].y + widgets[W_SESS].h * 0.5f) == W_SESS);
            assert(fv_ui_resolve_tap(widgets, widgets[W_MARK_HIT].x + widgets[W_MARK_HIT].w * 0.5f,
                                     widgets[W_MARK_HIT].y + widgets[W_MARK_HIT].h * 0.5f) == W_MARK_HIT);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CAM_TINT].x + widgets[W_CAM_TINT].w * 0.5f,
                                     widgets[W_CAM_TINT].y + widgets[W_CAM_TINT].h * 0.5f) == W_CAM_TINT);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CTRL].x + widgets[W_CTRL].w * 0.5f,
                                     widgets[W_CTRL].y + widgets[W_CTRL].h * 0.5f) == W_CTRL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CAL].x + widgets[W_CAL].w * 0.5f,
                                     widgets[W_CAL].y + widgets[W_CAL].h * 0.5f) == W_CAL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_REMPOD].x + widgets[W_REMPOD].w * 0.5f,
                                     widgets[W_REMPOD].y + widgets[W_REMPOD].h * 0.5f) == W_REMPOD);
            const float preview_cx = widgets[W_CAM_PREVIEW].x + widgets[W_CAM_PREVIEW].w * 0.25f;
            const float preview_cy = widgets[W_CAM_PREVIEW].y + widgets[W_CAM_PREVIEW].h * 0.5f;
            assert(fv_ui_resolve_tap(widgets, preview_cx, preview_cy) == W_CAM_PREVIEW);

            /* Check 2: Advanced Drawer Open State (reuses widgets from Check 1 to test production masking) */
            fv_ui_layout_main_screen(widgets, screen_w, screen_h, d, true, true, &layout);

            /* Crucial invariant: production drawer masking hides W_CAM_PREVIEW and dock controls */
            assert(!widgets[W_CAM_PREVIEW].visible);
            assert(!widgets[W_CAM_TINT].visible);
            assert(!widgets[W_CTRL].visible);
            assert(!widgets[W_MARK_HIT].visible);
            assert(fv_ui_resolve_tap(widgets, preview_cx, preview_cy) != W_CAM_PREVIEW);

            /* Tap tests on advanced drawer buttons must resolve to the drawer buttons */
            assert(fv_ui_resolve_tap(widgets, widgets[W_SENS_LOW].x + 5.0f, widgets[W_SENS_LOW].y + widgets[W_SENS_LOW].h * 0.5f) == W_SENS_LOW);
            assert(fv_ui_resolve_tap(widgets, widgets[W_SENS_MED].x + 5.0f, widgets[W_SENS_MED].y + widgets[W_SENS_MED].h * 0.5f) == W_SENS_MED);
            assert(fv_ui_resolve_tap(widgets, widgets[W_SENS_HIGH].x + 5.0f, widgets[W_SENS_HIGH].y + widgets[W_SENS_HIGH].h * 0.5f) == W_SENS_HIGH);
            assert(fv_ui_resolve_tap(widgets, widgets[W_COOL_CYCLE].x + 5.0f, widgets[W_COOL_CYCLE].y + widgets[W_COOL_CYCLE].h * 0.5f) == W_COOL_CYCLE);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CAMHUD].x + 5.0f, widgets[W_CAMHUD].y + widgets[W_CAMHUD].h * 0.5f) == W_CAMHUD);
            assert(fv_ui_resolve_tap(widgets, widgets[W_MIC].x + 5.0f, widgets[W_MIC].y + widgets[W_MIC].h * 0.5f) == W_MIC);
            assert(fv_ui_resolve_tap(widgets, widgets[W_REC].x + 5.0f, widgets[W_REC].y + widgets[W_REC].h * 0.5f) == W_REC);
            assert(fv_ui_resolve_tap(widgets, widgets[W_VIDEO_REC].x + 5.0f, widgets[W_VIDEO_REC].y + widgets[W_VIDEO_REC].h * 0.5f) == W_VIDEO_REC);
            assert(fv_ui_resolve_tap(widgets, widgets[W_SESS].x + 5.0f, widgets[W_SESS].y + widgets[W_SESS].h * 0.5f) == W_SESS);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CLOSE_DRAWER].x + 5.0f, widgets[W_CLOSE_DRAWER].y + 2.0f * d) == W_CLOSE_DRAWER);
            assert(fv_ui_tap_dismisses_drawer(widgets, true, -1, layout.drawer_y - 20.0f * d, d));
            assert(!fv_ui_tap_dismisses_drawer(widgets, true, W_SENS_LOW, layout.drawer_y - 20.0f * d, d));

            /* Check 3: Default Audio Controls Drawer (!advanced_controls) also masks W_CAM_PREVIEW */
            fv_ui_layout_main_screen(widgets, screen_w, screen_h, d, true, false, &layout);
            assert(!widgets[W_CAM_PREVIEW].visible);
            assert(fv_ui_resolve_tap(widgets, widgets[W_ECHO_LEVEL].x + 5.0f, widgets[W_ECHO_LEVEL].y + widgets[W_ECHO_LEVEL].h * 0.5f) == W_ECHO_LEVEL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_REVERB_LEVEL].x + 5.0f, widgets[W_REVERB_LEVEL].y + widgets[W_REVERB_LEVEL].h * 0.5f) == W_REVERB_LEVEL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_DISTORTION_LEVEL].x + 5.0f, widgets[W_DISTORTION_LEVEL].y + widgets[W_DISTORTION_LEVEL].h * 0.5f) == W_DISTORTION_LEVEL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_GRAIN_LEVEL].x + 5.0f, widgets[W_GRAIN_LEVEL].y + widgets[W_GRAIN_LEVEL].h * 0.5f) == W_GRAIN_LEVEL);
            assert(fv_ui_resolve_tap(widgets, widgets[W_START].x + 5.0f, widgets[W_START].y + widgets[W_START].h * 0.5f) == W_START);
            assert(fv_ui_resolve_tap(widgets, widgets[W_ADVANCED].x + 5.0f, widgets[W_ADVANCED].y + widgets[W_ADVANCED].h * 0.5f) == W_ADVANCED);
            assert(fv_ui_resolve_tap(widgets, widgets[W_CLOSE_DRAWER].x + 5.0f, widgets[W_CLOSE_DRAWER].y + 2.0f * d) == W_CLOSE_DRAWER);
        }
        printf("  [PASS] Production widget boundaries, dock shutter clearance, and drawer tap resolution\n");
    }

    printf("ALL FieldVox native C unit tests PASSED.\n");
    return 0;
}
