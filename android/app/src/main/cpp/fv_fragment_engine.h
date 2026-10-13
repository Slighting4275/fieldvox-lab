#ifndef FV_FRAGMENT_ENGINE_H
#define FV_FRAGMENT_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fv_sensor_dsp.h"

#define FV_FRAGMENT_RATE 48000u
#define FV_FRAGMENT_DECKS 10
#define FV_FRAGMENT_CACHE_MAX 100
#define FV_FRAGMENT_VOICES 10
#define FV_FRAGMENT_CLIP_FRAMES (FV_FRAGMENT_RATE * 30u)

typedef struct FvFragmentEngine FvFragmentEngine;
typedef struct {
    float echo, reverb;
    bool noise;
    bool paused; /* Suspends fragment playback, static, and effect tails. */
    unsigned sweep_ms;
} FvFragmentSettings;
typedef struct {
    int station;
    unsigned voices, cache_count, admitted;
    bool playing, cached;
    float pitch, speed;
    int grain_ms;
    float seek_fraction;
} FvFragmentStatus;

/* One owner (the audio thread). PCM is always copied, never borrowed. */
FvFragmentEngine *fv_fragment_create(void);
void fv_fragment_destroy(FvFragmentEngine *engine);
void fv_fragment_reset_playback(FvFragmentEngine *engine);
void fv_fragment_set_settings(FvFragmentEngine *engine, FvFragmentSettings settings);
bool fv_fragment_cache_add(FvFragmentEngine *engine, const int16_t *pcm, size_t frames, int station);
size_t fv_fragment_cache_count(const FvFragmentEngine *engine);
const int16_t *fv_fragment_cache_latest(const FvFragmentEngine *engine, size_t *frames, int *station);
/* Returns true whenever a complete four-second live clip was cached. */
bool fv_fragment_feed(FvFragmentEngine *engine, int deck, int station, const int16_t *pcm, size_t frames);
bool fv_fragment_trigger(FvFragmentEngine *engine, const FvVoxDecimalTrigger *trigger,
                         bool local_only, uint64_t clock, bool speech);
bool fv_fragment_trigger_pcm(FvFragmentEngine *engine, const FvVoxDecimalTrigger *trigger,
                             const int16_t *pcm, size_t frames, int station, uint64_t clock, bool speech);
/* Reserve the cooldown before a network request. Completion never restarts it. */
bool fv_fragment_begin_live(FvFragmentEngine *engine, const FvVoxDecimalTrigger *trigger,
                            int station, uint64_t clock, bool speech);
bool fv_fragment_finish_live(FvFragmentEngine *engine, const int16_t *pcm, size_t frames,
                             uint64_t clock, bool speech);
bool fv_fragment_live_pending(const FvFragmentEngine *engine);
void fv_fragment_render(FvFragmentEngine *engine, int16_t *clean, int16_t *processed,
                        size_t frames, uint64_t clock, FvFragmentStatus *status);
/* Deterministic, independent pitch/tempo conversion used by production voices. */
size_t fv_fragment_transform(const int16_t *source, size_t frames, float pitch, float speed,
                             int16_t *output, size_t capacity);
#endif
