/**
 *  Copyright (C) 2012-2015  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *===================================================================
 * modified by fduncanh 2021-25
 */

#ifndef RAOP_H
#define RAOP_H

#include "dnssd.h"
#include "stream.h"
#include "raop_ntp.h"
#include "airplay_video.h"

#define RAOP_API
#define MAX_AIRPLAY_VIDEO 10
#define MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS 90   //dont store advertisement playlists

#ifdef __cplusplus
extern "C" {
#endif

typedef struct raop_s raop_t;

typedef void (*raop_log_callback_t)(void *cls, int level, const char *msg);


typedef struct playback_info_s {
  //char * uuid;
    uint32_t stallcount;
    double duration;
    double position;
    double seek_start;
    double seek_duration;
    float rate;
    bool ready_to_play;
    bool playback_buffer_empty;
    bool playback_buffer_full;
    bool playback_likely_to_keep_up;
    int num_loaded_time_ranges;
    int num_seekable_time_ranges;
    void *loadedTimeRanges;
    void *seekableTimeRanges;
} playback_info_t;
  
typedef enum video_codec_e {
    VIDEO_CODEC_UNKNOWN,
    VIDEO_CODEC_H264,
    VIDEO_CODEC_H265
} video_codec_t;

typedef enum reset_type_e {
  RESET_TYPE_NOHOLD,
  RESET_TYPE_RTP_SHUTDOWN,
  RESET_TYPE_HLS_SHUTDOWN,
  RESET_TYPE_HLS_EOS
} reset_type_t;

struct raop_callbacks_s {
    void* cls;

    void  (*audio_process)(void *cls, raop_ntp_t *ntp, audio_decode_struct *data);
    void  (*video_process)(void *cls, raop_ntp_t *ntp, video_decode_struct *data);
    void  (*video_pause)(void *cls);
    void  (*video_resume)(void *cls);
    void  (*conn_feedback) (void *cls);
    void  (*conn_reset) (void *cls, int reason);
    void  (*video_reset) (void *cls, reset_type_t reset_type);
  
  
    /* Optional but recommended callback functions (probably not optional, check this)*/
    void  (*conn_init)(void *cls);
    void  (*conn_destroy)(void *cls);
    void  (*conn_teardown)(void *cls, bool *teardown_96, bool *teardown_110 );
    void  (*audio_flush)(void *cls);
    void  (*video_flush)(void *cls);
    double (*audio_set_client_volume)(void *cls);
    void  (*audio_set_volume)(void *cls, float volume);
    void  (*audio_set_metadata)(void *cls, const void *buffer, int buflen);
    void  (*audio_set_coverart)(void *cls, const void *buffer, int buflen);
    void  (*audio_stop_coverart_rendering) (void* cls);
    void  (*audio_remote_control_id)(void *cls, const char *dacp_id, const char *active_remote_header);
    void  (*audio_set_progress)(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end);
    void  (*audio_get_format)(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat);
    void  (*video_report_size)(void *cls, float *width_source, float *height_source, float *width, float *height);
    void  (*mirror_video_activity)(void *cls, double *txusage);
    void  (*report_client_request) (void *cls, char *deviceid, char *model, char *name, bool *admit);
    void  (*display_pin) (void *cls, char * pin);
    void  (*register_client) (void *cls, const char *device_id, const char *pk_str, const char *name);
    bool  (*check_register) (void *cls, const char *pk_str);
    const char*  (*passwd) (void *cls, int *len);
    void  (*export_dacp) (void *cls, const char *active_remote, const char *dacp_id);
    int   (*video_set_codec)(void *cls, video_codec_t codec);
    /* for HLS video player controls */
    void  (*on_video_play) (void *cls, const char *location, const float start_position);
    void  (*on_video_scrub) (void *cls, const float position);
    void  (*on_video_rate) (void *cls, const float rate);
    void  (*on_video_stop) (void *cls);
    void  (*on_video_acquire_playback_info) (void *cls, playback_info_t *playback_video);
    float  (*on_video_playlist_remove) (void *cls);
};

typedef struct raop_callbacks_s raop_callbacks_t;
raop_ntp_t *raop_ntp_init(logger_t *logger, raop_callbacks_t *callbacks, const char *remote,
                          int remote_addr_len, unsigned short timing_rport,
                          timing_protocol_t *time_protocol);

airplay_video_t *airplay_video_init(raop_t *raop, unsigned short port, const char *lang);
char *raop_get_lang(raop_t *raop);
uint64_t get_local_time();
void raop_handle_eos(raop_t *raop);

RAOP_API raop_t *raop_init(raop_callbacks_t *callbacks);
RAOP_API int raop_init2(raop_t *raop, int nohold, const char *device_id, const char *keyfile);
RAOP_API void raop_set_log_level(raop_t *raop, int level);
RAOP_API void raop_set_log_callback(raop_t *raop, raop_log_callback_t callback, void *cls);
RAOP_API int raop_set_plist(raop_t *raop, const char *plist_item, const int value);
RAOP_API void raop_set_port(raop_t *raop, unsigned short port);
RAOP_API void raop_set_lang(raop_t *raop, const char *lang);
RAOP_API void raop_set_udp_ports(raop_t *raop, unsigned short port[3]);
RAOP_API void raop_set_tcp_ports(raop_t *raop, unsigned short port[2]);
RAOP_API unsigned short raop_get_port(raop_t *raop);
RAOP_API void *raop_get_callback_cls(raop_t *raop);
RAOP_API int raop_start_httpd(raop_t *raop, unsigned short *port);
RAOP_API int raop_is_running(raop_t *raop);
RAOP_API void raop_stop_httpd(raop_t *raop);
RAOP_API void raop_set_dnssd(raop_t *raop, dnssd_t *dnssd);
RAOP_API void raop_destroy(raop_t *raop);
RAOP_API void raop_remove_known_connections(raop_t * raop);
RAOP_API void raop_remove_hls_connections(raop_t * raop);
RAOP_API void raop_destroy_airplay_video(raop_t *raop, int id);
RAOP_API void raop_playlist_remove(raop_t *raop, void *airplay_video, float position);
  
#ifdef __cplusplus
}
#endif
#endif
