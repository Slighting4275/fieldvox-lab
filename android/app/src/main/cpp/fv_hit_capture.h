#ifndef FIELDVOX_HIT_CAPTURE_H
#define FIELDVOX_HIT_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fv_grain_gate.h"

#define FV_HIT_RATE 48000u
#define FV_HIT_FRAMES (3u * FV_HIT_RATE)

enum {
    FV_HIT_FX_VOCAL = 1u << 0u, FV_HIT_FX_ECHO = 1u << 1u,
    FV_HIT_FX_REVERB = 1u << 2u, FV_HIT_FX_NOISE = 1u << 3u,
    FV_HIT_FX_REVERSE = 1u << 4u, FV_HIT_FX_REMPOD = 1u << 5u,
    FV_HIT_FX_PAN = 1u << 6u, FV_HIT_FX_DISTORTION = 1u << 7u
};

typedef struct {
    uint64_t sample_start;
    bool auto_enabled, microphone, muted;
    bool voice_duck_enabled, speaker_ducked;
    int echo_percent, reverb_percent, distortion_percent, grain_ms;
    int station_a, station_b;
    char label_a[32], label_b[32];
    char url_a[160], url_b[160];
    bool audible_a, audible_b;
    FvGateReason reason_a, reason_b;
    float emf_delta, emf_threshold;
    unsigned effects;
    int pitch_semitones;
    float pitch_factor, playback_speed, seek_fraction;
    bool cached_source;
    unsigned active_voices;
} FvHitBlock;

/* Single owner: call only from the audio thread. Dry PCM is the primary pitched/tempo-adjusted fragment/mic mix
 * before echo, reverb, static and alert tones; processed PCM matches the mono recording bus. */
void fv_hit_capture_reset(void);
void fv_hit_capture_push(const int16_t *dry, const int16_t *processed,
                         uint32_t frames, const FvHitBlock *block);

/* Saves two WAVs plus the historical block timeline. JSON is the completion
 * marker and is published last. On failure, removes only this attempt's files.
 * Existing names are never overwritten. Directory must already exist. */
bool fv_hit_capture_save(const char *directory, uint64_t wall_time_ms, unsigned sequence,
                         char *processed_path, size_t path_capacity);

#endif
