/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_station_policy.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

static int ascii_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (int)(c + ('a' - 'A')) : (int)c;
}

static void normalize_lang(const char *src, char dst[8]) {
    memset(dst, 0, 8u);
    if (!src) return;
    for (size_t i = 0; i < 7u && src[i] && src[i] != ','; i++) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = c == '-' ? '_' : (char)ascii_tolower(c);
    }
}

static bool matches(const char *station, const char *language) {
    if (strcmp(language, "all") == 0 || strcmp(language, "*") == 0) return true;
    if (strcmp(station, language) == 0) return true;
    if (strcmp(station, strncmp(language, "en", 2) == 0 ? "en" : "00") == 0) return true;
    if (strcmp(language, "nb") == 0) return strcmp(station, "sv") == 0 || strcmp(station, "da") == 0;
    if (strcmp(language, "da") == 0) return strcmp(station, "nb") == 0 || strcmp(station, "sv") == 0;
    if (strcmp(language, "cs") == 0) return strcmp(station, "pl") == 0;
    if (strcmp(language, "pl") == 0) return strcmp(station, "cs") == 0;
    return false;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

static void shuffle_u32(unsigned *arr, size_t n, uint32_t seed) {
    uint32_t rng = seed ? seed : 0x9e3779b9u;
    for (size_t i = n; i > 1u; i--) {
        const size_t j = (size_t)xorshift32(&rng) % i;
        const unsigned tmp = arr[i - 1u];
        arr[i - 1u] = arr[j];
        arr[j] = tmp;
    }
}

const char *fv_station_bank_code(FvRadioBank bank) {
    static const char *const codes[FV_RADIO_BANK_COUNT] = {"en_us", "en_uk", "en_au", "en", "all"};
    return (unsigned)bank < (unsigned)FV_RADIO_BANK_COUNT ? codes[bank] : "en_us";
}

const char *fv_station_bank_label(FvRadioBank bank) {
    static const char *const labels[FV_RADIO_BANK_COUNT] = {"US", "UK", "AU", "EN", "ALL"};
    return (unsigned)bank < (unsigned)FV_RADIO_BANK_COUNT ? labels[bank] : "US";
}

bool fv_station_url_is_valid(const char *url) {
    if (!url || !*url) return false;
    size_t off = 0;
    if (fv_radio_ascii_ieq_n(url, "https://", 8u)) off = 8u;
    else if (fv_radio_ascii_ieq_n(url, "http://", 7u)) off = 7u;
    else return false;
    if (!url[off] || url[off] == '/') return false;
    const size_t len = strlen(url);
    if (len >= 5u && strcmp(url + len - 5u, "/null") == 0) return false;
    if (strstr(url, "radio-browser.info/null") != NULL) return false;
    return true;
}

static bool station_matches_at(const char (*languages)[8], size_t idx, const char *lang,
                             bool allow_de_fallback, size_t *german_count) {
    char station[8];
    memcpy(station, languages[idx], 8u);
    station[7] = 0;
    if (matches(station, lang)) return true;
    return allow_de_fallback && strcmp(lang, "nl") == 0 && strcmp(station, "de") == 0 &&
           german_count && (*german_count)++ < 3u;
}

size_t fv_station_group(const char (*languages)[8], size_t count, const char *language,
                        uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]) {
    if (!languages || !language || !group || !count || count > FV_STATION_MAX) return 0;
    char lang[8];
    normalize_lang(language, lang);
    unsigned candidates[FV_STATION_MAX] = {0};
    size_t n = 0, german = 0;
    for (size_t i = 0; i < count; i++) {
        if (station_matches_at(languages, i, lang, true, &german)) {
            candidates[n++] = (unsigned)i;
        }
    }
    shuffle_u32(candidates, n, seed);
    size_t used = n < FV_STATION_GROUP_SIZE ? n : FV_STATION_GROUP_SIZE;
    memcpy(group, candidates, used * sizeof(*group));
    for (size_t i = 0; used < FV_STATION_GROUP_SIZE && i < count; i++) {
        bool present = false;
        for (size_t j = 0; j < used; j++) present |= group[j] == i;
        if (!present) group[used++] = (unsigned)i;
    }
    return used;
}

static bool in_group(const unsigned *group, size_t used, unsigned idx) {
    for (size_t j = 0; j < used; j++) {
        if (group[j] == idx) return true;
    }
    return false;
}

size_t fv_station_group_healthy(const char (*languages)[8], const char *const *urls,
                                const uint8_t *health, size_t count, const char *language,
                                uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]) {
    if (!languages || !language || !group || !count || count > FV_STATION_MAX) return 0;
    char lang[8];
    normalize_lang(language, lang);
    unsigned candidates[FV_STATION_MAX] = {0};
    size_t n = 0, german = 0;

    for (size_t i = 0; i < count; i++) {
        if (urls && !fv_station_url_is_valid(urls[i])) continue;
        if (health && health[i] == FV_STATION_FAILED) continue;
        if (station_matches_at(languages, i, lang, true, &german)) {
            candidates[n++] = (unsigned)i;
        }
    }
    shuffle_u32(candidates, n, seed);
    size_t used = n < FV_STATION_GROUP_SIZE ? n : FV_STATION_GROUP_SIZE;
    memcpy(group, candidates, used * sizeof(*group));

    /* Fill remaining slots from other non-failed valid stations, then failed valid stations as last resort. */
    for (int pass = 0; pass < 2 && used < FV_STATION_GROUP_SIZE; pass++) {
        unsigned extra[FV_STATION_MAX] = {0};
        size_t en = 0;
        for (size_t i = 0; i < count; i++) {
            if (in_group(group, used, (unsigned)i)) continue;
            if (urls && !fv_station_url_is_valid(urls[i])) continue;
            const bool is_failed = health && health[i] == FV_STATION_FAILED;
            if ((pass == 0 && is_failed) || (pass == 1 && !is_failed)) continue;
            extra[en++] = (unsigned)i;
        }
        shuffle_u32(extra, en, seed ^ (uint32_t)(pass + 1));
        for (size_t i = 0; i < en && used < FV_STATION_GROUP_SIZE; i++) {
            group[used++] = extra[i];
        }
    }
    return used;
}

int fv_station_replace_failed(const char (*languages)[8], const char *const *urls,
                              const uint8_t *health, size_t count, const char *language,
                              uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE],
                              size_t group_count, int failed_station) {
    if (!languages || !language || !group || !group_count || group_count > FV_STATION_GROUP_SIZE ||
        !count || count > FV_STATION_MAX || failed_station < 0) return -1;
    size_t target_slot = group_count;
    for (size_t i = 0; i < group_count; i++) {
        if (group[i] == (unsigned)failed_station) {
            target_slot = i;
            break;
        }
    }
    if (target_slot == group_count) return -1;

    char lang[8];
    normalize_lang(language, lang);
    for (int pass = 0; pass < 2; pass++) {
        unsigned pool[FV_STATION_MAX] = {0};
        size_t pn = 0;
        for (size_t i = 0; i < count; i++) {
            if (in_group(group, group_count, (unsigned)i)) continue;
            if (urls && !fv_station_url_is_valid(urls[i])) continue;
            if (health && health[i] == FV_STATION_FAILED) continue;
            if (pass == 0 && !station_matches_at(languages, i, lang, false, NULL)) continue;
            pool[pn++] = (unsigned)i;
        }
        if (pn > 0u) {
            uint32_t rng = seed ? seed : 0x9e3779b9u;
            const unsigned pick = pool[(size_t)xorshift32(&rng) % pn];
            group[target_slot] = pick;
            return (int)pick;
        }
    }
    return -1;
}

static size_t parse_json_string(const char *src, size_t len, size_t pos, char *out, size_t out_cap) {
    if (pos >= len || src[pos] != '"') return pos;
    pos++;
    size_t w = 0;
    while (pos < len) {
        const char ch = src[pos++];
        if (ch == '"') break;
        char decoded = ch;
        if (ch == '\\' && pos < len) {
            const char esc = src[pos++];
            switch (esc) {
                case '"': case '\\': case '/': decoded = esc; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u':
                    for (int k = 0; k < 4 && pos < len; k++) pos++;
                    decoded = ' ';
                    break;
                default: decoded = esc; break;
            }
        }
        if (out && out_cap > 0u && w + 1u < out_cap) {
            out[w++] = decoded;
        }
    }
    if (out && out_cap > 0u) out[w] = '\0';
    return pos;
}

size_t fv_station_parse_radiobrowser_json(const char *json, size_t json_len,
                                          FvDynamicStation *out, size_t capacity) {
    if (!json || json_len == 0u || !out || capacity == 0u) return 0u;
    size_t count = 0u;
    int obj_depth = 0;
    int arr_depth = 0;
    bool in_object = false;
    char name_buf[64] = {0}, url_res[FV_RADIO_URL_MAX] = {0}, url_raw[FV_RADIO_URL_MAX] = {0};
    char country_buf[4] = {0}, lang_buf[16] = {0};
    char key[48] = {0};
    bool expect_key = false;

    for (size_t i = 0; i < json_len && count < capacity;) {
        const char c = json[i];
        if (c == '"') {
            char val[FV_RADIO_URL_MAX] = {0};
            const size_t next = parse_json_string(json, json_len, i, val, sizeof(val));
            if (in_object && obj_depth == 1 && arr_depth == 0) {
                if (expect_key) {
                    snprintf(key, sizeof(key), "%s", val);
                    expect_key = false;
                } else {
                    if (strcmp(key, "name") == 0) snprintf(name_buf, sizeof(name_buf), "%s", val);
                    else if (strcmp(key, "url_resolved") == 0) snprintf(url_res, sizeof(url_res), "%s", val);
                    else if (strcmp(key, "url") == 0) snprintf(url_raw, sizeof(url_raw), "%s", val);
                    else if (strcmp(key, "countrycode") == 0) snprintf(country_buf, sizeof(country_buf), "%s", val);
                    else if (strcmp(key, "languagecodes") == 0 || (strcmp(key, "language") == 0 && !lang_buf[0])) {
                        snprintf(lang_buf, sizeof(lang_buf), "%s", val);
                    }
                    key[0] = '\0';
                }
            }
            i = next;
            continue;
        }
        if (c == '{') {
            obj_depth++;
            if (obj_depth == 1 && arr_depth == 0) {
                in_object = true;
                expect_key = true;
                key[0] = name_buf[0] = url_res[0] = url_raw[0] = country_buf[0] = lang_buf[0] = '\0';
            }
        } else if (c == '}') {
            if (in_object && obj_depth == 1 && arr_depth == 0) {
                const char *chosen_url = fv_station_url_is_valid(url_res) ? url_res : url_raw;
                if (fv_station_url_is_valid(chosen_url) && name_buf[0] != '\0') {
                    FvDynamicStation *dst = &out[count++];
                    *dst = (FvDynamicStation){0};
                    snprintf(dst->callsign, sizeof(dst->callsign), "%s", name_buf);
                    snprintf(dst->name, sizeof(dst->name), "%s", name_buf);
                    snprintf(dst->country, sizeof(dst->country), "%s", country_buf);
                    normalize_lang(lang_buf[0] ? lang_buf : "00", dst->lang);
                    snprintf(dst->url, sizeof(dst->url), "%s", chosen_url);
                }
                in_object = false;
            }
            if (obj_depth > 0) obj_depth--;
        } else if (in_object && c == '[') {
            arr_depth++;
        } else if (in_object && c == ']') {
            if (arr_depth > 0) arr_depth--;
        } else if (in_object && obj_depth == 1 && arr_depth == 0 && c == ',') {
            expect_key = true;
            key[0] = '\0';
        }
        i++;
    }
    return count;
}
