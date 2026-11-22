/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_RADIO_PLAYLIST_H
#define FV_RADIO_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>

#define FV_RADIO_URL_MAX 512u

bool fv_radio_ascii_ieq_n(const char *a, const char *b, size_t n);
bool fv_radio_is_playlist_url(const char *url, const char *content_type);
bool fv_radio_body_is_playlist(const char *body, size_t len);
bool fv_radio_resolve_relative_url(const char *base_url, const char *ref,
                                   char *out_url, size_t out_cap);
bool fv_radio_extract_stream_url(const char *base_url, const char *body, size_t body_len,
                                 char *out_url, size_t out_cap, bool *out_is_nested_playlist);

#endif
