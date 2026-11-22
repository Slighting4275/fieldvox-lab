/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "fv_native_radio.h"
#include "fv_fragment_engine.h"
#include "fv_jni_helpers.h"
#include "fv_media_decode.h"
#include "fv_radio_playlist.h"
#include "fv_station_policy.h"
#include "fv_station_blob_data.h"
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FieldVoxRadio", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "FieldVoxRadio", __VA_ARGS__)
#define FETCH_BYTES 50000u
#define PLAYLIST_BYTES FETCH_BYTES
#define JSON_BYTES 131072u
#define REDIRECT_HOPS 5
#define PLAYLIST_HOPS 3

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    JavaVM *vm;
    bool started, enabled, queued, busy, ready;
    bool test_queued, test_busy, test_ready;
    bool refresh_queued, refresh_busy;
    unsigned generation, job_generation;
    FvRadioBank bank;
    FvDynamicStation stations[FV_STATION_MAX];
    char languages[FV_STATION_MAX][8];
    const char *url_ptrs[FV_STATION_MAX];
    uint8_t health[FV_STATION_MAX];
    size_t station_count;
    unsigned group[FV_STATION_GROUP_SIZE];
    size_t group_count;
    int station, result_station, result_fd;
    int test_slot, test_station, test_result_station, test_result_fd, last_tested_slot;
    int last_refresh_added;
    FvVoxDecimalTrigger trigger, result_trigger;
    int16_t *pcm, *test_pcm;
    size_t frames, test_frames;
    char directory[512];
} radio = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .result_fd = -1,
    .test_result_fd = -1,
    .last_tested_slot = -1
};

static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool still_active(bool require_enabled, unsigned generation) {
    if (!require_enabled) return true;
    pthread_mutex_lock(&radio.mutex);
    const bool ok = radio.enabled && radio.generation == generation;
    pthread_mutex_unlock(&radio.mutex);
    return ok;
}

static void copy_cstr(char *dst, size_t cap, const char *src, size_t src_cap) {
    if (!dst || !cap) return;
    size_t n = 0;
    while (n + 1u < cap && n < src_cap && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static FvRadioBank bank_from_language(const char *language) {
    if (!language) return FV_RADIO_BANK_EN_US;
    if (strcmp(language, "en_uk") == 0 || strcmp(language, "en-GB") == 0) return FV_RADIO_BANK_EN_UK;
    if (strcmp(language, "en_au") == 0 || strcmp(language, "en-AU") == 0) return FV_RADIO_BANK_EN_AU;
    if (strcmp(language, "en") == 0) return FV_RADIO_BANK_EN;
    if (strcmp(language, "all") == 0 || strcmp(language, "*") == 0) return FV_RADIO_BANK_ALL;
    return FV_RADIO_BANK_EN_US;
}

static void rebuild_group_locked(uint32_t seed) {
    if (!radio.station_count) {
        radio.group_count = 0u;
        return;
    }
    radio.group_count = fv_station_group_healthy(
        radio.languages, radio.url_ptrs, radio.health,
        radio.station_count, fv_station_bank_code(radio.bank), seed, radio.group);
}

static bool has_station_url_locked(const char *url) {
    for (size_t i = 0; i < radio.station_count; i++) {
        if (strcmp(radio.stations[i].url, url) == 0) return true;
    }
    return false;
}

static int merge_dynamic_stations_locked(const FvDynamicStation *dyn, size_t count) {
    int added = 0;
    for (size_t i = 0; i < count && radio.station_count < FV_STATION_MAX; i++) {
        if (!fv_station_url_is_valid(dyn[i].url) || has_station_url_locked(dyn[i].url)) continue;
        const size_t idx = radio.station_count++;
        FvDynamicStation *e = &radio.stations[idx];
        *e = dyn[i];
        memcpy(radio.languages[idx], e->lang, 8u);
        radio.url_ptrs[idx] = e->url;
        radio.health[idx] = FV_STATION_UNTESTED;
        added++;
    }
    return added;
}

static void load_cached_json_locked(void) {
    char path[560];
    snprintf(path, sizeof(path), "%s/fieldvox-radio-stations.json", radio.directory);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    const size_t cap = JSON_BYTES * 2u;
    char *buf = malloc(cap + 1u);
    if (!buf) {
        fclose(f);
        return;
    }
    const size_t n = fread(buf, 1u, cap, f);
    fclose(f);
    if (n > 0u) {
        buf[n] = '\0';
        FvDynamicStation *dyn = calloc(128u, sizeof(*dyn));
        if (dyn) {
            const size_t parsed = fv_station_parse_radiobrowser_json(buf, n, dyn, 128u);
            const int added = merge_dynamic_stations_locked(dyn, parsed);
            LOGI("loaded cached directory: parsed=%zu added=%d total=%zu", parsed, added, radio.station_count);
            free(dyn);
        }
    }
    free(buf);
}

static void jstring_to_cstr(JNIEnv *env, jstring jstr, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!jstr) return;
    const char *utf = (*env)->GetStringUTFChars(env, jstr, NULL);
    if (utf) {
        snprintf(out, cap, "%s", utf);
        (*env)->ReleaseStringUTFChars(env, jstr, utf);
    }
}

/* Fetches a single HTTP/HTTPS URL following up to REDIRECT_HOPS redirects
 * (including cross-protocol HTTP<->HTTPS). Returns bytes captured into mem_buf
 * (when file == NULL) or writes directly to file (and fills head_buf with the
 * first <=256 bytes so callers can sniff #EXTM3U / [playlist]). */
static bool http_fetch_once(JNIEnv *env, const char *start_url, bool require_enabled,
                            unsigned generation, FILE *file, uint8_t *mem_buf,
                            size_t max_bytes, size_t *out_bytes,
                            char *out_final_url, size_t final_cap,
                            char *out_content_type, size_t ct_cap,
                            char *out_head, size_t *out_head_len) {
    if (out_bytes) *out_bytes = 0u;
    if (out_head_len) *out_head_len = 0u;
    if (out_content_type && ct_cap) out_content_type[0] = '\0';
    if (out_final_url && final_cap) snprintf(out_final_url, final_cap, "%s", start_url);

    char current_url[FV_RADIO_URL_MAX];
    snprintf(current_url, sizeof(current_url), "%s", start_url);

    if ((*env)->PushLocalFrame(env, 32) != JNI_OK) {
        fv_jni_clear(env);
        return false;
    }

    bool ok = false;
    jobject connection = NULL;
    jobject input = NULL;
    jclass url_class = fv_jni_find_class(env, NULL, "java/net/URL", false);
    jclass http_class = fv_jni_find_class(env, NULL, "java/net/HttpURLConnection", false);
    jclass input_class = fv_jni_find_class(env, NULL, "java/io/InputStream", false);
    if (!url_class || !http_class || !input_class) goto done;

    jmethodID ctor = fv_jni_method(env, NULL, url_class, "<init>", "(Ljava/lang/String;)V", false);
    jmethodID open = fv_jni_method(env, NULL, url_class, "openConnection", "()Ljava/net/URLConnection;", false);
    jmethodID set_follow = fv_jni_method(env, NULL, http_class, "setInstanceFollowRedirects", "(Z)V", false);
    jmethodID set_conn_to = fv_jni_method(env, NULL, http_class, "setConnectTimeout", "(I)V", false);
    jmethodID set_read_to = fv_jni_method(env, NULL, http_class, "setReadTimeout", "(I)V", false);
    jmethodID set_prop = fv_jni_method(env, NULL, http_class, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", false);
    jmethodID get_code = fv_jni_method(env, NULL, http_class, "getResponseCode", "()I", false);
    jmethodID get_hdr = fv_jni_method(env, NULL, http_class, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;", false);
    jmethodID get_ct = fv_jni_method(env, NULL, http_class, "getContentType", "()Ljava/lang/String;", false);
    jmethodID get_input = fv_jni_method(env, NULL, http_class, "getInputStream", "()Ljava/io/InputStream;", false);
    jmethodID disconnect = fv_jni_method(env, NULL, http_class, "disconnect", "()V", false);
    jmethodID read_m = fv_jni_method(env, NULL, input_class, "read", "([BII)I", false);
    jmethodID close_m = fv_jni_method(env, NULL, input_class, "close", "()V", false);
    if (!ctor || !open || !set_follow || !set_conn_to || !set_read_to || !set_prop ||
        !get_code || !get_hdr || !get_ct || !get_input || !disconnect || !read_m || !close_m) goto done;

    jstring ua_k = (*env)->NewStringUTF(env, "User-Agent");
    jstring ua_v = (*env)->NewStringUTF(env, "FieldVoxLab/0.5.2");
    jstring loc_k = (*env)->NewStringUTF(env, "Location");
    if (!ua_k || !ua_v || !loc_k || !fv_jni_clear(env)) goto done;

    for (int hop = 0; hop <= REDIRECT_HOPS; hop++) {
        if (!still_active(require_enabled, generation)) goto done;
        jstring addr = (*env)->NewStringUTF(env, current_url);
        if (!addr || !fv_jni_clear(env)) goto done;
        jobject url_obj = (*env)->NewObject(env, url_class, ctor, addr);
        (*env)->DeleteLocalRef(env, addr);
        if (!url_obj || !fv_jni_clear(env)) goto done;

        connection = (*env)->CallObjectMethod(env, url_obj, open);
        (*env)->DeleteLocalRef(env, url_obj);
        if (!connection || !fv_jni_clear(env) || !(*env)->IsInstanceOf(env, connection, http_class)) goto done;

        (*env)->CallVoidMethod(env, connection, set_follow, JNI_FALSE);
        (*env)->CallVoidMethod(env, connection, set_conn_to, 5000);
        (*env)->CallVoidMethod(env, connection, set_read_to, 5000);
        (*env)->CallVoidMethod(env, connection, set_prop, ua_k, ua_v);
        if (!fv_jni_clear(env)) goto done;

        const jint code = (*env)->CallIntMethod(env, connection, get_code);
        if (!fv_jni_clear(env) || !still_active(require_enabled, generation)) goto done;

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            jstring loc_val = (jstring)(*env)->CallObjectMethod(env, connection, get_hdr, loc_k);
            char loc_buf[FV_RADIO_URL_MAX] = {0};
            if (fv_jni_clear(env) && loc_val) {
                jstring_to_cstr(env, loc_val, loc_buf, sizeof(loc_buf));
                (*env)->DeleteLocalRef(env, loc_val);
            }
            (*env)->CallVoidMethod(env, connection, disconnect);
            fv_jni_clear(env);
            (*env)->DeleteLocalRef(env, connection);
            connection = NULL;
            char next_url[FV_RADIO_URL_MAX] = {0};
            if (!loc_buf[0] || !fv_radio_resolve_relative_url(current_url, loc_buf, next_url, sizeof(next_url))) {
                goto done;
            }
            snprintf(current_url, sizeof(current_url), "%s", next_url);
            continue;
        }

        if (code < 200 || code >= 300) goto done;

        if (out_final_url && final_cap) snprintf(out_final_url, final_cap, "%s", current_url);
        if (out_content_type && ct_cap) {
            jstring ct_val = (jstring)(*env)->CallObjectMethod(env, connection, get_ct);
            if (fv_jni_clear(env) && ct_val) {
                jstring_to_cstr(env, ct_val, out_content_type, ct_cap);
                (*env)->DeleteLocalRef(env, ct_val);
            }
        }
        break;
    }

    if (!connection) goto done;
    input = (*env)->CallObjectMethod(env, connection, get_input);
    if (!input || !fv_jni_clear(env)) goto done;

    jbyteArray bytes = (*env)->NewByteArray(env, 4096);
    if (!bytes) goto done;

    size_t total = 0u;
    size_t head_len = 0u;
    const long started = now_ms();
    while (total < max_bytes && still_active(require_enabled, generation) && now_ms() - started < 5000L) {
        const size_t rem = max_bytes - total;
        const jint wanted = (jint)(rem > 4096u ? 4096u : rem);
        const jint n = (*env)->CallIntMethod(env, input, read_m, bytes, 0, wanted);
        if (!fv_jni_clear(env) || n < 0) break;
        if (!n) continue;
        if (n > wanted) break;
        jbyte buffer[4096];
        (*env)->GetByteArrayRegion(env, bytes, 0, n, buffer);
        if (!fv_jni_clear(env)) goto done;

        if (out_head && head_len < 256u) {
            const size_t copy_n = ((size_t)n < (256u - head_len)) ? (size_t)n : (256u - head_len);
            memcpy(out_head + head_len, buffer, copy_n);
            head_len += copy_n;
        }
        if (mem_buf) {
            memcpy(mem_buf + total, buffer, (size_t)n);
        }
        if (file && fwrite(buffer, 1u, (size_t)n, file) != (size_t)n) goto done;
        total += (size_t)n;
    }
    if (out_bytes) *out_bytes = total;
    if (out_head_len) *out_head_len = head_len;
    ok = total > 0u && still_active(require_enabled, generation) && (!file || fflush(file) == 0);
    LOGI("http fetch: bytes=%zu in %ld ms (%s)", total, now_ms() - started, current_url);

done:
    fv_jni_clear(env);
    if (input) {
        jmethodID close_id = input_class ? fv_jni_method(env, NULL, input_class, "close", "()V", false) : NULL;
        if (close_id) (*env)->CallVoidMethod(env, input, close_id);
        fv_jni_clear(env);
    }
    if (connection) {
        jmethodID disc_id = http_class ? fv_jni_method(env, NULL, http_class, "disconnect", "()V", false) : NULL;
        if (disc_id) (*env)->CallVoidMethod(env, connection, disc_id);
        fv_jni_clear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
    return ok;
}

static bool download(JNIEnv *env, const char *url, FILE *file, bool require_enabled, unsigned generation) {
    char current_url[FV_RADIO_URL_MAX];
    snprintf(current_url, sizeof(current_url), "%s", url);

    for (int hop = 0; hop <= PLAYLIST_HOPS; hop++) {
        char final_url[FV_RADIO_URL_MAX] = {0};
        char content_type[96] = {0};
        char head[256] = {0};
        size_t head_len = 0u;
        size_t bytes = 0u;

        if (fv_radio_is_playlist_url(current_url, NULL)) {
            uint8_t *pl_buf = malloc(PLAYLIST_BYTES + 1u);
            if (!pl_buf) return false;
            const bool got = http_fetch_once(env, current_url, require_enabled, generation, NULL, pl_buf,
                                             PLAYLIST_BYTES, &bytes, final_url, sizeof(final_url),
                                             content_type, sizeof(content_type), NULL, NULL);
            if (!got) {
                free(pl_buf);
                return false;
            }
            pl_buf[bytes] = '\0';
            char next_url[FV_RADIO_URL_MAX] = {0};
            bool nested = false;
            const bool extracted = fv_radio_extract_stream_url(
                final_url[0] ? final_url : current_url, (const char *)pl_buf, bytes,
                next_url, sizeof(next_url), &nested);
            free(pl_buf);
            if (!extracted) return false;
            snprintf(current_url, sizeof(current_url), "%s", next_url);
            continue;
        }

        rewind(file);
        if (ftruncate(fileno(file), 0) != 0) return false;
        const bool got = http_fetch_once(env, current_url, require_enabled, generation, file, NULL,
                                         FETCH_BYTES, &bytes, final_url, sizeof(final_url),
                                         content_type, sizeof(content_type), head, &head_len);
        if (!got) return false;

        if (fv_radio_is_playlist_url(final_url, content_type) || fv_radio_body_is_playlist(head, head_len)) {
            if (bytes <= PLAYLIST_BYTES) {
                char *pl_buf = malloc(bytes + 1u);
                if (!pl_buf) return false;
                rewind(file);
                const size_t rd = fread(pl_buf, 1u, bytes, file);
                pl_buf[rd] = '\0';
                char next_url[FV_RADIO_URL_MAX] = {0};
                bool nested = false;
                const bool extracted = rd > 0u && fv_radio_extract_stream_url(
                    final_url[0] ? final_url : current_url, pl_buf, rd,
                    next_url, sizeof(next_url), &nested);
                free(pl_buf);
                if (!extracted) return false;
                snprintf(current_url, sizeof(current_url), "%s", next_url);
                continue;
            }
            return false;
        }
        return true;
    }
    return false;
}

static size_t append_pcm(int16_t *pcm, size_t count, const uint8_t *data, size_t bytes,
                         int rate, int channels, int encoding, double *phase, float *previous) {
    if (rate < 4000 || rate > 192000 || channels < 1 || channels > 32 || (encoding != 2 && encoding != 4)) return count;
    const size_t width = encoding == 4 ? sizeof(float) : sizeof(int16_t);
    const size_t frames = bytes / (width * (size_t)channels);
    const double step = (double)rate / (double)FV_FRAGMENT_RATE;
    for (size_t i = 0; i < frames && count < FV_FRAGMENT_CLIP_FRAMES; i++) {
        float mono = 0.0f;
        for (int c = 0; c < channels; c++) {
            const uint8_t *at = data + (i * (size_t)channels + (size_t)c) * width;
            if (encoding == 4) {
                float value;
                memcpy(&value, at, width);
                mono += isfinite(value) ? value : 0.0f;
            } else {
                int16_t value;
                memcpy(&value, at, width);
                mono += (float)value / 32768.0f;
            }
        }
        mono /= (float)channels;
        while (*phase <= 1.0 && count < FV_FRAGMENT_CLIP_FRAMES) {
            const float sample = *previous + (mono - *previous) * (float)*phase;
            pcm[count++] = (int16_t)(fmaxf(-1.0f, fminf(1.0f, sample)) * 32767.0f);
            *phase += step;
        }
        *phase -= 1.0;
        *previous = mono;
    }
    return count;
}

static int16_t *decode(const char *path, bool require_enabled, unsigned generation, size_t *frames) {
    AMediaExtractor *extractor = NULL;
    AMediaCodec *codec = NULL;
    int16_t *pcm = NULL;
    *frames = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    const off_t bytes = lseek(fd, 0, SEEK_END);
    int32_t rate = 48000, channels = 1, encoding = 2;
    const bool opened = bytes > 0 && fv_media_open_audio_track(fd, bytes, false, &extractor, &codec, &rate, &channels, NULL);
    close(fd);
    if (!opened) goto done;
    pcm = malloc(FV_FRAGMENT_CLIP_FRAMES * sizeof(*pcm));
    if (!pcm) goto done;
    double phase = 0.0;
    float previous = 0.0f;
    bool input_eos = false;
    const long started = now_ms();
    while (still_active(require_enabled, generation) && now_ms() - started < 5000L && *frames < FV_FRAGMENT_CLIP_FRAMES) {
        if (!input_eos) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (index >= 0) {
                size_t capacity = 0;
                uint8_t *buffer = AMediaCodec_getInputBuffer(codec, (size_t)index, &capacity);
                const ssize_t n = buffer ? AMediaExtractor_readSampleData(extractor, buffer, capacity) : -1;
                input_eos = n < 0;
                AMediaCodec_queueInputBuffer(codec, (size_t)index, 0, n < 0 ? 0u : (size_t)n,
                                             n < 0 ? 0u : (uint64_t)AMediaExtractor_getSampleTime(extractor),
                                             n < 0 ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0u);
                if (n >= 0) AMediaExtractor_advance(extractor);
            }
        }
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (index >= 0) {
            size_t capacity = 0;
            const uint8_t *buffer = AMediaCodec_getOutputBuffer(codec, (size_t)index, &capacity);
            if (buffer && info.offset >= 0 && info.size > 0 && (size_t)info.offset <= capacity &&
                (size_t)info.size <= capacity - (size_t)info.offset) {
                *frames = append_pcm(pcm, *frames, buffer + info.offset, (size_t)info.size,
                                     rate, channels, encoding, &phase, &previous);
            }
            AMediaCodec_releaseOutputBuffer(codec, (size_t)index, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *output = AMediaCodec_getOutputFormat(codec);
            if (output) {
                AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
                AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
                AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding);
                AMediaFormat_delete(output);
            }
        } else if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            break;
        }
    }
done:
    if (codec) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (extractor) AMediaExtractor_delete(extractor);
    if (!*frames || !still_active(require_enabled, generation)) {
        free(pcm);
        pcm = NULL;
        *frames = 0;
    }
    return pcm;
}

static void fetch_station_clip(JNIEnv *env, const char *dir, const char *url, bool require_enabled,
                               unsigned generation, int16_t **out_pcm, size_t *out_frames, int *out_fd) {
    *out_pcm = NULL;
    *out_frames = 0u;
    *out_fd = -1;
    char path[560];
    snprintf(path, sizeof(path), "%s/radio-slice-XXXXXX", dir);
    const int fd = mkstemp(path);
    FILE *file = fd >= 0 ? fdopen(fd, "wb+") : NULL;
    if (fd >= 0 && !file) close(fd);
    if (file) {
        const bool fetched = download(env, url, file, require_enabled, generation);
        const bool closed = fclose(file) == 0;
        file = NULL;
        if (fetched && closed) *out_pcm = decode(path, require_enabled, generation, out_frames);
    }
    if (file) fclose(file);
    if (*out_pcm) *out_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) unlink(path);
}

static bool fetch_with_fallback(JNIEnv *env, const char *primary, const char *fallback,
                                uint8_t *buf, size_t cap, size_t *out_bytes) {
    *out_bytes = 0u;
    if (http_fetch_once(env, primary, false, 0u, NULL, buf, cap, out_bytes,
                        NULL, 0u, NULL, 0u, NULL, NULL) && *out_bytes > 2u) {
        buf[*out_bytes] = '\0';
        return true;
    }
    *out_bytes = 0u;
    if (http_fetch_once(env, fallback, false, 0u, NULL, buf, cap, out_bytes,
                        NULL, 0u, NULL, 0u, NULL, NULL) && *out_bytes > 2u) {
        buf[*out_bytes] = '\0';
        return true;
    }
    return false;
}

static bool extract_json_array_inner(const uint8_t *buf, size_t len, size_t *out_start, size_t *out_end) {
    size_t s = 0u, e = len;
    while (s < e && buf[s] != '[') s++;
    if (s >= e) return false;
    s++;
    while (e > s && buf[e - 1u] != ']') e--;
    if (e <= s) return false;
    e--;
    while (s < e && (buf[s] == ' ' || buf[s] == '\t' || buf[s] == '\r' || buf[s] == '\n')) s++;
    while (e > s && (buf[e - 1u] == ' ' || buf[e - 1u] == '\t' || buf[e - 1u] == '\r' || buf[e - 1u] == '\n')) e--;
    if (s >= e) return false;
    *out_start = s;
    *out_end = e;
    return true;
}

static void run_directory_refresh(JNIEnv *env, const char *dir) {
    static const char *const news_urls[2] = {
        "https://de1.api.radio-browser.info/json/stations/search?limit=40&hidebroken=true&order=votes&reverse=true&tag=news",
        "https://at1.api.radio-browser.info/json/stations/search?limit=40&hidebroken=true&order=votes&reverse=true&tag=news"
    };
    static const char *const talk_urls[2] = {
        "https://at1.api.radio-browser.info/json/stations/search?limit=40&hidebroken=true&order=votes&reverse=true&tag=talk",
        "https://de1.api.radio-browser.info/json/stations/search?limit=40&hidebroken=true&order=votes&reverse=true&tag=talk"
    };
    int added = 0;
    uint8_t *news_buf = malloc(JSON_BYTES + 1u);
    uint8_t *talk_buf = malloc(JSON_BYTES + 1u);
    FvDynamicStation *dyn = calloc(128u, sizeof(*dyn));
    if (!news_buf || !talk_buf || !dyn) goto cleanup;

    size_t news_bytes = 0u, talk_bytes = 0u;
    const bool got_news = fetch_with_fallback(env, news_urls[0], news_urls[1], news_buf, JSON_BYTES, &news_bytes);
    const bool got_talk = fetch_with_fallback(env, talk_urls[0], talk_urls[1], talk_buf, JSON_BYTES, &talk_bytes);

    size_t parsed = 0u;
    if (got_news) {
        parsed += fv_station_parse_radiobrowser_json((const char *)news_buf, news_bytes, dyn + parsed, 128u - parsed);
    }
    if (got_talk && parsed < 128u) {
        parsed += fv_station_parse_radiobrowser_json((const char *)talk_buf, talk_bytes, dyn + parsed, 128u - parsed);
    }

    if (parsed > 0u) {
        size_t ns = 0u, ne = 0u, ts = 0u, te = 0u;
        const bool has_n = got_news && extract_json_array_inner(news_buf, news_bytes, &ns, &ne);
        const bool has_t = got_talk && extract_json_array_inner(talk_buf, talk_bytes, &ts, &te);
        if (has_n || has_t) {
            char cache_path[560];
            snprintf(cache_path, sizeof(cache_path), "%s/fieldvox-radio-stations.json", dir);
            FILE *cf = fopen(cache_path, "wb");
            if (cf) {
                fputc('[', cf);
                if (has_n) fwrite(news_buf + ns, 1u, ne - ns, cf);
                if (has_n && has_t) fputc(',', cf);
                if (has_t) fwrite(talk_buf + ts, 1u, te - ts, cf);
                fputc(']', cf);
                fclose(cf);
            }
        }
        pthread_mutex_lock(&radio.mutex);
        added = merge_dynamic_stations_locked(dyn, parsed);
        rebuild_group_locked((uint32_t)now_ms());
        pthread_mutex_unlock(&radio.mutex);
    }

cleanup:
    free(dyn);
    free(talk_buf);
    free(news_buf);
    pthread_mutex_lock(&radio.mutex);
    radio.last_refresh_added = added;
    radio.refresh_busy = false;
    pthread_mutex_unlock(&radio.mutex);
    LOGI("directory refresh complete: added=%d", added);
}

static void copy_job_inputs_locked(int station, char dir[512], char url[FV_RADIO_URL_MAX]) {
    snprintf(dir, 512u, "%s", radio.directory);
    url[0] = '\0';
    if (station >= 0 && (size_t)station < radio.station_count) {
        snprintf(url, FV_RADIO_URL_MAX, "%s", radio.stations[station].url);
    }
}

static void *worker(void *unused) {
    (void)unused;
    JNIEnv *env = NULL;
    if ((*radio.vm)->AttachCurrentThread(radio.vm, &env, NULL) != JNI_OK) {
        pthread_mutex_lock(&radio.mutex);
        radio.started = false;
        radio.busy = false;
        radio.test_busy = false;
        radio.refresh_busy = false;
        if (radio.queued && radio.enabled && radio.job_generation == radio.generation) {
            radio.result_station = radio.station;
            radio.result_trigger = radio.trigger;
            radio.ready = true;
        }
        radio.queued = false;
        radio.test_queued = false;
        radio.refresh_queued = false;
        pthread_mutex_unlock(&radio.mutex);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&radio.mutex);
        while (!radio.queued && !radio.test_queued && !radio.refresh_queued) {
            pthread_cond_wait(&radio.changed, &radio.mutex);
        }
        if (radio.test_queued) {
            const int station = radio.test_station;
            const int slot = radio.test_slot;
            char dir[512];
            char url[FV_RADIO_URL_MAX];
            copy_job_inputs_locked(station, dir, url);
            radio.test_queued = false;
            pthread_mutex_unlock(&radio.mutex);

            int16_t *pcm = NULL;
            size_t frames = 0u;
            int source_fd = -1;
            if (url[0]) fetch_station_clip(env, dir, url, false, 0u, &pcm, &frames, &source_fd);
            LOGI("tuner test: slot=%d station=%d frames=%zu", slot, station, frames);

            pthread_mutex_lock(&radio.mutex);
            if (station >= 0 && (size_t)station < radio.station_count) {
                radio.health[station] = (pcm && frames > 0u) ? FV_STATION_OK : FV_STATION_FAILED;
            }
            radio.last_tested_slot = slot;
            radio.test_busy = false;
            if (pcm && frames > 0u) {
                free(radio.test_pcm);
                if (radio.test_result_fd >= 0) close(radio.test_result_fd);
                radio.test_pcm = pcm;
                radio.test_frames = frames;
                radio.test_result_station = station;
                radio.test_result_fd = source_fd;
                radio.test_ready = true;
            } else {
                free(pcm);
                if (source_fd >= 0) close(source_fd);
            }
            pthread_mutex_unlock(&radio.mutex);
            continue;
        }

        if (radio.refresh_queued) {
            char dir[512];
            snprintf(dir, sizeof(dir), "%s", radio.directory);
            radio.refresh_queued = false;
            pthread_mutex_unlock(&radio.mutex);
            run_directory_refresh(env, dir);
            continue;
        }

        const int station = radio.station;
        const unsigned generation = radio.job_generation;
        const FvVoxDecimalTrigger trigger = radio.trigger;
        char dir[512];
        char url[FV_RADIO_URL_MAX];
        copy_job_inputs_locked(station, dir, url);
        radio.queued = false;
        pthread_mutex_unlock(&radio.mutex);

        int16_t *pcm = NULL;
        size_t frames = 0u;
        int source_fd = -1;
        if (url[0]) fetch_station_clip(env, dir, url, true, generation, &pcm, &frames, &source_fd);
        LOGI("clip result: station=%d frames=%zu duration=%.3fs", station, frames, (double)frames / 48000.0);

        pthread_mutex_lock(&radio.mutex);
        radio.busy = false;
        const bool active_gen = radio.enabled && radio.generation == generation;
        if (station >= 0 && (size_t)station < radio.station_count && active_gen) {
            if (pcm && frames > 0u) {
                radio.health[station] = FV_STATION_OK;
            } else {
                radio.health[station] = FV_STATION_FAILED;
                const int rep = fv_station_replace_failed(
                    radio.languages, radio.url_ptrs, radio.health, radio.station_count,
                    fv_station_bank_code(radio.bank), (uint32_t)now_ms(),
                    radio.group, radio.group_count, station);
                if (rep >= 0) LOGI("replaced dead station %d with %d", station, rep);
            }
        }
        if (radio.enabled && radio.generation == generation) {
            radio.pcm = pcm;
            radio.result_fd = source_fd;
            radio.frames = frames;
            radio.result_station = station;
            radio.result_trigger = trigger;
            radio.ready = true;
        } else {
            free(pcm);
            if (source_fd >= 0) close(source_fd);
        }
        pthread_mutex_unlock(&radio.mutex);
    }
    return NULL;
}

bool fv_radio_start(JavaVM *vm, const char *app_directory, const char *language, int station_count) {
    if (!vm || !app_directory || !language || station_count < 1 || station_count > (int)FV_STATION_BLOB_COUNT) {
        return false;
    }
    pthread_mutex_lock(&radio.mutex);
    if (radio.started) {
        pthread_mutex_unlock(&radio.mutex);
        return true;
    }
    const int n = snprintf(radio.directory, sizeof(radio.directory), "%s", app_directory);
    if (n < 0 || (size_t)n >= sizeof(radio.directory)) {
        pthread_mutex_unlock(&radio.mutex);
        return false;
    }
    radio.station_count = (size_t)station_count;
    for (int i = 0; i < station_count; i++) {
        const FvBlobStationEntry *src =
            (const FvBlobStationEntry *)(FV_STATION_PLAN_BINARY_DATA + 16u + (size_t)i * sizeof(FvBlobStationEntry));
        FvDynamicStation *dst = &radio.stations[i];
        *dst = (FvDynamicStation){0};
        copy_cstr(dst->callsign, sizeof(dst->callsign), src->callsign, sizeof(src->callsign));
        copy_cstr(dst->name, sizeof(dst->name), src->name, sizeof(src->name));
        copy_cstr(dst->country, sizeof(dst->country), src->country, sizeof(src->country));
        copy_cstr(dst->lang, sizeof(dst->lang), src->lang, sizeof(src->lang));
        copy_cstr(dst->url, sizeof(dst->url), src->url, sizeof(src->url));
        memcpy(radio.languages[i], dst->lang, 8u);
        radio.url_ptrs[i] = dst->url;
        radio.health[i] = FV_STATION_UNTESTED;
    }
    load_cached_json_locked();
    radio.bank = bank_from_language(language);
    rebuild_group_locked((uint32_t)now_ms());

    radio.vm = vm;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    radio.started = pthread_create(&thread, &attr, worker, NULL) == 0;
    pthread_attr_destroy(&attr);
    const bool started = radio.started;
    LOGI("on-demand radio: bank=%s total=%zu group=%zu",
         fv_station_bank_label(radio.bank), radio.station_count, radio.group_count);
    for (size_t i = 0; i < radio.group_count; i++) {
        LOGI("source %zu: station=%u %s", i, radio.group[i], radio.stations[radio.group[i]].callsign);
    }
    pthread_mutex_unlock(&radio.mutex);
    return started;
}

void fv_radio_set_enabled(bool enabled) {
    pthread_mutex_lock(&radio.mutex);
    if (radio.enabled != enabled) {
        radio.enabled = enabled;
        radio.generation++;
        free(radio.pcm);
        radio.pcm = NULL;
        radio.frames = 0u;
        radio.ready = false;
        if (radio.result_fd >= 0) close(radio.result_fd);
        radio.result_fd = -1;
    }
    pthread_mutex_unlock(&radio.mutex);
}

int fv_radio_selected_station(int channel) {
    pthread_mutex_lock(&radio.mutex);
    const int station = channel >= 0 && radio.group_count
                            ? (int)radio.group[(size_t)(unsigned)channel % radio.group_count]
                            : -1;
    pthread_mutex_unlock(&radio.mutex);
    return station;
}

bool fv_radio_request(int channel, const FvVoxDecimalTrigger *trigger) {
    if (!trigger || channel < 0) return false;
    pthread_mutex_lock(&radio.mutex);
    const bool ok = radio.started && radio.enabled && !radio.busy && !radio.ready && radio.group_count;
    if (ok) {
        radio.station = (int)radio.group[(size_t)(unsigned)channel % radio.group_count];
        radio.trigger = *trigger;
        radio.job_generation = radio.generation;
        radio.queued = radio.busy = true;
        pthread_cond_signal(&radio.changed);
    }
    pthread_mutex_unlock(&radio.mutex);
    return ok;
}

bool fv_radio_take_result(int16_t **pcm, size_t *frames, int *station, FvVoxDecimalTrigger *trigger, int *source_fd) {
    if (!pcm || !frames || !station || !trigger || !source_fd) return false;
    pthread_mutex_lock(&radio.mutex);
    const bool ready = radio.ready;
    if (ready) {
        *pcm = radio.pcm;
        *source_fd = radio.result_fd;
        radio.result_fd = -1;
        *frames = radio.frames;
        *station = radio.result_station;
        *trigger = radio.result_trigger;
        radio.pcm = NULL;
        radio.frames = 0u;
        radio.ready = false;
    }
    pthread_mutex_unlock(&radio.mutex);
    return ready;
}

bool fv_radio_busy(void) {
    pthread_mutex_lock(&radio.mutex);
    const bool busy = radio.busy;
    pthread_mutex_unlock(&radio.mutex);
    return busy;
}

FvRadioBank fv_radio_get_bank(void) {
    pthread_mutex_lock(&radio.mutex);
    const FvRadioBank bank = radio.bank;
    pthread_mutex_unlock(&radio.mutex);
    return bank;
}

void fv_radio_cycle_bank(void) {
    pthread_mutex_lock(&radio.mutex);
    radio.bank = (FvRadioBank)(((unsigned)radio.bank + 1u) % (unsigned)FV_RADIO_BANK_COUNT);
    rebuild_group_locked((uint32_t)now_ms());
    radio.last_tested_slot = -1;
    pthread_mutex_unlock(&radio.mutex);
}

void fv_radio_reshuffle(void) {
    pthread_mutex_lock(&radio.mutex);
    rebuild_group_locked((uint32_t)now_ms() ^ (uint32_t)radio.generation);
    radio.last_tested_slot = -1;
    pthread_mutex_unlock(&radio.mutex);
}

bool fv_radio_refresh_catalog(void) {
    pthread_mutex_lock(&radio.mutex);
    const bool ok = radio.started && !radio.refresh_busy && !radio.refresh_queued;
    if (ok) {
        radio.refresh_queued = radio.refresh_busy = true;
        pthread_cond_signal(&radio.changed);
    }
    pthread_mutex_unlock(&radio.mutex);
    return ok;
}

bool fv_radio_test_slot(int group_slot) {
    pthread_mutex_lock(&radio.mutex);
    const bool ok = radio.started && !radio.test_busy && !radio.test_queued &&
                    group_slot >= 0 && (size_t)group_slot < radio.group_count;
    if (ok) {
        radio.test_slot = group_slot;
        radio.test_station = (int)radio.group[group_slot];
        radio.test_queued = radio.test_busy = true;
        pthread_cond_signal(&radio.changed);
    }
    pthread_mutex_unlock(&radio.mutex);
    return ok;
}

bool fv_radio_take_test_result(int16_t **pcm, size_t *frames, int *station, int *source_fd) {
    if (!pcm || !frames || !station || !source_fd) return false;
    pthread_mutex_lock(&radio.mutex);
    const bool ready = radio.test_ready;
    if (ready) {
        *pcm = radio.test_pcm;
        *frames = radio.test_frames;
        *station = radio.test_result_station;
        *source_fd = radio.test_result_fd;
        radio.test_pcm = NULL;
        radio.test_frames = 0u;
        radio.test_result_fd = -1;
        radio.test_ready = false;
    }
    pthread_mutex_unlock(&radio.mutex);
    return ready;
}

void fv_radio_tuner_snapshot(FvRadioTunerSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&radio.mutex);
    out->bank = radio.bank;
    out->total_stations = radio.station_count;
    out->group_count = radio.group_count;
    out->last_tested_slot = radio.last_tested_slot;
    out->last_refresh_added = radio.last_refresh_added;
    out->refreshing = radio.refresh_busy || radio.refresh_queued;
    out->testing = radio.test_busy || radio.test_queued;
    for (size_t i = 0; i < radio.group_count && i < FV_STATION_GROUP_SIZE; i++) {
        const unsigned idx = radio.group[i];
        FvRadioSlotSnapshot *dst = &out->slots[i];
        if (idx < radio.station_count) {
            const FvDynamicStation *src = &radio.stations[idx];
            dst->station_index = (int)idx;
            snprintf(dst->callsign, sizeof(dst->callsign), "%s", src->callsign);
            snprintf(dst->name, sizeof(dst->name), "%s", src->name);
            snprintf(dst->lang, sizeof(dst->lang), "%s", src->lang);
            dst->health = radio.health[idx];
            dst->active = true;
        }
    }
    pthread_mutex_unlock(&radio.mutex);
}

bool fv_radio_station_info(int station_index, char *out_callsign, size_t callsign_cap,
                           char *out_url, size_t url_cap) {
    if (station_index < 0) return false;
    pthread_mutex_lock(&radio.mutex);
    const bool ok = (size_t)station_index < radio.station_count;
    if (ok) {
        const FvDynamicStation *src = &radio.stations[station_index];
        if (out_callsign && callsign_cap) snprintf(out_callsign, callsign_cap, "%s", src->callsign);
        if (out_url && url_cap) snprintf(out_url, url_cap, "%s", src->url);
    }
    pthread_mutex_unlock(&radio.mutex);
    return ok;
}

static bool live_is_busy(void *ctx) { (void)ctx; return fv_radio_busy(); }
static int live_select(void *ctx, int hop) { (void)ctx; return fv_radio_selected_station(hop); }
static bool live_request(void *ctx, int hop, const FvVoxDecimalTrigger *t) { (void)ctx; return fv_radio_request(hop, t); }
static bool live_take(void *ctx, int16_t **pcm, size_t *frames, int *station, FvVoxDecimalTrigger *t, int *fd) {
    (void)ctx;
    return fv_radio_take_result(pcm, frames, station, t, fd);
}
FvFragmentLiveSource fv_radio_live_source(void) {
    return (FvFragmentLiveSource){
        .is_busy = live_is_busy,
        .select_station = live_select,
        .request = live_request,
        .take_result = live_take
    };
}
