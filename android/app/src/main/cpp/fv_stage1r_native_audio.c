#include <jni.h>
#include <stdint.h>
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
