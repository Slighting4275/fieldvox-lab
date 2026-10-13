#include "fv_fragment_engine.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

#define MAX_GRAIN 48000u
#define MAX_RENDER (MAX_GRAIN * 2u)
#define WINDOW 1024u
#define HOP (WINDOW / 2u)
#define SEARCH 160
#define COMBS 8
#define COMB_CAP 3200u

typedef struct { int16_t *pcm; size_t frames; int station; unsigned uses; } Clip;
typedef struct { int16_t pcm[FV_FRAGMENT_CLIP_FRAMES]; size_t count; int station; } Deck;
typedef struct {
    int16_t pcm[MAX_RENDER];
    size_t frames;
    uint64_t start, release;
    float volume, send;
    int station;
    bool active, cached, primary;
} Voice;
struct FvFragmentEngine {
    Deck decks[FV_FRAGMENT_DECKS];
    Clip cache[FV_FRAGMENT_CACHE_MAX];
    size_t cache_count;
    Voice voices[FV_FRAGMENT_VOICES];
    FvFragmentSettings settings;
    uint64_t last_trigger, station_trigger[512];
    bool triggered, station_seen[512];
    unsigned admitted;
    int last_station;
    bool last_cached;
    uint32_t rng;
    float pitch, speed;
    int grain_ms;
    float seek_fraction;
    float predelay[2400], comb[COMBS][COMB_CAP], allpass[2][600];
    unsigned predelay_pos, comb_pos[COMBS], allpass_pos[2];
    float feedback[COMBS], diffusion, wet;
};
static constexpr unsigned delays[COMBS] = {1399, 1601, 1777, 1973, 2087, 2357, 2791, 3163};
static constexpr unsigned allpass_delays[2] = {241, 557};

static int16_t saturate(float value) {
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return (int16_t)lrintf(value);
}
static float unit(float value) { return isfinite(value) ? fmaxf(0.0f, fminf(1.0f, value)) : 0.0f; }

/* Pitch resampling followed by waveform similarity overlap-add restores tempo
 * independently. Fractional reads and crossfades avoid the old circular-buffer
 * discontinuities. Work and storage are bounded by MAX_GRAIN/MAX_RENDER. */
size_t fv_fragment_transform(const int16_t *source, size_t frames, float pitch, float speed,
                             int16_t *output, size_t capacity) {
    if (!source || !output || !frames || frames > MAX_GRAIN ||
        !isfinite(pitch) || !isfinite(speed) || pitch < 0.5f || pitch > 2.0f || speed < 0.5f || speed > 2.0f) return 0;
    const size_t wanted = (size_t)llround((double)frames / (double)speed);
    if (!wanted || wanted > capacity || wanted > MAX_RENDER) return 0;
    if (pitch == 1.0f && speed == 1.0f) { memcpy(output, source, frames * sizeof(*output)); return frames; }
    const size_t resampled_frames = (size_t)ceil((double)frames / (double)pitch);
    float *resampled = malloc((resampled_frames + WINDOW) * sizeof(*resampled));
    if (!resampled) return 0;
    for (size_t i = 0; i < resampled_frames; i++) {
        const double p = (double)i * (double)pitch;
        size_t a = (size_t)p;
        if (a >= frames) a = frames - 1u;
        const size_t b = a + 1u < frames ? a + 1u : a;
        resampled[i] = (float)source[a] + ((float)source[b] - (float)source[a]) * (float)(p - floor(p));
    }
    memset(resampled + resampled_frames, 0, WINDOW * sizeof(*resampled));
    memset(output, 0, wanted * sizeof(*output));
    size_t previous = 0;
    for (size_t off = 0; off < wanted; off += HOP) {
        const double nominal = (double)off * (double)speed / (double)pitch;
        size_t position = (size_t)nominal;
        if (position >= resampled_frames) position = resampled_frames - 1u;
        if (off && position + WINDOW < resampled_frames) {
            const size_t low = position > SEARCH ? position - SEARCH : 0u;
            size_t high = position + SEARCH;
            if (high + WINDOW >= resampled_frames) high = resampled_frames - WINDOW;
            double best = -2.0;
            size_t best_position = position;
            for (size_t candidate = low; candidate <= high; candidate += 4u) {
                double dot = 0.0, ea = 1.0, eb = 1.0;
                for (size_t j = 0; j < HOP; j += 4u) {
                    const float a = resampled[previous + HOP + j], b = resampled[candidate + j];
                    dot += (double)a * b; ea += (double)a * a; eb += (double)b * b;
                }
                const double correlation = dot / sqrt(ea * eb);
                if (correlation > best) { best = correlation; best_position = candidate; }
            }
            position = best_position;
        }
        for (size_t j = 0; j < WINDOW && off + j < wanted; j++) {
            const size_t index = position + j;
            const float value = index < resampled_frames ? resampled[index] : 0.0f;
            if (off && j < HOP) {
                const float weight = (float)j / (float)HOP;
                output[off + j] = saturate((float)output[off + j] * (1.0f - weight) + value * weight);
            } else output[off + j] = saturate(value);
        }
        previous = position;
    }
    free(resampled);
    return wanted;
}

FvFragmentEngine *fv_fragment_create(void) {
    FvFragmentEngine *e = calloc(1, sizeof(*e));
    if (e) { e->rng = 0x91873645u; e->last_station = -1; for (int i = 0; i < FV_FRAGMENT_DECKS; i++) e->decks[i].station = -1; }
    return e;
}
void fv_fragment_destroy(FvFragmentEngine *e) {
    if (!e) return;
    for (size_t i = 0; i < e->cache_count; i++) free(e->cache[i].pcm);
    free(e);
}
void fv_fragment_reset_playback(FvFragmentEngine *e) {
    if (!e) return;
    for (unsigned i = 0; i < FV_FRAGMENT_VOICES; i++) e->voices[i].active = false;
    for (unsigned i = 0; i < FV_FRAGMENT_DECKS; i++) e->decks[i].count = 0;
    memset(e->predelay, 0, sizeof(e->predelay)); memset(e->comb, 0, sizeof(e->comb));
    memset(e->allpass, 0, sizeof(e->allpass));
    e->triggered = false;
}
void fv_fragment_set_settings(FvFragmentEngine *e, FvFragmentSettings settings) {
    if (!e) return;
    settings.echo = unit(settings.echo); settings.reverb = unit(settings.reverb);
    if (settings.sweep_ms < 100u) settings.sweep_ms = 100u;
    if (settings.sweep_ms > 1000u) settings.sweep_ms = 1000u;
    if (settings.reverb != e->settings.reverb) {
        const float t60 = fmaxf(0.001f, settings.reverb * 9.0f);
        for (unsigned i = 0; i < COMBS; i++) e->feedback[i] = expf(-6.9077553f * (float)delays[i] / ((float)FV_FRAGMENT_RATE * t60));
        e->diffusion = settings.reverb * 0.7f;
        /* EnvironmentalReverb reverbLevel=(10000*mix)-10000 millibels;
         * roomLevel=-1000 millibels. The native reverberator is an approximation
         * of Android's implementation, not an identical impulse response. */
        e->wet = settings.reverb > 0.0f ? powf(10.0f, ((10000.0f * settings.reverb - 10000.0f) - 1000.0f) / 2000.0f) : 0.0f;
    }
    e->settings = settings;
}
bool fv_fragment_cache_add(FvFragmentEngine *e, const int16_t *pcm, size_t frames, int station) {
    if (!e || !pcm || !frames || frames > FV_FRAGMENT_CLIP_FRAMES) return false;
    int16_t *copy = malloc(frames * sizeof(*copy));
    if (!copy) return false;
    memcpy(copy, pcm, frames * sizeof(*copy));
    if (e->cache_count == FV_FRAGMENT_CACHE_MAX) {
        free(e->cache[0].pcm);
        memmove(e->cache, e->cache + 1, (FV_FRAGMENT_CACHE_MAX - 1u) * sizeof(e->cache[0]));
        e->cache_count--;
    }
    e->cache[e->cache_count++] = (Clip){.pcm = copy, .frames = frames, .station = station};
    return true;
}
size_t fv_fragment_cache_count(const FvFragmentEngine *e) { return e ? e->cache_count : 0u; }
const int16_t *fv_fragment_cache_latest(const FvFragmentEngine *e, size_t *frames, int *station) {
    if (!e || !e->cache_count || !frames || !station) return NULL;
    const Clip *c = &e->cache[e->cache_count - 1u];
    *frames = c->frames; *station = c->station; return c->pcm;
}
bool fv_fragment_feed(FvFragmentEngine *e, int deck, int station, const int16_t *pcm, size_t frames) {
    if (!e || !pcm || deck < 0 || deck >= FV_FRAGMENT_DECKS || station < 0 || frames > FV_FRAGMENT_CLIP_FRAMES) return false;
    Deck *d = &e->decks[deck];
    if (d->station != station) { d->count = 0; d->station = station; }
    bool added = false;
    while (frames) {
        size_t n = FV_FRAGMENT_CLIP_FRAMES - d->count;
        if (n > frames) n = frames;
        memcpy(d->pcm + d->count, pcm, n * sizeof(*pcm)); d->count += n; pcm += n; frames -= n;
        if (d->count == FV_FRAGMENT_CLIP_FRAMES) { added = fv_fragment_cache_add(e, d->pcm, d->count, station) || added; d->count = 0; }
    }
    return added;
}
static bool eligible(FvFragmentEngine *e, int station, uint64_t clock, bool speech) {
    if (speech) return false;
    const uint64_t cooldown = (uint64_t)((2000.0 + 8000.0 * (double)e->settings.reverb) * 48.0);
    if (e->triggered && clock - e->last_trigger < cooldown) return false;
    return station < 0 || station >= 512 || !e->station_seen[station] || clock - e->station_trigger[station] >= 5u * FV_FRAGMENT_RATE;
}
bool fv_fragment_trigger_pcm(FvFragmentEngine *e, const FvVoxDecimalTrigger *t,
                             const int16_t *pcm, size_t frames, int station, uint64_t clock, bool speech) {
    if (!e || !t || !pcm || !frames || !t->passed || clock > UINT64_MAX - 600000u || !eligible(e, station, clock, speech)) return false;
    if (!isfinite(t->pitch_factor) || !isfinite(t->speed_factor) || !isfinite(t->seek_offset) ||
        t->grain_duration_ms < 1 || t->grain_duration_ms > 1000) return false;
    int slot = -1;
    for (unsigned i = 0; i < FV_FRAGMENT_VOICES; i++) {
        if (e->voices[i].active && clock >= e->voices[i].release) e->voices[i].active = false;
        if (slot < 0 && !e->voices[i].active) slot = (int)i;
    }
    if (slot < 0) return false; /* Never steal a playing voice or its tail. */
    const double offset = (double)unit(t->seek_offset) * (double)frames;
    size_t seek = (size_t)offset;
    size_t duration = (size_t)t->grain_duration_ms * 48u;
    if (duration > frames) duration = frames;
    if (seek > frames - duration) seek = frames > duration + 480u ? frames - duration - 480u : 0u;
    Voice *v = &e->voices[slot];
    const size_t count = fv_fragment_transform(pcm + seek, duration, t->pitch_factor, t->speed_factor, v->pcm, MAX_RENDER);
    if (!count) return false;
    v->frames = count; v->start = clock; v->release = clock + count + (uint64_t)(432000.0f * e->settings.reverb);
    v->volume = 1.0f; v->send = e->settings.reverb; v->station = station; v->active = true; v->cached = false; v->primary = true;
    e->last_trigger = clock; e->triggered = true; e->admitted++; e->pitch = t->pitch_factor; e->speed = t->speed_factor;
    e->grain_ms = (int)(duration / 48u); e->seek_fraction = (float)((double)seek / (double)frames);
    e->last_station = station; e->last_cached = false;
    if (station >= 0 && station < 512) { e->station_seen[station] = true; e->station_trigger[station] = clock; }
    if (e->settings.echo > 0.0f) {
        const uint64_t interval = (uint64_t)(e->settings.echo * 200.0f) * 48u;
        for (int tap = 1; tap <= 4; tap++) {
            for (unsigned i = 0; i < FV_FRAGMENT_VOICES; i++) if (!e->voices[i].active) {
                Voice *echo = &e->voices[i];
                memcpy(echo->pcm, v->pcm, count * sizeof(*echo->pcm));
                echo->frames = count; echo->start = clock + (uint64_t)tap * interval;
                echo->release = echo->start + count; echo->volume = fv_vox_echo_tap_volume(1.0f, tap);
                echo->send = 0.0f; echo->station = station; echo->active = true; echo->cached = false; echo->primary = false;
                break;
            }
        }
    }
    return true;
}
bool fv_fragment_trigger(FvFragmentEngine *e, const FvVoxDecimalTrigger *t,
                         bool local_only, uint64_t clock, bool speech) {
    if (!e || !t || speech) return false;
    if (!local_only && t->prefer_live) {
        const unsigned first = (unsigned)t->channel_hop % FV_FRAGMENT_DECKS;
        for (unsigned n = 0; n < FV_FRAGMENT_DECKS; n++) {
            Deck *d = &e->decks[(first + n) % FV_FRAGMENT_DECKS];
            if (d->count >= (size_t)t->grain_duration_ms * 48u &&
                fv_fragment_trigger_pcm(e, t, d->pcm, d->count, d->station, clock, false)) return true;
        }
    }
    if (e->cache_count) {
        const size_t first = (size_t)(unsigned)t->channel_hop % e->cache_count;
        for (size_t n = 0; n < e->cache_count; n++) {
            Clip *c = &e->cache[(first + n) % e->cache_count];
            if (c->uses >= 5u) continue;
            if (fv_fragment_trigger_pcm(e, t, c->pcm, c->frames, c->station, clock, false)) {
                c->uses++;
                e->last_cached = true;
                for (unsigned i = 0; i < FV_FRAGMENT_VOICES; i++) if (e->voices[i].active && e->voices[i].start >= clock) e->voices[i].cached = true;
                return true;
            }
        }
    }
    return false;
}
static float reverb_sample(FvFragmentEngine *e, float input) {
    const float delayed = e->predelay[e->predelay_pos];
    e->predelay[e->predelay_pos] = input;
    e->predelay_pos = (e->predelay_pos + 1u) % 2400u;
    float sum = 0.0f;
    for (unsigned i = 0; i < COMBS; i++) {
        float *cell = &e->comb[i][e->comb_pos[i]];
        const float old = *cell;
        *cell = delayed + old * e->feedback[i]; sum += old;
        e->comb_pos[i] = (e->comb_pos[i] + 1u) % delays[i];
    }
    sum /= (float)COMBS;
    for (unsigned i = 0; i < 2; i++) {
        float *cell = &e->allpass[i][e->allpass_pos[i]];
        const float out = *cell - e->diffusion * sum;
        *cell = sum + e->diffusion * out; sum = out;
        e->allpass_pos[i] = (e->allpass_pos[i] + 1u) % allpass_delays[i];
    }
    return sum * e->wet;
}
void fv_fragment_render(FvFragmentEngine *e, int16_t *clean, int16_t *processed,
                        size_t frames, uint64_t clock, FvFragmentStatus *status) {
    if (!e || !clean || !processed) return;
    FvFragmentStatus s = {.station = e->last_station, .cached = e->last_cached, .cache_count = (unsigned)e->cache_count, .admitted = e->admitted,
        .pitch = e->pitch, .speed = e->speed, .grain_ms = e->grain_ms, .seek_fraction = e->seek_fraction};
    for (size_t i = 0; i < frames; i++) {
        const uint64_t now = clock + i;
        float dry = 0.0f, original = 0.0f, send = 0.0f;
        for (unsigned j = 0; j < FV_FRAGMENT_VOICES; j++) {
            Voice *v = &e->voices[j];
            if (!v->active) continue;
            if (now >= v->release) { v->active = false; continue; }
            if (now < v->start || now - v->start >= v->frames) continue;
            const float sample = (float)v->pcm[now - v->start] * v->volume;
            dry += sample; send += sample * v->send;
            if (v->primary) original += sample;
            s.station = v->station; s.playing = true; s.cached = v->cached;
        }
        clean[i] = saturate(original);
        float noise = 0.0f;
        const uint64_t ms = now / 48u;
        if (e->settings.noise && ms % ((uint64_t)e->settings.sweep_ms + 40u) < e->settings.sweep_ms) {
            e->rng ^= e->rng << 13u; e->rng ^= e->rng >> 17u; e->rng ^= e->rng << 5u;
            noise = (((float)(e->rng & 65535u) / 65535.0f) * 0.4f - 0.2f) * 32767.0f * 0.02f;
        }
        processed[i] = saturate(dry + reverb_sample(e, send) + noise);
    }
    for (unsigned i = 0; i < FV_FRAGMENT_VOICES; i++) if (e->voices[i].active) s.voices++;
    if (status) *status = s;
}
