/**
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef MUX_RENDERER_H
#define MUX_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

void mux_renderer_init(logger_t *logger, const char *filename, bool use_audio, bool use_video);
void mux_renderer_choose_audio_codec(unsigned char audio_ct);
void mux_renderer_choose_video_codec(bool is_h265);
void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time);
void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time);
void mux_renderer_stop(void);
void mux_renderer_destroy(void);

#ifdef __cplusplus
}
#endif

#endif //MUX_RENDERER_H
