/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
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

/* 
 * H264 renderer using gstreamer
*/

#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "../lib/logger.h"

typedef enum videoflip_e {
    NONE,
    LEFT,
    RIGHT,
    INVERT,
    VFLIP,
    HFLIP,
} videoflip_t;

typedef struct video_renderer_s video_renderer_t;

void video_renderer_init (logger_t *logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char *rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options,
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support,
                          guint playbin_version,  const char *uri);
void video_renderer_start ();
void video_renderer_stop ();
void video_renderer_pause ();
void video_renderer_hls_ready ();
void video_renderer_seek(float position);
void video_renderer_set_start(float position);
void video_renderer_resume ();
int video_renderer_cycle ();
bool video_renderer_is_paused();
bool video_renderer_eos_watch();
uint64_t  video_renderer_render_buffer (unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time);
void video_renderer_display_jpeg(const void *data, int *data_len);
void video_renderer_flush ();
unsigned int video_renderer_listen(void *loop, int id);
void video_renderer_destroy ();
void video_renderer_size(float *width_source, float *height_source, float *width, float *height);
bool waiting_for_x11_window();
bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full);
int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265);
unsigned int video_renderer_listen(void *loop, int id);
bool video_renderer_eos_watch();
#ifdef __cplusplus
}
#endif

#endif //VIDEO_RENDERER_H

