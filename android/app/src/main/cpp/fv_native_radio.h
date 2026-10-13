#ifndef FIELDVOX_NATIVE_RADIO_H
#define FIELDVOX_NATIVE_RADIO_H

#include <jni.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Native live-radio carrier engine.
 *
 * FV_RADIO_DECKS worker threads each hold one real station stream from the
 * packed station blob open at a time (AMediaExtractor + AMediaCodec), decode
 * it to 48 kHz mono PCM and keep a short ring buffer. The audio thread pulls
 * every deck each block and the sweep chooses which deck is audible, so the
 * sweep can hop between live stations at 100-1000 ms even though opening a
 * network stream takes seconds. Each deck rotates to the next station in the
 * sweep direction after its dwell expires.
 */

#define FV_RADIO_DECKS 10

enum {
    FV_RADIO_IDLE = 0,
    FV_RADIO_CONNECTING = 1,
    FV_RADIO_LIVE = 2,
};

/* Starts the deck threads once per process. Safe to call again. */
bool fv_radio_start(JavaVM *vm, int station_count);

/* true = decks connect and stream; false = decks close their streams and idle. */
void fv_radio_set_enabled(bool enabled);

/* 1 = forward, -1 = reverse, 0 = random. Controls the next station a deck loads. */
void fv_radio_set_direction(int direction);

/*
 * Copies up to `frames` 48 kHz mono samples from a deck into `out`, zero
 * padding on underrun. Returns the number of real frames delivered.
 */
int fv_radio_pull(int deck, int16_t *out, int frames);

/* Pull PCM and its source identity under the same ring lock. */
int fv_radio_pull_with_station(int deck, int16_t *out, int frames, int *station);

/* Deck state (FV_RADIO_*), and the station it holds (-1 when none). */
int fv_radio_deck_state(int deck);
int fv_radio_deck_station(int deck);

/* True when the deck is live and has at least `frames` buffered. */
bool fv_radio_deck_ready(int deck, int frames);

/* Frames currently buffered in a deck (diagnostics). */
int fv_radio_deck_buffered(int deck);

/* Number of decks currently live. */
int fv_radio_live_count(void);

#define FV_STATION_REFRACTORY_MS 5000L

/*
 * 5-second per-station anti-repeat refractory lockout.
 */
bool fv_radio_station_is_refractory(int station, long now_ms);
void fv_radio_mark_station_triggered(int station, long now_ms);

#endif
