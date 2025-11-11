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
  int num;
};

struct airplay_video_s {
    raop_t *raop;
    char apple_session_id[37];
    char playback_uuid[37];
    char *uri_prefix;
    const char *lang;
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
                               const char *lang, const char *session_id) {
    char uri[] = "http://localhost:xxxxx";
    assert(raop);

    airplay_video_t *airplay_video = deregister_airplay_video(raop);
    if (airplay_video) {
        airplay_video_service_destroy(airplay_video);
    }

    /* calloc guarantees that the 36-character strings apple_session_id and 
       playback_uuid are null-terminated */
    airplay_video =  (airplay_video_t *) calloc(1, sizeof(airplay_video_t));
    if (!airplay_video) {
        return -1;
    }

    airplay_video->lang = lang;
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

    //printf(" %p %p\n", airplay_video, get_airplay_video(raop));

    airplay_video->raop = raop;
    airplay_video->FCUP_RequestID = 0;

    size_t len = strlen(session_id);
    assert(len == 36);
    strncpy(airplay_video->apple_session_id, session_id, len);
        
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

const char *get_playback_uuid(airplay_video_t *airplay_video) {
    return (const char *) airplay_video->playback_uuid; 
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

typedef struct language_s {
    char *start;
    int len;
    char type;
    char code[6];
} language_t;

language_t* master_playlist_process_language(char * data, int *slices, int *language_count) {
    *language_count = 0;
    char *ptr = data;
    int count = 0, count1 = 0, count2 = 0, count3 = 0;
    while (ptr) {
        ptr = strstr(ptr,"#EXT-X-MEDIA:URI=");
        if(!ptr) {
            break;
        }
        ptr = strstr(ptr, "LANGUAGE=");
        if(!ptr) {
            break;
        }
        ptr = strstr(ptr,"YT-EXT-AUDIO-CONTENT-ID=");
        if(!ptr) {
            break;
        }
        count++;
    }
    if (count == 0) {
        return NULL;
    }
    language_t *languages = (language_t *) calloc(count + 2, sizeof(language_t));
    languages[0].start = data;
    ptr = data;
    for (int i = 1; i <= count; i++) {
        char *end;
        if (!(ptr = strstr(ptr, "#EXT-X-MEDIA"))) {
            break;
        }
        count1++;
        if (i == 1) {
            languages[0].len = (int) (ptr - data);
            languages[0].type = ' ';
        }
        languages[i].start = ptr;
        if (!(ptr = strstr(ptr, "LANGUAGE="))) {
            break;
        }
        if (!strncmp(ptr - strlen("dubbed-auto") - 2, "dubbed-auto", strlen("dubbed-auto"))) {
            languages[i].type = 'd';
        } else if (!strncmp(ptr - strlen("dubbed") - 2, "dubbed", strlen("dubbed"))) {
            languages[i].type = 'd';
        } else if (!strncmp(ptr - strlen("original") - 2, "original", strlen("original"))) {
            languages[i].type = 'o';
        } else {
            languages[i].type = 'u';
        }
        count2++;
        if (!(ptr = strchr(ptr,'"'))) {
            break;
        }
        ptr++;
        if (!(end = strchr(ptr,'"'))) {
            break;
	}
        strncpy(languages[i].code, ptr, end - ptr);
        if (!(ptr = strchr(ptr,'\n'))) {
            break;
        }
        count3++;
        languages[i].len = (int) (ptr + 1 - languages[i].start);
    }
    assert (count1 == count && count2 == count && count3 == count);
    languages[count + 1].start = ++ptr;
    languages[count + 1].len = strlen(ptr);
    languages[count + 1].type = ' ';
    *slices = count + 2;
    int len = 0;
    int copies = 0;
    for (int i = 0; i < *slices; i++) {
        if (!strcmp(languages[i].code, languages[1].code)) {
            copies++;
        }
        len += languages[i].len;
     }
    if (copies == count) {
        /* only one language is offered, nothing to do */
        free (languages);
        return NULL;
    }

    *language_count = count/copies;
    assert(count == *language_count * copies);
    assert(len == (int) strlen(data));
    /* verify expected structure of language choice information */
    for (int i = 1; i <= count; i++) {
        if (i % *language_count) {
            assert(languages[i].type != 'o');
        } else {
            assert(languages[i].type == 'o');
        }
	int j = i - *language_count;
        if (j > 0) {
            assert (!strcmp(languages[i].code, languages[j].code));
        }
    }
    return languages;
}

void store_master_playlist(airplay_video_t *airplay_video, char *master_playlist) {
    int language_count, slices;  
    if (airplay_video->master_playlist) {
        free (airplay_video->master_playlist);
    }
    airplay_video->master_playlist = master_playlist;
    language_t *languages;
    if (!(languages = master_playlist_process_language(airplay_video->master_playlist,
                                                       &slices, &language_count))) {
        return;
    }
    /* audio is offered in multiple languages */ 
    char *str = calloc(6 * language_count, sizeof(char));
    int i;
    char *ptr = str;
    for (i = 0;  i <  language_count; i++) {
        sprintf(ptr,"%s ", languages[i + 1].code);
        ptr += strlen(languages[i + 1].code);
        ptr++;
        if ( i % 16 == 15) {
            sprintf(ptr++,"\n");
        }
    }
    if (i % 16 != 15) {
        sprintf(ptr++,"\n");
    }
    printf("%d available languages: %s", language_count, str);
    free(str);

    const char *ptrc = airplay_video->lang;
    char *lang = NULL;
    while (ptrc) {
        for (int i = 1; i <= language_count; i++) {
            if (!strncmp(languages[i].code, ptrc, 2)) {
                lang = languages[i].code;
                break;
            }
        }
        if (lang) {
            break;
        }
        ptrc = strchr(ptrc,':');
        if(ptrc) {
            ptrc++;
            if (strlen(ptrc) < 2) {
                break;
            }
        }
    }
    if (lang) {
        printf("language choice: %s (based on prefered languages list %s)\n\n",
               lang, airplay_video->lang);
    } else {
        if (airplay_video->lang) {
            printf("no match with prefered language list %s\n", airplay_video->lang);
        }
        lang = languages[language_count].code;
        printf("default language choice: %s\n\n", lang);
    }
    int len = 0;
    for (int i = 0; i < slices; i++) {
        if (strlen(languages[i].code) == 0 || !strcmp(languages[i].code, lang)) {
            len += languages[i].len;	
        }
    }
    airplay_video->master_playlist = (char *) calloc(len + 1, sizeof(char));
    ptr = airplay_video->master_playlist;
    for (int i = 0; i < slices; i++) {
        if (strlen(languages[i].code) == 0 || !strcmp(languages[i].code, lang)) {
            strncpy(ptr, languages[i].start, languages[i].len);
            ptr += languages[i].len;
        }
    }
    free (languages);
    free(master_playlist);
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
        media_data_store[i].num = i;
    }
    airplay_video->media_data_store = media_data_store;
    airplay_video->num_uri = num_uri;
}

int store_media_playlist(airplay_video_t *airplay_video, char * media_playlist, int num) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if ( num < 0 ||  num >= airplay_video->num_uri) {
        return -1;
    } else if (media_data_store[num].playlist) {
        return -2;
    }
    for (int i = 0; i < num ; i++) {
        if (strcmp(media_data_store[i].uri, media_data_store[num].uri) == 0) {
            assert(strcmp(media_data_store[i].playlist, media_playlist) == 0);
            media_data_store[num].num = i;
            free (media_playlist);
            return 1;
        }
    }
    media_data_store[num].playlist = media_playlist;
    return 0;
}

char * get_media_playlist(airplay_video_t *airplay_video, const char *uri) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if (media_data_store == NULL) {
        return NULL;
    }
    for (int i = 0; i < airplay_video->num_uri; i++) {
        if (strstr(media_data_store[i].uri, uri)) {
            return media_data_store[media_data_store[i].num].playlist;
        }
    }
    return NULL;
}

char * get_media_uri_by_num(airplay_video_t *airplay_video, int num) {
    media_item_t * media_data_store = airplay_video->media_data_store;
    if (num >= 0 && num < airplay_video->num_uri) {
        return  media_data_store[num].uri;
    }
    return NULL;
}

int analyze_media_playlist(char *playlist, float *duration) {
    float next;
    int count = 0;
    char *ptr = strstr(playlist, "#EXTINF:");
    *duration = 0.0f;
    while (ptr != NULL) {
        char *end;
        ptr += strlen("#EXTINF:");
        next = strtof(ptr, &end);
        *duration += next;
        count++;
        ptr = strstr(end, "#EXTINF:");
    }
    return count;
}

/* parse Master Playlist, make table of Media Playlist uri's that it lists */
int create_media_uri_table(const char *url_prefix, const char *master_playlist_data,
                           int datalen, char ***media_uri_table, int *num_uri) {
    char *ptr = strstr(master_playlist_data, url_prefix);
    char ** table = NULL;
    if (ptr == NULL) {
        return -1;
    }
    int count = 0;
    while (ptr != NULL) {
        char *end = strstr(ptr, "m3u8");
        if (end == NULL) {
            return 1;
        }
        end += sizeof("m3u8");
        count++;
        ptr = strstr(end, url_prefix);
    }
    table  = (char **)  calloc(count, sizeof(char *));
    if (!table) {
      return -1;
    }
    for (int i = 0; i < count; i++) {
        table[i] = NULL;
    }
    ptr = strstr(master_playlist_data, url_prefix);
    count = 0;
    while (ptr != NULL) {
        char *end = strstr(ptr, "m3u8");
	char *uri;
        if (end == NULL) {
            return 0;
        }
        end += sizeof("m3u8");
        size_t len = end - ptr - 1;
	uri  = (char *) calloc(len + 1, sizeof(char));
	memcpy(uri , ptr, len);
        table[count] = uri;
        uri =  NULL;	
	count ++;
	ptr = strstr(end, url_prefix);
    }
    *num_uri = count;

    *media_uri_table = table;
    return 0;
}

/* Adjust uri prefixes in the Master Playlist, for sending to the Media Player */
char *adjust_master_playlist (char *fcup_response_data, int fcup_response_datalen,
                              char *uri_prefix, char *uri_local_prefix) {
    size_t uri_prefix_len = strlen(uri_prefix);
    size_t uri_local_prefix_len = strlen(uri_local_prefix);
    int counter = 0;
    char *ptr = strstr(fcup_response_data, uri_prefix);
    while (ptr != NULL) {
        counter++;
        ptr++;
        ptr = strstr(ptr, uri_prefix);
    }

    size_t len = uri_local_prefix_len - uri_prefix_len;
    len *= counter;
    len += fcup_response_datalen;    
    char *new_master = (char *) malloc(len + 1);
    *(new_master + len) = '\0';
    char *first = fcup_response_data;
    char *new = new_master;
    char *last = strstr(first, uri_prefix);
    counter  = 0;
    while (last != NULL) {
        counter++;
        len = last - first;
        memcpy(new, first, len);
	first = last + uri_prefix_len;
        new += len;
        memcpy(new, uri_local_prefix, uri_local_prefix_len);
        new += uri_local_prefix_len;
        last = strstr(last + uri_prefix_len, uri_prefix);
	if (last  == NULL) {
            len = fcup_response_data  + fcup_response_datalen  - first;
            memcpy(new, first, len);
            break;
	}
    }
    return new_master;
}

char *adjust_yt_condensed_playlist(const char *media_playlist) {
/* this copies a Media Playlist into a null-terminated string. 
   If it has the "#YT-EXT-CONDENSED-URI" header, it is also expanded into 
   the full Media Playlist format.
   It  returns a pointer to the expanded playlist, WHICH MUST BE FREED AFTER USE */

    const char *base_uri_begin;
    const char *params_begin;
    const char *prefix_begin;
    size_t base_uri_len;
    size_t params_len;
    size_t prefix_len;
    const char* ptr = strstr(media_playlist, "#EXTM3U\n");

    ptr += strlen("#EXTM3U\n");
    assert(ptr);
    if (strncmp(ptr, "#YT-EXT-CONDENSED-URL", strlen("#YT-EXT-CONDENSED-URL"))) {
        size_t len = strlen(media_playlist);
        char * playlist_copy = (char *) malloc(len + 1);
        memcpy(playlist_copy, media_playlist, len);
        playlist_copy[len] = '\0';
        return playlist_copy;
    }
    ptr = strstr(ptr, "BASE-URI=");
    base_uri_begin = strchr(ptr, '"');
    base_uri_begin++;
    ptr = strchr(base_uri_begin, '"');
    base_uri_len = ptr - base_uri_begin;
    char *base_uri = (char *) calloc(base_uri_len + 1, sizeof(char));
    assert(base_uri);
    memcpy(base_uri, base_uri_begin, base_uri_len);  //must free

    ptr = strstr(ptr, "PARAMS=");
    params_begin = strchr(ptr, '"');
    params_begin++;
    ptr = strchr(params_begin,'"');
    params_len = ptr - params_begin;
    char *params = (char *) calloc(params_len + 1, sizeof(char));
    assert(params);
    memcpy(params, params_begin, params_len);  //must free

    ptr = strstr(ptr, "PREFIX=");
    prefix_begin = strchr(ptr, '"');
    prefix_begin++;
    ptr = strchr(prefix_begin,'"');
    prefix_len = ptr - prefix_begin;
    char *prefix = (char *) calloc(prefix_len + 1, sizeof(char));
    assert(prefix);
    memcpy(prefix, prefix_begin, prefix_len);  //must free

    /* expand params */
    int nparams = 0;
    int *params_size = NULL;
    const char **params_start = NULL;
    if (strlen(params)) {
        nparams = 1;
        char * comma = strchr(params, ',');
        while (comma) {
            nparams++;
            comma++;
            comma = strchr(comma, ',');
        }
        params_start = (const char **) calloc(nparams, sizeof(char *));  //must free
        params_size = (int *)  calloc(nparams, sizeof(int));     //must free
        ptr = params;
        for (int i = 0; i < nparams; i++) {
            comma = strchr(ptr, ',');
            params_start[i] = ptr;
            if (comma) {
                params_size[i] = (int) (comma - ptr);
                ptr = comma;
                ptr++;
            } else {
                params_size[i] = (int) (params + params_len - ptr);
                break;
            }
        }
    }

    int count = 0;
    ptr = strstr(media_playlist, "#EXTINF");
    while (ptr) {
        count++;
        ptr = strstr(++ptr, "#EXTINF");
    }

    size_t old_size = strlen(media_playlist);
    size_t new_size = old_size;
    new_size += count * (base_uri_len + params_len);
 
    char * new_playlist = (char *) calloc( new_size + 100, sizeof(char));
    const char *old_pos = media_playlist;
    char *new_pos = new_playlist;
    ptr = old_pos;
    ptr = strstr(old_pos, "#EXTINF:");
    size_t len = ptr - old_pos;
    /* copy header section before chunks */
    memcpy(new_pos, old_pos, len);
    old_pos += len;
    new_pos += len;
    while (ptr) {
        /* for each chunk */
        const char *end = NULL;
        char *start = strstr(ptr, prefix);
        len = start - ptr;
        /* copy first line of chunk entry */
        memcpy(new_pos, old_pos, len);
        old_pos += len;
        new_pos += len;
	
	/* copy base uri  to replace prefix*/
        memcpy(new_pos, base_uri, base_uri_len);
        new_pos += base_uri_len;
        old_pos += prefix_len;
        ptr = strstr(old_pos, "#EXTINF:");

        /* insert the PARAMS separators on the slices line  */
        end = old_pos;
        int last = nparams - 1;
        for (int i = 0; i < nparams; i++) {
            if (i != last) {
                end = strchr(end, '/');
            } else {
                /* the next line starts with either #EXTINF (usually) 
                or #EXT-X-ENDLIST (at last chunk)*/
	        end = strstr(end, "#EXT");
            }
            *new_pos = '/';
            new_pos++;
            memcpy(new_pos, params_start[i], params_size[i]);
            new_pos += params_size[i];
            *new_pos = '/';
            new_pos++;

            len = end - old_pos;
            end++;

            memcpy (new_pos, old_pos, len);
            new_pos += len;
            old_pos += len;
            if (i != last) {
                old_pos++; /* last entry is not followed by "/" separator */
            }
        }
    }
    /* copy tail */
     
    len = media_playlist + strlen(media_playlist) - old_pos;
    memcpy(new_pos, old_pos, len);
    new_pos += len;
    old_pos += len;

    new_playlist[new_size] = '\0';

    free (prefix);
    free (base_uri);
    free (params);
    if (params_size) {
        free (params_size);
    }
    if (params_start) {
        free (params_start);
    }  

    return new_playlist;
}
