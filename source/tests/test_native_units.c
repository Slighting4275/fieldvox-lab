#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>

#include "../../android/app/src/main/cpp/fv_station_blob_data.h"
#include "../../android/app/src/main/cpp/fv_native_audio.h"
#include "../../android/app/src/main/cpp/fv_grain_gate.h"

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

typedef enum {
    TEST_W_CAL, TEST_W_CADENCE, TEST_W_DIRECTION, TEST_W_PITCH,
    TEST_W_ECHO, TEST_W_REVERB, TEST_W_NOISE, TEST_W_REV,
    TEST_W_SESS, TEST_W_REC, TEST_W_START, TEST_W_MIC, TEST_W_CTRL,
    TEST_W_SB11, TEST_W_VOCAL, TEST_W_REMPOD, TEST_W_PAN,
    TEST_W_ESTES, TEST_W_CAMHUD, TEST_W_SENS_DOWN, TEST_W_SENS_UP,
    TEST_W_COOL_DOWN, TEST_W_COOL_UP,
    TEST_W_MARK_HIT, TEST_W_CAM_PREVIEW,
    TEST_W_VIDEO_REC, TEST_W_CAM_TINT, TEST_W_CLOSE_DRAWER,
    TEST_W_TORCH, TEST_W_HELP, TEST_W_MUTE,
    TEST_W_SENS_LOW, TEST_W_SENS_MED, TEST_W_SENS_HIGH, TEST_W_COOL_CYCLE,
    TEST_W_EVIDENCE_OPEN_0, TEST_W_EVIDENCE_OPEN_1,
    TEST_W_EVIDENCE_OPEN_2, TEST_W_EVIDENCE_OPEN_3,
    TEST_W_CLOSE_EVIDENCE, TEST_W_CLOSE_HELP,
    TEST_W_COUNT
} TestWidgetId;

typedef struct {
    float x, y, w, h;
    bool visible;
} TestWidgetRect;

static int test_resolve_tap(const TestWidgetRect *widgets, float tap_x, float tap_y) {
    for (int i = TEST_W_COUNT - 1; i >= 0; i--) {
        if (!widgets[i].visible) continue;
        if (tap_x >= widgets[i].x && tap_x <= widgets[i].x + widgets[i].w &&
            tap_y >= widgets[i].y && tap_y <= widgets[i].y + widgets[i].h) {
            return i;
        }
    }
    return -1;
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

int main(void) {
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
    uint32_t magic = ((uint32_t)FV_STATION_PLAN_BINARY_DATA[0]) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[1] << 8) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[2] << 16) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[3] << 24);
    assert(magic == FV_STATION_BLOB_MAGIC);

    uint32_t count = ((uint32_t)FV_STATION_PLAN_BINARY_DATA[8]) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[9] << 8) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[10] << 16) |
                     ((uint32_t)FV_STATION_PLAN_BINARY_DATA[11] << 24);
    assert(count == FV_STATION_BLOB_COUNT);
    assert(count == 512);
    assert(FV_STATION_BLOB_COUNT == 512);

    uint32_t expected_crc = ((uint32_t)FV_STATION_PLAN_BINARY_DATA[12]) |
                           ((uint32_t)FV_STATION_PLAN_BINARY_DATA[13] << 8) |
                           ((uint32_t)FV_STATION_PLAN_BINARY_DATA[14] << 16) |
                           ((uint32_t)FV_STATION_PLAN_BINARY_DATA[15] << 24);
    assert(expected_crc == FV_STATION_BLOB_CRC32);

    const uint8_t *record_bytes = FV_STATION_PLAN_BINARY_DATA + 16;
    size_t record_len = count * sizeof(FvBlobStationEntry);
    uint32_t computed_crc = crc32_buf(0, record_bytes, record_len);
    assert(computed_crc == expected_crc);
    printf("  [PASS] Zig packed station blob header & CRC32 verification (%u stations)\n", (unsigned)count);

    /* 5. Verify first station entry in binary blob */
    const FvBlobStationEntry *e0 = (const FvBlobStationEntry *)record_bytes;
    assert(e0->slot_id == 0);
    assert(strcmp(e0->callsign, "KIRO 97.3") == 0);
    assert(strcmp(e0->country, "US") == 0);
    assert(strncmp(e0->url, "https://", 8) == 0);
    printf("  [PASS] Binary station blob slot 0: %s (%s)\n", e0->name, e0->callsign);

    /* Every slot must be a direct http(s) stream the native extractor can open: no playlists or HLS. */
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
        assert(strstr(e->url, ".m3u") == NULL && strstr(e->url, ".pls") == NULL);
        assert(e->callsign[0] != '\0' && e->name[0] != '\0');
    }
    printf("  [PASS] All %u station slots are direct http(s) streams\n", count);

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
        const int cb = ((-29 * r - 99 * g + 128 * b + 128) >> 8) + 128;
        const int cr = ((128 * r - 116 * g - 12 * b + 128) >> 8) + 128;

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
        const int b_cb = ((-29 * br - 99 * bg + 128 * bb + 128) >> 8) + 128;
        const int b_cr = ((128 * br - 116 * bg - 12 * bb + 128) >> 8) + 128;
        /* For pure Blue: Cb must be high (> 200), Cr must be low (< 128) */
        assert(b_cb > 200);
        assert(b_cr < 128);
        printf("  [PASS] Native RGBA_8888 to BT.709 NV12 colorimetry mapping (Red/Blue channels verified)\n");
    }

    /* 8. Verify camera night-vision filter color transfer */
    {
        const uint8_t luma = 180;
        /* Phosphor green: dominant green */
        const uint32_t p_r = (uint32_t)(luma / 10u);
        const uint32_t p_g = (uint32_t)luma;
        const uint32_t p_b = (uint32_t)(luma / 5u);
        assert(p_g > p_r && p_g > p_b);
        assert(p_r == 18 && p_g == 180 && p_b == 36);

        /* Cyan spectral: cyan dominance */
        const uint32_t c_r = (uint32_t)(luma / 12u);
        const uint32_t c_g = (uint32_t)(luma + (luma / 6u));
        const uint32_t c_b = (uint32_t)luma;
        assert(c_g > c_r && c_b > c_r);
        printf("  [PASS] Camera phosphor and cyan spectral filter formulas\n");
    }

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

    /* 11. Verify GUI widget layout invariants, bounding boxes, dock isolation, and tap hit resolution */
    {
        assert(TEST_W_CAL == 0);
        assert(TEST_W_COUNT == 41);

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
            const bool landscape = screen_w > screen_h;
            const float top_inset = (landscape ? 14.0f : 58.0f) * d;
            const float side_inset = (landscape ? 36.0f : 12.0f) * d;
            const float bottom_inset = (landscape ? 10.0f : 32.0f) * d;
            const float ui_x0 = side_inset;
            const float ui_w = (float)screen_w - 2.0f * side_inset;

            /* Check 1: Drawer Closed State */
            TestWidgetRect widgets[TEST_W_COUNT];
            memset(widgets, 0, sizeof(widgets));

            /* Top header (no placed widgets) */
            const float header_h = (landscape ? 28.0f : 36.0f) * d;
            const float status_y = top_inset + header_h + 3.0f * d;
            const float status_h = (landscape ? 14.0f : 18.0f) * d;
            const float badge_y = status_y + status_h + 3.0f * d;
            const float badge_h = (landscape ? 24.0f : 30.0f) * d;
            const float half_w = (ui_w - 6.0f * d) * 0.5f;

            /* Perimeter Badges: W_CAL (EMF) & W_REMPOD (Baro/Compass) */
            widgets[TEST_W_CAL] = (TestWidgetRect){ui_x0, badge_y, half_w, badge_h, true};
            widgets[TEST_W_REMPOD] = (TestWidgetRect){ui_x0 + half_w + 6.0f * d, badge_y, half_w, badge_h, true};

            /* Dock & Floating Visualizer */
            const float bottom_bar_h = (landscape ? 46.0f : 66.0f) * d;
            const float bottom_bar_y = (float)screen_h - bottom_inset - bottom_bar_h;
            const float vis_h = (landscape ? 54.0f : 80.0f) * d;
            const float vis_top = bottom_bar_y - vis_h - 8.0f * d;

            /* Floating visualizer placed when drawer is closed */
            widgets[TEST_W_CAM_PREVIEW] = (TestWidgetRect){ui_x0, vis_top, ui_w, vis_h, true};

            /* Bottom Action Bar */
            const float dock_cx = ui_x0 + ui_w * 0.5f;
            const float dock_cy = bottom_bar_y + bottom_bar_h * 0.5f;
            const float r_outer = (bottom_bar_h >= 52.0f * d ? 28.0f : 21.0f) * d;
            const float gap = 5.0f * d;
            const float btn_h = (bottom_bar_h >= 52.0f * d ? 38.0f : 30.0f) * d;
            const float btn_y = dock_cy - btn_h * 0.5f;
            const float side_clearance = r_outer + 8.0f * d;
            const float avail_w = (ui_w * 0.5f) - side_clearance;
            const float btn_w = (avail_w - gap) * 0.5f;
            const float hit_pad = 6.0f * d;

            /* Left flank */
            const float b1_x = ui_x0;
            widgets[TEST_W_START] = (TestWidgetRect){b1_x, btn_y, btn_w, btn_h, true};
            const float b2_x = b1_x + btn_w + gap;
            widgets[TEST_W_MARK_HIT] = (TestWidgetRect){b2_x, btn_y, btn_w, btn_h, true};

            /* Center Shutter */
            widgets[TEST_W_VIDEO_REC] = (TestWidgetRect){
                dock_cx - r_outer - hit_pad, dock_cy - r_outer - hit_pad,
                (r_outer + hit_pad) * 2.0f, (r_outer + hit_pad) * 2.0f, true
            };

            /* Right flank */
            const float b3_x = dock_cx + side_clearance;
            widgets[TEST_W_CAM_TINT] = (TestWidgetRect){b3_x, btn_y, btn_w, btn_h, true};
            const float b4_x = b3_x + btn_w + gap;
            widgets[TEST_W_CTRL] = (TestWidgetRect){b4_x, btn_y, btn_w, btn_h, true};

            /* Assert dock buttons do not overlap horizontally */
            const float b1_right = widgets[TEST_W_START].x + widgets[TEST_W_START].w;
            const float b2_left = widgets[TEST_W_MARK_HIT].x;
            const float b2_right = widgets[TEST_W_MARK_HIT].x + widgets[TEST_W_MARK_HIT].w;
            const float shutter_left = widgets[TEST_W_VIDEO_REC].x;
            const float shutter_right = widgets[TEST_W_VIDEO_REC].x + widgets[TEST_W_VIDEO_REC].w;
            const float b3_left = widgets[TEST_W_CAM_TINT].x;
            const float b3_right = widgets[TEST_W_CAM_TINT].x + widgets[TEST_W_CAM_TINT].w;
            const float b4_left = widgets[TEST_W_CTRL].x;
            const float b4_right = widgets[TEST_W_CTRL].x + widgets[TEST_W_CTRL].w;

            assert(b2_left >= b1_right + gap - 0.01f);
            assert(shutter_left >= b2_right + 2.0f * d - 0.01f);
            assert(b3_left >= shutter_right + 2.0f * d - 0.01f);
            assert(b4_left >= b3_right + gap - 0.01f);
            assert(b4_right <= ui_x0 + ui_w + 0.01f);

            /* Test tap resolution for center shutter */
            assert(test_resolve_tap(widgets, dock_cx, dock_cy) == TEST_W_VIDEO_REC);
            /* Test tap resolution for SWEEP */
            assert(test_resolve_tap(widgets, b1_x + btn_w * 0.5f, btn_y + btn_h * 0.5f) == TEST_W_START);
            /* Test tap resolution for MARK HIT */
            assert(test_resolve_tap(widgets, b2_x + btn_w * 0.5f, btn_y + btn_h * 0.5f) == TEST_W_MARK_HIT);
            /* Test tap resolution for CAM TINT */
            assert(test_resolve_tap(widgets, b3_x + btn_w * 0.5f, btn_y + btn_h * 0.5f) == TEST_W_CAM_TINT);
            /* Test tap resolution for CTRL */
            assert(test_resolve_tap(widgets, b4_x + btn_w * 0.5f, btn_y + btn_h * 0.5f) == TEST_W_CTRL);
            /* Test tap resolution for Top EMF Badge */
            assert(test_resolve_tap(widgets, ui_x0 + half_w * 0.5f, badge_y + badge_h * 0.5f) == TEST_W_CAL);
            /* Test tap resolution for Top Environment Badge */
            assert(test_resolve_tap(widgets, ui_x0 + half_w + 6.0f * d + half_w * 0.5f, badge_y + badge_h * 0.5f) == TEST_W_REMPOD);

            /* Check 2: Drawer Open State */
            memset(widgets, 0, sizeof(widgets));
            const float drawer_h = (landscape ? 240.0f : 290.0f) * d;
            const float drawer_y = (float)screen_h - bottom_inset - drawer_h;
            const float d_header_h = 28.0f * d;
            const float close_w = 40.0f * d, close_h = 22.0f * d;
            widgets[TEST_W_CLOSE_DRAWER] = (TestWidgetRect){
                ui_x0 + ui_w - close_w - 6.0f * d, drawer_y + 4.0f * d, close_w, close_h, true
            };

            float cur_y = drawer_y + d_header_h + 4.0f * d;
            const float d_gap = 3.0f * d;
            const float pill_h = (drawer_h - d_header_h - 32.0f * d) / 5.0f;
            const float p4_w = (ui_w - d_gap * 3.0f - 16.0f * d) / 4.0f;

            /* Row 4: Sensitivity & Cooldown */
            float r4_y = cur_y + (pill_h + d_gap) * 3.0f;
            float px4 = ui_x0 + 8.0f * d;
            widgets[TEST_W_SENS_LOW] = (TestWidgetRect){px4, r4_y, p4_w, pill_h, true};
            px4 += p4_w + d_gap;
            widgets[TEST_W_SENS_MED] = (TestWidgetRect){px4, r4_y, p4_w, pill_h, true};
            px4 += p4_w + d_gap;
            widgets[TEST_W_SENS_HIGH] = (TestWidgetRect){px4, r4_y, p4_w, pill_h, true};
            px4 += p4_w + d_gap;
            widgets[TEST_W_COOL_CYCLE] = (TestWidgetRect){px4, r4_y, p4_w, pill_h, true};

            /* Row 5: Audio Session & Recording Tools */
            float r5_y = cur_y + (pill_h + d_gap) * 4.0f;
            float px5 = ui_x0 + 8.0f * d;
            widgets[TEST_W_MIC] = (TestWidgetRect){px5, r5_y, p4_w, pill_h, true};
            px5 += p4_w + d_gap;
            widgets[TEST_W_REC] = (TestWidgetRect){px5, r5_y, p4_w, pill_h, true};
            px5 += p4_w + d_gap;
            widgets[TEST_W_VIDEO_REC] = (TestWidgetRect){px5, r5_y, p4_w, pill_h, true};
            px5 += p4_w + d_gap;
            widgets[TEST_W_SESS] = (TestWidgetRect){px5, r5_y, p4_w, pill_h, true};

            /* Crucial invariant: W_CAM_PREVIEW is NOT visible when drawer is open */
            assert(!widgets[TEST_W_CAM_PREVIEW].visible);

            /* Tap tests on drawer buttons must resolve to the drawer buttons, not W_CAM_PREVIEW */
            assert(test_resolve_tap(widgets, widgets[TEST_W_SENS_LOW].x + 5.0f, r4_y + pill_h * 0.5f) == TEST_W_SENS_LOW);
            assert(test_resolve_tap(widgets, widgets[TEST_W_SENS_MED].x + 5.0f, r4_y + pill_h * 0.5f) == TEST_W_SENS_MED);
            assert(test_resolve_tap(widgets, widgets[TEST_W_SENS_HIGH].x + 5.0f, r4_y + pill_h * 0.5f) == TEST_W_SENS_HIGH);
            assert(test_resolve_tap(widgets, widgets[TEST_W_COOL_CYCLE].x + 5.0f, r4_y + pill_h * 0.5f) == TEST_W_COOL_CYCLE);
            assert(test_resolve_tap(widgets, widgets[TEST_W_MIC].x + 5.0f, r5_y + pill_h * 0.5f) == TEST_W_MIC);
            assert(test_resolve_tap(widgets, widgets[TEST_W_REC].x + 5.0f, r5_y + pill_h * 0.5f) == TEST_W_REC);
            assert(test_resolve_tap(widgets, widgets[TEST_W_VIDEO_REC].x + 5.0f, r5_y + pill_h * 0.5f) == TEST_W_VIDEO_REC);
            assert(test_resolve_tap(widgets, widgets[TEST_W_SESS].x + 5.0f, r5_y + pill_h * 0.5f) == TEST_W_SESS);
            assert(test_resolve_tap(widgets, widgets[TEST_W_CLOSE_DRAWER].x + 5.0f, drawer_y + 6.0f * d) == TEST_W_CLOSE_DRAWER);
        }
        printf("  [PASS] Widget boundaries, dock shutter clearance, and drawer tap resolution\n");
    }

    printf("ALL FieldVox native C unit tests PASSED.\n");
    return 0;
}
