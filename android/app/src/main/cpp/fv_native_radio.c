/*
 * FieldVox native live-radio carrier engine (C, NDK media).
 * See fv_native_radio.h for the deck model.
 */

#include "fv_native_radio.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fv_station_blob_data.h"

#define TAG "FieldVoxRadio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

#define OUT_RATE 48000
#define RING_FRAMES (OUT_RATE * 30)
/*
 * Android's HTTP cache hands the extractor 64 KB pages, so a live stream arrives in chunks of
 * roughly 4 s (128 kbps) to 16 s (32 kbps) of audio. The ring must hold several chunks; only a
 * runaway backlog beyond MAX_BUFFERED_FRAMES is trimmed.
 */
#define MAX_BUFFERED_FRAMES (OUT_RATE * 26)
#define TRIM_TO_FRAMES (OUT_RATE * 16)
#define DWELL_MIN_MS 25000L
#define DWELL_JITTER_MS 15000L
/* Default prebuffer before chunk gaps are measured; then 1.3x the largest measured gap, clamped. */
#define PREBUFFER_DEFAULT_FRAMES (OUT_RATE * 6)
#define PREBUFFER_MIN_FRAMES (OUT_RATE)
#define PREBUFFER_MAX_FRAMES (OUT_RATE * 20)
#define NO_AUDIO_TIMEOUT_MS 20000L
#define FAIL_BACKOFF_MS 60000L
#define PCM_ENCODING_FLOAT 4

typedef struct {
    pthread_mutex_t lock;
    int16_t ring[RING_FRAMES];
    uint32_t read_pos;
    uint32_t count;
    int pcm_station; /* guarded by lock; identity of the buffered PCM */
    bool primed;
    uint32_t prime_frames; /* guarded by lock */
    int dwell_count;
    _Atomic int state;
    _Atomic int station;
    pthread_t thread;
    /* Decoder-thread-only resampler state. */
    double phase;
    float prev;
    int in_rate;
    int in_channels;
    int pcm_encoding;
} FvRadioDeck;

static FvRadioDeck g_decks[FV_RADIO_DECKS];
static JavaVM *g_vm;
static int g_station_count;
static atomic_bool g_started;
static atomic_bool g_enabled;
static _Atomic int g_direction = 1;

static pthread_mutex_t g_pick_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_cursor = -1;
static long g_fail_until_ms[FV_STATION_BLOB_COUNT];
static long g_station_last_trigger_ms[FV_STATION_BLOB_COUNT];
static uint32_t g_rand = 0x9E3779B9u;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

bool fv_radio_station_is_refractory(int station, long now_msec) {
    if (station < 0 || (size_t)station >= FV_STATION_BLOB_COUNT) return false;
    pthread_mutex_lock(&g_pick_mutex);
    const bool refractory = (now_msec - g_station_last_trigger_ms[station] < FV_STATION_REFRACTORY_MS);
    pthread_mutex_unlock(&g_pick_mutex);
    return refractory;
}

void fv_radio_mark_station_triggered(int station, long now_msec) {
    if (station < 0 || (size_t)station >= FV_STATION_BLOB_COUNT) return;
    pthread_mutex_lock(&g_pick_mutex);
    g_station_last_trigger_ms[station] = now_msec;
    pthread_mutex_unlock(&g_pick_mutex);
}

static uint32_t next_rand(void) {
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return g_rand;
}

static const FvBlobStationEntry *entry_at(int index) {
    if (index < 0) index = 0;
    index %= (int)FV_STATION_BLOB_COUNT;
    return (const FvBlobStationEntry *)(FV_STATION_PLAN_BINARY_DATA + 16 +
                                        (size_t)index * sizeof(FvBlobStationEntry));
}

static bool station_held_elsewhere(int station, int self) {
    for (int d = 0; d < FV_RADIO_DECKS; d++) {
        if (d != self && atomic_load(&g_decks[d].station) == station) return true;
    }
    return false;
}

/* Picks the next station in the sweep direction that no other deck holds and is not backing off. */
static int pick_station(int self) {
    pthread_mutex_lock(&g_pick_mutex);
    if (g_station_count <= 0) {
        pthread_mutex_unlock(&g_pick_mutex);
        return -1;
    }
    const long now = now_ms();
    const int dir = atomic_load(&g_direction);
    int chosen = -1;
    for (int attempt = 0; attempt < g_station_count * 2 && chosen < 0; attempt++) {
        int candidate;
        if (dir == 0) {
            candidate = (int)(next_rand() % (uint32_t)g_station_count);
        } else {
            g_cursor = ((g_cursor + dir) % g_station_count + g_station_count) % g_station_count;
            candidate = g_cursor;
        }
        if (candidate >= 0 && (size_t)candidate < FV_STATION_BLOB_COUNT) {
            if (g_fail_until_ms[candidate] > now) continue;
            if (station_held_elsewhere(candidate, self)) continue;
            chosen = candidate;
        }
    }
    if (chosen >= 0) {
        atomic_store(&g_decks[self].station, chosen);
    }
    pthread_mutex_unlock(&g_pick_mutex);
    return chosen;
}

static void mark_failed(int station) {
    if (station < 0 || (size_t)station >= FV_STATION_BLOB_COUNT) return;
    pthread_mutex_lock(&g_pick_mutex);
    g_fail_until_ms[station] = now_ms() + FAIL_BACKOFF_MS;
    pthread_mutex_unlock(&g_pick_mutex);
}

static void ring_reset(FvRadioDeck *deck, int station) {
    pthread_mutex_lock(&deck->lock);
    deck->read_pos = 0;
    deck->count = 0;
    deck->pcm_station = station;
    deck->primed = false;
    deck->prime_frames = PREBUFFER_DEFAULT_FRAMES;
    pthread_mutex_unlock(&deck->lock);
}

static void ring_push(FvRadioDeck *deck, const int16_t *samples, int n) {
    pthread_mutex_lock(&deck->lock);
    for (int i = 0; i < n; i++) {
        if (deck->count == RING_FRAMES) {
            deck->read_pos = (deck->read_pos + 1) % RING_FRAMES;
            deck->count--;
        }
        deck->ring[(deck->read_pos + deck->count) % RING_FRAMES] = samples[i];
        deck->count++;
    }
    pthread_mutex_unlock(&deck->lock);
}

/* Downmixes one decoded block to mono and linearly resamples it to 48 kHz into the ring. */
static void push_decoded(FvRadioDeck *deck, const uint8_t *data, size_t bytes) {
    const int ch = deck->in_channels > 0 ? deck->in_channels : 1;
    const bool is_float = deck->pcm_encoding == PCM_ENCODING_FLOAT;
    const size_t sample_bytes = is_float ? sizeof(float) : sizeof(int16_t);
    const size_t in_frames = bytes / (sample_bytes * (size_t)ch);
    if (in_frames == 0 || deck->in_rate < 4000 || deck->in_rate > 192000) return;

    const double step = (double)deck->in_rate / (double)OUT_RATE;
    int16_t out[1024];
    int out_n = 0;
    for (size_t f = 0; f < in_frames; f++) {
        float mono = 0.0f;
        for (int c = 0; c < ch; c++) {
            if (is_float) {
                float v;
                memcpy(&v, data + (f * (size_t)ch + (size_t)c) * sizeof(float), sizeof(float));
                mono += v;
            } else {
                int16_t v;
                memcpy(&v, data + (f * (size_t)ch + (size_t)c) * sizeof(int16_t), sizeof(int16_t));
                mono += (float)v / 32768.0f;
            }
        }
        mono /= (float)ch;
        if (!isfinite(mono)) mono = 0.0f;
        /* Emit every output sample that falls between prev (t=0) and this input frame (t=1). */
        while (deck->phase <= 1.0) {
            float s = deck->prev + (mono - deck->prev) * (float)deck->phase;
            if (s != s) s = 0.0f; /* NaN from a float decoder would make the int16 conversion undefined */
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            out[out_n++] = (int16_t)(s * 32767.0f);
            if (out_n == (int)(sizeof(out) / sizeof(out[0]))) {
                ring_push(deck, out, out_n);
                out_n = 0;
            }
            deck->phase += step;
        }
        deck->phase -= 1.0;
        deck->prev = mono;
    }
    if (out_n > 0) ring_push(deck, out, out_n);
}

static void read_output_format(FvRadioDeck *deck, AMediaFormat *format) {
    int32_t v = 0;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) deck->in_rate = v;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) deck->in_channels = v;
    deck->pcm_encoding = 2;
    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, &v)) deck->pcm_encoding = v;
}

/* Counts other decks that are actually playable (live and primed), not merely connected. */
static int live_decks_other_than(FvRadioDeck *self) {
    int live = 0;
    for (int d = 0; d < FV_RADIO_DECKS; d++) {
        FvRadioDeck *other = &g_decks[d];
        if (other == self || atomic_load(&other->state) != FV_RADIO_LIVE) continue;
        pthread_mutex_lock(&other->lock);
        if (other->primed) live++;
        pthread_mutex_unlock(&other->lock);
    }
    return live;
}

/*
 * Leaves LIVE only while another deck is playable. The check and the state change happen under
 * g_pick_mutex so two decks whose dwell expires together cannot both see the other and rotate out.
 */
static bool try_rotate_out(FvRadioDeck *deck) {
    pthread_mutex_lock(&g_pick_mutex);
    const bool other_live = live_decks_other_than(deck) > 0;
    if (other_live) atomic_store(&deck->state, FV_RADIO_CONNECTING);
    pthread_mutex_unlock(&g_pick_mutex);
    return other_live;
}

/* Streams one station until its dwell ends, the sweep is disabled, or the stream fails. */
static bool stream_station(FvRadioDeck *deck, int station) {
    const FvBlobStationEntry *entry = entry_at(station);
    char url[sizeof(entry->url) + 1];
    memcpy(url, entry->url, sizeof(entry->url));
    url[sizeof(entry->url)] = '\0';

    AMediaExtractor *extractor = AMediaExtractor_new();
    AMediaCodec *codec = NULL;
    bool got_audio = false;
    if (!extractor) return false;

    const long open_started = now_ms();
    if (AMediaExtractor_setDataSource(extractor, url) != AMEDIA_OK) {
        LOGW("deck %d: open failed for %s after %ld ms", (int)(deck - g_decks), entry->callsign,
             now_ms() - open_started);
        AMediaExtractor_delete(extractor);
        return false;
    }

    int track = -1;
    char mime[64] = {0};
    AMediaFormat *format = NULL;
    const size_t tracks = AMediaExtractor_getTrackCount(extractor);
    for (size_t t = 0; t < tracks && track < 0; t++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(extractor, t);
        const char *m = NULL;
        if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && strncmp(m, "audio/", 6) == 0) {
            track = (int)t;
            format = f;
            snprintf(mime, sizeof(mime), "%s", m);
        } else if (f) {
            AMediaFormat_delete(f);
        }
    }
    if (track < 0) {
        LOGW("deck %d: no audio track in %s", (int)(deck - g_decks), entry->callsign);
        AMediaExtractor_delete(extractor);
        return false;
    }

    AMediaExtractor_selectTrack(extractor, (size_t)track);
    codec = AMediaCodec_createDecoderByType(mime);
    if (!codec || AMediaCodec_configure(codec, format, NULL, NULL, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec) != AMEDIA_OK) {
        LOGW("deck %d: decoder setup failed for %s (%s)", (int)(deck - g_decks), entry->callsign, mime);
        if (codec) AMediaCodec_delete(codec);
        AMediaFormat_delete(format);
        AMediaExtractor_delete(extractor);
        return false;
    }

    deck->in_rate = OUT_RATE;
    deck->in_channels = 1;
    read_output_format(deck, format);
    AMediaFormat_delete(format);
    deck->phase = 0.0;
    deck->prev = 0.0f;
    ring_reset(deck, station);

    /* The first dwell is staggered per deck so the decks never all rotate out at the same time. */
    const long stagger = deck->dwell_count++ == 0 ? (long)(deck - g_decks) * (DWELL_MIN_MS / FV_RADIO_DECKS) : 0L;
    pthread_mutex_lock(&g_pick_mutex); /* g_rand is shared by every deck thread */
    const long jitter = (long)(next_rand() % (uint32_t)DWELL_JITTER_MS);
    pthread_mutex_unlock(&g_pick_mutex);
    const long dwell_until = now_ms() + DWELL_MIN_MS + stagger + jitter;
    long last_audio_ms = now_ms();
    long max_gap_ms = 0;
    bool input_eos = false;
    LOGI("deck %d: connected %s [slot %d] %s in %ld ms", (int)(deck - g_decks), entry->callsign, station, mime,
         now_ms() - open_started);

    while (atomic_load(&g_enabled)) {
        const long now = now_ms();
        /* Rotate only while another deck is live, so the sweep never goes silent during a hop. */
        if (now >= dwell_until && got_audio && try_rotate_out(deck)) break;
        if (now - last_audio_ms > NO_AUDIO_TIMEOUT_MS) {
            LOGW("deck %d: %s stalled, rotating", (int)(deck - g_decks), entry->callsign);
            break;
        }

        if (!input_eos) {
            const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (in_idx >= 0) {
                size_t cap = 0;
                uint8_t *buf = AMediaCodec_getInputBuffer(codec, (size_t)in_idx, &cap);
                const ssize_t n = buf ? AMediaExtractor_readSampleData(extractor, buf, cap) : -1;
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(codec, (size_t)in_idx, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    input_eos = true;
                } else {
                    AMediaCodec_queueInputBuffer(codec, (size_t)in_idx, 0, (size_t)n,
                                                 (uint64_t)AMediaExtractor_getSampleTime(extractor), 0);
                    AMediaExtractor_advance(extractor);
                }
            }
        }

        AMediaCodecBufferInfo info;
        const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (out_idx >= 0) {
            if (info.size > 0) {
                size_t cap = 0;
                const uint8_t *buf = AMediaCodec_getOutputBuffer(codec, (size_t)out_idx, &cap);
                if (buf && info.offset >= 0 && info.size > 0 && (size_t)info.offset + (size_t)info.size <= cap) {
                    push_decoded(deck, buf + info.offset, (size_t)info.size);
                    const long arrival = now_ms();
                    const long gap_ms = arrival - last_audio_ms;
                    if (got_audio && gap_ms > max_gap_ms) {
                        // A new longest wait between chunks: size the prebuffer to ride out the next one.
                        max_gap_ms = gap_ms;
                        long want = gap_ms * (OUT_RATE / 1000) * 13 / 10;
                        if (want < PREBUFFER_MIN_FRAMES) want = PREBUFFER_MIN_FRAMES;
                        if (want > PREBUFFER_MAX_FRAMES) want = PREBUFFER_MAX_FRAMES;
                        pthread_mutex_lock(&deck->lock);
                        deck->prime_frames = (uint32_t)want;
                        pthread_mutex_unlock(&deck->lock);
                    }
                    last_audio_ms = arrival;
                    if (!got_audio) {
                        got_audio = true;
                        atomic_store(&deck->state, FV_RADIO_LIVE);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out_idx, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *out_format = AMediaCodec_getOutputFormat(codec);
            if (out_format) {
                read_output_format(deck, out_format);
                AMediaFormat_delete(out_format);
            }
        }
    }

    atomic_store(&deck->state, FV_RADIO_CONNECTING);
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaExtractor_delete(extractor);
    ring_reset(deck, -1);
    return got_audio;
}

static void *deck_thread(void *arg) {
    FvRadioDeck *deck = (FvRadioDeck *)arg;
    const int self = (int)(deck - g_decks);
    JNIEnv *env = NULL;
    /* AMediaExtractor_setDataSource(url) uses the platform HTTP service, which needs a JVM-attached thread. */
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
        LOGW("deck %d: JVM attach failed; live radio unavailable", self);
        return NULL;
    }
    for (;;) {
        if (!atomic_load(&g_enabled)) {
            atomic_store(&deck->state, FV_RADIO_IDLE);
            atomic_store(&deck->station, -1);
            usleep(150000);
            continue;
        }
        atomic_store(&deck->state, FV_RADIO_CONNECTING);
        const int station = pick_station(self);
        if (station < 0) {
            atomic_store(&deck->station, -1);
            usleep(500000);
            continue;
        }
        if (!stream_station(deck, station)) {
            mark_failed(station);
            usleep(250000);
        }
        atomic_store(&deck->station, -1);
    }
    return NULL;
}

bool fv_radio_start(JavaVM *vm, int station_count) {
    if (!vm || station_count <= 0) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_started, &expected, true)) return true;
    g_vm = vm;
    g_station_count = station_count > (int)FV_STATION_BLOB_COUNT ? (int)FV_STATION_BLOB_COUNT : station_count;
    g_rand ^= (uint32_t)now_ms();
    g_cursor = -1;
    int started = 0;
    for (int d = 0; d < FV_RADIO_DECKS; d++) {
        pthread_mutex_init(&g_decks[d].lock, NULL);
        g_decks[d].pcm_station = -1;
        atomic_init(&g_decks[d].state, FV_RADIO_IDLE);
        atomic_init(&g_decks[d].station, -1);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        /* Decks live for the process and may be blocked in network I/O, so they are never joined. */
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&g_decks[d].thread, &attr, deck_thread, &g_decks[d]) == 0) started++;
        pthread_attr_destroy(&attr);
    }
    LOGI("live radio engine started: %d decks over %d stations", started, g_station_count);
    return started > 0;
}

void fv_radio_set_enabled(bool enabled) {
    atomic_store(&g_enabled, enabled);
}

void fv_radio_set_direction(int direction) {
    atomic_store(&g_direction, direction > 0 ? 1 : (direction < 0 ? -1 : 0));
}

int fv_radio_pull_with_station(int deck_index, int16_t *out, int frames, int *station) {
    if (station) *station = -1;
    if (!out || frames <= 0) return 0;
    memset(out, 0, (size_t)frames * sizeof(int16_t));
    if (deck_index < 0 || deck_index >= FV_RADIO_DECKS || !atomic_load(&g_started)) return 0;
    FvRadioDeck *deck = &g_decks[deck_index];
    pthread_mutex_lock(&deck->lock);
    if (deck->count > MAX_BUFFERED_FRAMES) {
        const uint32_t drop = deck->count - TRIM_TO_FRAMES;
        deck->read_pos = (deck->read_pos + drop) % RING_FRAMES;
        deck->count -= drop;
    }
    if (!deck->primed && deck->count >= deck->prime_frames) deck->primed = true;
    if (!deck->primed || (int)deck->count < frames) {
        /* Prebuffering, or an underrun: keep the partial audio and rebuild the cushion before playing. */
        deck->primed = false;
        pthread_mutex_unlock(&deck->lock);
        return 0;
    }
    const int n = frames;
    for (int i = 0; i < n; i++) {
        out[i] = deck->ring[deck->read_pos];
        deck->read_pos = (deck->read_pos + 1) % RING_FRAMES;
    }
    deck->count -= (uint32_t)n;
    if (station) *station = deck->pcm_station;
    pthread_mutex_unlock(&deck->lock);
    return n;
}

int fv_radio_pull(int deck_index, int16_t *out, int frames) {
    return fv_radio_pull_with_station(deck_index, out, frames, NULL);
}

int fv_radio_deck_state(int deck) {
    if (deck < 0 || deck >= FV_RADIO_DECKS || !atomic_load(&g_started)) return FV_RADIO_IDLE;
    return atomic_load(&g_decks[deck].state);
}

int fv_radio_deck_station(int deck) {
    if (deck < 0 || deck >= FV_RADIO_DECKS || !atomic_load(&g_started)) return -1;
    return atomic_load(&g_decks[deck].station);
}

bool fv_radio_deck_ready(int deck, int frames) {
    if (fv_radio_deck_state(deck) != FV_RADIO_LIVE) return false;
    pthread_mutex_lock(&g_decks[deck].lock);
    const bool ready = g_decks[deck].count >= (uint32_t)frames;
    pthread_mutex_unlock(&g_decks[deck].lock);
    return ready;
}

int fv_radio_live_count(void) {
    int live = 0;
    for (int d = 0; d < FV_RADIO_DECKS; d++) {
        if (fv_radio_deck_state(d) == FV_RADIO_LIVE) live++;
    }
    return live;
}

int fv_radio_deck_buffered(int deck) {
    if (deck < 0 || deck >= FV_RADIO_DECKS || !atomic_load(&g_started)) return 0;
    pthread_mutex_lock(&g_decks[deck].lock);
    const int count = (int)g_decks[deck].count;
    pthread_mutex_unlock(&g_decks[deck].lock);
    return count;
}
