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

struct media_item_s {
  char *uri;
  char *playlist;
  int access;
};

struct airplay_video_s {
    raop_t *raop;
    char apple_session_id[37];
    char playback_uuid[37];
    char *uri_prefix;
    char local_uri_prefix[23];
    int next_uri;
    int FCUP_RequestID;
    float start_position_seconds;
    playback_info_t *playback_info;
    // The local port of the airplay server on the AirPlay server
    unsigned short airplay_port;
    char *master_uri;
    char *master_playlist;
    media_item_t *media_data_store;
    int num_uri;
};

//  initialize airplay_video service.
int airplay_video_service_init(raop_t *raop, unsigned short http_port,
                                            const char *session_id) {
    char uri[] = "http://localhost:xxxxx";
    assert(raop);

    airplay_video_t *airplay_video = deregister_airplay_video(raop);
    if (airplay_video) {
      airplay_video_service_destroy(airplay_video);
    }

    airplay_video =  (airplay_video_t *) calloc(1, sizeof(airplay_video_t));
    if (!airplay_video) {
        return -1;
    }

    /* create local_uri_prefix string */
    strncpy(airplay_video->local_uri_prefix, uri, sizeof(airplay_video->local_uri_prefix));
    char *ptr  = strstr(airplay_video->local_uri_prefix, "xxxxx");
    snprintf(ptr, 6, "%-5u", http_port);
    ptr = strstr(airplay_video->local_uri_prefix, " ");
    if (ptr) {
      *ptr = '\0';
    }

    if (!register_airplay_video(raop, airplay_video)) {
      return -2;
    }

    printf(" %p %p\n", airplay_video, get_airplay_video(raop));

    airplay_video->raop = raop;


    airplay_video->FCUP_RequestID = 0;


    size_t len = strlen(session_id);
    assert(len == 36);
    strncpy(airplay_video->apple_session_id, session_id, len);
    (airplay_video->apple_session_id)[len] = '\0';

    airplay_video->start_position_seconds = 0.0f;

    airplay_video->master_uri = NULL;
    airplay_video->media_data_store = NULL;
    airplay_video->master_playlist = NULL;
    airplay_video->num_uri = 0;
    airplay_video->next_uri = 0;
    return 0;
}

// destroy the airplay_video service
void
airplay_video_service_destroy(airplay_video_t *airplay_video)
{

    if (airplay_video->uri_prefix) {
        free(airplay_video->uri_prefix);
    }
    if (airplay_video->master_uri) {
        free (airplay_video->master_uri);
    }
    if (airplay_video->media_data_store) {
        destroy_media_data_store(airplay_video);
    }
    if (airplay_video->master_playlist) {
        free (airplay_video->master_playlist);
    }

    
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

void set_uri_prefix(airplay_video_t *airplay_video, char *uri_prefix, int uri_prefix_len) {
  if (airplay_video->uri_prefix) {
      free (airplay_video->uri_prefix);
  }
  airplay_video->uri_prefix = (char *) calloc(uri_prefix_len + 1, sizeof(char));
  memcpy(airplay_video->uri_prefix, uri_prefix, uri_prefix_len);
}

char *get_uri_prefix(airplay_video_t *airplay_video) {
  return airplay_video->uri_prefix;
}

char *get_uri_local_prefix(airplay_video_t *airplay_video) {
  return airplay_video->local_uri_prefix;
}


char *get_master_uri(airplay_video_t *airplay_video) {
    return airplay_video->master_uri;
}


int get_next_FCUP_RequestID(airplay_video_t *airplay_video) {    
    return ++(airplay_video->FCUP_RequestID);
}

void  set_next_media_uri_id(airplay_video_t *airplay_video, int num) {
    airplay_video->next_uri = num;
}

int get_next_media_uri_id(airplay_video_t *airplay_video) {
    return airplay_video->next_uri;
}


/* master playlist */

void store_master_playlist(airplay_video_t *airplay_video, char *master_playlist) {
    if (airplay_video->master_playlist) {
        free (airplay_video->master_playlist);
    }
    airplay_video->master_playlist = master_playlist;
}

char *get_master_playlist(airplay_video_t *airplay_video) {
    return  airplay_video->master_playlist;
}

/* media_data_store */

int get_num_media_uri(airplay_video_t *airplay_video) {
    return airplay_video->num_uri;
}

void destroy_media_data_store(airplay_video_t *airplay_video) {
    media_item_t *media_data_store = airplay_video->media_data_store; 
    if (media_data_store) {
        for (int i = 0; i < airplay_video->num_uri ; i ++ ) {
	  if (media_data_store[i].uri) {
                free (media_data_store[i].uri);
            }
            if (media_data_store[i].playlist) {
                free (media_data_store[i].playlist);
            }
        }
    }
    free (media_data_store);
    airplay_video->num_uri = 0;
}

void create_media_data_store(airplay_video_t * airplay_video, char ** uri_list, int num_uri) {  
    destroy_media_data_store(airplay_video);
    media_item_t *media_data_store = calloc(num_uri, sizeof(media_item_t));
    for (int i = 0; i < num_uri; i++) {
        media_data_store[i].uri = uri_list[i];
        media_data_store[i].playlist = NULL;
        media_data_store[i].access = 0;
    }
    airplay_video->media_data_store = media_data_store;
    airplay_video->num_uri = num_uri;
}

int store_media_data_playlist_by_num(airplay_video_t *airplay_video, char * media_playlist, int num) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if ( num < 0 ||  num >= airplay_video->num_uri) {
      return -1;
    } else if (media_data_store[num].playlist) {
      return -2;
    }
    media_data_store[num].playlist = media_playlist;
    return 0;
}

char * get_media_playlist_by_num(airplay_video_t *airplay_video, int num) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if (media_data_store == NULL) {
        return NULL;
    }
    if (num >= 0 && num <airplay_video->num_uri) {
        return media_data_store[num].playlist;
    }
    return NULL;
}

char * get_media_playlist_by_uri(airplay_video_t *airplay_video, const char *uri) {
  /* Problem: there can be more than one StreamInf playlist with the same uri:
   * they differ by choice of partner Media (audio, subtitles) playlists 
   * If the same uri is requested again, one of the other ones  will be returned
   * (the least-previously-requested one will be served up)
   */ 
    media_item_t *media_data_store = airplay_video->media_data_store;
    if (media_data_store == NULL) {
        return NULL;
    }
    int found = 0;;
    int num = -1;
    int access = -1;
    for (int i = 0; i < airplay_video->num_uri; i++) {
        if (strstr(media_data_store[i].uri, uri)) {
            if (!found) {
                found = 1;
                num = i;
                access  = media_data_store[i].access;
            } else {
                /* change > below to >= to reverse the order of choice */
                if (access > media_data_store[i].access) {
                    access = media_data_store[i].access;
                    num = i;
                }
            }
        }
    }
    if (found) {
        printf("found %s\n", media_data_store[num].uri);
        ++media_data_store[num].access;
        return media_data_store[num].playlist;
    }
    return NULL;
}

char * get_media_uri_by_num(airplay_video_t *airplay_video, int num) {
    media_item_t * media_data_store = airplay_video->media_data_store;
    if (media_data_store == NULL) {
        return NULL;
    }
    if (num >= 0 && num < airplay_video->num_uri) {
        return  media_data_store[num].uri;
      }
    return NULL;
}

int get_media_uri_num(airplay_video_t *airplay_video, char * uri) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    for (int i = 0; i < airplay_video->num_uri ; i++) {
        if (strstr(media_data_store[i].uri, uri)) {
            return i;
        }
    }
    return -1;
}
