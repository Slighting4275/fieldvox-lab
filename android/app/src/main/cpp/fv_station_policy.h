/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_STATION_POLICY_H
#define FV_STATION_POLICY_H
#include "fv_radio_playlist.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FV_STATION_GROUP_SIZE 10u
#define FV_STATION_MAX 512u

typedef enum {
    FV_RADIO_BANK_EN_US = 0,
    FV_RADIO_BANK_EN_UK,
    FV_RADIO_BANK_EN_AU,
    FV_RADIO_BANK_EN,
    FV_RADIO_BANK_ALL,
    FV_RADIO_BANK_COUNT
} FvRadioBank;

typedef enum {
    FV_STATION_UNTESTED = 0,
    FV_STATION_OK = 1,
    FV_STATION_FAILED = 2
} FvStationHealth;

typedef struct {
    char callsign[32];
    char name[64];
    char country[4];
    char lang[8];
    char url[FV_RADIO_URL_MAX];
} FvDynamicStation;

const char *fv_station_bank_code(FvRadioBank bank);
const char *fv_station_bank_label(FvRadioBank bank);
bool fv_station_url_is_valid(const char *url);

/* Language strings are at most seven bytes plus a terminator. */
size_t fv_station_group(const char (*languages)[8], size_t count, const char *language,
                        uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]);

size_t fv_station_group_healthy(const char (*languages)[8], const char *const *urls,
                                const uint8_t *health, size_t count, const char *language,
                                uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]);

int fv_station_replace_failed(const char (*languages)[8], const char *const *urls,
                              const uint8_t *health, size_t count, const char *language,
                              uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE],
                              size_t group_count, int failed_station);

size_t fv_station_parse_radiobrowser_json(const char *json, size_t json_len,
                                          FvDynamicStation *out, size_t capacity);
#endif
