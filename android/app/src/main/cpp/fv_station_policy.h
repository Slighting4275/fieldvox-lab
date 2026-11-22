/* SPDX-License-Identifier: GPL-3.0-or-later AND AGPL-3.0-or-later */
#ifndef FV_STATION_POLICY_H
#define FV_STATION_POLICY_H
#include <stddef.h>
#include <stdint.h>
#define FV_STATION_GROUP_SIZE 10u
/* Language strings are at most seven bytes plus a terminator. */
size_t fv_station_group(const char (*languages)[8], size_t count, const char *language,
                        uint32_t seed, unsigned group[FV_STATION_GROUP_SIZE]);
#endif
