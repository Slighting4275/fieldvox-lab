#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#define FV1U_TAG "FieldVox"
#define FV1U_RING_COUNT 1000u
#define FV1U_FAST_SWEEP_PERIOD_MS 1000u
#define FV1U_AUDIO_DWELL_PERIOD_MS 6000u
/* FIELDVOX_STAGE1U_N_AUDIO_DWELL_SINGLE_GUI_TICK: source/audio dwell is slower than visible channel clock for network streams. */
/* FIELDVOX_STAGE1U_L_FAST_NATIVE_SWEEP: previous 180 ms fast sweep marker. */
/* FIELDVOX_STAGE1U_M_ONE_SECOND_NATIVE_SWEEP: user-tuned 1000 ms/channel cadence from native C. */
#define FV1U_BASE_COUNT 16u
#define FV1U_MAGIC 0x46563155u /* FV1U */

typedef struct Fv1UStationBase {
    const char *label;
    const char *url;
    uint32_t weight;
} Fv1UStationBase;

static const Fv1UStationBase FV1U_BASE[FV1U_BASE_COUNT] = {
    { "BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", 101 },
    { "Iowa Public Radio News", "https://news-stream.iowapublicradio.org/NewsLow.mp3", 103 },
    { "Wisconsin Public Radio News", "https://wpr-ice.streamguys1.com/wpr-ideas-mp3-64", 107 },
    { "KQED Radio News/Talk", "https://streams.kqed.org/kqedradio", 109 },
    { "WNYC AM News/Talk", "https://fm939.wnyc.org/wnycam", 113 },
    { "WBUR Boston", "https://wbur-sc.streamguys1.com/wbur", 127 },
    { "WAMU Washington", "https://wamu-1.streamguys1.com/", 131 },
    { "Minnesota Public Radio News", "https://nis.stream.publicradio.org/nis.mp3", 137 },
    { "Vermont Public", "https://vpr.streamguys1.com/vpr64.mp3", 139 },
    { "WBEZ Chicago", "https://stream.wbez.org/wbez128.mp3", 149 },
    { "OPB Radio", "https://stream1.opb.org/radio.mp3", 151 },
    { "KCRW News", "https://kcrw.streamguys1.com/kcrw_192k_mp3_news", 157 },
    { "WUNC North Carolina", "https://wunc-ice.streamguys1.com/wunc-128-mp3", 163 },
    { "WHYY Philadelphia", "https://whyy.streamguys1.com/whyy-mp3", 167 },
    { "KERA News", "https://kera-ice.streamguys1.com/kera", 173 },
    { "WLRN Miami", "https://stream.wlrn.mobi/WLRNFMAAC", 179 }
};

/*
 * Binary blob layout:
 *   u32 magic
 *   u32 version
 *   u32 ring_count
 *   u32 base_count
 *   repeated logical records are generated deterministically from this compact blob.
 *
 * This is intentionally a binary-blob station plan, not Java arrays as the source of truth.
 * URLs are still app-private runtime data; exports should use labels/hashes only.
 */
static const uint8_t FV1U_STATION_PLAN_BLOB[] = {
    0x55,0x31,0x56,0x46, 0x01,0x00,0x00,0x00, 0xE8,0x03,0x00,0x00, 0x10,0x00,0x00,0x00,
    0x1d,0x37,0x59,0x7b, 0x9d,0xbf,0xd1,0xf3, 0x42,0x24,0x66,0x88, 0xaa,0xcc,0xee,0x10
};

static uint32_t fv1u_u32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fv1u_mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t fv1u_station_index(uint32_t logical_index) {
    uint32_t seed = fv1u_u32_le(FV1U_STATION_PLAN_BLOB + 16 + ((logical_index & 3u) * 4u));
    uint32_t mixed = fv1u_mix32(logical_index * 2654435761u ^ seed);
    return mixed % FV1U_BASE_COUNT;
}

static uint32_t fv1u_crc32_step(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)(-(int)(crc & 1u));
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t fv1u_hash_string(const char *s) {
    return fv1u_crc32_step(0, (const uint8_t *)s, strlen(s));
}

static int fv1u_blob_ok(void) {
    uint32_t magic = fv1u_u32_le(FV1U_STATION_PLAN_BLOB + 0);
    uint32_t version = fv1u_u32_le(FV1U_STATION_PLAN_BLOB + 4);
    uint32_t ring_count = fv1u_u32_le(FV1U_STATION_PLAN_BLOB + 8);
    uint32_t base_count = fv1u_u32_le(FV1U_STATION_PLAN_BLOB + 12);
    return magic == FV1U_MAGIC && version == 1u && ring_count == FV1U_RING_COUNT && base_count == FV1U_BASE_COUNT;
}

/*
 * Decoder scaffold:
 * For Stage 1U-A, prove the native binary blob / station-ring decode boundary in C.
 * Later Stage 1U-B should wire AMediaExtractor + AMediaCodec decode and feed PCM into AAudio.
 */
static int fv1u_native_decoder_probe(uint32_t logical_index, int16_t *pcm_out, uint32_t frames) {
    if (!fv1u_blob_ok() || pcm_out == NULL || frames == 0) {
        return -1;
    }
    const Fv1UStationBase *st = &FV1U_BASE[fv1u_station_index(logical_index)];
    uint32_t h = fv1u_hash_string(st->label) ^ fv1u_hash_string(st->url)
            ^ st->weight ^ logical_index;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t v = fv1u_mix32(h + i * 1103515245u);
        int32_t sample = (int32_t)((v >> 17) & 0x3ff) - 512;
        pcm_out[i] = (int16_t)(sample * 18);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UStationRingCount(JNIEnv *env, jobject thiz) {
    (void) env;
    (void) thiz;
    return fv1u_blob_ok() ? (jint)FV1U_RING_COUNT : 0;
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UStationAt(JNIEnv *env, jobject thiz, jint logical_index) {
    (void) thiz;
    if (!fv1u_blob_ok()) {
        return (*env)->NewStringUTF(env, "native_blob_error");
    }
    uint32_t idx = (uint32_t)logical_index;
    const Fv1UStationBase *st = &FV1U_BASE[fv1u_station_index(idx)];

    char buf[768];
    uint32_t label_hash = fv1u_hash_string(st->label);
    uint32_t url_hash = fv1u_hash_string(st->url);
    snprintf(buf, sizeof(buf),
             "{\"stage\":\"1U-A\",\"logical\":%u,\"ring_count\":%u,\"base_slot\":%u,\"weight\":%u,\"label\":\"%s\",\"url\":\"%s\",\"label_hash\":%u,\"url_hash\":%u}",
             idx % FV1U_RING_COUNT,
             FV1U_RING_COUNT,
             fv1u_station_index(idx),
             st->weight,
             st->label,
             st->url,
             label_hash,
             url_hash);
    return (*env)->NewStringUTF(env, buf);
}


JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1ULFastSweepPeriodMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    return (jint)FV1U_FAST_SWEEP_PERIOD_MS;
}

JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UNAudioDwellPeriodMs(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    return (jint)FV1U_AUDIO_DWELL_PERIOD_MS;
}
JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UDecoderProbe(JNIEnv *env, jobject thiz) {
    (void) thiz;
    int16_t pcm[512];
    uint32_t ring_hash = fv1u_crc32_step(0, FV1U_STATION_PLAN_BLOB, sizeof(FV1U_STATION_PLAN_BLOB));
    uint32_t label_roll = 0;
    int failures = 0;

    for (uint32_t i = 0; i < FV1U_RING_COUNT; i++) {
        uint32_t si = fv1u_station_index(i);
        const Fv1UStationBase *st = &FV1U_BASE[si];
        label_roll ^= fv1u_mix32(fv1u_hash_string(st->label) + i);
        if (fv1u_native_decoder_probe(i, pcm, 512) != 0) {
            failures++;
        }
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "stage=1U-A native_blob_ok=%d ring_count=%u base_count=%u blob_crc32=%u label_roll=%u decode_probe_failures=%d decoder=custom_binary_blob_pcm_probe future=AMediaExtractor_AMediaCodec_AAudio",
             fv1u_blob_ok(),
             FV1U_RING_COUNT,
             FV1U_BASE_COUNT,
             ring_hash,
             label_roll,
             failures);
    __android_log_print(ANDROID_LOG_INFO, FV1U_TAG, "Stage 1U-A native decoder probe: %s", buf);
    return (*env)->NewStringUTF(env, buf);
}
