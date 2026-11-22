/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_LOCAL_BANK_H
#define FV_LOCAL_BANK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FV_LOCAL_DECKS 4

typedef struct {
    bool ready, loading, failed;
    char path[512], name[256], status[128];
    uint64_t frames, generation, source_offset;
} FvLocalBankInfo;
bool fv_local_bank_init(const char *settings_directory);
void fv_local_bank_shutdown(void);
void fv_local_bank_load(const char *path);
void fv_local_bank_info(FvLocalBankInfo *info);

#define FV_LOCAL_BANK_MAX_FILES 100

/*
 * Enforces an eviction cap of 100 files in the local grain bank cache directory.
 * When file count > max_files, evicts the oldest files by modification time.
 * Returns the number of evicted files.
 */
int fv_local_bank_enforce_cache_cap(const char *directory, size_t max_files);

size_t fv_local_bank_count(void);
/* Copies the selected source window under its ownership lock. */
size_t fv_local_bank_fragment(float seek, size_t frames, int16_t *output, size_t capacity);

#endif
