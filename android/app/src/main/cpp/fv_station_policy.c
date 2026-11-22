/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_station_policy.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool matches(const char *station, const char *language) {
    if (strcmp(station, language) == 0) return true;
    if (strcmp(station, strncmp(language, "en", 2) == 0 ? "en" : "00") == 0) return true;
    if (strcmp(language, "nb") == 0) return strcmp(station, "sv") == 0 || strcmp(station, "da") == 0;
    if (strcmp(language, "da") == 0) return strcmp(station, "nb") == 0 || strcmp(station, "sv") == 0;
    if (strcmp(language, "cs") == 0) return strcmp(station, "pl") == 0;
    if (strcmp(language, "pl") == 0) return strcmp(station, "cs") == 0;
    return false;
}
size_t fv_station_group(const char (*languages)[8], size_t count, const char *language,
                        uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]) {
    if (!languages || !language || !group || !count || count > 512u) return 0;
    char lang[8] = {0};
    for (size_t i = 0; i < 7u && language[i]; i++) {
        const unsigned char c = (unsigned char)language[i];
        lang[i] = c == '-' ? '_' : (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    unsigned candidates[512]={0}; size_t n = 0, german = 0;
    for (size_t i = 0; i < count; i++) {
        char station[8]; memcpy(station, languages[i], 8); station[7] = 0;
        if (matches(station, lang) || (strcmp(lang, "nl") == 0 && strcmp(station, "de") == 0 && german++ < 3u))
            candidates[n++] = (unsigned)i;
    }
    uint32_t rng = seed ? seed : 0x9e3779b9u;
    for (size_t i = n; i > 1u; i--) {
        rng ^= rng << 13u; rng ^= rng >> 17u; rng ^= rng << 5u;
        const size_t j = (size_t)rng % i; const unsigned tmp = candidates[i-1u];
        candidates[i-1u] = candidates[j]; candidates[j] = tmp;
    }
    size_t used = n < FV_STATION_GROUP_SIZE ? n : FV_STATION_GROUP_SIZE;
    memcpy(group, candidates, used * sizeof(*group));
    for (size_t i = 0; used < FV_STATION_GROUP_SIZE && i < count; i++) {
        bool present = false;
        for (size_t j = 0; j < used; j++) present |= group[j] == i;
        if (!present) group[used++] = (unsigned)i;
    }
    return used;
}
