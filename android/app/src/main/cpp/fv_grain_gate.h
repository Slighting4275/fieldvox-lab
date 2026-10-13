#ifndef FIELDVOX_GRAIN_GATE_H
#define FIELDVOX_GRAIN_GATE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Bounded phonetic-grain gate, ported from the Java carrier (openOnlineCarrierGate and
 * startOnlineCarrierAutoGate). The carrier station plays muted; a gate "packet" opens it for
 * 1-5 short grains (65-390 ms, 30-260 ms apart), then forces 1.1-4.7 s of silence before the
 * next packet. Packets come from a 1.7-6.0 s auto-scan timer or from sensor spikes. When a
 * packet closes the carrier advances to the next station 350 ms later.
 *
 * All times are in samples of the caller's clock so grains are sample accurate.
 */

#define FV_GATE_MAX_GRAINS 5

typedef enum {
    FV_GATE_AUTO_SCAN = 0,
    FV_GATE_MAGNETOMETER,
    FV_GATE_GYROSCOPE,
    FV_GATE_VIBE,
    FV_GATE_PROXIMITY,
    FV_GATE_REASON_COUNT
} FvGateReason;

typedef struct {
    uint32_t sample_rate;
    uint32_t rng;
    bool auto_enabled;
    int grain_ms; /* 0 retains the original varied durations; otherwise 65..1000 ms. */

    bool open;
    uint64_t packet_start;
    uint64_t packet_len;
    int grain_count;
    uint64_t grain_start[FV_GATE_MAX_GRAINS];
    uint64_t grain_len[FV_GATE_MAX_GRAINS];
    FvGateReason reason;

    uint64_t next_allowed;
    uint64_t last_gate;
    bool has_last_gate;
    uint64_t next_auto;
    uint64_t advance_at; /* 0 = no pending station advance */

    bool proximity_pending;
    float proximity_delta;

    /* FieldVox Lockout scaled by reverb mix */
    float reverb_mix;
} FvGrainGate;

/* Called once. `now` is the current sample clock. */
void fv_gate_init(FvGrainGate *gate, uint32_t sample_rate, uint32_t seed, uint64_t now);

/* Sets the reverb mix level (0.0..1.0), scaling the trigger lockout to (2000 ms + 8000 ms * mix). */
void fv_gate_set_reverb_mix(FvGrainGate *gate, float reverb_mix);

/* A new carrier is ready: first auto-scan packet 1.2 s later (Java startOnlineCarrierAutoGate). */
void fv_gate_carrier_started(FvGrainGate *gate, uint64_t now);

/* Disable timer packets immediately; sensor packets retain their normal cooldowns. */
void fv_gate_set_auto_enabled(FvGrainGate *gate, bool enabled, uint64_t now);
void fv_gate_set_grain_ms(FvGrainGate *gate, int milliseconds);

/* Sensitivity is expressed directly in microteslas, matching the UI presets. */
bool fv_gate_emf_exceeds_threshold(float delta, float threshold);

/*
 * Sensor or auto-scan request. Mirrors the Java cooldowns: the gate must be closed, past its
 * forced silence, and past the per-sensor cooldown (which grows with `cooldown_ms`). Proximity
 * bypasses the forced silence and is queued while a packet is open. Returns true if a packet opened.
 */
bool fv_gate_request(FvGrainGate *gate, uint64_t now, FvGateReason reason, float delta,
                     int cooldown_ms, bool carrier_ready);

/*
 * Fills `gain` (0..1) for `frames` samples starting at `now`, running the auto-scan timer and the
 * queued proximity request. Returns true when the carrier should advance to the next station.
 */
bool fv_gate_process(FvGrainGate *gate, uint64_t now, float *gain, int frames, bool carrier_ready,
                     int cooldown_ms);

const char *fv_gate_reason_name(FvGateReason reason);

#endif
