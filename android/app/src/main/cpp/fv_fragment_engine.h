/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
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
    float pitch_override; /* 0.0f = trigger-driven; otherwise 0.6f, 1.0f, or 1.2f. */
    bool noise;
    bool paused; /* Suspends fragment playback, static, and effect tails. */
    bool auto_enabled;
    bool local_only;
    unsigned sweep_ms;
} FvFragmentSettings;
typedef struct {
    int station;
    unsigned voices, cache_count, admitted;
    bool playing, cached, live_pending;
    float pitch, speed;
    int grain_ms;
    float seek_fraction;
    FvGateReason reason;
} FvFragmentStatus;

/* Optional speaker adapter receives the whole source, never a truncated grain.
 * Source descriptors and PCM are borrowed during the callback only. */
typedef bool (*FvFragmentSpeaker)(void *context, int source_fd, const int16_t *pcm,
    size_t frames, const FvVoxDecimalTrigger *trigger, FvFragmentSettings settings);

/* Optional live radio adapter fetched asynchronously behind the engine seam. */
typedef struct {
    void *context;
    bool (*is_busy)(void *context);
    int (*select_station)(void *context, int channel_hop);
    bool (*request)(void *context, int channel_hop, const FvVoxDecimalTrigger *trigger);
    bool (*take_result)(void *context, int16_t **pcm, size_t *frames, int *station,
                        FvVoxDecimalTrigger *trigger, int *source_fd);
} FvFragmentLiveSource;

/* Optional persistent store adapter called on fresh live clip arrival. */
typedef struct {
    void *context;
    bool (*submit)(void *context, const int16_t *pcm, size_t frames, int station, int source_fd);
} FvFragmentClipStore;

/* Optional local-bank slice provider when local_only is active. */
typedef struct {
    void *context;
    size_t (*read_fragment)(void *context, float seek_offset, size_t frames,
                            int16_t *output, size_t capacity);
} FvFragmentLocalProvider;

typedef struct {
    bool admitted;
    bool request_started;
    bool used_auto_trigger;
    FvGateReason reason;
} FvFragmentStepResult;

FvFragmentEngine *fv_fragment_create(void);
void fv_fragment_destroy(FvFragmentEngine *engine);
void fv_fragment_reset_playback(FvFragmentEngine *engine);
void fv_fragment_set_settings(FvFragmentEngine *engine, FvFragmentSettings settings);
void fv_fragment_set_speaker(FvFragmentEngine *engine, FvFragmentSpeaker speaker, void *context);
void fv_fragment_set_live_source(FvFragmentEngine *engine, FvFragmentLiveSource live_source);
void fv_fragment_set_clip_store(FvFragmentEngine *engine, FvFragmentClipStore clip_store);

bool fv_fragment_cache_add(FvFragmentEngine *engine, const int16_t *pcm, size_t frames,
                           int station, int source_fd);
size_t fv_fragment_cache_count(const FvFragmentEngine *engine);

void fv_fragment_submit_trigger(FvFragmentEngine *engine, FvGateReason reason,
                                const FvVoxDecimalTrigger *trigger);
bool fv_fragment_step(FvFragmentEngine *engine, uint64_t clock, bool speech,
                      const FvFragmentLocalProvider *local, FvFragmentStepResult *out_result);

bool fv_fragment_trigger(FvFragmentEngine *engine, const FvVoxDecimalTrigger *trigger,
                         bool local_only, uint64_t clock, bool speech);
bool fv_fragment_trigger_pcm(FvFragmentEngine *engine, const FvVoxDecimalTrigger *trigger,
                             const int16_t *pcm, size_t frames, int station, uint64_t clock, bool speech);
bool fv_fragment_audition_pcm(FvFragmentEngine *engine, const int16_t *pcm, size_t frames,
                              int station, int source_fd, uint64_t clock);
bool fv_fragment_live_pending(const FvFragmentEngine *engine);
void fv_fragment_render(FvFragmentEngine *engine, int16_t *clean, int16_t *processed, int16_t *aux,
                        size_t frames, uint64_t clock, FvFragmentStatus *status);
size_t fv_fragment_transform(const int16_t *source, size_t frames, float pitch, float speed,
                             int16_t *output, size_t capacity);
#endif
