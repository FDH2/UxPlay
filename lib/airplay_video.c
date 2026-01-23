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

typedef enum playlist_type_e {
    NONE,
    VOD,
    EVENT
} playlist_type_t;

struct media_item_s {
    char *uri;
    char *playlist;
    int num;
    int count;
    float duration;
    bool endlist;
    playlist_type_t playlist_type;
    int hls_version;
    int media_sequence;
};

struct airplay_video_s {
    raop_t *raop;
    char *apple_session_id;
    char *playback_uuid;
    char *uri_prefix;
    char *local_uri_prefix;
    char *playback_location;
    char *language_name;
    char *language_code;
    const char *lang;
    int next_uri;
    int FCUP_RequestID;
    float start_position_seconds;
    float resume_position_seconds;
    playback_info_t *playback_info;
    char *master_playlist;
    media_item_t *media_data_store;
    int num_uri;
};

//  initialize airplay_video service.
airplay_video_t *airplay_video_init(raop_t *raop, unsigned short http_port, const char *lang) {
    char uri[] = "http://localhost:";
    char port[6] = { '\0' };
    assert(raop);

    /* calloc guarantees that the 36-character strings apple_session_id and 
       playback_uuid are null-terminated */
    airplay_video_t *airplay_video =  (airplay_video_t *) calloc(1, sizeof(airplay_video_t));

    if (!airplay_video) {
        return NULL;
    }

    airplay_video->lang = lang;
     /* create local_uri_prefix string */
    snprintf(port, sizeof(port), "%u", http_port);
    size_t len = strlen(uri) + strlen(port);
    airplay_video->local_uri_prefix = (char *) calloc (len + 1, sizeof(char));
    strcat(airplay_video->local_uri_prefix, uri);
    strcat(airplay_video->local_uri_prefix, port);

    airplay_video->raop = raop;
    airplay_video->FCUP_RequestID = 0;
    airplay_video->apple_session_id = NULL;
    airplay_video->start_position_seconds = 0.0f;
    airplay_video->playback_uuid = NULL;
    airplay_video->uri_prefix = NULL;
    airplay_video->playback_location = NULL;
    airplay_video->language_code = NULL;
    airplay_video->language_name = NULL;
    airplay_video->media_data_store = NULL;
    airplay_video->master_playlist = NULL;
    airplay_video->num_uri = 0;
    airplay_video->next_uri = 0;
    return airplay_video;
}

// destroy the airplay_video service
void
airplay_video_destroy(airplay_video_t *airplay_video) {
    if (airplay_video->apple_session_id) {
        free(airplay_video->apple_session_id);
    }
    if (airplay_video->playback_uuid) {
        free(airplay_video->playback_uuid);
    }
    if (airplay_video->uri_prefix) {
        free(airplay_video->uri_prefix);
    }
    if (airplay_video->local_uri_prefix) {
        free(airplay_video->local_uri_prefix);
    }
    if (airplay_video->playback_location) {
        free(airplay_video->playback_location);
    }
    if (airplay_video->language_name) {
        free(airplay_video->language_name);
    }
    if (airplay_video->language_code) {
       free(airplay_video->language_code);
    }
    if (airplay_video->media_data_store) {
        destroy_media_data_store(airplay_video);
    }
    if (airplay_video->master_playlist){
        free (airplay_video->master_playlist);
    }
    free (airplay_video);
    airplay_video = NULL;
}

void set_apple_session_id(airplay_video_t *airplay_video, const char * apple_session_id, size_t len) {
    assert(apple_session_id && len == 36);
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, apple_session_id, len);
    if (airplay_video->apple_session_id) {
        free(airplay_video->apple_session_id);
    }
    airplay_video->apple_session_id = str;
    str = NULL;
}

void set_playback_uuid(airplay_video_t *airplay_video, const char *playback_uuid, size_t len) {
    assert(playback_uuid && len == 36);
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, playback_uuid, len);
    if (airplay_video->playback_uuid) {
        free(airplay_video->playback_uuid);
    }
    airplay_video->playback_uuid = str;
    str = NULL;
}

void set_uri_prefix(airplay_video_t *airplay_video, const char *uri_prefix, size_t len) {
    assert(uri_prefix && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, uri_prefix, len);
    if (airplay_video->uri_prefix) {
        free(airplay_video->uri_prefix);
    }
    airplay_video->uri_prefix = str;
    str = NULL;
}

void set_playback_location(airplay_video_t *airplay_video, const char *location, size_t len) {
    assert(location && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, location, len);
    if (airplay_video->playback_location) {
        free(airplay_video->playback_location);
    }
    airplay_video->playback_location = str;
    str = NULL;
}

void set_language_name(airplay_video_t *airplay_video, const char *language_name, size_t len) {
    assert(language_name && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, language_name, len);
    if (airplay_video->language_name) {
        free(airplay_video->language_name);
    }
    airplay_video->language_name = str;
    str = NULL;
}

void set_language_code(airplay_video_t *airplay_video, const char *language_code, size_t len) {
    assert(language_code && len );
    char *str = (char *) calloc(len + 1, sizeof(char));
    if (!str) {
        printf("Memory allocation failed (str)\n");
        exit(1);
    }
    strncpy(str, language_code, len);
    if (airplay_video->language_code) {
        free(airplay_video->language_code);
    }
    airplay_video->language_code = str;
    str = NULL;
}


const char *get_apple_session_id(airplay_video_t *airplay_video) {
    if (!airplay_video || !airplay_video->apple_session_id) {
        return NULL;
    }
    return airplay_video->apple_session_id;
}

float get_duration(airplay_video_t *airplay_video) {
    if (!airplay_video || !airplay_video->media_data_store || !airplay_video->media_data_store->duration) {
        return 0.0f;
    }
    return airplay_video->media_data_store->duration;
}

float get_start_position_seconds(airplay_video_t *airplay_video) {
    return airplay_video->start_position_seconds;
}

float get_resume_position_seconds(airplay_video_t *airplay_video) {
    return airplay_video->resume_position_seconds;
}

void set_start_position_seconds(airplay_video_t *airplay_video, float start_position_seconds) {
    airplay_video->start_position_seconds = start_position_seconds;
}

void set_resume_position_seconds(airplay_video_t *airplay_video, float resume_position_seconds) {
    airplay_video->resume_position_seconds = resume_position_seconds;
}

const char *get_playback_uuid(airplay_video_t *airplay_video) {
    return (const char *) (!airplay_video ? NULL : airplay_video->playback_uuid); 
}

const char *get_playback_location(airplay_video_t *airplay_video) {
    return (const char *) (!airplay_video ? NULL : airplay_video->playback_location); 
}

const char *get_uri_prefix(airplay_video_t *airplay_video) {
  return (const char *) airplay_video->uri_prefix;
}

const char *get_language_name(airplay_video_t *airplay_video) {
  return (const char *)airplay_video->language_name;
}

const char *get_language_code(airplay_video_t *airplay_video) {
  return (const char *) airplay_video->language_code;
}

char *get_uri_local_prefix(airplay_video_t *airplay_video) {
    return airplay_video->local_uri_prefix;
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

void store_master_playlist(airplay_video_t *airplay_video, char *master_playlist) {
    if (airplay_video->master_playlist) {
        free (airplay_video->master_playlist);
    }
    airplay_video->master_playlist = master_playlist;
}

typedef struct language_s {
    const char *start;
    int len;
    bool is_default;
    char code[6];
    char *name;
} language_t;

language_t* master_playlist_process_language(const char * data, int *slices, int *language_count) {
    *language_count = 0;
    const char *ptr = data;
    int count = 0, count1 = 0;
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
    size_t length = 0;
    ptr = data;
    for (int i = 1; i <= count; i++) {
        char *end;
	int len_name;
        if (!(ptr = strstr(ptr, "#EXT-X-MEDIA"))) {
            break;
        }
        if (i == 1) {
            length = (int) (ptr - data);
            languages[0].start = data;
            languages[0].len = length;
            *languages[0].code = '\0';
            languages[0].name = NULL;
        }
        languages[i].start = ptr;

	if (!(ptr = strstr(ptr, "DEFAULT="))) {
            break;
        }
	ptr += strlen("DEFAULT=");
	languages[i].is_default = !strncmp(ptr, "YES", strlen("YES"));
	if (!(ptr = strstr(ptr, "NAME="))) {
            break;
        }
	ptr += strlen("NAME=");
	end = strchr(++ptr,'"');
	if (!end) {
            break;
        }
	len_name = end - ptr;
	languages[i].name = (char *) calloc(len_name + 1, sizeof(char));
	memcpy(languages[i].name, ptr, len_name *sizeof(char));
	if (!(ptr = strstr(ptr, "LANGUAGE="))) {
            break;
        }
        if (!(ptr = strchr(ptr,'"'))) {
            break;
        }
        if (!(end = strchr(++ptr,'"'))) {
            break;
        }
        strncpy(languages[i].code, ptr, end - ptr);
        if (!(ptr = strchr(ptr,'\n'))) {
            break;
        }
        count1++;
        languages[i].len = (int) (ptr + 1 - languages[i].start);
	length += languages[i].len;
    }
    assert (count1 == count);
    
    languages[count + 1].start = ++ptr;
    languages[count + 1].len = strlen(ptr);
    *languages[count + 1].code = '\0';
    languages[count + 1].name = NULL;

    length += languages[count + 1].len;
    assert(length == strlen(data));
    *slices = count + 2;

    int copies = 0;
    for (int i = 1; i < count; i++) {
        if (!strcmp(languages[i].code, languages[1].code)) {
            copies++;
        }
     }

    *language_count = count/copies;
    assert(count == *language_count * copies);

    /* verify expected structure of language choice information */
    for (int i = 1; i <= count; i++) {
  	int j = i - *language_count;
        if (j > 0) {
            assert (!strcmp(languages[i].code, languages[j].code));
        }
    }
    return languages;
}

char * select_master_playlist_language(airplay_video_t *airplay_video, char *master_playlist) {
    int language_count, slices;  
    language_t *languages;
    assert(master_playlist);
    if (!(languages = master_playlist_process_language(master_playlist,
                                                       &slices, &language_count))) {
        return master_playlist;
    }

    /* audio is offered in multiple languages */ 

    char *code = NULL;
    char *name = NULL;

    assert(airplay_video);
    printf("%d available languages:\n\n", language_count);
    int i_default = -1;
    
    const char *language_name = get_language_name(airplay_video);
    for (int i = 1; i <= language_count; i ++) {
        if (language_name) {
            if (!strcmp(language_name, languages[i].name)) {
                i_default = i;
            }
        } else if (languages[i].is_default) {
            i_default = i;
        }
        printf("%2d %-5.5s \"%s\" %s\n",i, languages[i].code, languages[i].name, (languages[i].is_default ? "(DEFAULT)" : ""));
    }
    printf("\n");
    assert(i_default >= 0);

    const char *ptrc = airplay_video->lang;;
    code = NULL;
    name = NULL;
    while (ptrc){
        for (int i = 1; i <= language_count; i++) {
            if (!strncmp(languages[i].code, ptrc, 2)) {
                code = languages[i].code;
                name = languages[i].name;
                printf("language choice: %s \"%s\" (based on prefered languages list %s)\n\n",
                       code, name,  airplay_video->lang);
                break;
            }
        }
        if (code) {
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

    if (!code) {
        code = languages[i_default].code;
        name = languages[i_default].name; 
        if (airplay_video->lang) {
            printf("no match with prefered language list %s\n", airplay_video->lang);
        }
        if (language_name) {
            printf("using HLS-specified language choice: %s \"%s\"\n\n", code, name); 
        } else {
            printf("using default language choice: %s \"%s\"\n\n", code, name);
        }
    } 

    /* update stored language code, name if changed */
    if (name != language_name) {   /* compare addresses */
        size_t len = strlen(name);
        char *new_language_name = (char *) calloc(len + 1, sizeof(char));
        char *new_language_code = (char *) calloc(len + 1, sizeof(char));
        if (!new_language_name || !new_language_code) {
            printf("Memory allocation failure (new_language_name/code\n");
            exit (1);
        }
        memcpy(new_language_name, name, len);
        set_language_name(airplay_video, new_language_name, len);
        len = strlen(code);
        memcpy(new_language_code, code, len);
        set_language_code(airplay_video, new_language_code, len);
    }
    
    int len = 0;
    for (int i = 0; i < slices; i++) {
        if (strlen(languages[i].code) == 0 || !strcmp(languages[i].code, code)) {
            len += languages[i].len;	
        }
    }
    char *new_master_playlist = (char *) calloc(len + 1, sizeof(char));

    char *ptr = new_master_playlist;
    for (int i = 0; i < slices; i++) {
        if (strlen(languages[i].code) == 0 || !strcmp(languages[i].code, code)) {
            strncpy(ptr, languages[i].start, languages[i].len);
            ptr += languages[i].len;
        }
    }

    for (int i = 1; i <= slices - 2 ; i++) {
        free (languages[i].name);
    }
    free (languages);
    free (master_playlist);
    
    return new_master_playlist;
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
    if (!media_data_store) {
        printf("Memory allocation failure (media_data_store)\n");
        exit(1);
    }
    for (int i = 0; i < num_uri; i++) {
        media_data_store[i].uri = uri_list[i];
        media_data_store[i].playlist = NULL;
        media_data_store[i].num = i;
        media_data_store[i].count = 0;
        media_data_store[i].duration = 0;
        media_data_store[i].endlist = false;
        media_data_store[i].playlist_type = NONE;
        media_data_store[i].hls_version = 0;
        media_data_store[i].media_sequence = 0;
    }
    airplay_video->media_data_store = media_data_store;
    airplay_video->num_uri = num_uri;
}


static int parse_media_playlist(media_item_t *media_item) {
    const char *ptr = media_item->playlist;
    char extm3u[] = "#EXTM3U";
    char extinf[] = "#EXTINF:";
    char extx[] = "#EXT-X-";
    char playlist_type[] = "PLAYLIST-TYPE:";
    char version[] = "VERSION:";
    char media_sequence[] = "MEDIA-SEQUENCE:";
    ptr = strstr(ptr, extm3u);
    if (!ptr) {
        return -1;
    }
    ptr++;
    while (ptr) {
        const char *ptr1 = NULL;
        ptr = strstr(ptr, "#EXT");
        if (!ptr || !memcmp(ptr, extinf, strlen(extinf))) {
            break;
        }
        ptr = strstr(ptr, extx);
        if (!ptr) {
            break;
        }
        if ((ptr1 = strstr(ptr, playlist_type))) {
            ptr1 += strlen(playlist_type);
            if (!memcmp(ptr1,"VOD", strlen("VOD"))) {
                media_item->playlist_type = VOD;
            } else if (!memcmp(ptr1,"EVENT", strlen("EVENT"))) {
                media_item->playlist_type = EVENT;
            }
            ptr1 = NULL;
        }
        if ((ptr1 = strstr(ptr, version))) {
            char *endptr = NULL;
            ptr1 += strlen(version);
            media_item->hls_version = (int) strtol(ptr1, &endptr, 10);
            ptr1 = NULL;
        }
        if ((ptr1 = strstr(ptr, media_sequence))) {
            char *endptr = NULL;
            ptr1 += strlen(media_sequence);
            media_item->media_sequence = (int) strtol(ptr1, &endptr, 10);
            ptr1 = NULL;
        }
        ptr += strlen(extx);
    }
    return 0;
}

int store_media_playlist(airplay_video_t *airplay_video, char * media_playlist, int *count, float *duration, bool *endlist, int num) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if ( num < 0 ||  num >= airplay_video->num_uri) {
        return -1;
    } else if (media_data_store[num].playlist) {
        return -2;
    }
    /* dont store duplicate media paylists */
    for (int i = 0; i < num ; i++) {
        if (strcmp(media_data_store[i].uri, media_data_store[num].uri) == 0) {
            assert(strcmp(media_data_store[i].playlist, media_playlist) == 0);
            media_data_store[num].num = i;
            free (media_playlist);
            return 1;
        }
    }
    media_item_t *media_item = &media_data_store[num];
    media_item->playlist = media_playlist;
    media_item->count = *count;
    media_item->duration = *duration;
    media_item->endlist = *endlist;
    parse_media_playlist(media_item);
    return 0;
}

char * get_media_playlist(airplay_video_t *airplay_video, int *count, float *duration, const char *uri) {
    media_item_t *media_data_store = airplay_video->media_data_store;
    if (media_data_store == NULL) {
        return NULL;
    }
    for (int i = 0; i < airplay_video->num_uri; i++) {
        if (strstr(media_data_store[i].uri, uri)) {
            *count = media_data_store[media_data_store[i].num].count;
            *duration = media_data_store[media_data_store[i].num].duration;
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

int analyze_media_playlist(char *playlist, float *duration, bool *endlist) {
    float next;
    int count = 0;
    char *ptr = strstr(playlist, "#EXTINF:");
    *duration = 0.0f;
    *endlist = false;
    char *end = NULL;
    while (ptr != NULL) {
        ptr += strlen("#EXTINF:");
        next = strtof(ptr, &end);
        *duration += next;
        count++;
        ptr = strstr(end, "#EXTINF:");
    }
    if (end) {
        *endlist = (strstr(end, "#EXT-X-ENDLIST"));
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
        if (!uri) {
            printf("Memory allocation failure (uri)\n");
            exit(1);
        }
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
                              const char *uri_prefix, char *uri_local_prefix) {
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
    int byte_count = 0;
    int new_len = (int) len;
    char *new_master = (char *) malloc(new_len + 1);
    if (!new_master) {
        printf("Memory allocation failure (new_master)\n");
        exit(1);
    }
    new_master[new_len] = '\0';
    char *first = fcup_response_data;
    char *new = new_master;
    char *last = strstr(first, uri_prefix);
    counter  = 0;
    while (last != NULL) {
        counter++;
        len = last - first;
        memcpy(new, first, len);
        byte_count += len;
        first = last + uri_prefix_len;
        new += len;
        memcpy(new, uri_local_prefix, uri_local_prefix_len);
        byte_count += uri_local_prefix_len;
        new += uri_local_prefix_len;
        last = strstr(last + uri_prefix_len, uri_prefix);
        if (last  == NULL) {
            len = fcup_response_data  + fcup_response_datalen  - first;
            memcpy(new, first, len);
            byte_count += len;
            break;
        }
    }
    assert(byte_count == new_len); 
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
        if (!playlist_copy) {
            printf("Memory allocation failure (playlist_copy)\n");
            exit(1);
        }
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
        if (!params_start || !params_size) {
            printf("Memory allocation failure (params_start/size)\n");
            exit(1);
        }
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
    size_t new_len = old_size;
    new_len += count * (base_uri_len + params_len);

    int byte_count = 0;
    char * new_playlist = (char *) malloc(new_len + 1);
    if (!new_playlist) {
        printf("Memory allocation failure (new_playlist)\n");
        exit(1);
    }
    new_playlist[new_len] = '\0';
    const char *old_pos = media_playlist;
    char *new_pos = new_playlist;
    ptr = old_pos;
    ptr = strstr(old_pos, "#EXTINF:");
    size_t len = ptr - old_pos;
    /* copy header section before chunks */
    memcpy(new_pos, old_pos, len);
    byte_count += len;
    old_pos += len;
    new_pos += len;
    while (ptr) {
        /* for each chunk */
        const char *end = NULL;
        char *start = strstr(ptr, prefix);
        len = start - ptr;
        /* copy first line of chunk entry */
        memcpy(new_pos, old_pos, len);
        byte_count += len;
        old_pos += len;
        new_pos += len;
	
	/* copy base uri  to replace prefix*/
        memcpy(new_pos, base_uri, base_uri_len);
        byte_count += base_uri_len;
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
            byte_count++;
            new_pos++;
            memcpy(new_pos, params_start[i], params_size[i]);
            byte_count += params_size[i];
            new_pos += params_size[i];
            *new_pos = '/';
            byte_count++;
            new_pos++;

            len = end - old_pos;
            end++;

            memcpy (new_pos, old_pos, len);
            byte_count += len;
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
    byte_count += len;
    new_pos += len;
    old_pos += len;

    assert(byte_count == (int) new_len);

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
