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
 * modified by fduncanh 2021-23
 */

#ifndef RAOP_H
#define RAOP_H

#include "dnssd.h"
#include "stream.h"
#include "raop_ntp.h"
#include "airplay_video.h"

#if defined (WIN32) && defined(DLL_EXPORT)
# define RAOP_API __declspec(dllexport)
#else
# define RAOP_API
#endif

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

struct raop_callbacks_s {
    void* cls;

    void  (*audio_process)(void *cls, raop_ntp_t *ntp, audio_decode_struct *data);
    void  (*video_process)(void *cls, raop_ntp_t *ntp, video_decode_struct *data);
    void  (*video_pause)(void *cls);
    void  (*video_resume)(void *cls);

    /* Optional but recommended callback functions */
    void  (*conn_init)(void *cls);
    void  (*conn_destroy)(void *cls);
    void  (*conn_reset) (void *cls, int timeouts, bool reset_video);
    void  (*conn_teardown)(void *cls, bool *teardown_96, bool *teardown_110 );
    void  (*audio_flush)(void *cls);
    void  (*video_flush)(void *cls);
    void  (*audio_set_volume)(void *cls, float volume);
    void  (*audio_set_metadata)(void *cls, const void *buffer, int buflen);
    void  (*audio_set_coverart)(void *cls, const void *buffer, int buflen);
    void  (*audio_remote_control_id)(void *cls, const char *dacp_id, const char *active_remote_header);
    void  (*audio_set_progress)(void *cls, unsigned int start, unsigned int curr, unsigned int end);
    void  (*audio_get_format)(void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat);
    void  (*video_report_size)(void *cls, float *width_source, float *height_source, float *width, float *height);
    void  (*report_client_request) (void *cls, char *deviceid, char *model, char *name, bool *admit);
    void  (*display_pin) (void *cls, char * pin);
    void  (*register_client) (void *cls, const char *device_id, const char *pk_str, const char *name);
    bool  (*check_register) (void *cls, const char *pk_str);
    void  (*export_dacp) (void *cls, const char *active_remote, const char *dacp_id);
    void  (*video_reset) (void *cls);
    void  (*video_set_codec)(void *cls, video_codec_t codec);
    /* for HLS video player controls */
    void  (*on_video_play) (void *cls, const char *location, const float start_position);
    void  (*on_video_scrub) (void *cls, const float position);
    void  (*on_video_rate) (void *cls, const float rate);
    void  (*on_video_stop) (void *cls);
    void  (*on_video_acquire_playback_info) (void *cls, playback_info_t *playback_video);
  
};
typedef struct raop_callbacks_s raop_callbacks_t;
raop_ntp_t *raop_ntp_init(logger_t *logger, raop_callbacks_t *callbacks, const char *remote,
                          int remote_addr_len, unsigned short timing_rport,
                          timing_protocol_t *time_protocol);

int airplay_video_service_init(raop_t *raop, unsigned short port, const char *session_id);

bool register_airplay_video(raop_t *raop, airplay_video_t *airplay_video);
airplay_video_t *get_airplay_video(raop_t *raop);
airplay_video_t *deregister_airplay_video(raop_t *raop);
  
RAOP_API raop_t *raop_init(raop_callbacks_t *callbacks);
RAOP_API int raop_init2(raop_t *raop, int nohold, const char *device_id, const char *keyfile);
RAOP_API void raop_set_log_level(raop_t *raop, int level);
RAOP_API void raop_set_log_callback(raop_t *raop, raop_log_callback_t callback, void *cls);
RAOP_API int raop_set_plist(raop_t *raop, const char *plist_item, const int value);
RAOP_API void raop_set_port(raop_t *raop, unsigned short port);
RAOP_API void raop_set_udp_ports(raop_t *raop, unsigned short port[3]);
RAOP_API void raop_set_tcp_ports(raop_t *raop, unsigned short port[2]);
RAOP_API unsigned short raop_get_port(raop_t *raop);
RAOP_API void *raop_get_callback_cls(raop_t *raop);
RAOP_API int raop_start(raop_t *raop, unsigned short *port);
RAOP_API int raop_is_running(raop_t *raop);
RAOP_API void raop_stop(raop_t *raop);
RAOP_API void raop_set_dnssd(raop_t *raop, dnssd_t *dnssd);
RAOP_API void raop_destroy(raop_t *raop);
RAOP_API void raop_remove_known_connections(raop_t * raop);
RAOP_API void raop_destroy_airplay_video(raop_t *raop);

#ifdef __cplusplus
}
#endif
#endif
