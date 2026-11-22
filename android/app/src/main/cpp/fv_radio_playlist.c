/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#include "fv_radio_playlist.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#error FieldVox requires C23
#endif

static int ascii_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (int)(c + ('a' - 'A')) : (int)c;
}

bool fv_radio_ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!a[i] || !b[i]) return false;
        if (ascii_tolower((unsigned char)a[i]) != ascii_tolower((unsigned char)b[i])) return false;
    }
    return true;
}

static bool ascii_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (fv_radio_ascii_ieq_n(p, needle, nlen)) return true;
    }
    return false;
}

static bool path_has_ext_ci(const char *url, const char *ext) {
    if (!url || !ext) return false;
    const size_t elen = strlen(ext);
    size_t end = 0;
    while (url[end] && url[end] != '?' && url[end] != '#') end++;
    if (end < elen) return false;
    return fv_radio_ascii_ieq_n(url + end - elen, ext, elen);
}

bool fv_radio_is_playlist_url(const char *url, const char *content_type) {
    if (url && (path_has_ext_ci(url, ".m3u8") || path_has_ext_ci(url, ".m3u") || path_has_ext_ci(url, ".pls"))) {
        return true;
    }
    if (content_type && (ascii_contains_ci(content_type, "mpegurl") ||
                         ascii_contains_ci(content_type, "scpls") ||
                         ascii_contains_ci(content_type, "pls+xml"))) {
        return true;
    }
    return false;
}

bool fv_radio_body_is_playlist(const char *body, size_t len) {
    if (!body || len == 0u) return false;
    size_t i = 0;
    if (len >= 3u && (unsigned char)body[0] == 0xEFu && (unsigned char)body[1] == 0xBBu && (unsigned char)body[2] == 0xBFu) {
        i = 3u;
    }
    while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) i++;
    const size_t rem = len - i;
    if (rem >= 7u && fv_radio_ascii_ieq_n(body + i, "#EXTM3U", 7u)) return true;
    if (rem >= 10u && fv_radio_ascii_ieq_n(body + i, "[playlist]", 10u)) return true;
    return false;
}

bool fv_radio_resolve_relative_url(const char *base_url, const char *ref,
                                   char *out_url, size_t out_cap) {
    if (!ref || !*ref || !out_url || out_cap == 0u) return false;
    if (fv_radio_ascii_ieq_n(ref, "http://", 7u) || fv_radio_ascii_ieq_n(ref, "https://", 8u)) {
        const int n = snprintf(out_url, out_cap, "%s", ref);
        return n > 0 && (size_t)n < out_cap;
    }
    if (!base_url || !*base_url) return false;
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) return false;
    const size_t scheme_len = (size_t)(scheme_end - base_url);

    if (ref[0] == '/' && ref[1] == '/') {
        const int n = snprintf(out_url, out_cap, "%.*s:%s", (int)scheme_len, base_url, ref);
        return n > 0 && (size_t)n < out_cap;
    }

    const char *auth_start = scheme_end + 3;
    const char *path_start = strpbrk(auth_start, "/?#");
    const size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);

    if (ref[0] == '/') {
        const int n = snprintf(out_url, out_cap, "%.*s%s", (int)origin_len, base_url, ref);
        return n > 0 && (size_t)n < out_cap;
    }

    size_t base_end = 0;
    while (base_url[base_end] && base_url[base_end] != '?' && base_url[base_end] != '#') base_end++;
    size_t dir_end = origin_len;
    for (size_t i = origin_len; i < base_end; i++) {
        if (base_url[i] == '/') dir_end = i + 1u;
    }
    if (dir_end <= origin_len) {
        const int n = snprintf(out_url, out_cap, "%.*s/%s", (int)origin_len, base_url, ref);
        return n > 0 && (size_t)n < out_cap;
    }
    const int n = snprintf(out_url, out_cap, "%.*s%s", (int)dir_end, base_url, ref);
    return n > 0 && (size_t)n < out_cap;
}

static size_t trim_line(const char *src, size_t len, const char **out_start) {
    size_t s = 0, e = len;
    while (s < e && (src[s] == ' ' || src[s] == '\t' || src[s] == '\r')) s++;
    while (e > s && (src[e - 1u] == ' ' || src[e - 1u] == '\t' || src[e - 1u] == '\r')) e--;
    *out_start = src + s;
    return e - s;
}

static const char *match_pls_file_key(const char *line, size_t len) {
    if (len < 6u || !fv_radio_ascii_ieq_n(line, "File", 4u)) return NULL;
    size_t i = 4u;
    if (!isdigit((unsigned char)line[i])) return NULL;
    while (i < len && isdigit((unsigned char)line[i])) i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != '=') return NULL;
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    return i < len ? line + i : NULL;
}

bool fv_radio_extract_stream_url(const char *base_url, const char *body, size_t body_len,
                                 char *out_url, size_t out_cap, bool *out_is_nested_playlist) {
    if (out_is_nested_playlist) *out_is_nested_playlist = false;
    if (!base_url || !body || body_len == 0u || !out_url || out_cap == 0u) return false;

    bool is_pls = path_has_ext_ci(base_url, ".pls");
    size_t pos = 0;
    if (body_len >= 3u && (unsigned char)body[0] == 0xEFu && (unsigned char)body[1] == 0xBBu && (unsigned char)body[2] == 0xBFu) {
        pos = 3u;
    }

    while (pos < body_len) {
        size_t eol = pos;
        while (eol < body_len && body[eol] != '\n') eol++;
        const char *line = NULL;
        const size_t llen = trim_line(body + pos, eol - pos, &line);
        pos = (eol < body_len) ? (eol + 1u) : body_len;
        if (llen == 0u) continue;

        if (llen == 10u && fv_radio_ascii_ieq_n(line, "[playlist]", 10u)) {
            is_pls = true;
            continue;
        }
        if (line[0] == '#') continue;

        const char *candidate = NULL;
        size_t clen = 0u;
        if (is_pls) {
            const char *val = match_pls_file_key(line, llen);
            if (!val) continue;
            candidate = val;
            clen = llen - (size_t)(val - line);
        } else {
            candidate = line;
            clen = llen;
        }

        if (clen == 0u || clen >= FV_RADIO_URL_MAX) continue;
        char ref_buf[FV_RADIO_URL_MAX];
        memcpy(ref_buf, candidate, clen);
        ref_buf[clen] = '\0';

        if (fv_radio_resolve_relative_url(base_url, ref_buf, out_url, out_cap)) {
            if (out_is_nested_playlist) *out_is_nested_playlist = fv_radio_is_playlist_url(out_url, NULL);
            return true;
        }
    }
    return false;
}
