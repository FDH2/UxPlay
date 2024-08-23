/**
 * Copyright (c) 2024 fduncanh
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
 */

// it should only start and stop the media_data_store that handles all HLS transactions, without
// otherwise participating in them.  

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "raop.h"
#include "airplay_video.h"

struct airplay_video_s {
    raop_t *raop;
    void *conn_opaque;
    char apple_session_id[37];
    char playback_uuid[37];
    char local_uri_prefix[23];
    float start_position_seconds;
    playback_info_t *playback_info;
    // The local port of the airplay server on the AirPlay server
    unsigned short airplay_port;
};

//  initialize airplay_video service.
airplay_video_t *airplay_video_service_init(void *conn_opaque, raop_t *raop, unsigned short http_port,
                                            const char *session_id) {
    void *media_data_store = NULL;
    char uri[] = "http://localhost:xxxxx";
    assert(conn_opaque);
    assert(raop);
    airplay_video_t *airplay_video =  (airplay_video_t *) calloc(1, sizeof(airplay_video_t));
    if (!airplay_video) {
        return NULL;
    }

    /* create local_uri_prefix string */
    strncpy(airplay_video->local_uri_prefix, uri, sizeof(airplay_video->local_uri_prefix));
    char *ptr  = strstr(airplay_video->local_uri_prefix, "xxxxx");
    snprintf(ptr, 6, "%-5u", http_port);
    ptr = strstr(airplay_video->local_uri_prefix, " ");
    if (ptr) {
      *ptr = '\0';
    }
    
    /* destroy any existing media_data_store and create a new instance*/
    set_media_data_store(raop, media_data_store);  
    media_data_store = media_data_store_create(conn_opaque, http_port);
    set_media_data_store(raop, media_data_store);

    airplay_video->raop = raop;
    airplay_video->conn_opaque = conn_opaque;

    size_t len = strlen(session_id);
    assert(len == 36);
    strncpy(airplay_video->apple_session_id, session_id, len);
    (airplay_video->apple_session_id)[len] = '\0';

    airplay_video->start_position_seconds = 0.0f;
    return airplay_video;
}

// destroy the airplay_video service
void
airplay_video_service_destroy(airplay_video_t *airplay_video)
{
    void* media_data_store = NULL;
    /* destroys media_data_store if called with media_data_store = NULL */
    set_media_data_store(airplay_video->raop, media_data_store);
    free (airplay_video);
}

const char *get_apple_session_id(airplay_video_t *airplay_video) {
    return airplay_video->apple_session_id;
}

float get_start_position_seconds(airplay_video_t *airplay_video) {
    return airplay_video->start_position_seconds;
}

void set_start_position_seconds(airplay_video_t *airplay_video, float start_position_seconds) {
    airplay_video->start_position_seconds = start_position_seconds;
}
  
void set_playback_uuid(airplay_video_t *airplay_video, const char *playback_uuid) {
    size_t len = strlen(playback_uuid);
    assert(len == 36);
    memcpy(airplay_video->playback_uuid, playback_uuid, len);
    (airplay_video->playback_uuid)[len] = '\0';
}
