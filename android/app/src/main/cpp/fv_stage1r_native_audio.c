#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "fv_native_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef FIELDVOX_ENABLE_LEGACY_JNI
#ifndef JNIEXPORT
#define JNIEXPORT
#endif
#ifndef JNICALL
#define JNICALL
#endif
#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/log.h>

#define FV_TAG "FieldVoxStage1R"

// FIELDVOX_STAGE1R_NATIVE_AUDIO_WINDOW
// Static 64-sample signed PCM waveform blob.
// This is not TTS, not a word bank, and not a stream decoder.
static const int16_t FV_STAGE1R_SINE_BLOB_64[64] = {
        0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
        23170, 25330, 27246, 28898, 30273, 31356, 32137, 32609,
        32767, 32609, 32137, 31356, 30273, 28898, 27246, 25330,
        23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
        0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
        -23170, -25330, -27246, -28898, -30273, -31356, -32137, -32609,
        -32767, -32609, -32137, -31356, -30273, -28898, -27246, -25330,
        -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212
};

JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativePlayStage1RShortBurst(
        JNIEnv *env,
        jobject thiz,
        jint duration_ms,
        jint frequency_hz,
        jfloat amplitude) {
    (void) env;
    (void) thiz;

    if (duration_ms < 80) duration_ms = 80;
    if (duration_ms > 1000) duration_ms = 1000;
    if (frequency_hz < 220) frequency_hz = 220;
    if (frequency_hz > 1760) frequency_hz = 1760;
    if (amplitude < 0.02f) amplitude = 0.02f;
    if (amplitude > 0.35f) amplitude = 0.35f;

    AAudioStreamBuilder *builder = 0;
    AAudioStream *stream = 0;

    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || builder == 0) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "AAudio_createStreamBuilder failed: %d", (int) r);
        return -1;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);

    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    builder = 0;

    if (r != AAUDIO_OK || stream == 0) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "AAudioStreamBuilder_openStream failed: %d", (int) r);
        return -2;
    }

    int32_t sample_rate = AAudioStream_getSampleRate(stream);
    if (sample_rate <= 0) sample_rate = 48000;

    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "AAudioStream_requestStart failed: %d", (int) r);
        AAudioStream_close(stream);
        return -3;
    }

    const int32_t total_frames = (int32_t) (((int64_t) sample_rate * (int64_t) duration_ms) / 1000);
    const int32_t chunk_frames = 192;
    int16_t buffer[192];

    uint32_t phase_q16 = 0;
    uint32_t step_q16 = (uint32_t) ((((uint64_t) frequency_hz) * 64ULL * 65536ULL) / (uint64_t) sample_rate);
    int32_t gain_q15 = (int32_t) (amplitude * 32767.0f);

    int32_t frames_done = 0;
    while (frames_done < total_frames) {
        int32_t want = total_frames - frames_done;
        if (want > chunk_frames) want = chunk_frames;

        for (int32_t i = 0; i < want; i++) {
            uint32_t idx = (phase_q16 >> 16) & 63U;
            int32_t s = FV_STAGE1R_SINE_BLOB_64[idx];
            s = (s * gain_q15) >> 15;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            buffer[i] = (int16_t) s;
            phase_q16 += step_q16;
        }

        aaudio_result_t wr = AAudioStream_write(stream, buffer, want, 100000000L);
        if (wr < 0) {
            __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "AAudioStream_write failed: %d", (int) wr);
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            return -4;
        }
        frames_done += (int32_t) wr;
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    __android_log_print(ANDROID_LOG_INFO, FV_TAG,
                        "native short audio proof complete duration_ms=%d frequency_hz=%d",
                        (int) duration_ms, (int) frequency_hz);

    return 0;
}

// FIELDVOX_STAGE1R_B_NATIVE_WINDOW_SEQUENCER
// Low-level short-window sequencer using the same static PCM waveform blob.
// This is not TTS, not a word bank, not a decoder, and not radio playback.
JNIEXPORT jint JNICALL
Java_org_fieldvox_lab_MainActivity_nativePlayStage1RWindowSequence(
        JNIEnv *env,
        jobject thiz,
        jint burst_count,
        jint burst_ms,
        jint gap_ms,
        jfloat amplitude) {
    (void) env;
    (void) thiz;

    if (burst_count < 1) burst_count = 1;
    if (burst_count > 9) burst_count = 9;
    if (burst_ms < 40) burst_ms = 40;
    if (burst_ms > 180) burst_ms = 180;
    if (gap_ms < 20) gap_ms = 20;
    if (gap_ms > 160) gap_ms = 160;
    if (amplitude < 0.02f) amplitude = 0.02f;
    if (amplitude > 0.35f) amplitude = 0.35f;

    static const int32_t freqs_hz[9] = {330, 440, 660, 880, 550, 770, 495, 990, 605};

    AAudioStreamBuilder *builder = 0;
    AAudioStream *stream = 0;

    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || builder == 0) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "sequence builder failed: %d", (int) r);
        return -11;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);

    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    builder = 0;

    if (r != AAUDIO_OK || stream == 0) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "sequence open stream failed: %d", (int) r);
        return -12;
    }

    int32_t sample_rate = AAudioStream_getSampleRate(stream);
    if (sample_rate <= 0) sample_rate = 48000;

    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "sequence requestStart failed: %d", (int) r);
        AAudioStream_close(stream);
        return -13;
    }

    int16_t buffer[192];
    int32_t gain_q15 = (int32_t) (amplitude * 32767.0f);

    for (int32_t b = 0; b < burst_count; b++) {
        int32_t freq = freqs_hz[b % 9];
        uint32_t phase_q16 = 0;
        uint32_t step_q16 = (uint32_t) ((((uint64_t) freq) * 64ULL * 65536ULL) / (uint64_t) sample_rate);
        int32_t burst_frames = (int32_t) (((int64_t) sample_rate * (int64_t) burst_ms) / 1000);
        int32_t frames_done = 0;

        while (frames_done < burst_frames) {
            int32_t want = burst_frames - frames_done;
            if (want > 192) want = 192;

            for (int32_t i = 0; i < want; i++) {
                uint32_t idx = (phase_q16 >> 16) & 63U;
                int32_t s = FV_STAGE1R_SINE_BLOB_64[idx];
                s = (s * gain_q15) >> 15;

                // Very short edge ramp to avoid harsh clicks.
                int32_t absolute_frame = frames_done + i;
                int32_t ramp = 64;
                if (absolute_frame < ramp) {
                    s = (s * absolute_frame) / ramp;
                }
                int32_t remaining = burst_frames - absolute_frame;
                if (remaining < ramp) {
                    s = (s * remaining) / ramp;
                }

                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buffer[i] = (int16_t) s;
                phase_q16 += step_q16;
            }

            aaudio_result_t wr = AAudioStream_write(stream, buffer, want, 100000000L);
            if (wr < 0) {
                __android_log_print(ANDROID_LOG_ERROR, FV_TAG, "sequence burst write failed: %d", (int) wr);
                AAudioStream_requestStop(stream);
                AAudioStream_close(stream);
                return -14;
            }
            frames_done += (int32_t) wr;
        }

        if (b + 1 < burst_count) {
            int32_t gap_frames = (int32_t) (((int64_t) sample_rate * (int64_t) gap_ms) / 1000);
            int32_t gap_done = 0;
            while (gap_done < gap_frames) {
                int32_t want = gap_frames - gap_done;
                if (want > 192) want = 192;
                for (int32_t i = 0; i < want; i++) buffer[i] = 0;
                aaudio_result_t wr = AAudioStream_write(stream, buffer, want, 100000000L);
                if (wr < 0) {
                    AAudioStream_requestStop(stream);
                    AAudioStream_close(stream);
                    return -15;
                }
                gap_done += (int32_t) wr;
            }
        }
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    __android_log_print(ANDROID_LOG_INFO, FV_TAG,
                        "native window sequence complete burst_count=%d burst_ms=%d gap_ms=%d",
                        (int) burst_count, (int) burst_ms, (int) gap_ms);

    return 0;
}


// FIELDVOX_STAGE1S_A_NATIVE_MIC_METRICS
// Native microphone metrics window.
// Records no message, performs no translation, and makes no EVP determination.
static uint32_t fv_stage1s_isqrt_u64(uint64_t x) {
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (uint32_t) res;
}

JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeProbeStage1SMicWindow(
        JNIEnv *env,
        jobject thiz,
        jint duration_ms) {
    (void) thiz;

    if (duration_ms < 120) duration_ms = 120;
    if (duration_ms > 2000) duration_ms = 2000;

    AAudioStreamBuilder *builder = 0;
    AAudioStream *stream = 0;

    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || builder == 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-21 step=create_builder result=%d", (int) r);
        return (*env)->NewStringUTF(env, msg);
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);

    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    builder = 0;

    if (r != AAUDIO_OK || stream == 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-22 step=open_input result=%d", (int) r);
        return (*env)->NewStringUTF(env, msg);
    }

    int32_t sample_rate = AAudioStream_getSampleRate(stream);
    if (sample_rate <= 0) sample_rate = 48000;

    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-23 step=request_start result=%d", (int) r);
        AAudioStream_close(stream);
        return (*env)->NewStringUTF(env, msg);
    }

    const int32_t target_frames = (int32_t) (((int64_t) sample_rate * (int64_t) duration_ms) / 1000);
    int32_t frames_done = 0;
    int16_t buffer[192];

    uint64_t sum_sq = 0;
    uint64_t sum_abs = 0;
    int32_t peak = 0;
    int32_t zero_crossings = 0;
    int32_t have_prev = 0;
    int32_t prev = 0;
    int32_t read_calls = 0;

    while (frames_done < target_frames && read_calls < 2000) {
        int32_t want = target_frames - frames_done;
        if (want > 192) want = 192;

        aaudio_result_t got = AAudioStream_read(stream, buffer, want, 100000000L);
        read_calls++;

        if (got < 0) {
            char msg[192];
            snprintf(msg, sizeof(msg), "rc=-24 step=read result=%d frames=%d", (int) got, (int) frames_done);
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            return (*env)->NewStringUTF(env, msg);
        }

        if (got == 0) {
            continue;
        }

        for (int32_t i = 0; i < got; i++) {
            int32_t v = buffer[i];
            int32_t av = v < 0 ? -v : v;
            if (av > peak) peak = av;
            sum_abs += (uint64_t) av;
            sum_sq += (uint64_t) ((int64_t) v * (int64_t) v);
            if (have_prev) {
                if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) zero_crossings++;
            }
            prev = v;
            have_prev = 1;
        }

        frames_done += (int32_t) got;
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    uint32_t avg_abs = frames_done > 0 ? (uint32_t) (sum_abs / (uint64_t) frames_done) : 0;
    uint32_t rms = frames_done > 0 ? fv_stage1s_isqrt_u64(sum_sq / (uint64_t) frames_done) : 0;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "rc=0 duration_ms=%d sample_rate=%d frames=%d peak=%d rms=%u avg_abs=%u zero_crossings=%d read_calls=%d",
             (int) duration_ms,
             (int) sample_rate,
             (int) frames_done,
             (int) peak,
             (unsigned) rms,
             (unsigned) avg_abs,
             (int) zero_crossings,
             (int) read_calls);

    __android_log_print(ANDROID_LOG_INFO, FV_TAG, "native mic metrics: %s", msg);
    return (*env)->NewStringUTF(env, msg);
}


// FIELDVOX_STAGE1S_B_NATIVE_MIC_BASELINE
// Native microphone baseline profiler.
// Collects windowed signal metrics only; stores no audio and makes no interpretation.
JNIEXPORT jstring JNICALL
Java_org_fieldvox_lab_MainActivity_nativeProbeStage1SMicBaseline(
        JNIEnv *env,
        jobject thiz,
        jint window_count,
        jint window_ms,
        jint gap_ms) {
    (void) thiz;

    if (window_count < 2) window_count = 2;
    if (window_count > 12) window_count = 12;
    if (window_ms < 80) window_ms = 80;
    if (window_ms > 500) window_ms = 500;
    if (gap_ms < 0) gap_ms = 0;
    if (gap_ms > 250) gap_ms = 250;

    AAudioStreamBuilder *builder = 0;
    AAudioStream *stream = 0;

    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || builder == 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-31 step=create_builder result=%d", (int) r);
        return (*env)->NewStringUTF(env, msg);
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 48000);

    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    builder = 0;

    if (r != AAUDIO_OK || stream == 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-32 step=open_input result=%d", (int) r);
        return (*env)->NewStringUTF(env, msg);
    }

    int32_t sample_rate = AAudioStream_getSampleRate(stream);
    if (sample_rate <= 0) sample_rate = 48000;

    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "rc=-33 step=request_start result=%d", (int) r);
        AAudioStream_close(stream);
        return (*env)->NewStringUTF(env, msg);
    }

    int16_t buffer[192];

    uint64_t total_frames = 0;
    uint64_t total_sum_sq = 0;
    uint64_t total_sum_abs = 0;
    int32_t total_zero_crossings = 0;
    int32_t read_calls = 0;

    int32_t peak_min = 32767;
    int32_t peak_max = 0;
    uint32_t rms_min = 0xffffffffU;
    uint32_t rms_max = 0;
    uint64_t rms_sum = 0;

    for (int32_t w = 0; w < window_count; w++) {
        const int32_t target_frames = (int32_t) (((int64_t) sample_rate * (int64_t) window_ms) / 1000);
        int32_t frames_done = 0;
        uint64_t win_sum_sq = 0;
        uint64_t win_sum_abs = 0;
        int32_t win_peak = 0;
        int32_t win_zc = 0;
        int32_t have_prev = 0;
        int32_t prev = 0;

        while (frames_done < target_frames && read_calls < 5000) {
            int32_t want = target_frames - frames_done;
            if (want > 192) want = 192;

            aaudio_result_t got = AAudioStream_read(stream, buffer, want, 100000000L);
            read_calls++;

            if (got < 0) {
                char msg[192];
                snprintf(msg, sizeof(msg), "rc=-34 step=read result=%d window=%d frames=%d", (int) got, (int) w, (int) frames_done);
                AAudioStream_requestStop(stream);
                AAudioStream_close(stream);
                return (*env)->NewStringUTF(env, msg);
            }

            if (got == 0) {
                continue;
            }

            for (int32_t i = 0; i < got; i++) {
                int32_t v = buffer[i];
                int32_t av = v < 0 ? -v : v;
                if (av > win_peak) win_peak = av;
                win_sum_abs += (uint64_t) av;
                win_sum_sq += (uint64_t) ((int64_t) v * (int64_t) v);
                if (have_prev) {
                    if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) win_zc++;
                }
                prev = v;
                have_prev = 1;
            }

            frames_done += (int32_t) got;
        }

        uint32_t win_rms = frames_done > 0 ? fv_stage1s_isqrt_u64(win_sum_sq / (uint64_t) frames_done) : 0;

        if (win_peak < peak_min) peak_min = win_peak;
        if (win_peak > peak_max) peak_max = win_peak;
        if (win_rms < rms_min) rms_min = win_rms;
        if (win_rms > rms_max) rms_max = win_rms;

        rms_sum += (uint64_t) win_rms;
        total_frames += (uint64_t) frames_done;
        total_sum_sq += win_sum_sq;
        total_sum_abs += win_sum_abs;
        total_zero_crossings += win_zc;

        if (gap_ms > 0 && w + 1 < window_count) {
            const int32_t gap_frames = (int32_t) (((int64_t) sample_rate * (int64_t) gap_ms) / 1000);
            int32_t gap_done = 0;
            while (gap_done < gap_frames && read_calls < 5000) {
                int32_t want = gap_frames - gap_done;
                if (want > 192) want = 192;
                aaudio_result_t got = AAudioStream_read(stream, buffer, want, 100000000L);
                read_calls++;
                if (got < 0) {
                    char msg[192];
                    snprintf(msg, sizeof(msg), "rc=-35 step=gap_read result=%d window=%d", (int) got, (int) w);
                    AAudioStream_requestStop(stream);
                    AAudioStream_close(stream);
                    return (*env)->NewStringUTF(env, msg);
                }
                gap_done += (int32_t) got;
            }
        }
    }

    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);

    if (peak_min == 32767) peak_min = 0;
    if (rms_min == 0xffffffffU) rms_min = 0;

    uint32_t rms_avg = window_count > 0 ? (uint32_t) (rms_sum / (uint64_t) window_count) : 0;
    uint32_t total_rms = total_frames > 0 ? fv_stage1s_isqrt_u64(total_sum_sq / total_frames) : 0;
    uint32_t avg_abs = total_frames > 0 ? (uint32_t) (total_sum_abs / total_frames) : 0;

    char msg[320];
    snprintf(msg, sizeof(msg),
             "rc=0 windows=%d window_ms=%d gap_ms=%d sample_rate=%d frames=%llu peak_min=%d peak_max=%d rms_min=%u rms_max=%u rms_avg=%u total_rms=%u avg_abs=%u zero_crossings=%d read_calls=%d",
             (int) window_count,
             (int) window_ms,
             (int) gap_ms,
             (int) sample_rate,
             (unsigned long long) total_frames,
             (int) peak_min,
             (int) peak_max,
             (unsigned) rms_min,
             (unsigned) rms_max,
             (unsigned) rms_avg,
             (unsigned) total_rms,
             (unsigned) avg_abs,
             (int) total_zero_crossings,
             (int) read_calls);

    __android_log_print(ANDROID_LOG_INFO, FV_TAG, "native mic baseline: %s", msg);
    return (*env)->NewStringUTF(env, msg);
}


// FIELDVOX_STAGE1S_C_NATIVE_CARRIER_KILL
// Native C policy gate for disabling legacy Java stream carriers.
// This keeps long-form MediaPlayer carrier streams disabled while native C AAudio work continues.
JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1SCDisableLegacyCarrier(JNIEnv *env, jobject thiz) {
    (void) env;
    (void) thiz;
    // FIELDVOX_STAGE1T_A_BOUNDED_VOICE_FIELD_TEST
    // FIELDVOX_STAGE1T_B_FORCE_BOUNDED_GRAIN_FIELD_TEST
    // Temporary field-test mode: allow bounded station/field carrier windows.
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeStage1SCNoteLegacyCarrierBlocked(JNIEnv *env, jobject thiz, jstring method_name) {
    (void) thiz;
    const char *m = method_name ? (*env)->GetStringUTFChars(env, method_name, 0) : 0;
    __android_log_print(ANDROID_LOG_WARN, FV_TAG, "Stage 1S-C native C blocked legacy carrier path: %s", m ? m : "unknown");
    if (m) {
        (*env)->ReleaseStringUTFChars(env, method_name, m);
    }
}
#endif

// NATIVE AUDIO DSP & SPECTROGRAM ENGINE (C)
// Full Suite: 256-point FFT waterfall, Schroeder Reverb, Granular Pitch Shift, Pre-roll Ring Buffer
#define FV_DSP_ECHO_MAX_INTERVAL_SAMPLES (48000 * 200 / 1000) // 9600 samples (200ms @ 48kHz)
#define FV_DSP_ECHO_BUFFER_SAMPLES (FV_DSP_ECHO_MAX_INTERVAL_SAMPLES * 3) // 28800 samples (600ms @ 48kHz)
#define FV_DSP_ECHO_DEFAULT_INTERVAL_SAMPLES (48000 * 150 / 1000) // 7200 samples (150ms @ 48kHz)
static int16_t s_native_echo_buffer[FV_DSP_ECHO_BUFFER_SAMPLES];
static uint32_t s_native_echo_index = 0;
static uint64_t s_native_noise_prng = 0x854351239ULL;

/* 5-second Pre-roll Ring Buffer for instant, zero-latency hit marking */
#define FV_PREROLL_SECONDS 5
#define FV_PREROLL_SAMPLES (48000 * FV_PREROLL_SECONDS)
static int16_t s_preroll_ring[FV_PREROLL_SAMPLES];
static uint32_t s_preroll_write = 0;

/* Schroeder reverb (native environmental reverb approximation):
 * - 50 ms pre-delay ring buffer (2400 samples @ 48kHz)
 * - 4 parallel comb filters (density = 1000, decay = 9000ms * mix)
 * - 2 series allpass filters (diffusion = 1000 * mix)
 */
#define FV_REV_PREDELAY_SAMPLES (48000 * 50 / 1000) // 2400 samples (50ms @ 48kHz)
#define FV_REV_C1 1399
#define FV_REV_C2 1777
#define FV_REV_C3 1973
#define FV_REV_C4 2087
#define FV_REV_A1 241
#define FV_REV_A2 83
static int16_t s_rev_predelay[FV_REV_PREDELAY_SAMPLES];
static uint32_t s_rev_ipredelay = 0;
static int16_t s_rev_c1[FV_REV_C1];
static int16_t s_rev_c2[FV_REV_C2];
static int16_t s_rev_c3[FV_REV_C3];
static int16_t s_rev_c4[FV_REV_C4];
static int16_t s_rev_a1[FV_REV_A1];
static int16_t s_rev_a2[FV_REV_A2];
static uint32_t s_rev_ic1 = 0, s_rev_ic2 = 0, s_rev_ic3 = 0, s_rev_ic4 = 0;
static uint32_t s_rev_ia1 = 0, s_rev_ia2 = 0;

/* Granular Pitch Shifter (+/- 3 semitones) */
#define FV_PITCH_BUF_SIZE 4096
static int16_t s_pitch_buf[FV_PITCH_BUF_SIZE];
static uint32_t s_pitch_write = 0;
static float s_pitch_read = 0.0f;

/* Vocal Formant Bandpass Filter (300Hz - 3400Hz @ 48kHz, Direct Form II Transposed) */
static float s_bp_hp_d1 = 0.0f, s_bp_hp_d2 = 0.0f;
static float s_bp_lp_d1 = 0.0f, s_bp_lp_d2 = 0.0f;

static inline int16_t fv_clamp_i16(int32_t value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

static inline float fv_vocal_bandpass_single(float in_val) {
    // Stage 1: High-pass at 300 Hz
    float hp_out = 0.972614f * in_val + s_bp_hp_d1;
    s_bp_hp_d1 = -1.945228f * in_val - (-1.944487f * hp_out) + s_bp_hp_d2;
    s_bp_hp_d2 = 0.972614f * in_val - (0.945978f * hp_out);

    // Stage 2: Low-pass at 3400 Hz
    float lp_out = 0.037320f * hp_out + s_bp_lp_d1;
    s_bp_lp_d1 = 0.074639f * hp_out - (-1.384079f * lp_out) + s_bp_lp_d2;
    s_bp_lp_d2 = 0.037320f * hp_out - (0.533358f * lp_out);

    return lp_out;
}

void fv_vocal_bandpass(int16_t *samples, int count, uint32_t sample_rate) {
    (void)sample_rate;
    for (int i = 0; i < count; i++) {
        float f = fv_vocal_bandpass_single((float)samples[i]);
        if (f > 32767.0f) f = 32767.0f;
        if (f < -32768.0f) f = -32768.0f;
        samples[i] = (int16_t)f;
    }
}

void fv_apply_schroeder_reverb_ex(int16_t *samples, int count, float mix) {
    if (!samples || count <= 0) return;
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    /* EnvironmentalReverb:
     * Diffusion: 1000 * mix -> allpass feedback gain scaled with mix
     * Density: 1000 (all 4 combs active)
     * Reverb Delay: 50 ms pre-delay ring buffer (FV_REV_PREDELAY_SAMPLES)
     * Decay Time: decay * mix (base decay 9000 ms -> comb feedback gain scaled with mix)
     */
    float t60 = 9.0f * mix;
    if (t60 < 0.15f) t60 = 0.15f;
    int32_t g1 = (int32_t)(expf(-6.9078f * (float)FV_REV_C1 / (48000.0f * t60)) * 1000.0f);
    int32_t g2 = (int32_t)(expf(-6.9078f * (float)FV_REV_C2 / (48000.0f * t60)) * 1000.0f);
    int32_t g3 = (int32_t)(expf(-6.9078f * (float)FV_REV_C3 / (48000.0f * t60)) * 1000.0f);
    int32_t g4 = (int32_t)(expf(-6.9078f * (float)FV_REV_C4 / (48000.0f * t60)) * 1000.0f);
    if (g1 > 950) g1 = 950;
    if (g2 > 950) g2 = 950;
    if (g3 > 950) g3 = 950;
    if (g4 > 950) g4 = 950;

    const int32_t allpass_gain = 500 + (int32_t)(200.0f * mix); // 500..700 permille (diffusion 1000 * mix)

    for (int i = 0; i < count; i++) {
        int32_t in_val = samples[i];

        /* 50 ms pre-delay ring buffer */
        int32_t predelay_out = (int32_t)s_rev_predelay[s_rev_ipredelay];
        s_rev_predelay[s_rev_ipredelay] = (int16_t)in_val;
        s_rev_ipredelay = (s_rev_ipredelay + 1u) % FV_REV_PREDELAY_SAMPLES;

        /* 4 parallel comb filters (density = 1000) */
        int32_t c1_out = s_rev_c1[s_rev_ic1];
        s_rev_c1[s_rev_ic1] = fv_clamp_i16(predelay_out + (int32_t)(((int64_t)c1_out * g1) / 1000LL));
        s_rev_ic1 = (s_rev_ic1 + 1u) % FV_REV_C1;

        int32_t c2_out = s_rev_c2[s_rev_ic2];
        s_rev_c2[s_rev_ic2] = fv_clamp_i16(predelay_out + (int32_t)(((int64_t)c2_out * g2) / 1000LL));
        s_rev_ic2 = (s_rev_ic2 + 1u) % FV_REV_C2;

        int32_t c3_out = s_rev_c3[s_rev_ic3];
        s_rev_c3[s_rev_ic3] = fv_clamp_i16(predelay_out + (int32_t)(((int64_t)c3_out * g3) / 1000LL));
        s_rev_ic3 = (s_rev_ic3 + 1u) % FV_REV_C3;

        int32_t c4_out = s_rev_c4[s_rev_ic4];
        s_rev_c4[s_rev_ic4] = fv_clamp_i16(predelay_out + (int32_t)(((int64_t)c4_out * g4) / 1000LL));
        s_rev_ic4 = (s_rev_ic4 + 1u) % FV_REV_C4;

        int32_t comb_sum = (c1_out + c2_out + c3_out + c4_out) / 4;

        /* 2 series allpass filters (diffusion) */
        int32_t a1_delayed = s_rev_a1[s_rev_ia1];
        int32_t a1_out = -(int32_t)(((int64_t)comb_sum * allpass_gain) / 1000LL) + a1_delayed;
        s_rev_a1[s_rev_ia1] = fv_clamp_i16(comb_sum + (int32_t)(((int64_t)a1_out * allpass_gain) / 1000LL));
        s_rev_ia1 = (s_rev_ia1 + 1u) % FV_REV_A1;

        int32_t a2_delayed = s_rev_a2[s_rev_ia2];
        int32_t a2_out = -(int32_t)(((int64_t)a1_out * allpass_gain) / 1000LL) + a2_delayed;
        s_rev_a2[s_rev_ia2] = fv_clamp_i16(a1_out + (int32_t)(((int64_t)a2_out * allpass_gain) / 1000LL));
        s_rev_ia2 = (s_rev_ia2 + 1u) % FV_REV_A2;

        int32_t wet = (int32_t)(((int64_t)in_val * 700LL + (int64_t)a2_out * 450LL) / 1000LL);
        samples[i] = fv_clamp_i16(wet);
    }
}

void fv_apply_schroeder_reverb(int16_t *samples, int count) {
    fv_apply_schroeder_reverb_ex(samples, count, 0.75f);
}

void fv_apply_granular_pitch(int16_t *samples, int count, int semitones) {
    if (semitones == 0) return;
    const float step = powf(2.0f, (float)semitones / 12.0f);
    for (int i = 0; i < count; i++) {
        s_pitch_buf[s_pitch_write] = samples[i];
        s_pitch_write = (s_pitch_write + 1) % FV_PITCH_BUF_SIZE;
        s_pitch_read += step;
        if (s_pitch_read >= FV_PITCH_BUF_SIZE) s_pitch_read -= FV_PITCH_BUF_SIZE;
        int r_idx = (int)s_pitch_read;
        samples[i] = s_pitch_buf[r_idx];
    }
}

/* Acoustic REM-Pod Dynamic Multi-Tone Synthesizer */
#ifdef FIELDVOX_ENABLE_LEGACY_JNI
static float s_rem_pod_phase = 0.0f;
static uint32_t s_rem_pod_sample_count = 0;

static inline int16_t fv_rem_pod_generate(int level) {
    if (level <= 0) return 0;
    float freq = level == 1 ? 440.0f : (level == 2 ? 880.0f : (level == 3 ? 1320.0f : 1760.0f));
    uint32_t period = level == 1 ? 24000 : (level == 2 ? 12000 : (level == 3 ? 4800 : 1));
    uint32_t on_samples = level == 1 ? 2400 : (level == 2 ? 2880 : (level == 3 ? 1920 : 1));

    s_rem_pod_sample_count++;
    uint32_t cycle = s_rem_pod_sample_count % period;
    if (level < 4 && cycle > on_samples) {
        return 0;
    }

    s_rem_pod_phase += (2.0f * (float)M_PI * freq) / 48000.0f;
    if (s_rem_pod_phase >= 2.0f * (float)M_PI) s_rem_pod_phase -= 2.0f * (float)M_PI;

    return (int16_t)(sinf(s_rem_pod_phase) * 5500.0f);
}
#endif

/* Real-Time Radix-2 FFT (256 points) for 2D Waterfall Spectrogram */
#define FV_FFT_SIZE 256
#define FV_SPEC_BINS 64
static float s_fft_re[FV_FFT_SIZE];
static float s_fft_im[FV_FFT_SIZE];
static int16_t s_fft_acc[FV_FFT_SIZE];
static int s_fft_acc_count = 0;
static float s_latest_spec_bins[FV_SPEC_BINS];
static float s_hanning[FV_FFT_SIZE];
static int s_hanning_inited = 0;

static void fv_init_hanning(void) {
    if (s_hanning_inited) return;
    for (int i = 0; i < FV_FFT_SIZE; i++) {
        s_hanning[i] = 0.5f * (1.0f - cosf((float)(2.0 * M_PI * i / (FV_FFT_SIZE - 1))));
    }
    s_hanning_inited = 1;
}

static void fv_radix2_fft(float *re, float *im, int n) {
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * (float)M_PI / (float)len;
        float wlen_r = cosf(angle);
        float wlen_i = sinf(angle);
        for (int i = 0; i < n; i += len) {
            float w_r = 1.0f;
            float w_i = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float u_r = re[i + k];
                float u_i = im[i + k];
                float v_r = re[i + k + len / 2] * w_r - im[i + k + len / 2] * w_i;
                float v_i = re[i + k + len / 2] * w_i + im[i + k + len / 2] * w_r;
                re[i + k] = u_r + v_r;
                im[i + k] = u_i + v_i;
                re[i + k + len / 2] = u_r - v_r;
                im[i + k + len / 2] = u_i - v_i;
                float next_w_r = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = next_w_r;
            }
        }
    }
}

void fv_audio_dsp_reset(void) {
    memset(s_native_echo_buffer, 0, sizeof(s_native_echo_buffer));
    s_native_echo_index = 0;
    s_native_noise_prng = 0x854351239ULL;
    memset(s_preroll_ring, 0, sizeof(s_preroll_ring));
    s_preroll_write = 0;
    memset(s_rev_predelay, 0, sizeof(s_rev_predelay));
    s_rev_ipredelay = 0;
    memset(s_rev_c1, 0, sizeof(s_rev_c1));
    memset(s_rev_c2, 0, sizeof(s_rev_c2));
    memset(s_rev_c3, 0, sizeof(s_rev_c3));
    memset(s_rev_c4, 0, sizeof(s_rev_c4));
    memset(s_rev_a1, 0, sizeof(s_rev_a1));
    memset(s_rev_a2, 0, sizeof(s_rev_a2));
    s_rev_ic1 = s_rev_ic2 = s_rev_ic3 = s_rev_ic4 = 0;
    s_rev_ia1 = s_rev_ia2 = 0;
    memset(s_pitch_buf, 0, sizeof(s_pitch_buf));
    memset(s_latest_spec_bins, 0, sizeof(s_latest_spec_bins));
    s_pitch_write = 0;
    s_pitch_read = 0.0f;
    s_fft_acc_count = 0;
    s_bp_hp_d1 = s_bp_hp_d2 = 0.0f;
    s_bp_lp_d1 = s_bp_lp_d2 = 0.0f;
#ifdef FIELDVOX_ENABLE_LEGACY_JNI
    s_rem_pod_phase = 0.0f;
    s_rem_pod_sample_count = 0;
#endif
}

void fv_audio_capture_and_analyze(const int16_t *samples, int count) {
    if (!samples || count <= 0) return;
    fv_init_hanning();
    for (int i = 0; i < count; i++) {
        const int16_t raw = samples[i];
        s_preroll_ring[s_preroll_write] = raw;
        s_preroll_write = (s_preroll_write + 1) % FV_PREROLL_SAMPLES;
        s_fft_acc[s_fft_acc_count++] = raw;
        if (s_fft_acc_count < FV_FFT_SIZE) continue;
        for (int k = 0; k < FV_FFT_SIZE; k++) {
            s_fft_re[k] = (float)s_fft_acc[k] * s_hanning[k];
            s_fft_im[k] = 0.0f;
        }
        fv_radix2_fft(s_fft_re, s_fft_im, FV_FFT_SIZE);
        for (int b = 0; b < FV_SPEC_BINS; b++) {
            const float r = s_fft_re[b];
            const float im = s_fft_im[b];
            const float mag = sqrtf(r * r + im * im) / 32768.0f;
            float log_mag = log1pf(mag * 12.0f) / 2.5f;
            if (log_mag > 1.0f) log_mag = 1.0f;
            s_latest_spec_bins[b] = s_latest_spec_bins[b] * 0.4f + log_mag * 0.6f;
        }
        s_fft_acc_count = 0;
    }
}

void fv_audio_apply_echo_ex(int16_t *samples, int count, int delay_ms, float mix) {
    if (!samples || count <= 0) return;
    if (delay_ms <= 0) delay_ms = 150;
    if (delay_ms > 200) delay_ms = 200;
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    /* FieldVox 3-tap discrete echo model:
     * Scheduled via ScheduledExecutorService across 3 repetitions at delay intervals j:
     *   Volume_k = vol * 0.2 * (0.3)^k
     * k=1: delay j,     weight 0.0600 * mix
     * k=2: delay 2 * j, weight 0.0180 * mix
     * k=3: delay 3 * j, weight 0.0054 * mix
     */
    const uint32_t j = (uint32_t)(48000 * delay_ms / 1000);
    const float w1 = 0.0600f * mix;
    const float w2 = 0.0180f * mix;
    const float w3 = 0.0054f * mix;

    for (int i = 0; i < count; i++) {
        const int16_t in_val = samples[i];
        const uint32_t idx1 = (s_native_echo_index + FV_DSP_ECHO_BUFFER_SAMPLES - j) % FV_DSP_ECHO_BUFFER_SAMPLES;
        const uint32_t idx2 = (s_native_echo_index + FV_DSP_ECHO_BUFFER_SAMPLES - 2u * j) % FV_DSP_ECHO_BUFFER_SAMPLES;
        const uint32_t idx3 = (s_native_echo_index + FV_DSP_ECHO_BUFFER_SAMPLES - 3u * j) % FV_DSP_ECHO_BUFFER_SAMPLES;

        const float echo_rep = (float)s_native_echo_buffer[idx1] * w1
                             + (float)s_native_echo_buffer[idx2] * w2
                             + (float)s_native_echo_buffer[idx3] * w3;

        int32_t mixed = (int32_t)in_val + (int32_t)lrintf(echo_rep);
        samples[i] = fv_clamp_i16(mixed);

        s_native_echo_buffer[s_native_echo_index] = in_val;
        s_native_echo_index = (s_native_echo_index + 1u) % FV_DSP_ECHO_BUFFER_SAMPLES;
    }
}

void fv_audio_apply_echo(int16_t *samples, int count) {
    fv_audio_apply_echo_ex(samples, count, 150, 1.0f);
}

void fv_audio_apply_noise(int16_t *samples, int count, int amplitude) {
    if (!samples || count <= 0 || amplitude <= 0) return;
    for (int i = 0; i < count; i++) {
        s_native_noise_prng = s_native_noise_prng * 6364136223846793005ULL + 1442695040888963407ULL;
        const int32_t noise = (int32_t)(((int64_t)(s_native_noise_prng >> 33) * amplitude) / 1073741824LL) - amplitude;
        int32_t mixed = (int32_t)samples[i] + noise;
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        samples[i] = (int16_t)mixed;
    }
}

void fv_audio_reverse(int16_t *samples, int count) {
    if (!samples || count <= 1) return;
    for (int i = 0; i < count / 2; i++) {
        const int16_t tmp = samples[i];
        samples[i] = samples[count - 1 - i];
        samples[count - 1 - i] = tmp;
    }
}

void fv_audio_get_spectrum(float out_bins[FV_NATIVE_SPECTRUM_BINS]) {
    if (out_bins) memcpy(out_bins, s_latest_spec_bins, sizeof(s_latest_spec_bins));
}

bool fv_audio_export_preroll_wav(const char *path, int pre_seconds) {
    if (!path || !*path) return false;
    const int seconds = pre_seconds <= 0 ? 3 : (pre_seconds > FV_PREROLL_SECONDS ? FV_PREROLL_SECONDS : pre_seconds);
    const uint32_t sample_count = (uint32_t)(seconds * 48000);
    const uint32_t pcm_bytes = sample_count * 2;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const uint32_t riff_size = pcm_bytes + 36;
    const uint32_t sample_rate = 48000;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * (bits / 8);
    const uint16_t block_align = channels * (bits / 8);
    uint8_t header[44] = {0};
    memcpy(&header[0], "RIFF", 4);
    memcpy(&header[4], &riff_size, 4);
    memcpy(&header[8], "WAVEfmt ", 8);
    header[16] = 16;
    header[20] = 1;
    memcpy(&header[22], &channels, 2);
    memcpy(&header[24], &sample_rate, 4);
    memcpy(&header[28], &byte_rate, 4);
    memcpy(&header[32], &block_align, 2);
    memcpy(&header[34], &bits, 2);
    memcpy(&header[36], "data", 4);
    memcpy(&header[40], &pcm_bytes, 4);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    const uint32_t start_idx = (s_preroll_write + FV_PREROLL_SAMPLES - sample_count) % FV_PREROLL_SAMPLES;
    const uint32_t first_count = sample_count < FV_PREROLL_SAMPLES - start_idx
        ? sample_count : FV_PREROLL_SAMPLES - start_idx;
    if (ok) {
        ok = fwrite(&s_preroll_ring[start_idx], sizeof(int16_t), first_count, f) == first_count;
    }
    if (ok && first_count < sample_count) {
        const uint32_t second_count = sample_count - first_count;
        ok = fwrite(s_preroll_ring, sizeof(int16_t), second_count, f) == second_count;
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) unlink(path);
    return ok;
}

#ifdef FIELDVOX_ENABLE_LEGACY_JNI
JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeResetAudioDsp(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    fv_audio_dsp_reset();
}

JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeGetSpectrogramColumn(JNIEnv *env, jobject thiz, jfloatArray j_out) {
    (void)thiz;
    if (!j_out) return JNI_FALSE;
    (*env)->SetFloatArrayRegion(env, j_out, 0, FV_SPEC_BINS, s_latest_spec_bins);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_org_fieldvox_lab_MainActivity_nativeExportPreRollWav(JNIEnv *env, jobject thiz, jstring j_path, jint pre_seconds) {
    (void)thiz;
    if (!j_path) return JNI_FALSE;
    const char *path = (*env)->GetStringUTFChars(env, j_path, NULL);
    if (!path) return JNI_FALSE;

    int seconds = pre_seconds <= 0 ? 3 : (pre_seconds > FV_PREROLL_SECONDS ? FV_PREROLL_SECONDS : pre_seconds);
    uint32_t sample_count = (uint32_t)(seconds * 48000);
    uint32_t pcm_bytes = sample_count * 2;

    FILE *f = fopen(path, "wb");
    if (!f) {
        (*env)->ReleaseStringUTFChars(env, j_path, path);
        return JNI_FALSE;
    }

    uint8_t header[44];
    uint32_t riff_size = pcm_bytes + 36;
    uint32_t sample_rate = 48000;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t byte_rate = sample_rate * channels * (bits / 8);
    uint16_t block_align = channels * (bits / 8);

    memcpy(&header[0], "RIFF", 4);
    header[4] = (uint8_t)(riff_size & 0xff); header[5] = (uint8_t)((riff_size >> 8) & 0xff);
    header[6] = (uint8_t)((riff_size >> 16) & 0xff); header[7] = (uint8_t)((riff_size >> 24) & 0xff);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;
    header[22] = (uint8_t)(channels & 0xff); header[23] = 0;
    header[24] = (uint8_t)(sample_rate & 0xff); header[25] = (uint8_t)((sample_rate >> 8) & 0xff);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xff); header[27] = (uint8_t)((sample_rate >> 24) & 0xff);
    header[28] = (uint8_t)(byte_rate & 0xff); header[29] = (uint8_t)((byte_rate >> 8) & 0xff);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xff); header[31] = (uint8_t)((byte_rate >> 24) & 0xff);
    header[32] = (uint8_t)(block_align & 0xff); header[33] = 0;
    header[34] = (uint8_t)(bits & 0xff); header[35] = 0;
    memcpy(&header[36], "data", 4);
    header[40] = (uint8_t)(pcm_bytes & 0xff); header[41] = (uint8_t)((pcm_bytes >> 8) & 0xff);
    header[42] = (uint8_t)((pcm_bytes >> 16) & 0xff); header[43] = (uint8_t)((pcm_bytes >> 24) & 0xff);

    fwrite(header, 1, 44, f);

    // Read circular buffer backwards
    uint32_t start_idx = (s_preroll_write + FV_PREROLL_SAMPLES - sample_count) % FV_PREROLL_SAMPLES;
    for (uint32_t i = 0; i < sample_count; i++) {
        int16_t s = s_preroll_ring[(start_idx + i) % FV_PREROLL_SAMPLES];
        uint8_t b[2];
        b[0] = (uint8_t)(s & 0xff);
        b[1] = (uint8_t)((s >> 8) & 0xff);
        fwrite(b, 1, 2, f);
    }
    fclose(f);

    (*env)->ReleaseStringUTFChars(env, j_path, path);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_org_fieldvox_lab_MainActivity_nativeProcessAudioDsp(
        JNIEnv *env,
        jobject thiz,
        jshortArray j_samples,
        jbyteArray j_pcm,
        jshortArray j_monitor,
        jint count,
        jboolean reverse_enabled,
        jboolean echo_enabled,
        jboolean noise_enabled,
        jboolean reverb_enabled,
        jint pitch_shift,
        jboolean vocal_filter_enabled,
        jint rem_pod_level,
        jfloat spatial_pan,
        jfloat boost_factor,
        jfloatArray j_metrics) {
    (void)thiz;
    if (!j_samples || count <= 0) return;

    jshort *samples = (*env)->GetShortArrayElements(env, j_samples, NULL);
    if (!samples) return;

    jbyte *pcm = j_pcm ? (*env)->GetByteArrayElements(env, j_pcm, NULL) : NULL;
    jshort *monitor = j_monitor ? (*env)->GetShortArrayElements(env, j_monitor, NULL) : NULL;

    fv_init_hanning();

    int32_t peak = 0;
    uint64_t sum_squares = 0;

    for (jint i = 0; i < count; i++) {
        int16_t raw = samples[i];
        int32_t av = raw < 0 ? -raw : raw;
        if (av > peak) peak = av;
        sum_squares += (uint64_t)((int64_t)raw * (int64_t)raw);

        // Pre-roll ring buffer recording
        s_preroll_ring[s_preroll_write] = raw;
        s_preroll_write = (s_preroll_write + 1) % FV_PREROLL_SAMPLES;

        // FFT accumulator
        s_fft_acc[s_fft_acc_count++] = raw;
        if (s_fft_acc_count >= FV_FFT_SIZE) {
            for (int k = 0; k < FV_FFT_SIZE; k++) {
                s_fft_re[k] = (float)s_fft_acc[k] * s_hanning[k];
                s_fft_im[k] = 0.0f;
            }
            fv_radix2_fft(s_fft_re, s_fft_im, FV_FFT_SIZE);

            // Compute 64 spectral bins (0 Hz to ~8000 Hz)
            for (int b = 0; b < FV_SPEC_BINS; b++) {
                float r = s_fft_re[b];
                float im = s_fft_im[b];
                float mag = sqrtf(r * r + im * im) / 32768.0f;
                // Log compression for visual clarity
                float log_mag = log1pf(mag * 12.0f) / 2.5f;
                if (log_mag > 1.0f) log_mag = 1.0f;
                s_latest_spec_bins[b] = s_latest_spec_bins[b] * 0.4f + log_mag * 0.6f;
            }
            s_fft_acc_count = 0;
        }

        if (pcm) {
            pcm[i * 2] = (jbyte)(raw & 0xff);
            pcm[i * 2 + 1] = (jbyte)((raw >> 8) & 0xff);
        }

        if (monitor) {
            float boosted = (float)raw * boost_factor;
            if (boosted > 32767.0f) boosted = 32767.0f;
            else if (boosted < -32768.0f) boosted = -32768.0f;
            monitor[i] = (int16_t)boosted;
        }
    }

    if (monitor) {
        if (reverse_enabled) {
            for (jint i = 0; i < count / 2; i++) {
                int16_t tmp = monitor[i];
                monitor[i] = monitor[count - 1 - i];
                monitor[count - 1 - i] = tmp;
            }
        }

        // Granular Pitch Shift
        if (pitch_shift != 0) {
            float step = pitch_shift > 0 ? 1.1892f : 0.8409f;
            for (jint i = 0; i < count; i++) {
                s_pitch_buf[s_pitch_write] = monitor[i];
                s_pitch_write = (s_pitch_write + 1) % FV_PITCH_BUF_SIZE;

                s_pitch_read += step;
                if (s_pitch_read >= FV_PITCH_BUF_SIZE) s_pitch_read -= FV_PITCH_BUF_SIZE;
                int r_idx = (int)s_pitch_read;
                monitor[i] = s_pitch_buf[r_idx];
            }
        }

        // Voice Formant Bandpass Filter (300Hz - 3400Hz speech isolation)
        if (vocal_filter_enabled) {
            for (jint i = 0; i < count; i++) {
                float filtered = fv_vocal_bandpass_single((float)monitor[i]);
                if (filtered > 32767.0f) filtered = 32767.0f;
                else if (filtered < -32768.0f) filtered = -32768.0f;
                monitor[i] = (int16_t)filtered;
            }
        }

        if (echo_enabled) {
            fv_audio_apply_echo(monitor, count);
        }

        if (reverb_enabled) {
            fv_apply_schroeder_reverb(monitor, count);
        }

        if (noise_enabled) {
            for (jint i = 0; i < count; i++) {
                s_native_noise_prng = s_native_noise_prng * 6364136223846793005ULL + 1442695040888963407ULL;
                int32_t noise = (int32_t)(((int64_t)(s_native_noise_prng >> 33) * 1400LL) / 2147483648LL) - 700;
                int32_t mixed = (int32_t)monitor[i] + noise;
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                monitor[i] = (int16_t)mixed;
            }
        }

        // Acoustic REM-Pod Dynamic Multi-Tone Synthesizer
        if (rem_pod_level > 0) {
            for (jint i = 0; i < count; i++) {
                int16_t tone = fv_rem_pod_generate(rem_pod_level);
                int32_t mixed = (int32_t)monitor[i] + (int32_t)tone;
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                monitor[i] = (int16_t)mixed;
            }
        }

        // Stereo expansion & 3D Binaural Spatial Panning
        jsize monitor_len = j_monitor ? (*env)->GetArrayLength(env, j_monitor) : 0;
        if (monitor_len >= count * 2) {
            float p = spatial_pan < -1.0f ? -1.0f : (spatial_pan > 1.0f ? 1.0f : spatial_pan);
            float angle = (float)M_PI * 0.25f * (1.0f + p);
            float g_l = cosf(angle);
            float g_r = sinf(angle);
            for (jint i = count - 1; i >= 0; i--) {
                int32_t s = monitor[i];
                int32_t l = (int32_t)(s * g_l);
                int32_t r = (int32_t)(s * g_r);
                monitor[i * 2] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
                monitor[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
            }
        }
    }

    if (j_metrics) {
        jfloat *metrics = (*env)->GetFloatArrayElements(env, j_metrics, NULL);
        if (metrics) {
            float rms = count > 0 ? (float)sqrt((double)sum_squares / (double)count) : 0.0f;
            metrics[0] = (float)peak;
            metrics[1] = rms;
            (*env)->ReleaseFloatArrayElements(env, j_metrics, metrics, 0);
        }
    }

    (*env)->ReleaseShortArrayElements(env, j_samples, samples, JNI_ABORT);
    if (pcm) (*env)->ReleaseByteArrayElements(env, j_pcm, pcm, 0);
    if (monitor) (*env)->ReleaseShortArrayElements(env, j_monitor, monitor, 0);
}
#endif

void fv_vox_voice_pool_init(FvVoxVoicePool *pool, float reverb_mix) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
    pool->reverb_mix = fmaxf(0.0f, fminf(1.0f, reverb_mix));
}

void fv_vox_voice_pool_set_reverb(FvVoxVoicePool *pool, float reverb_mix) {
    if (!pool) return;
    pool->reverb_mix = fmaxf(0.0f, fminf(1.0f, reverb_mix));
}

void fv_vox_voice_pool_update(FvVoxVoicePool *pool, uint64_t now_ms) {
    if (!pool) return;
    for (int i = 0; i < FV_VOX_MAX_VOICES; i++) {
        if (pool->voices[i].active && now_ms >= pool->voices[i].release_time_ms) {
            pool->voices[i].active = false;
        }
    }
}

int fv_vox_voice_pool_allocate_ex(FvVoxVoicePool *pool, uint64_t now_ms, uint64_t grain_duration_ms,
                                  float playback_speed, int station_id, float volume,
                                  int pitch_semitones, int tap_index) {
    if (!pool || grain_duration_ms == 0) return -1;
    fv_vox_voice_pool_update(pool, now_ms);

    const float speed = (playback_speed > 0.05f) ? playback_speed : 1.0f;
    const uint64_t play_duration_ms = (uint64_t)((float)grain_duration_ms / speed + 0.5f);
    const uint64_t reverb_tail_ms = (uint64_t)(9000.0f * pool->reverb_mix + 0.5f);
    const uint64_t release_time_ms = now_ms + play_duration_ms + reverb_tail_ms;

    int slot = -1;
    for (int i = 0; i < FV_VOX_MAX_VOICES; i++) {
        if (!pool->voices[i].active) {
            slot = i;
            break;
        }
    }

    /* Reject saturation; playing voices and their tails retain ownership. */

    if (slot >= 0) {
        pool->voices[slot] = (FvVoxVoiceSlot){
            .active = true,
            .start_time_ms = now_ms,
            .play_duration_ms = play_duration_ms,
            .release_time_ms = release_time_ms,
            .station_id = station_id,
            .speed = speed,
            .volume = (volume > 0.0f) ? volume : 1.0f,
            .pitch_semitones = pitch_semitones,
            .tap_index = tap_index
        };
    }
    return slot;
}

int fv_vox_voice_pool_allocate(FvVoxVoicePool *pool, uint64_t now_ms, uint64_t grain_duration_ms,
                               float playback_speed, int station_id) {
    return fv_vox_voice_pool_allocate_ex(pool, now_ms, grain_duration_ms, playback_speed,
                                         station_id, 1.0f, 0, 0);
}

bool fv_vox_voice_is_playing(const FvVoxVoicePool *pool, int voice_idx, uint64_t now_ms) {
    if (!pool || voice_idx < 0 || voice_idx >= FV_VOX_MAX_VOICES) return false;
    const FvVoxVoiceSlot *v = &pool->voices[voice_idx];
    if (!v->active) return false;
    return (now_ms >= v->start_time_ms && now_ms < v->start_time_ms + v->play_duration_ms);
}

bool fv_vox_voice_is_busy(const FvVoxVoicePool *pool, int voice_idx, uint64_t now_ms) {
    if (!pool || voice_idx < 0 || voice_idx >= FV_VOX_MAX_VOICES) return false;
    const FvVoxVoiceSlot *v = &pool->voices[voice_idx];
    if (!v->active) return false;
    return (now_ms < v->release_time_ms);
}

void fv_audio_generate_sweep_static(int16_t *samples, int count, uint32_t *rng, float volume) {
    if (!samples || count <= 0) return;
    const float vol = (volume > 0.0f) ? fminf(1.0f, volume) : 0.02f;
    uint32_t state = rng ? *rng : 0x85435123u;
    for (int i = 0; i < count; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float r = ((float)(state & 0xFFFFu) / 65535.0f) * 0.4f - 0.2f;
        const int32_t s = (int32_t)(r * 32767.0f * vol);
        samples[i] = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, (float)s));
    }
    if (rng) *rng = state;
}

bool fv_audio_is_sweep_burst_active(uint64_t clock_ms, uint32_t sweep_rate_ms) {
    if (sweep_rate_ms == 0) return false;
    const uint32_t cycle_period_ms = sweep_rate_ms + 40u;
    const uint32_t phase = (uint32_t)(clock_ms % (uint64_t)cycle_period_ms);
    return (phase < sweep_rate_ms);
}
