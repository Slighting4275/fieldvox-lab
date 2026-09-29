#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <android/log.h>

#include "fv_station_blob_data.h"

#define FV1U_TAG "FieldVox"
#define FV1U_FAST_SWEEP_PERIOD_MS 1000u
#define FV1U_AUDIO_DWELL_PERIOD_MS 6000u

static uint32_t fv1u_u32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t fv1u_crc32_step(uint32_t crc, const uint8_t *buf, size_t len) {
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

static int fv_blob_ok(void) {
    if (sizeof(FV_STATION_PLAN_BINARY_DATA) < 16) return 0;
    uint32_t magic = fv1u_u32_le(FV_STATION_PLAN_BINARY_DATA + 0);
    uint32_t version = fv1u_u32_le(FV_STATION_PLAN_BINARY_DATA + 4);
    uint32_t count = fv1u_u32_le(FV_STATION_PLAN_BINARY_DATA + 8);
    uint32_t blob_crc = fv1u_u32_le(FV_STATION_PLAN_BINARY_DATA + 12);
    if (magic != FV_STATION_BLOB_MAGIC || version != FV_STATION_BLOB_VERSION || count != FV_STATION_BLOB_COUNT) {
        return 0;
    }
    const uint8_t *data = FV_STATION_PLAN_BINARY_DATA + 16;
    size_t data_len = FV_STATION_BLOB_COUNT * sizeof(FvBlobStationEntry);
    uint32_t calc_crc = fv1u_crc32_step(0, data, data_len);
    return (calc_crc == blob_crc) && (blob_crc == FV_STATION_BLOB_CRC32);
}

static const FvBlobStationEntry *fv_blob_get_entry(uint32_t slot) {
    if (FV_STATION_BLOB_COUNT == 0) return NULL;
    uint32_t idx = slot % FV_STATION_BLOB_COUNT;
    const uint8_t *entries = FV_STATION_PLAN_BINARY_DATA + 16;
    return (const FvBlobStationEntry *)(entries + (idx * sizeof(FvBlobStationEntry)));
}

JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UStationRingCount(JNIEnv *env, jobject thiz) {
    (void) env;
    (void) thiz;
    return fv_blob_ok() ? (jint)FV_STATION_BLOB_COUNT : 0;
}

JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeGetStationCount(JNIEnv *env, jobject thiz) {
    (void) env;
    (void) thiz;
    return fv_blob_ok() ? (jint)FV_STATION_BLOB_COUNT : 0;
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeGetStationLabel(JNIEnv *env, jobject thiz, jint slot) {
    (void) thiz;
    if (!fv_blob_ok()) return (*env)->NewStringUTF(env, "unknown");
    const FvBlobStationEntry *entry = fv_blob_get_entry((uint32_t)slot);
    return entry ? (*env)->NewStringUTF(env, entry->name) : (*env)->NewStringUTF(env, "unknown");
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeGetStationCallsign(JNIEnv *env, jobject thiz, jint slot) {
    (void) thiz;
    if (!fv_blob_ok()) return (*env)->NewStringUTF(env, "unknown");
    const FvBlobStationEntry *entry = fv_blob_get_entry((uint32_t)slot);
    return entry ? (*env)->NewStringUTF(env, entry->callsign) : (*env)->NewStringUTF(env, "unknown");
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeGetStationUrl(JNIEnv *env, jobject thiz, jint slot) {
    (void) thiz;
    if (!fv_blob_ok()) return (*env)->NewStringUTF(env, "");
    const FvBlobStationEntry *entry = fv_blob_get_entry((uint32_t)slot);
    return entry ? (*env)->NewStringUTF(env, entry->url) : (*env)->NewStringUTF(env, "");
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1UStationAt(JNIEnv *env, jobject thiz, jint logical_index) {
    (void) thiz;
    if (!fv_blob_ok()) {
        return (*env)->NewStringUTF(env, "native_blob_error");
    }
    uint32_t idx = (uint32_t)logical_index;
    const FvBlobStationEntry *entry = fv_blob_get_entry(idx);
    if (!entry) {
        return (*env)->NewStringUTF(env, "native_blob_error");
    }

    char buf[1024];
    int written = snprintf(buf, sizeof(buf),
             "{\"stage\":\"1U-A\",\"logical\":%u,\"ring_count\":%u,\"base_slot\":%u,\"slot\":%u,\"callsign\":\"%s\",\"label\":\"%s\",\"url\":\"%s\",\"country\":\"%s\",\"lang\":\"%s\",\"hash\":\"%s\"}",
             idx % FV_STATION_BLOB_COUNT,
             FV_STATION_BLOB_COUNT,
             entry->slot_id,
             entry->slot_id,
             entry->callsign,
             entry->name,
             entry->url,
             entry->country,
             entry->lang,
             entry->url_hash);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        return (*env)->NewStringUTF(env, "native_blob_error");
    }
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
    int ok = fv_blob_ok();
    int failures = 0;

    for (uint32_t i = 0; i < FV_STATION_BLOB_COUNT; i++) {
        const FvBlobStationEntry *entry = fv_blob_get_entry(i);
        if (!entry || entry->slot_id != i || entry->name[0] == '\0') {
            failures++;
        }
    }

    char buf[512];
    int written = snprintf(buf, sizeof(buf),
             "stage=1U-A native_blob_ok=%d ring_count=%u base_count=%u blob_crc32=0x%08X decode_probe_failures=%d decoder=packed_binary_blob_zig",
             ok,
             FV_STATION_BLOB_COUNT,
             FV_STATION_BLOB_COUNT,
             FV_STATION_BLOB_CRC32,
             failures);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        return (*env)->NewStringUTF(env, "native_decoder_probe_error");
    }
    __android_log_print(ANDROID_LOG_INFO, FV1U_TAG, "Stage 1U-A native decoder probe: %s", buf);
    return (*env)->NewStringUTF(env, buf);
}
