/**                                                                                                                                                                               
 * GStreamer-based renderer backend for UxPlay - An open-source AirPlay mirroring server                                                                                                                               
 * Copyright (C) 2026 F. Duncanh                                                                                                                                               
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

#ifndef MIRROR_RENDERER_H
#define MIRROR_RENDERER_H

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

typedef struct  mirror_renderer_s mirror_renderer_t;
bool gstreamer_init();

void audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time);
uint64_t  video_renderer_render_buffer (unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time);
void video_renderer_display_jpeg(const void *data, int *data_len);


  mirror_renderer_t * create_mirror_renderer(gboolean is_h265, gboolean is_audio);
  
  void mirror_renderer_init (logger_t *logger, const char *audiosink, const char* videosink, const char * videosink_options, videoflip_t videoflip[2],
			   bool video_sync, bool audio_sync, bool h265_support, bool coverart_support, bool rgb_fix );

#ifdef __cplusplus
}
#endif

#endif //MRROR_RENDERER_H                                                                                                                                                         

