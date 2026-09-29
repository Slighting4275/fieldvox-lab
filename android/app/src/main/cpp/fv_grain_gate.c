/*
 * FieldVox bounded phonetic-grain gate (C). See fv_grain_gate.h.
 * Timing constants and random ranges match the Java MainActivity carrier gate.
 */

#include "fv_grain_gate.h"

#include <math.h>
#include <string.h>

/* Grains fade in and out over 2.5 ms so hard gate edges do not click. */
#define FV_GATE_RAMP_MS 2.5f

static uint64_t ms_to_samples(const FvGrainGate *gate, double ms) {
    if (!gate || gate->sample_rate == 0 || ms <= 0.0) return 0;
    return (uint64_t)(ms * (double)gate->sample_rate / 1000.0);
}

/* Uniform in [0, 1), like Java's Math.random(). */
static double gate_random(FvGrainGate *gate) {
    uint32_t x = gate->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gate->rng = x;
    return (double)x / 4294967296.0;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

const char *fv_gate_reason_name(FvGateReason reason) {
    switch (reason) {
        case FV_GATE_AUTO_SCAN: return "auto-scan";
        case FV_GATE_MAGNETOMETER: return "magnetometer";
        case FV_GATE_GYROSCOPE: return "gyroscope";
        case FV_GATE_VIBE: return "vibe-geophone";
        case FV_GATE_PROXIMITY: return "proximity";
        default: return "unknown";
    }
}

void fv_gate_init(FvGrainGate *gate, uint32_t sample_rate, uint32_t seed, uint64_t now) {
    if (!gate) return;
    memset(gate, 0, sizeof(*gate));
    gate->sample_rate = sample_rate > 0 ? sample_rate : 48000u;
    gate->rng = seed != 0 ? seed : 0x9E3779B9u;
    gate->next_allowed = now;
    gate->next_auto = now + ms_to_samples(gate, 1200.0);
}

void fv_gate_carrier_started(FvGrainGate *gate, uint64_t now) {
    if (!gate) return;
    const uint64_t first = now + ms_to_samples(gate, 1200.0);
    if (!gate->open && gate->next_auto < first) gate->next_auto = first;
}

/* Builds and opens one packet (Java openOnlineCarrierGate after its admission checks). */
static void open_packet(FvGrainGate *gate, uint64_t now, FvGateReason reason, float delta) {
    int grains;
    switch (reason) {
        case FV_GATE_PROXIMITY: grains = 2 + (int)(gate_random(gate) * 2.0); break;
        case FV_GATE_MAGNETOMETER: grains = 2 + clamp_int((int)fmaxf(0.0f, delta / 2.0f), 0, 3); break;
        case FV_GATE_GYROSCOPE: grains = 1 + clamp_int((int)fmaxf(0.0f, delta * 6.0f), 0, 3); break;
        case FV_GATE_AUTO_SCAN: grains = 1 + (int)(gate_random(gate) * 3.0); break;
        default: grains = 2; break;
    }
    grains = clamp_int(grains, 1, FV_GATE_MAX_GRAINS);

    const float signature = fminf(1.0f, fabsf(delta) / 8.0f);
    uint64_t offset = 0;
    for (int i = 0; i < grains; i++) {
        int d = 95 + (int)(gate_random(gate) * 210.0);
        int gap = 45 + (int)(gate_random(gate) * 155.0);
        if (reason == FV_GATE_PROXIMITY) {
            d += 35;
            gap += 25;
        } else if (reason == FV_GATE_MAGNETOMETER) {
            d += (int)(signature * 80.0f);
        } else if (reason == FV_GATE_GYROSCOPE) {
            d = d - 30 > 75 ? d - 30 : 75;
            gap = gap - 15 > 35 ? gap - 15 : 35;
        }
        d = clamp_int(d, 65, 390);
        gap = clamp_int(gap, 30, 260);
        gate->grain_start[i] = offset;
        gate->grain_len[i] = ms_to_samples(gate, d);
        offset += gate->grain_len[i];
        if (i + 1 < grains) offset += ms_to_samples(gate, gap);
    }

    int forced_silence_ms = 1100 + (int)(gate_random(gate) * 3600.0);
    if (reason == FV_GATE_PROXIMITY) forced_silence_ms += 600;

    gate->open = true;
    gate->reason = reason;
    gate->grain_count = grains;
    gate->packet_start = now;
    gate->packet_len = offset;
    gate->last_gate = now;
    gate->has_last_gate = true;
    gate->next_allowed = now + offset + ms_to_samples(gate, forced_silence_ms);
    gate->advance_at = 0;
}

static uint64_t sensor_cooldown_ms(FvGateReason reason, int cooldown_ms) {
    switch (reason) {
        case FV_GATE_VIBE: return (uint64_t)(cooldown_ms + 1000 > 1600 ? cooldown_ms + 1000 : 1600);
        case FV_GATE_MAGNETOMETER: return (uint64_t)(cooldown_ms + 1200 > 1800 ? cooldown_ms + 1200 : 1800);
        case FV_GATE_GYROSCOPE: return (uint64_t)(cooldown_ms + 1300 > 1900 ? cooldown_ms + 1300 : 1900);
        default: return 0;
    }
}

bool fv_gate_request(FvGrainGate *gate, uint64_t now, FvGateReason reason, float delta,
                     int cooldown_ms, bool carrier_ready) {
    if (!gate) return false;
    if (reason == FV_GATE_PROXIMITY) {
        // Proximity is an intentional user gate: it skips the forced silence but never overlaps a packet.
        if (carrier_ready && !gate->open) {
            if (gate->next_allowed > now) gate->next_allowed = now;
            open_packet(gate, now, reason, delta);
            return true;
        }
        gate->proximity_pending = true;
        gate->proximity_delta = delta;
        return false;
    }
    if (!carrier_ready) {
        // Java: while the carrier is still preparing, hold the next gate back by 500 ms.
        const uint64_t hold = now + ms_to_samples(gate, 500.0);
        if (gate->next_allowed < hold) gate->next_allowed = hold;
        return false;
    }
    if (gate->open || now < gate->next_allowed) return false;
    const uint64_t cooldown = ms_to_samples(gate, (double)sensor_cooldown_ms(reason, cooldown_ms));
    if (cooldown > 0 && gate->has_last_gate && now - gate->last_gate < cooldown) return false;
    open_packet(gate, now, reason, delta);
    return true;
}

bool fv_gate_process(FvGrainGate *gate, uint64_t now, float *gain, int frames, bool carrier_ready,
                     int cooldown_ms) {
    if (!gate || frames <= 0) return false;
    bool advance = false;

    if (!gate->open && gate->proximity_pending && carrier_ready) {
        gate->proximity_pending = false;
        fv_gate_request(gate, now, FV_GATE_PROXIMITY, gate->proximity_delta, cooldown_ms, true);
    }
    if (now >= gate->next_auto) {
        // Auto-scan fires every 1.7-6.0 s whether or not the gate admits it, as in Java.
        if (carrier_ready) fv_gate_request(gate, now, FV_GATE_AUTO_SCAN, 0.0f, cooldown_ms, true);
        gate->next_auto = now + ms_to_samples(gate, 1700.0 + gate_random(gate) * 4300.0);
    }

    const uint64_t ramp = ms_to_samples(gate, FV_GATE_RAMP_MS) > 0 ? ms_to_samples(gate, FV_GATE_RAMP_MS) : 1;
    for (int i = 0; i < frames; i++) {
        const uint64_t t = now + (uint64_t)i;
        float g = 0.0f;
        if (gate->open) {
            const uint64_t offset = t - gate->packet_start;
            if (offset >= gate->packet_len) {
                gate->open = false;
                gate->advance_at = gate->packet_start + gate->packet_len + ms_to_samples(gate, 350.0);
            } else {
                for (int k = 0; k < gate->grain_count; k++) {
                    const uint64_t start = gate->grain_start[k];
                    const uint64_t len = gate->grain_len[k];
                    if (offset < start || offset >= start + len) continue;
                    const uint64_t into = offset - start;
                    const uint64_t left = start + len - offset;
                    const uint64_t edge = into < left ? into : left;
                    g = edge >= ramp ? 1.0f : (float)edge / (float)ramp;
                    break;
                }
            }
        }
        if (gain) gain[i] = g;
    }

    const uint64_t end = now + (uint64_t)frames;
    if (gate->advance_at != 0 && end >= gate->advance_at) {
        gate->advance_at = 0;
        advance = true;
    }
    return advance;
}
