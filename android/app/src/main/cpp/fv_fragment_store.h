/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_FRAGMENT_STORE_H
#define FV_FRAGMENT_STORE_H
#include "fv_fragment_engine.h"
bool fv_fragment_store_start(const char *app_directory, FvFragmentEngine *engine);
bool fv_fragment_store_submit(const int16_t *pcm, size_t frames, int station);
bool fv_fragment_store_submit_source(const int16_t *pcm, size_t frames, int station, int source_fd);
void fv_fragment_store_stop(void);
#endif
