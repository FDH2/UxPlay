/*
 * Copyright (c) 2019 dsafa22, modified by Florian Draschbacher,
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *=================================================================
 * modified by fduncanh 2021-2023
 */

#ifndef RAOP_NTP_H
#define RAOP_NTP_H

#include <stdbool.h>
#include <stdint.h>
#include "logger.h"

typedef struct raop_ntp_s raop_ntp_t;

typedef enum timing_protocol_e { NTP, TP_NONE, TP_OTHER, TP_UNSPECIFIED } timing_protocol_t;

void raop_ntp_start(raop_ntp_t *raop_ntp, unsigned short *timing_lport);

void raop_ntp_stop(raop_ntp_t *raop_ntp);

unsigned short raop_ntp_get_port(raop_ntp_t *raop_ntp);

void raop_ntp_destroy(raop_ntp_t *raop_rtp);

uint64_t raop_ntp_timestamp_to_nano_seconds(uint64_t ntp_timestamp, bool account_for_epoch_diff);
uint64_t raop_remote_timestamp_to_nano_seconds(raop_ntp_t *raop_ntp, uint64_t timestamp);

uint64_t raop_ntp_get_local_time();
uint64_t raop_ntp_get_remote_time(raop_ntp_t *raop_ntp);
uint64_t raop_ntp_convert_remote_time(raop_ntp_t *raop_ntp, uint64_t remote_time);
uint64_t raop_ntp_convert_local_time(raop_ntp_t *raop_ntp, uint64_t local_time);

void  raop_ntp_set_video_arrival_offset(raop_ntp_t* raop_ntp, const uint64_t *offset);
uint64_t raop_ntp_get_video_arrival_offset(raop_ntp_t* raop_ntp);

#endif //RAOP_NTP_H
