/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FIELDVOX_NATIVE_RADIO_H
#define FIELDVOX_NATIVE_RADIO_H
#include <jni.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fv_fragment_engine.h"
#include "fv_station_policy.h"
#define FV_RADIO_DECKS 10

typedef struct {
    int station_index;
    char callsign[32];
    char name[64];
    char lang[8];
    uint8_t health;
    bool active;
} FvRadioSlotSnapshot;

typedef struct {
    FvRadioBank bank;
    size_t total_stations;
    size_t group_count;
    int last_tested_slot;
    int last_refresh_added;
    bool refreshing;
    bool testing;
    FvRadioSlotSnapshot slots[FV_STATION_GROUP_SIZE];
} FvRadioTunerSnapshot;

/* One process-wide, bounded network worker. No continuous background streams. */
bool fv_radio_start(JavaVM *vm, const char *app_directory, const char *language, int station_count);
void fv_radio_set_enabled(bool enabled);
FvFragmentLiveSource fv_radio_live_source(void);
int fv_radio_selected_station(int channel);
bool fv_radio_request(int channel, const FvVoxDecimalTrigger *trigger);
/* Transfers PCM and source descriptor ownership to the audio thread, including a NULL result on failure. */
bool fv_radio_take_result(int16_t **pcm, size_t *frames, int *station, FvVoxDecimalTrigger *trigger, int *source_fd);
bool fv_radio_busy(void);

FvRadioBank fv_radio_get_bank(void);
void fv_radio_cycle_bank(void);
void fv_radio_reshuffle(void);
bool fv_radio_refresh_catalog(void);
bool fv_radio_test_slot(int group_slot);
bool fv_radio_take_test_result(int16_t **pcm, size_t *frames, int *station, int *source_fd);
void fv_radio_tuner_snapshot(FvRadioTunerSnapshot *out);
bool fv_radio_station_info(int station_index, char *out_callsign, size_t callsign_cap,
                           char *out_url, size_t url_cap);
#endif
