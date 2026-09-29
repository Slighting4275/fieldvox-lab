#include <jni.h>
#include <android/log.h>
#include <android/sensor.h>
#include <android/looper.h>
#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <sstream>
#include <string>
#include <cstdio>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <vector>
#include <algorithm>

#include "fv_core.h"
#include "fv_station_blob_data.h"

#define FV_LOG_TAG "FieldVoxNative"
#define FV_LOGI(...) __android_log_print(ANDROID_LOG_INFO, FV_LOG_TAG, __VA_ARGS__)
#define FV_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, FV_LOG_TAG, __VA_ARGS__)

static constexpr int LOOPER_ID_SENSOR = 7;
static constexpr uint32_t RING_FRAMES = 1u << 18u;
static constexpr uint32_t RING_MASK = RING_FRAMES - 1u;

static std::atomic<bool> g_running{false};
static std::atomic<bool> g_thread_live{false};
static pthread_t g_thread{};
static pthread_mutex_t g_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_thread_joinable = false;

static std::atomic<uint64_t> g_sensor_events{0};
static std::atomic<uint64_t> g_baseline_events{0};
static std::atomic<uint64_t> g_triggers{0};
static std::atomic<uint64_t> g_audio_underruns{0};
static std::atomic<uint64_t> g_ring_drops{0};

static std::atomic<int> g_audio_started{0};
static std::atomic<int> g_sensor_available{0};
static std::atomic<int> g_baseline_ready{0};

static std::atomic<float> g_last_mx{0.0f};
static std::atomic<float> g_last_my{0.0f};
static std::atomic<float> g_last_mz{0.0f};
static std::atomic<float> g_last_mag{0.0f};
static std::atomic<float> g_baseline_mag{0.0f};
static std::atomic<float> g_last_delta{0.0f};
static std::atomic<float> g_last_freq{0.0f};
static std::atomic<float> g_threshold{1.65f};
static std::atomic<uint64_t> g_cooldown_ns{450000000ULL};

static int16_t g_ring[RING_FRAMES];
static std::atomic<uint32_t> g_ring_read{0};
static std::atomic<uint32_t> g_ring_write{0};

static AAudioStream *g_audio_stream = nullptr;
static std::string g_package_name = "org.fieldvox.lab";
static std::string g_files_dir;
static std::string g_session_path;
static FILE *g_session_fp = nullptr;
static pthread_mutex_t g_session_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<int> g_session_open{0};
static std::atomic<uint64_t> g_session_events{0};
static std::atomic<uint64_t> g_session_seq{0};
static std::string g_wav_path;
static FILE *g_wav_fp = nullptr;
static std::atomic<uint64_t> g_wav_data_bytes{0};
static constexpr uint64_t WAV_MAX_DATA_BYTES = 0xffffffffULL - 36ULL;

static aaudio_data_callback_result_t audio_callback(
    AAudioStream *,
    void *,
    void *audio_data,
    int32_t num_frames
) {
    int16_t *out = static_cast<int16_t *>(audio_data);
    uint32_t read_i = g_ring_read.load(std::memory_order_relaxed);
    const uint32_t write_i = g_ring_write.load(std::memory_order_acquire);

    for (int32_t i = 0; i < num_frames; i++) {
        if (read_i != write_i) {
            out[i] = g_ring[read_i & RING_MASK];
            read_i++;
        } else {
            out[i] = 0;
            g_audio_underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

    g_ring_read.store(read_i, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void ring_clear() {
    g_ring_read.store(0, std::memory_order_release);
    g_ring_write.store(0, std::memory_order_release);
    std::memset(g_ring, 0, sizeof(g_ring));
}

static void ring_push(const int16_t *pcm, uint32_t frames) {
    uint32_t write_i = g_ring_write.load(std::memory_order_relaxed);

    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t read_i = g_ring_read.load(std::memory_order_acquire);
        if ((write_i - read_i) >= (RING_FRAMES - 1u)) {
            g_ring_drops.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        g_ring[write_i & RING_MASK] = pcm[i];
        write_i++;
    }

    g_ring_write.store(write_i, std::memory_order_release);
}

static bool start_audio() {
    if (g_audio_stream != nullptr) {
        return true;
    }

    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || builder == nullptr) {
        FV_LOGE("AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(r));
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(builder, FV_SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setDataCallback(builder, audio_callback, nullptr);

    r = AAudioStreamBuilder_openStream(builder, &g_audio_stream);
    AAudioStreamBuilder_delete(builder);

    if (r != AAUDIO_OK || g_audio_stream == nullptr) {
        FV_LOGE("AAudio openStream failed: %s", AAudio_convertResultToText(r));
        g_audio_stream = nullptr;
        g_audio_started.store(0);
        return false;
    }

    r = AAudioStream_requestStart(g_audio_stream);
    if (r != AAUDIO_OK) {
        FV_LOGE("AAudio requestStart failed: %s", AAudio_convertResultToText(r));
        AAudioStream_close(g_audio_stream);
        g_audio_stream = nullptr;
        g_audio_started.store(0);
        return false;
    }

    g_audio_started.store(1);
    return true;
}

static void stop_audio() {
    AAudioStream *s = g_audio_stream;
    g_audio_stream = nullptr;

    if (s != nullptr) {
        AAudioStream_requestStop(s);
        AAudioStream_close(s);
    }

    g_audio_started.store(0);
}

static void push_tone(float freq_hz, float noise_mix, float echo_mix, float distortion, uint32_t frames, FV_Rng *rng) {
    if (frames > FV_SAMPLE_RATE / 2u) {
        frames = FV_SAMPLE_RATE / 2u;
    }

    int16_t pcm[FV_SAMPLE_RATE / 2u];
    fv_make_blob_audio(pcm, frames, freq_hz, noise_mix, echo_mix, distortion, rng);
    ring_push(pcm, frames);
}



static bool wav_write_u16(FILE *fp, uint16_t v) {
    return std::fputc(static_cast<int>(v & 0xffu), fp) != EOF
        && std::fputc(static_cast<int>((v >> 8u) & 0xffu), fp) != EOF;
}

static bool wav_write_u32(FILE *fp, uint32_t v) {
    return std::fputc(static_cast<int>(v & 0xffu), fp) != EOF
        && std::fputc(static_cast<int>((v >> 8u) & 0xffu), fp) != EOF
        && std::fputc(static_cast<int>((v >> 16u) & 0xffu), fp) != EOF
        && std::fputc(static_cast<int>((v >> 24u) & 0xffu), fp) != EOF;
}

static uint32_t wav_cap_u32(uint64_t v) {
    if (v > WAV_MAX_DATA_BYTES) {
        return static_cast<uint32_t>(WAV_MAX_DATA_BYTES);
    }
    return static_cast<uint32_t>(v);
}

static bool wav_write_header(FILE *fp, uint64_t data_bytes_u64) {
    if (fp == nullptr) {
        return false;
    }
    const uint32_t data_bytes = wav_cap_u32(data_bytes_u64);
    const uint32_t riff_size = 36u + data_bytes;
    const uint16_t channels = 1u;
    const uint16_t bits_per_sample = 16u;
    const uint32_t sample_rate = FV_SAMPLE_RATE;
    const uint16_t block_align = static_cast<uint16_t>(channels * bits_per_sample / 8u);
    const uint32_t byte_rate = sample_rate * block_align;

    return std::fwrite("RIFF", 1, 4, fp) == 4
        && wav_write_u32(fp, riff_size)
        && std::fwrite("WAVE", 1, 4, fp) == 4
        && std::fwrite("fmt ", 1, 4, fp) == 4
        && wav_write_u32(fp, 16u)
        && wav_write_u16(fp, 1u)
        && wav_write_u16(fp, channels)
        && wav_write_u32(fp, sample_rate)
        && wav_write_u32(fp, byte_rate)
        && wav_write_u16(fp, block_align)
        && wav_write_u16(fp, bits_per_sample)
        && std::fwrite("data", 1, 4, fp) == 4
        && wav_write_u32(fp, data_bytes);
}

static void session_write_pcm(const int16_t *pcm, uint32_t frames) {
    pthread_mutex_lock(&g_session_mutex);

    if (g_wav_fp != nullptr && pcm != nullptr && frames > 0) {
        const uint64_t current = g_wav_data_bytes.load(std::memory_order_relaxed);
        const uint64_t room_frames = current < WAV_MAX_DATA_BYTES
            ? (WAV_MAX_DATA_BYTES - current) / sizeof(int16_t)
            : 0;
        const size_t requested = static_cast<size_t>(std::min<uint64_t>(frames, room_frames));
        const size_t written = std::fwrite(pcm, sizeof(int16_t), requested, g_wav_fp);
        g_wav_data_bytes.fetch_add(static_cast<uint64_t>(written * sizeof(int16_t)), std::memory_order_relaxed);
        if (written != requested || std::fflush(g_wav_fp) != 0) {
            FV_LOGE("session WAV write failed: %s", std::strerror(errno));
        }
    }

    pthread_mutex_unlock(&g_session_mutex);
}


static bool fv_mkdir_if_missing(const std::string &path) {
    if (path.empty()) {
        return false;
    }

    if (mkdir(path.c_str(), 0700) == 0) {
        return true;
    }

    if (errno != EEXIST) {
        return false;
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void session_open() {
    pthread_mutex_lock(&g_session_mutex);

    if (g_session_fp != nullptr) {
        fclose(g_session_fp);
        g_session_fp = nullptr;
    }

    if (g_wav_fp != nullptr) {
        fclose(g_wav_fp);
        g_wav_fp = nullptr;
    }

    g_session_events.store(0, std::memory_order_relaxed);
    g_wav_data_bytes.store(0, std::memory_order_relaxed);

    const std::string base = g_files_dir.empty() ? std::string("/data/local/tmp") : g_files_dir;
    const std::string dir = base + "/fieldvox-sessions";
    if (!fv_mkdir_if_missing(dir)) {
        g_session_open.store(0, std::memory_order_relaxed);
        FV_LOGE("session directory unavailable: %s", dir.c_str());
        pthread_mutex_unlock(&g_session_mutex);
        return;
    }

    const uint64_t seq = g_session_seq.fetch_add(1, std::memory_order_relaxed) + 1u;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char name[128];
    const int name_length = std::snprintf(
        name,
        sizeof(name),
        "session-%lld-%llu.jsonl",
        static_cast<long long>(now_ms),
        static_cast<unsigned long long>(seq)
    );
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(name)) {
        g_session_open.store(0, std::memory_order_relaxed);
        FV_LOGE("session filename generation failed");
        pthread_mutex_unlock(&g_session_mutex);
        return;
    }

    g_session_path = dir + "/" + name;

    std::string wav_name = name;
    const size_t dot = wav_name.rfind(".jsonl");
    if (dot != std::string::npos) {
        wav_name.replace(dot, 6, ".wav");
    } else {
        wav_name += ".wav";
    }
    g_wav_path = dir + "/" + wav_name;

    g_session_fp = std::fopen(g_session_path.c_str(), "ab");
    g_wav_fp = std::fopen(g_wav_path.c_str(), "wb");
    bool wav_ready = false;
    if (g_wav_fp != nullptr) {
        wav_ready = wav_write_header(g_wav_fp, 0) && std::fflush(g_wav_fp) == 0;
        if (!wav_ready) {
            FV_LOGE("session WAV header write failed: %s", std::strerror(errno));
        }
        if (wav_ready) {
            FV_LOGI("session wav opened: %s", g_wav_path.c_str());
        }
    } else {
        FV_LOGE("session wav open failed: %s", g_wav_path.c_str());
    }

    bool session_ready = false;
    if (g_session_fp != nullptr) {
        const int result = std::fprintf(
            g_session_fp,
            "{\"type\":\"session_start\",\"package\":\"%s\",\"path\":\"%s\",\"sample_rate\":%u}\n",
            g_package_name.c_str(),
            g_session_path.c_str(),
            FV_SAMPLE_RATE
        );
        session_ready = result >= 0 && std::fflush(g_session_fp) == 0;
    }

    if (session_ready && wav_ready) {
        g_session_open.store(1, std::memory_order_relaxed);
        FV_LOGI("session opened: %s", g_session_path.c_str());
    } else {
        g_session_open.store(0, std::memory_order_relaxed);
        FV_LOGE("session pair open failed: json=%d wav=%d", session_ready ? 1 : 0, wav_ready ? 1 : 0);
        if (g_session_fp != nullptr) {
            std::fclose(g_session_fp);
            g_session_fp = nullptr;
        }
        if (g_wav_fp != nullptr) {
            std::fclose(g_wav_fp);
            g_wav_fp = nullptr;
        }
        std::remove(g_session_path.c_str());
        std::remove(g_wav_path.c_str());
    }

    pthread_mutex_unlock(&g_session_mutex);
}

static void session_write_trigger(
    uint64_t trigger_id,
    uint64_t t_ns,
    float mag,
    float delta,
    float freq
) {
    pthread_mutex_lock(&g_session_mutex);

    if (g_session_fp != nullptr) {
        const float threshold = g_threshold.load(std::memory_order_relaxed);
        const unsigned long long cooldown_ms =
            static_cast<unsigned long long>(g_cooldown_ns.load(std::memory_order_relaxed) / 1000000ULL);

        const int result = std::fprintf(
            g_session_fp,
            "{\"type\":\"trigger\",\"trigger\":%llu,\"t_ns\":%llu,\"mag\":%.6f,\"delta\":%.6f,\"freq_hz\":%.6f,\"threshold\":%.6f,\"cooldown_ms\":%llu}\n",
            static_cast<unsigned long long>(trigger_id),
            static_cast<unsigned long long>(t_ns),
            mag,
            delta,
            freq,
            threshold,
            cooldown_ms
        );
        if (result >= 0 && std::fflush(g_session_fp) == 0) {
            g_session_events.fetch_add(1, std::memory_order_relaxed);
        } else {
            FV_LOGE("session trigger write failed: %s", std::strerror(errno));
        }
    }

    pthread_mutex_unlock(&g_session_mutex);
}

static void session_close() {
    pthread_mutex_lock(&g_session_mutex);

    if (g_session_fp != nullptr) {
        const int result = std::fprintf(
            g_session_fp,
            "{\"type\":\"session_stop\",\"events\":%llu}\n",
            static_cast<unsigned long long>(g_session_events.load(std::memory_order_relaxed))
        );
        if (result < 0 || std::fflush(g_session_fp) != 0) {
            FV_LOGE("session final JSON write failed: %s", std::strerror(errno));
        }
        if (std::fclose(g_session_fp) != 0) {
            FV_LOGE("session JSON close failed: %s", std::strerror(errno));
        }
        g_session_fp = nullptr;
        FV_LOGI("session closed: %s events=%llu", g_session_path.c_str(),
                static_cast<unsigned long long>(g_session_events.load(std::memory_order_relaxed)));
    }

    if (g_wav_fp != nullptr) {
        const uint64_t data_bytes = g_wav_data_bytes.load(std::memory_order_relaxed);
        const bool finalized = std::fseek(g_wav_fp, 0, SEEK_SET) == 0
            && wav_write_header(g_wav_fp, data_bytes)
            && std::fflush(g_wav_fp) == 0;
        if (!finalized) {
            FV_LOGE("session WAV finalization failed: %s", std::strerror(errno));
        }
        if (std::fclose(g_wav_fp) != 0) {
            FV_LOGE("session WAV close failed: %s", std::strerror(errno));
        }
        g_wav_fp = nullptr;
        FV_LOGI("session wav closed: %s bytes=%llu", g_wav_path.c_str(),
                static_cast<unsigned long long>(data_bytes));
    }

    g_session_open.store(0, std::memory_order_relaxed);
    pthread_mutex_unlock(&g_session_mutex);
}


static void process_trigger_sound(uint64_t trigger_id, uint64_t t_ns, float mag, float delta, FV_Rng *rng) {
    const uint32_t frames = FV_SAMPLE_RATE / 4u;

    const float freq =
        90.0f + std::fmod((mag * 23.0f) + (delta * 43.0f) + static_cast<float>(trigger_id * 29u), 920.0f);

    const float noise_mix = std::fmin(0.88f, 0.18f + delta * 0.040f);
    const float echo_mix = std::fmin(0.75f, 0.18f + delta * 0.025f);
    const float dist = std::fmin(0.90f, 0.08f + delta * 0.030f);

    g_last_freq.store(freq, std::memory_order_relaxed);
    int16_t pcm[FV_SAMPLE_RATE / 2u];
    fv_make_blob_audio(pcm, frames, freq, noise_mix, echo_mix, dist, rng);
    ring_push(pcm, frames);
    session_write_pcm(pcm, frames);

    FV_LOGI(
        "trigger=%llu t_ns=%llu mag=%.3f delta=%.3f freq=%.3f threshold=%.3f cooldown_ms=%llu",
        static_cast<unsigned long long>(trigger_id),
        static_cast<unsigned long long>(t_ns),
        mag,
        delta,
        freq,
        g_threshold.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_cooldown_ns.load(std::memory_order_relaxed) / 1000000ULL)
    );

    session_write_trigger(trigger_id, t_ns, mag, delta, freq);
}

static void *sensor_thread_main(void *) {
    g_thread_live.store(true);
    FV_LOGI("Stage 1F native thread started");
    session_open();

    FV_Rng rng;
    fv_rng_seed(&rng, 0xA11CEBEEF1234567ULL);

    start_audio();
    push_tone(220.0f, 0.12f, 0.20f, 0.08f, FV_SAMPLE_RATE / 5u, &rng);

    ASensorManager *manager = ASensorManager_getInstanceForPackage(g_package_name.c_str());
    if (manager == nullptr) {
        FV_LOGE("ASensorManager unavailable");
        g_sensor_available.store(0);
        g_running.store(false);
        stop_audio();
        session_close();
        g_thread_live.store(false);
        return nullptr;
    }

    const ASensor *mag_sensor = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_MAGNETIC_FIELD);
    if (mag_sensor == nullptr) {
        FV_LOGE("magnetic field sensor unavailable");
        g_sensor_available.store(0);
        g_running.store(false);
        stop_audio();
        session_close();
        g_thread_live.store(false);
        return nullptr;
    }

    g_sensor_available.store(1);

    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (looper == nullptr) {
        FV_LOGE("ALooper_prepare failed");
        g_running.store(false);
        stop_audio();
        session_close();
        g_thread_live.store(false);
        return nullptr;
    }
    ASensorEventQueue *queue = ASensorManager_createEventQueue(
        manager,
        looper,
        LOOPER_ID_SENSOR,
        nullptr,
        nullptr
    );

    if (queue == nullptr) {
        FV_LOGE("ASensorManager_createEventQueue failed");
        g_running.store(false);
        stop_audio();
        session_close();
        g_thread_live.store(false);
        return nullptr;
    }

    const int enable_result = ASensorEventQueue_enableSensor(queue, mag_sensor);
    const int rate_result = enable_result >= 0
        ? ASensorEventQueue_setEventRate(queue, mag_sensor, 8333)
        : enable_result;
    if (enable_result < 0 || rate_result < 0) {
        FV_LOGE("magnetometer configuration failed: enable=%d rate=%d", enable_result, rate_result);
        ASensorManager_destroyEventQueue(manager, queue);
        g_running.store(false);
        stop_audio();
        session_close();
        g_thread_live.store(false);
        return nullptr;
    }

    FV_TriggerState trig{};

    uint64_t baseline_count = 0;
    double baseline_sum = 0.0;
    bool ready = false;

    while (g_running.load(std::memory_order_acquire)) {
        const int ident = ALooper_pollOnce(100, nullptr, nullptr, nullptr);

        if (ident != LOOPER_ID_SENSOR) {
            continue;
        }

        ASensorEvent event;
        while (ASensorEventQueue_getEvents(queue, &event, 1) > 0) {
            if (event.type != ASENSOR_TYPE_MAGNETIC_FIELD) {
                continue;
            }

            FV_SensorSample sample{};

            sample.t_ns = static_cast<uint64_t>(event.timestamp);
            sample.mx = event.magnetic.x;
            sample.my = event.magnetic.y;
            sample.mz = event.magnetic.z;

            const float mag = fv_mag3(sample.mx, sample.my, sample.mz);

            g_sensor_events.fetch_add(1, std::memory_order_relaxed);
            g_last_mx.store(sample.mx, std::memory_order_relaxed);
            g_last_my.store(sample.my, std::memory_order_relaxed);
            g_last_mz.store(sample.mz, std::memory_order_relaxed);
            g_last_mag.store(mag, std::memory_order_relaxed);

            if (!ready) {
                baseline_sum += static_cast<double>(mag);
                baseline_count++;
                g_baseline_events.store(baseline_count, std::memory_order_relaxed);

                if (baseline_count >= 128) {
                    const float baseline = static_cast<float>(baseline_sum / static_cast<double>(baseline_count));
                    g_baseline_mag.store(baseline, std::memory_order_relaxed);

                    fv_trigger_init(
                        &trig,
                        baseline,
                        g_threshold.load(std::memory_order_relaxed),
                        g_cooldown_ns.load(std::memory_order_relaxed)
                    );

                    ready = true;
                    g_baseline_ready.store(1);
                    push_tone(330.0f, 0.10f, 0.15f, 0.05f, FV_SAMPLE_RATE / 7u, &rng);
                    FV_LOGI("baseline ready: %.3f uT", baseline);
                }

                continue;
            }

            trig.trigger_threshold = g_threshold.load(std::memory_order_relaxed);
            trig.cooldown_ns = g_cooldown_ns.load(std::memory_order_relaxed);

            float out_mag = 0.0f;
            float out_delta = 0.0f;

            if (fv_trigger_update(&trig, sample, &out_mag, &out_delta)) {
                const uint64_t trigger_id = g_triggers.fetch_add(1, std::memory_order_relaxed) + 1u;
                g_last_delta.store(out_delta, std::memory_order_relaxed);
                process_trigger_sound(trigger_id, sample.t_ns, out_mag, out_delta, &rng);
            } else {
                g_last_delta.store(out_delta, std::memory_order_relaxed);
            }
        }
    }

    ASensorEventQueue_disableSensor(queue, mag_sensor);
    ASensorManager_destroyEventQueue(manager, queue);
    stop_audio();
    session_close();

    g_thread_live.store(false);
    FV_LOGI("Stage 1F native thread stopped");
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeInit(JNIEnv *env, jobject, jstring package_name, jstring files_dir) {
    const char *pkg = package_name == nullptr
        ? nullptr
        : env->GetStringUTFChars(package_name, nullptr);
    if (pkg != nullptr) {
        g_package_name = pkg;
        env->ReleaseStringUTFChars(package_name, pkg);
    }

    const char *dir = files_dir == nullptr
        ? nullptr
        : env->GetStringUTFChars(files_dir, nullptr);
    if (dir != nullptr) {
        g_files_dir = dir;
        env->ReleaseStringUTFChars(files_dir, dir);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStart(JNIEnv *, jobject) {
    pthread_mutex_lock(&g_thread_mutex);
    if (g_running.load(std::memory_order_acquire)) {
        pthread_mutex_unlock(&g_thread_mutex);
        return JNI_TRUE;
    }

    if (g_thread_joinable) {
        pthread_join(g_thread, nullptr);
        g_thread_joinable = false;
    }

    ring_clear();

    g_sensor_events.store(0);
    g_baseline_events.store(0);
    g_triggers.store(0);
    g_audio_underruns.store(0);
    g_ring_drops.store(0);
    g_audio_started.store(0);
    g_sensor_available.store(0);
    g_baseline_ready.store(0);
    g_last_delta.store(0.0f);
    g_last_freq.store(0.0f);
    g_baseline_mag.store(0.0f);

    g_running.store(true, std::memory_order_release);

    const int rc = pthread_create(&g_thread, nullptr, sensor_thread_main, nullptr);
    if (rc != 0) {
        g_running.store(false);
        FV_LOGE("pthread_create failed: %d", rc);
        pthread_mutex_unlock(&g_thread_mutex);
        return JNI_FALSE;
    }

    g_thread_joinable = true;
    pthread_mutex_unlock(&g_thread_mutex);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStop(JNIEnv *, jobject) {
    pthread_mutex_lock(&g_thread_mutex);
    g_running.store(false, std::memory_order_release);
    if (g_thread_joinable) {
        pthread_join(g_thread, nullptr);
        g_thread_joinable = false;
    }
    pthread_mutex_unlock(&g_thread_mutex);
}

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeSetThreshold(JNIEnv *, jobject, jfloat threshold) {
    float v = static_cast<float>(threshold);
    if (!std::isfinite(v)) {
        v = 1.65f;
    }
    if (v < 0.10f) {
        v = 0.10f;
    }
    if (v > 50.0f) {
        v = 50.0f;
    }
    g_threshold.store(v, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeSetCooldownMs(JNIEnv *, jobject, jint cooldown_ms) {
    int v = static_cast<int>(cooldown_ms);
    if (v < 50) {
        v = 50;
    }
    if (v > 10000) {
        v = 10000;
    }
    g_cooldown_ns.store(static_cast<uint64_t>(v) * 1000000ULL, std::memory_order_relaxed);
}


static bool has_suffix(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) {
        return false;
    }
    return s.compare(s.size() - n, n, suffix) == 0;
}

static std::string human_bytes(uint64_t bytes) {
    char buf[64];

    if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / 1048576.0);
    } else if (bytes >= 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }

    return std::string(buf);
}

static std::string build_session_list() {
    const std::string base = g_files_dir.empty() ? std::string("/data/local/tmp") : g_files_dir;
    const std::string dir = base + "/fieldvox-sessions";

    DIR *dp = opendir(dir.c_str());
    if (dp == nullptr) {
        return std::string("Sessions:\n(no session directory yet)\npath: ") + dir + "\n";
    }

    struct Item {
        std::string name;
        uint64_t size = 0;
        time_t mtime = 0;
    };

    std::vector<Item> items;

    while (true) {
        errno = 0;
        const struct dirent *ent = readdir(dp);
        if (ent == nullptr) {
            break;
        }

        const std::string name = ent->d_name;

        if (name == "." || name == "..") {
            continue;
        }

        if (!has_suffix(name, ".jsonl") && !has_suffix(name, ".wav")) {
            continue;
        }

        const std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            continue;
        }

        Item item{name, static_cast<uint64_t>(st.st_size), st.st_mtime};
        items.push_back(item);
    }

    closedir(dp);

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.mtime != b.mtime) {
            return a.mtime > b.mtime;
        }
        return a.name > b.name;
    });

    std::ostringstream ss;
    ss << "Sessions:\n";
    ss << "path: " << dir << "\n";
    ss << "count: " << items.size() << "\n\n";

    if (items.empty()) {
        ss << "(no .jsonl or .wav sessions yet)\n";
        return ss.str();
    }

    size_t shown = 0;
    for (const Item &item : items) {
        ss << item.name << "\n";
        ss << "  size: " << human_bytes(item.size) << "\n";
        ss << "  modified_unix: " << static_cast<long long>(item.mtime) << "\n";
        shown++;

        if (shown >= 24) {
            ss << "\n(showing newest 24 files)\n";
            break;
        }
    }

    return ss.str();
}



extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeSessionList(JNIEnv *env, jobject) {
    const std::string out = build_session_list();
    return env->NewStringUTF(out.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStatus(JNIEnv *env, jobject) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(3);

    const uint32_t read_i = g_ring_read.load(std::memory_order_relaxed);
    const uint32_t write_i = g_ring_write.load(std::memory_order_relaxed);
    const uint32_t queued = write_i - read_i;

    std::string session_path_copy;
    std::string wav_path_copy;
    pthread_mutex_lock(&g_session_mutex);
    session_path_copy = g_session_path;
    wav_path_copy = g_wav_path;
    pthread_mutex_unlock(&g_session_mutex);

    ss
        << "Stage 1F session browser native sensor/audio prototype\n"
        << "status: " << (g_running.load() ? "running" : "stopped") << "\n"
        << "native_thread: " << (g_thread_live.load() ? "live" : "off") << "\n"
        << "AAudio: " << (g_audio_started.load() ? "started" : "off") << "\n"
        << "magnetometer: " << (g_sensor_available.load() ? "available" : "unknown/unavailable") << "\n"
        << "baseline_ready: " << (g_baseline_ready.load() ? "yes" : "no") << "\n\n"

        << "sensor_events: " << g_sensor_events.load() << "\n"
        << "baseline_events: " << g_baseline_events.load() << " / 128\n"
        << "triggers: " << g_triggers.load() << "\n"
        << "session_open: " << (g_session_open.load() ? "yes" : "no") << "\n"
        << "session_events: " << g_session_events.load() << "\n"
        << "session_path: " << session_path_copy << "\n"
        << "session_wav_path: " << wav_path_copy << "\n"
        << "session_audio_bytes: " << g_wav_data_bytes.load() << "\n"
        << "ring_queued_frames: " << queued << "\n"
        << "audio_underruns: " << g_audio_underruns.load() << "\n"
        << "ring_drops: " << g_ring_drops.load() << "\n\n"

        << "mx: " << g_last_mx.load() << " uT\n"
        << "my: " << g_last_my.load() << " uT\n"
        << "mz: " << g_last_mz.load() << " uT\n"
        << "mag: " << g_last_mag.load() << " uT\n"
        << "baseline: " << g_baseline_mag.load() << " uT\n"
        << "ema_delta: " << g_last_delta.load() << "\n"
        << "threshold: " << g_threshold.load() << "\n"
        << "cooldown_ms: " << (g_cooldown_ns.load() / 1000000ULL) << "\n"
        << "last_freq: " << g_last_freq.load() << " Hz\n\n"

        << "Move the phone near/away from metal, magnets, speakers, laptops, or chargers to test sensor triggers.\n"
        << "Experimental only; does not prove paranormal activity or spirit communication.\n";

    const std::string out = ss.str();
    return env->NewStringUTF(out.c_str());
}

static std::atomic<float> g_telemetry_mag_baseline{-1.0f};
static std::atomic<float> g_telemetry_gyro_baseline{-1.0f};
static std::atomic<float> g_telemetry_mag_cal_offset{0.0f};
static float s_accel_gx = 0.0f;
static float s_accel_gy = 0.0f;
static float s_accel_gz = 9.8f;

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeResetSensorTelemetry(JNIEnv *, jobject) {
    g_telemetry_mag_baseline.store(-1.0f, std::memory_order_relaxed);
    g_telemetry_gyro_baseline.store(-1.0f, std::memory_order_relaxed);
    g_telemetry_mag_cal_offset.store(0.0f, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeCalibrateSensors(JNIEnv *, jobject) {
    float cur_baseline = g_telemetry_mag_baseline.load(std::memory_order_relaxed);
    if (cur_baseline > 0.0f) {
        g_telemetry_mag_cal_offset.store(cur_baseline, std::memory_order_relaxed);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeUpdateSensorTelemetry(
        JNIEnv *env,
        jobject,
        jint sensor_type,
        jfloat x,
        jfloat y,
        jfloat z,
        jfloat threshold_val,
        jfloatArray j_out) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return JNI_FALSE;
    }

    const float mag = fv_mag3(x, y, z);
    if (!std::isfinite(mag)) {
        return JNI_FALSE;
    }

    float delta = 0.0f;
    float baseline = 0.0f;
    bool is_spike = false;
    int active_segs = 1;
    int status_level = 0; // 0=BASELINE, 1=FLUCTUATING, 2=ELEVATED, 3=SPIKE DETECTED

    if (sensor_type == 1) { // TYPE_ACCELEROMETER (Geophone / Seismograph Vibration)
        s_accel_gx = s_accel_gx * 0.92f + x * 0.08f;
        s_accel_gy = s_accel_gy * 0.92f + y * 0.08f;
        s_accel_gz = s_accel_gz * 0.92f + z * 0.08f;
        float vx = x - s_accel_gx;
        float vy = y - s_accel_gy;
        float vz = z - s_accel_gz;
        float vibe = std::sqrt(vx * vx + vy * vy + vz * vz);
        delta = vibe;
        baseline = 0.0f;
        is_spike = vibe >= 0.45f; // Table knock / seismic vibration threshold
        active_segs = std::min(10, std::max(1, static_cast<int>(vibe * 12.0f)));
        status_level = is_spike ? 3 : (vibe > 0.25f ? 2 : (vibe > 0.10f ? 1 : 0));
    } else if (sensor_type == 2) { // TYPE_MAGNETIC_FIELD
        float cal = g_telemetry_mag_cal_offset.load(std::memory_order_relaxed);
        float net_mag = std::max(0.0f, mag - cal);

        float prev_baseline = g_telemetry_mag_baseline.load(std::memory_order_relaxed);
        if (prev_baseline < 0.0f) {
            g_telemetry_mag_baseline.store(net_mag, std::memory_order_relaxed);
            baseline = net_mag;
            delta = 0.0f;
        } else {
            baseline = prev_baseline * 0.985f + net_mag * 0.015f;
            g_telemetry_mag_baseline.store(baseline, std::memory_order_relaxed);
            delta = std::fabs(net_mag - baseline);
        }

        const float spike_threshold = std::max(2.10f, threshold_val * 0.90f);
        is_spike = delta >= spike_threshold;

        // Calculate 10-segment LED count:
        active_segs = static_cast<int>(std::min(10.0f, std::max(1.0f, (delta / 2.5f) * 10.0f)));

        if (is_spike) {
            status_level = 3;
        } else if (delta >= 1.5f) {
            status_level = 2;
        } else if (delta >= 0.5f) {
            status_level = 1;
        } else {
            status_level = 0;
        }
    } else if (sensor_type == 4) { // TYPE_GYROSCOPE
        float prev_baseline = g_telemetry_gyro_baseline.load(std::memory_order_relaxed);
        if (prev_baseline < 0.0f) {
            g_telemetry_gyro_baseline.store(mag, std::memory_order_relaxed);
            baseline = mag;
            delta = 0.0f;
        } else {
            baseline = prev_baseline * 0.965f + mag * 0.035f;
            g_telemetry_gyro_baseline.store(baseline, std::memory_order_relaxed);
            delta = std::fabs(mag - baseline);
        }
        is_spike = delta >= 0.18f;
    }

    if (j_out != nullptr) {
        jsize len = env->GetArrayLength(j_out);
        jfloat out_buf[7];
        out_buf[0] = mag;
        out_buf[1] = delta;
        out_buf[2] = baseline;
        out_buf[3] = is_spike ? 1.0f : 0.0f;
        out_buf[4] = static_cast<float>(active_segs);
        out_buf[5] = static_cast<float>(status_level);
        out_buf[6] = g_telemetry_mag_cal_offset.load(std::memory_order_relaxed);
        env->SetFloatArrayRegion(j_out, 0, std::min((jsize)7, len), out_buf);
    }

    return is_spike ? JNI_TRUE : JNI_FALSE;
}

// -------------------------------------------------------------
// NATIVE C SESSION MANAGEMENT & UTILITIES
// -------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeIsSafeSessionStem(JNIEnv *env, jclass, jstring j_stem) {
    if (j_stem == nullptr) return JNI_FALSE;
    const char *stem = env->GetStringUTFChars(j_stem, nullptr);
    if (!stem) return JNI_FALSE;
    size_t len = std::strlen(stem);
    if (len == 0 || len > 160) {
        env->ReleaseStringUTFChars(j_stem, stem);
        return JNI_FALSE;
    }
    if (std::strncmp(stem, "session-", 8) != 0) {
        env->ReleaseStringUTFChars(j_stem, stem);
        return JNI_FALSE;
    }
    if (std::strstr(stem, "..") != nullptr || std::strchr(stem, '/') != nullptr || std::strchr(stem, '\\') != nullptr) {
        env->ReleaseStringUTFChars(j_stem, stem);
        return JNI_FALSE;
    }
    for (size_t i = 0; i < len; i++) {
        char c = stem[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            env->ReleaseStringUTFChars(j_stem, stem);
            return JNI_FALSE;
        }
    }
    env->ReleaseStringUTFChars(j_stem, stem);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeCountTriggerEvents(JNIEnv *env, jclass, jstring j_path) {
    if (j_path == nullptr) return -1;
    const char *path = env->GetStringUTFChars(j_path, nullptr);
    if (!path) return -1;
    FILE *fp = std::fopen(path, "rb");
    env->ReleaseStringUTFChars(j_path, path);
    if (!fp) return -1;

    char line_buf[4096];
    int count = 0;
    while (std::fgets(line_buf, sizeof(line_buf), fp)) {
        if (std::strstr(line_buf, "\"event\":\"trigger\"") != nullptr ||
            std::strstr(line_buf, "\"type\":\"trigger\"") != nullptr) {
            count++;
        }
    }
    std::fclose(fp);
    return count;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeRewriteWavHeader(JNIEnv *env, jclass, jstring j_path, jlong pcm_bytes) {
    if (j_path == nullptr) return JNI_FALSE;
    const char *path = env->GetStringUTFChars(j_path, nullptr);
    if (!path) return JNI_FALSE;
    FILE *fp = std::fopen(path, "r+b");
    if (!fp) {
        fp = std::fopen(path, "wb");
    }
    env->ReleaseStringUTFChars(j_path, path);
    if (!fp) return JNI_FALSE;

    std::fseek(fp, 0, SEEK_SET);
    bool ok = wav_write_header(fp, static_cast<uint64_t>(pcm_bytes > 0 ? pcm_bytes : 0));
    std::fflush(fp);
    std::fclose(fp);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeBuildManifestTsv(JNIEnv *env, jobject, jstring j_dir) {
    if (j_dir == nullptr) return env->NewStringUTF("");
    const char *dir_path = env->GetStringUTFChars(j_dir, nullptr);
    if (!dir_path) return env->NewStringUTF("");

    DIR *dp = opendir(dir_path);
    if (dp == nullptr) {
        env->ReleaseStringUTFChars(j_dir, dir_path);
        return env->NewStringUTF("");
    }

    struct Row {
        std::string stem;
        std::string jsonl_name;
        std::string wav_name;
        uint64_t jsonl_bytes;
        uint64_t wav_bytes;
        int trigger_count;
        double duration_seconds;
        long long jsonl_mtime_ms;
        long long wav_mtime_ms;
    };
    std::vector<Row> rows;

    struct dirent *ent = nullptr;
    while ((ent = readdir(dp)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind("session-", 0) != 0 || name.size() < 15 || name.compare(name.size() - 6, 6, ".jsonl") != 0) {
            continue;
        }

        std::string stem = name.substr(0, name.size() - 6);
        std::string jsonl_path = std::string(dir_path) + "/" + name;
        std::string wav_name = stem + ".wav";
        std::string wav_path = std::string(dir_path) + "/" + wav_name;

        struct stat st_jsonl{};
        struct stat st_wav{};
        if (stat(jsonl_path.c_str(), &st_jsonl) != 0 || stat(wav_path.c_str(), &st_wav) != 0) {
            continue;
        }

        FILE *fp = std::fopen(jsonl_path.c_str(), "rb");
        int triggers = 0;
        if (fp) {
            char line[2048];
            while (std::fgets(line, sizeof(line), fp)) {
                if (std::strstr(line, "\"event\":\"trigger\"") || std::strstr(line, "\"type\":\"trigger\"")) {
                    triggers++;
                }
            }
            std::fclose(fp);
        }

        uint64_t wav_bytes = static_cast<uint64_t>(st_wav.st_size);
        double duration = 0.0;
        if (wav_bytes > 44) {
            duration = static_cast<double>(wav_bytes - 44ULL) / 96000.0;
        }

        Row r;
        r.stem = stem;
        r.jsonl_name = name;
        r.wav_name = wav_name;
        r.jsonl_bytes = static_cast<uint64_t>(st_jsonl.st_size);
        r.wav_bytes = wav_bytes;
        r.trigger_count = triggers;
        r.duration_seconds = duration;
        r.jsonl_mtime_ms = static_cast<long long>(st_jsonl.st_mtime) * 1000LL;
        r.wav_mtime_ms = static_cast<long long>(st_wav.st_mtime) * 1000LL;
        rows.push_back(r);
    }
    closedir(dp);
    env->ReleaseStringUTFChars(j_dir, dir_path);

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.stem < b.stem;
    });

    std::ostringstream ss;
    ss << "session\tjsonl_file\twav_file\tjsonl_bytes\twav_bytes\ttrigger_count\twav_duration_seconds\tjsonl_modified_ms\twav_modified_ms\n";
    ss.setf(std::ios::fixed);
    ss.precision(3);
    for (const auto &r : rows) {
        ss << r.stem << "\t"
           << r.jsonl_name << "\t"
           << r.wav_name << "\t"
           << r.jsonl_bytes << "\t"
           << r.wav_bytes << "\t"
           << r.trigger_count << "\t"
           << r.duration_seconds << "\t"
           << r.jsonl_mtime_ms << "\t"
           << r.wav_mtime_ms << "\n";
    }

    std::string result = ss.str();
    return env->NewStringUTF(result.c_str());
}

// -------------------------------------------------------------
// NATIVE SHA-256 & STRING SANITIZATION FOR RADIO BROWSER DISCOVERY
// -------------------------------------------------------------

struct FvSha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
};

static inline uint32_t fv_sha256_rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void fv_sha256_transform(uint32_t state[8], const uint8_t data[64]) {
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
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4+1] << 16) |
               ((uint32_t)data[i*4+2] << 8)  | ((uint32_t)data[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = fv_sha256_rotr(W[i-15], 7) ^ fv_sha256_rotr(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = fv_sha256_rotr(W[i-2], 17) ^ fv_sha256_rotr(W[i-2], 19) ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = fv_sha256_rotr(e, 6) ^ fv_sha256_rotr(e, 11) ^ fv_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = fv_sha256_rotr(a, 2) ^ fv_sha256_rotr(a, 13) ^ fv_sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void fv_sha256_init(FvSha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void fv_sha256_update(FvSha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t i = 0;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t part = 64 - idx;
        if (len < part) part = len;
        std::memcpy(&ctx->buffer[idx], &data[i], part);
        idx = (idx + part) & 63;
        i += part;
        len -= part;
        if (idx == 0) fv_sha256_transform(ctx->state, ctx->buffer);
    }
}

static void fv_sha256_final(FvSha256Ctx *ctx, uint8_t hash[32]) {
    uint8_t pad[64] = { 0x80 };
    uint64_t bits = ctx->count * 8ULL;
    size_t pad_len = (ctx->count & 63) < 56 ? (56 - (ctx->count & 63)) : (120 - (ctx->count & 63));
    fv_sha256_update(ctx, pad, pad_len);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[i] = (uint8_t)((bits >> ((7 - i) * 8)) & 0xff);
    fv_sha256_update(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (uint8_t)((ctx->state[i] >> 24) & 0xff);
        hash[i*4+1] = (uint8_t)((ctx->state[i] >> 16) & 0xff);
        hash[i*4+2] = (uint8_t)((ctx->state[i] >> 8) & 0xff);
        hash[i*4+3] = (uint8_t)(ctx->state[i] & 0xff);
    }
}

static std::string fv_sha256_hex_str(const std::string &input) {
    FvSha256Ctx ctx;
    fv_sha256_init(&ctx);
    fv_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    uint8_t hash[32];
    fv_sha256_final(&ctx, hash);
    char buf[65];
    for (int i = 0; i < 32; i++) {
        std::snprintf(&buf[i*2], 3, "%02x", hash[i]);
    }
    buf[64] = '\0';
    return std::string(buf);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_RadioBrowserDiscoveryCache_nativeSha256Hex(JNIEnv *env, jclass, jstring j_text) {
    if (j_text == nullptr) return env->NewStringUTF("");
    const char *text = env->GetStringUTFChars(j_text, nullptr);
    if (!text) return env->NewStringUTF("");
    std::string s(text);
    env->ReleaseStringUTFChars(j_text, text);
    std::string hex = fv_sha256_hex_str(s);
    return env->NewStringUTF(hex.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_RadioBrowserDiscoveryCache_nativeCleanText(JNIEnv *env, jclass, jstring j_text) {
    if (j_text == nullptr) return env->NewStringUTF("");
    const char *text = env->GetStringUTFChars(j_text, nullptr);
    if (!text) return env->NewStringUTF("");
    std::string s(text);
    env->ReleaseStringUTFChars(j_text, text);

    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 32 || c == 127) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }

    std::string res;
    bool in_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!in_space && !res.empty()) {
                res += ' ';
                in_space = true;
            }
        } else {
            res += c;
            in_space = false;
        }
    }
    while (!res.empty() && res.back() == ' ') res.pop_back();

    std::string defanged;
    defanged.reserve(res.size());
    for (size_t i = 0; i < res.size(); ) {
        if (res.compare(i, 8, "https://") == 0) {
            defanged += "hxxps://";
            i += 8;
        } else if (res.compare(i, 7, "http://") == 0) {
            defanged += "hxxp://";
            i += 7;
        } else {
            defanged += res[i++];
        }
    }

    if (defanged.size() > 240) {
        defanged = defanged.substr(0, 240);
    }
    return env->NewStringUTF(defanged.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_RadioBrowserDiscoveryCache_nativeMinimumAcceptableCatalogCount(JNIEnv *, jclass, jint target) {
    if (target >= 1000) return 500;
    return std::max(16, target / 2);
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeSha256Hex(JNIEnv *env, jclass clazz, jstring j_text) {
    return Java_org_fieldvox_lab_RadioBrowserDiscoveryCache_nativeSha256Hex(env, clazz, j_text);
}

extern "C" JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativeExportStationSlots(JNIEnv *env, jobject, jstring j_dirPath) {
    if (j_dirPath == nullptr) return -1;
    const char *dir_path = env->GetStringUTFChars(j_dirPath, nullptr);
    if (!dir_path) return -1;

    std::string base_dir(dir_path);
    env->ReleaseStringUTFChars(j_dirPath, dir_path);

    mkdir(base_dir.c_str(), 0755);

    const uint8_t *entries = FV_STATION_PLAN_BINARY_DATA + 16;
    uint32_t count = FV_STATION_BLOB_COUNT;

    // 1. Write source_slots.json
    std::string slots_file = base_dir + "/source_slots.json";
    FILE *f_slots = fopen(slots_file.c_str(), "w");
    if (f_slots) {
        std::ostringstream ss;
        ss << "{\n  \"event_type\": \"source_slots\",\n  \"stage\": \"1Q-B\",\n";
        ss << "  \"catalog_provider\": \"native_binary_blob_zig_v1\",\n";
        ss << "  \"radio_browser_compatible\": true,\n";
        ss << "  \"source_pool_policy\": \"native_binary_blob_ring_64_stations\",\n";
        ss << "  \"catalog_count\": " << count << ",\n";
        ss << "  \"slot_count\": " << count << ",\n";
        ss << "  \"session_pool_target_count\": " << count << ",\n";
        ss << "  \"server_selected\": \"native_zig_packed_binary_blob\",\n";
        ss << "  \"raw_stream_urls_exported\": false,\n";
        ss << "  \"playback_engine_changed\": false,\n";
        ss << "  \"slots\": [\n";

        for (uint32_t i = 0; i < count; i++) {
            const FvBlobStationEntry *e = (const FvBlobStationEntry *)(entries + (i * sizeof(FvBlobStationEntry)));
            std::string url(e->url);
            std::string url_hash = fv_sha256_hex_str(url);

            ss << "    {\n";
            ss << "      \"slot_index\": " << i << ",\n";
            ss << "      \"slot_id\": \"native_blob_slot_" << i << "\",\n";
            ss << "      \"source_label\": \"" << e->name << "\",\n";
            ss << "      \"callsign\": \"" << e->callsign << "\",\n";
            ss << "      \"countrycode\": \"" << e->country << "\",\n";
            ss << "      \"stream_url_sha256\": \"" << url_hash << "\",\n";
            ss << "      \"raw_stream_url_exported\": false\n";
            ss << "    }" << (i + 1 < count ? ",\n" : "\n");
        }
        ss << "  ]\n}\n";
        std::string s = ss.str();
        fwrite(s.data(), 1, s.size(), f_slots);
        fclose(f_slots);
    }

    // 2. Write radio_browser_catalog.json
    std::string cat_file = base_dir + "/radio_browser_catalog.json";
    FILE *f_cat = fopen(cat_file.c_str(), "w");
    if (f_cat) {
        std::ostringstream ss;
        ss << "{\n  \"event_type\": \"radio_browser_catalog\",\n  \"stage\": \"1Q-B\",\n";
        ss << "  \"catalog_provider\": \"native_binary_blob_zig_v1\",\n";
        ss << "  \"catalog_count\": " << count << ",\n";
        ss << "  \"stations\": [\n";

        for (uint32_t i = 0; i < count; i++) {
            const FvBlobStationEntry *e = (const FvBlobStationEntry *)(entries + (i * sizeof(FvBlobStationEntry)));
            std::string url(e->url);
            std::string url_hash = fv_sha256_hex_str(url);

            ss << "    {\n";
            ss << "      \"catalog_index\": " << i << ",\n";
            ss << "      \"source_label\": \"" << e->name << "\",\n";
            ss << "      \"callsign\": \"" << e->callsign << "\",\n";
            ss << "      \"countrycode\": \"" << e->country << "\",\n";
            ss << "      \"stream_url_sha256\": \"" << url_hash << "\"\n";
            ss << "    }" << (i + 1 < count ? ",\n" : "\n");
        }
        ss << "  ]\n}\n";
        std::string s = ss.str();
        fwrite(s.data(), 1, s.size(), f_cat);
        fclose(f_cat);
    }

    // 3. Write radio_browser_discovery_manifest.json
    std::string mf_file = base_dir + "/radio_browser_discovery_manifest.json";
    FILE *f_mf = fopen(mf_file.c_str(), "w");
    if (f_mf) {
        std::ostringstream ss;
        ss << "{\n  \"event_type\": \"radio_browser_discovery_manifest\",\n";
        ss << "  \"stage\": \"1Q-B\",\n";
        ss << "  \"catalog_provider\": \"native_binary_blob_zig_v1\",\n";
        ss << "  \"server_selected\": \"native_zig_packed_binary_blob\",\n";
        ss << "  \"vetted_catalog_count\": " << count << ",\n";
        ss << "  \"session_pool_count\": " << count << ",\n";
        ss << "  \"raw_stream_urls_exported\": false,\n";
        ss << "  \"catalog_file\": \"radio_browser_catalog.json\",\n";
        ss << "  \"source_slots_file\": \"source_slots.json\"\n}\n";
        std::string s = ss.str();
        fwrite(s.data(), 1, s.size(), f_mf);
        fclose(f_mf);
    }

    // 4. Write radio_browser_station_cache_private.jsonl
    std::string priv_file = base_dir + "/radio_browser_station_cache_private.jsonl";
    FILE *f_priv = fopen(priv_file.c_str(), "w");
    if (f_priv) {
        for (uint32_t i = 0; i < count; i++) {
            const FvBlobStationEntry *e = (const FvBlobStationEntry *)(entries + (i * sizeof(FvBlobStationEntry)));
            std::ostringstream ss;
            ss << "{\"event_type\":\"radio_browser_private_station\",\"catalog_index\":" << i;
            ss << ",\"source_label\":\"" << e->name << "\",\"callsign\":\"" << e->callsign << "\"";
            ss << ",\"countrycode\":\"" << e->country << "\",\"url\":\"" << e->url << "\"}\n";
            std::string s = ss.str();
            fwrite(s.data(), 1, s.size(), f_priv);
        }
        fclose(f_priv);
    }

    return (jint)count;
}
