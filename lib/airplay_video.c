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
    const char *lang;
    const char *lang_subtitles;
    const char *lang_system;
    int next_uri;
    int FCUP_RequestID;
    float start_position_seconds;
    float resume_position_seconds;
    playback_info_t *playback_info;
    char *master_playlist;
    media_item_t *media_data_store;
    int num_uri;
};

typedef struct slice_s{
    const char *first;
    const char *last;
    bool delete;
    unsigned char is_default;
    unsigned char is_autoselect;
    char type;
} slice_t;

//  initialize airplay_video service.
airplay_video_t *airplay_video_init(raop_t *raop, unsigned short http_port, const char *lang, const char *lang_subtitles, const char *lang_system) {
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
    airplay_video->lang_system = lang_system;
    airplay_video->lang_subtitles = lang_subtitles;
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

static char * list_languages(const char *master_playlist, int n_slice, slice_t *slice, char type, int *nlang, char **default_lang, bool autoselect)  {
    char * list = NULL;
    *default_lang = NULL;
    const char *lang = NULL;
    size_t len = 0;
    int count = 0;
    for (int i = 0; i < n_slice ; i++) {
        if (slice[i].type != type) {
            continue;
        }
        lang = strstr(slice[i].first, "LANGUAGE=\"");
        if (lang && lang < slice[i].last) {
            lang = strchr(lang, '"');
            lang++;
            len += strchr(lang, '"') - lang + 1;
        }
    }

    if (len == 0) {
        return NULL;
    }

    list = (char *) calloc(len + 1, sizeof(char));
    char *pos = list;
    for (int i = 0; i < n_slice ; i++) {
        if (slice[i].type != type) {
            continue;
        }
        lang = strstr(slice[i].first, "LANGUAGE=\"");
        char * autoselect_no = NULL;
        if (autoselect) {
            /* eliminate AUTOSELECT=NO entries when autoselect = true */
            autoselect_no = strstr(slice[i].first, "AUTOSELECT=NO");
        }
        autoselect_no = autoselect_no < slice[i].last ? autoselect_no : NULL;
        if (autoselect_no) {
            continue;
        }
        char *default_choice = strstr(slice[i].first, "DEFAULT=YES");
        default_choice = default_choice < slice[i].last ? default_choice : NULL;
        if (lang && lang < slice[i].last) {
            lang = strchr(lang, '"');
            lang++;
            len = strchr(lang, '"') - lang;
            memcpy(pos, lang, len);
            if (pos == strstr(list, pos)) {
                if (default_choice) {
                    *default_lang = calloc(len + 1, sizeof(char));  /* must be freed*/
                    memcpy(*default_lang, lang, len);
                }
                pos += len;
                *pos = ',';
                count++;
                pos++;
            } else {
                memset(pos, 0, len);
            }
        }
    }
    *nlang = count;
    return list;   /* must be free'd */
}

static const char **unpack_list(char *list, char sepchar, int *list_count) {
    /* Unpacks a list "aa,bb,cc," or "aa,bb,cc" where here sepchar = ','.
       Returns a  list of null-terminated strings
       char **unpacked_list = {"aa", "bb", "cc"}
       (which must be freed by user).   Note: instances of sepchar in
       list are overwritten with '\0'.  
       unpacked_list cannot be used after list is freed. */
    int count = 0;
    char *ptr = list;
    while (ptr) {
        ptr = strchr(ptr, sepchar);
        if (ptr) {
            ptr++;
            count++;
            ptr = *ptr ? ptr : NULL;
        } else {
            count++;
        }
    }
    *list_count = count;
    if (!count) {
        return NULL;
    }
    const char **unpacked_list = (const char **) calloc(count, sizeof(char*));
    ptr = list;
    for (int i = 0 ; i < count; i++) {
        unpacked_list[i] = ptr;
        ptr = strchr(ptr, sepchar);
        if (ptr) {
            *ptr = '\0';
            ptr++;
        }
    }
    return unpacked_list;
}

// Helper to normalize a single character safely for ASCII
static char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    if (c == '_') {
        return '-'; // Normalize underscores to hyphens
    }
    return c;
}

// Safely compares two HLS language tags
static bool match_hls_language(const char *tag1, const char *tag2) {
    /* matches pt to pt and pt to pt-BR
    and pt-BR to PT_br (case and hyphen/underscore insensitive),
    but not pt to ptt or pt-BR to pt */
    if (!tag1 || !tag2) return false;
    while (*tag2) {
        if (!(*tag1)) {
            return (ascii_tolower(*tag2) == '-');
        }
        if (ascii_tolower(*tag1) != ascii_tolower(*tag2)) {
            return false;
        }
        tag1++;
        tag2++;
        if (!(*tag1) && !(*tag2)) {
            return true;  //strict equivalence
        }
    }
    return false;
}

static const char* strict_match_language(const char* preferred[], size_t pref_count,
                                  const char* available[], size_t avail_count)
{
    // Loop through each preferred language in order of priority
    for (size_t i = 0; i < pref_count; i++) {
        for (size_t j = 0; j < avail_count; j++) {
            if (match_hls_language(preferred[i], available[j])) {
                return available[j];
            }
        }
    }
    return NULL;
}

#define MAX_CODE_LEN 16

/**
 * Finds the best available language based on user preferences.
 * 
 * @param preferred Array of preferred language codes, ordered by preference.
 * @param pref_count Number of elements in the preferred array.
 * @param available Array of available language codes.
 * @param avail_count Number of elements in the available array.
 * @param default_lang The fallback language to return if no match is found.
 * @return Pointer to the best available language string, or default_lang.
 */
static const char* match_language(const char* preferred[], size_t pref_count,
                           const char* available[], size_t avail_count,
                           const char* default_lang) 
{
    char buffer[MAX_CODE_LEN];

    // Loop through each preferred language in order of priority
    for (size_t i = 0; i < pref_count; i++) {
        const char* current_pref = preferred[i];
        if (!current_pref) continue;

        // Copy to a mutable buffer to safely truncate subtags
        strncpy(buffer, current_pref, MAX_CODE_LEN - 1);
        buffer[MAX_CODE_LEN - 1] = '\0';

        // Truncation loop (e.g., "en-US-ca" -> "en-US" -> "en")
        while (strlen(buffer) > 0) {
            // Check current state of buffer against all available languages
            for (size_t j = 0; j < avail_count; j++) {
                if (match_hls_language(buffer, available[j])) {
                    return available[j]; // Match found! Return the original pointer
                }
            }
            // Find the rightmost hyphen to strip the next subtag
            char* last_hyphen = strrchr(buffer, '-');
            if (last_hyphen != NULL) {
                *last_hyphen = '\0'; // Truncate at the hyphen
            } else {
                break; // No more subtags left to strip
            }
        }
    }

    // Return global fallback if no preferences matched any available tags
    return default_lang;
}

/*identify unwanted language renditions (AUDIO, SUBTITLES) in master_playlist */
static slice_t *master_playlist_slicer(const char *master_playlist, airplay_video_t *airplay_video, int *slices, bool *subtitles) {
    int count = 0;
    *subtitles = false;
    const char *ptr = master_playlist;
    while (ptr) {
        ptr = strchr(ptr + 1, '\n');
        if (ptr) {
            count++;
        }
    }
    *slices = count;
    /* count is number of lines ending with '\n' in playlist */ 

    slice_t *slice = (slice_t *) calloc(count, sizeof(slice_t));
    ptr = master_playlist;
    int index = 0;
    while (index < count) {
        slice[index].delete = false;
        slice[index].first = ptr;
        slice[index].last = (char *) strchr(slice[index].first,'\n');
        slice[index].is_default = 0;
        slice[index].is_autoselect = 0;
        ptr  = slice[index].last + 1;
        const char *type = strstr(slice[index].first,"TYPE=");
        if (type) {
            type = type < slice[index].last ? type : NULL;
        }
        if (type) {
            type += strlen("TYPE=");
            if (!strncmp(type, "AUDIO", strlen("AUDIO"))) {
                slice[index].type = 'a';
            } else if (!strncmp(type, "SUBTITLES", strlen("SUBTITLES"))) {
                slice[index].type = 's';
            } else if (!strncmp(type, "VIDEO", strlen("VIDEO"))) {
                slice[index].type = 'v';
            } else if (!strncmp(type, "CLOSED-CAPTIONS", strlen("CLOSED-CAPTIONS"))) {
                slice[index].type = 'c';
            } else {
                printf("invalid EXT-X-MEDIA tag  TYPE=%s",type);
                exit(1);
            }
            const char *text = strstr(slice[index].first, "DEFAULT=");
            text = text < slice[index].last ? text : NULL;
            if (text) {
                text = strchr(text, '=') + 1;
                slice[index].is_default = !strncmp(text, "NO", 2) ? 1 : 2;
            }

            text = strstr(slice[index].first, "AUTOSELECT=");
            text = text < slice[index].last ? text : NULL;
            if (text) {
                text = strchr(text, '=') + 1;
                slice[index].is_autoselect = !strncmp(text, "NO", 2) ? 1 : 2;
            }
        } else {
            slice[index].type = '\0';	  
        }
        index++;
    }

    char *lang_requested = NULL;
    const char **lang_requested_list = NULL;
    int n_lang_requested = 0;
    if (airplay_video->lang) {
        lang_requested = (char *) calloc(strlen(airplay_video->lang) + 1, sizeof(char));
        memcpy(lang_requested, airplay_video->lang, strlen(airplay_video->lang));
        printf("%s", lang_requested);
        lang_requested_list = unpack_list(lang_requested, ':', &n_lang_requested);
        printf(" (%d requested languages)\n", n_lang_requested);
        if (!n_lang_requested) {
            free(lang_requested);
        }
    }

    char *lang_subtitles = NULL;
    const char **lang_subtitles_list = NULL;
    bool lang_subtitles_request = false;
    int n_lang_subtitles = 0;
    if (airplay_video->lang_subtitles) {
        lang_subtitles = (char *) calloc(strlen(airplay_video->lang_subtitles) + 1, sizeof(char));
        memcpy(lang_subtitles, airplay_video->lang_subtitles, strlen(airplay_video->lang_subtitles));
        printf("%s", lang_subtitles);
        lang_subtitles_list = unpack_list(lang_subtitles, ':', &n_lang_subtitles);
        printf(" (%d requested languages (subtitles))\n", n_lang_subtitles);
        if (!n_lang_subtitles) {
            free(lang_subtitles);
        } else {
            lang_subtitles_request = true;
        }
    }

    char *lang_system = (char *) calloc(strlen(airplay_video->lang_system) + 1, sizeof(char));
    memcpy(lang_system, airplay_video->lang_system, strlen(airplay_video->lang_system));
    int n_lang_system  = 0;
    printf("%s", lang_system);
    const char **lang_system_list = unpack_list(lang_system, ':', &n_lang_system);
    printf(" (%d system languages)\n", n_lang_system);
    assert(n_lang_system);

    /* first try to match user-requested languages, if present, ignoring AUTOSELECT value */
    bool audio_lang_selected = false;
    bool audio_lang_default_selected = false;
    bool subtitle_lang_selected = false;
    bool listed_audio_languages = true;
    bool listed_subtitle_languages = true;
    for (int iter = 0; iter < 4; iter++) {
        const char **lang_list  = NULL;
        char *available = NULL;
        const char **available_list = NULL;
        int n_lang = 0;
        int n_items = 0;
        int n_list = 0;
        char *default_lang = NULL;
        bool autoselect = false;
        char type = '\0';
        const char *selected = NULL;
        switch (iter) {
        case 0:  /* requested audio */
            if (!n_lang_requested) {
                continue;
            }
            n_lang = n_lang_requested;
            lang_list = lang_requested_list;
            type = 'a';
            autoselect = false;
            break;
        case 1:  /* system audio */
            if (audio_lang_selected || !listed_audio_languages) {
                continue;
            }
            n_lang = n_lang_system;
            lang_list  = lang_system_list;
            type = 'a';
            autoselect = true;
            break;
        case 2:  /* requested subtitles */
            if (!n_lang_requested  && !n_lang_subtitles) {
                continue;
            }
           if (n_lang_subtitles) {
                n_lang = n_lang_subtitles;
                lang_list = lang_subtitles_list;
            } else {
                n_lang = n_lang_requested;
                lang_list = lang_requested_list;
            }
	    type = 's';
            autoselect = false;
            break;
        case 3:  /* system subtitles */
            if (subtitle_lang_selected || !listed_subtitle_languages) {
                continue;
            }
            /* if we are here, any subtitle language request was not matched */
            lang_subtitles_request = false;
            n_lang = n_lang_system;
            lang_list = lang_system_list;
            type = 's';
            autoselect = true;
        }

        available = list_languages(master_playlist, count, slice, type, &n_items, &default_lang, autoselect);
        switch (iter) {
        case 0:
            if (n_items) {
                printf("%d available languages (AUDIO) %s\n", n_items, available);
            } else {
                listed_audio_languages = false;
                continue;
            }
            break;
        case 1:
            if (n_items) {
                printf("%d available languages (AUDIO, AUTOSELECT) %s\n", n_items, available);
            } else {
                continue;
            }
            break;
        case 2:
            if (n_items) {
                printf("%d available languages (SUBTITLES) %s\n", n_items, available);
            } else {
                listed_subtitle_languages = false;
                continue;
            }
            break;
        case 3:
            if (n_items) {
                printf("%d available languages (SUBTITLES, AUTOSELECT) %s\n", n_items, available);
            } else {
                continue;
            }
        }
        available_list = unpack_list(available,',',&n_list);
        assert(n_list == n_items);
        if (autoselect) {
            selected = match_language(lang_list, n_lang, available_list, n_list, NULL);
            if (!selected && type == 'a') {
                selected = default_lang;
                audio_lang_default_selected = true;
                *subtitles = true;
            }
        } else {
            selected = strict_match_language(lang_list, n_lang, available_list, n_list);
        }

        if (selected) {
            printf("iteration %d type='%c',  selected language: %s\n",iter, type, selected);
            switch (iter) {
            case 0:
            case 1:
                audio_lang_selected = true;
                break;
            case 2:
            case 3:
                subtitle_lang_selected = true;
            }
            for (int i = 0; i < count ; i++) {
                if (slice[i].type == type) {
                    const char *language =  strstr(slice[i].first, "LANGUAGE=\"");
                    if (language) {
                        language = strchr(language, '"') + 1;
                        language = language < slice[i].last ? language : NULL;
                    }
                    if (language) {
                        size_t len = strchr(language, '"') - language;
                        if (strncmp(language, selected, len)) {
                            slice[i].delete = true;
                        }
                    }
                }
            }
        }
        free(available_list);
        free(available);
    }
    
    if (n_lang_requested) {
        free(lang_requested_list);
        free(lang_requested);
    }

    if (n_lang_subtitles) {
        free(lang_subtitles_list);
        free(lang_subtitles);
    }

    free(lang_system_list);
    free(lang_system);

    if (subtitle_lang_selected) {
        /* show subtitles if:
        (1) a subtitle language in list specifed by -slang ...  was matched
        (2) no audio language was selected (this generally means that there is
            a single AUDIO rendition with unspecified LANGUAGE)
        (3) a DEFAULT audio language (not found in lang_system) was selected
        */
        *subtitles =  (lang_subtitles_request || !audio_lang_selected || audio_lang_default_selected);
    }

    return slice;
}

char * select_master_playlist_language(airplay_video_t *airplay_video, char *master_playlist) {
    assert(master_playlist);
    /* filter out unwanted language renderings (AUDIO, SUBTITLES)  from  master playlist

    keep just one language per AUDIO rendition group, set DEFAULT=YES, AUTOSELECT= YES.

    if subtitles are present, keep only one language per SUBTITLE rendition group,
    if subtitles should be diplayed, set DEFAULT=YES, AUTOSELECT= YES.
    if they should not be displayed, set DEFAULT=NO, AUTOSELECT= NO.
    */
    int n_slice;  
    char *new_master_playlist = master_playlist;
    bool subtitles;
    slice_t *slice = master_playlist_slicer(master_playlist, airplay_video, &n_slice, &subtitles);

    size_t removed = 0;
    size_t added = 0;
    bool changed = false;

    char str_default_yes[] = "DEFAULT=YES";
    size_t len_default_yes = strlen(str_default_yes);
    char str_default_no[] = "DEFAULT=NO";
    size_t len_default_no = strlen(str_default_no);
    char str_autoselect_yes[] = "AUTOSELECT=YES";
    size_t len_autoselect_yes = strlen(str_autoselect_yes);
    char str_autoselect_no[] = "AUTOSELECT=NO";
    size_t len_autoselect_no = strlen(str_autoselect_no);

    for (int i = 0; i < n_slice; i++) {
        if (slice[i].delete) {
            removed += slice[i].last + 1 - slice[i].first;
            changed = true;
            continue;
        }
        if (slice[i].type == 'a' || (slice[i].type == 's' && subtitles)) {
            if (slice[i].is_default == 0) {
                added += len_default_yes + 1;
                changed = true;
            } else if (slice[i].is_default == 1) {
                added++;
                changed = true;
            }
            if (slice[i].is_autoselect == 0) {
                added += len_autoselect_yes + 1;
                changed = true;
            } else if (slice[i].is_autoselect == 1) {
                added++;
                changed = true;
            }
        } else if (slice[i].type == 's' && !subtitles) {
            if (slice[i].is_default == 0) {
                added += len_default_no + 1;
                changed = true;
            } else if (slice[i].is_default == 2) {
                removed++;
                changed = true;
            }
            if (slice[i].is_autoselect == 0) {
                added += len_autoselect_no + 1;
                changed = true;
            } else if (slice[i].is_autoselect == 2) {
                removed++;
                changed = true;
            }
        }
    }

    if (changed) {
        size_t newlen = strlen(master_playlist) + added  - removed;
        new_master_playlist = (char *) calloc(newlen + 1, sizeof(char));
        char *new = new_master_playlist;
        for (int i = 0; i < n_slice; i++) {
            if (slice[i].delete) {
                continue;
            }
            if (slice[i].type == 'a' || slice[i].type == 's') {
                const char *ptr = slice[i].first;
                while (ptr < slice[i].last) {
                    if (!strncmp(ptr, str_default_yes, len_default_yes)) {
                        ptr += len_default_yes;
                        if (*ptr == ',') {
                            ptr++;
                        }
                        continue;
                    } else if (!strncmp(ptr, str_default_no, len_default_no)) {
                        ptr += len_default_no;
                        if (*ptr == ',') {
                            ptr++;
                        }
                        continue;
                    } else if (!strncmp(ptr, str_autoselect_yes, len_autoselect_yes)) {
                        ptr += len_autoselect_yes;
                        if (*ptr == ',') {
                            ptr++;
                        }
                        continue;
                    } else if (!strncmp(ptr, str_autoselect_no, len_autoselect_no)) {
                        ptr += len_autoselect_no;
                        if (*ptr == ',') {
                            ptr++;
                        }
                        continue;
                    }
                    *(new++) = *ptr;
                    ptr++;
                }
                if (slice[i].type == 'a' || (slice[i].type == 's' && subtitles)) {
                    *(new++) = ',';
                    memcpy(new, str_default_yes, len_default_yes);
                    new += len_default_yes;
                    *(new++) = ',';
                    memcpy(new, str_autoselect_yes, len_autoselect_yes);
                    new += len_autoselect_yes;
                    *(new++)='\n';
                } else if (slice[i].type == 's' && !subtitles) {
                    *(new++) = ',';
                    memcpy(new, str_default_no, len_default_no);
                    new += len_default_no;
                    *(new++) = ',';
                    memcpy(new, str_autoselect_no, len_autoselect_no);
                    new += len_autoselect_no;
                    *(new++)='\n';
                }
            } else {
                size_t len = slice[i].last + 1 - slice[i].first;
                memcpy(new, slice[i].first, len);
                new += len;
            }
        }
        assert(new == new_master_playlist + newlen);
        free (master_playlist);
    }
    free(slice);
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
    const char *ptr = strstr(master_playlist_data, url_prefix);
    char ** table = NULL;
    if (ptr == NULL) {
        return -1;
    }
    int count = 0;
    while (ptr != NULL) {
        const char *end = strstr(ptr, "m3u8");
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
        const char *end = strstr(ptr, "m3u8");
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

char *adjust_yt_condensed_playlist(const char *media_playlist, int n_chunks) {
    /* this copies a Media Playlist of n_chunks segments into a null-terminated string. 
    If it has the "#YT-EXT-CONDENSED-URI" header, it is also expanded into 
    the full Media Playlist format.   (Valid YT-EXT-CONDENSED-URI headers are assumed to occur
    prior to #EXTINF tags in  media playlists, and to contain non-empty BASE-URI, PARAMS,
    and PREFIX entries; only one such tag per playlist.)

    It  returns a pointer to the expanded playlist, WHICH MUST BE FREED AFTER USE */

    const char* EXTINF = "#EXTINF";

    const char *base_uri = NULL;
    const char *params_begin = NULL;
    const char *prefix_begin = NULL;

    size_t base_uri_len = 0;
    size_t params_len = 0;
    size_t prefix_len = 0;
   
    const char *yt = NULL;
    const char *ptr = NULL;
    const char *end = NULL;
    const char *first_chunk = NULL;

    if (n_chunks > 0) {
        first_chunk = strstr(media_playlist, EXTINF);
        assert(first_chunk);
        yt = strstr(media_playlist, "#YT-EXT-CONDENSED-URL");
        if (yt && yt < first_chunk) {
            base_uri = strstr(yt, "BASE-URI=\"");
            if (base_uri) {
                base_uri = strchr(base_uri,'"');
                base_uri++;
                end = strchr(base_uri,'"');
                if (end) {
                    base_uri_len = end - base_uri;
                }
            }
            params_begin = strstr(yt,"PARAMS=\"");
            if (params_begin) {
                params_begin = strchr(params_begin, '"');
                params_begin++;
                end = strchr(params_begin,'"');
                if (end) {
                    params_len = end - params_begin;
                }
            }
            prefix_begin = strstr(yt,"PREFIX=\"");
            if (prefix_begin) {
                prefix_begin = strchr(prefix_begin, '"');
                prefix_begin++;
                end = strchr(prefix_begin,'"');
                if (end) {
                    prefix_len = end - prefix_begin;
                } 
            }
        }
    } 
    if (!yt) {
        /* no YT_CONDENSED_URI's to expand: return unchanged copy of media playlist */
        size_t len = strlen(media_playlist);
        char * playlist_copy = (char *) malloc(len + 1);
        if (!playlist_copy) {
            printf("Memory allocation failure (playlist_copy)\n");
            exit(1);
        }
        memcpy(playlist_copy, media_playlist, len);
        playlist_copy[len] = '\0';
        return playlist_copy;
    } else {
        if (!base_uri_len || !params_len || !prefix_len)  {	  
            /* invalid YT-EXT-CONDENSED-URI tag */
            return NULL;
        }
    }

    /* expand params */
    int nparams = 1;
    for (int i = 0; i < (int) params_len ; i++) {
        if (*(params_begin + i) == ',') {
            nparams++;
        }
    }

    int *params_size = NULL;
    const char **params = NULL;
    params = (const char **) calloc(nparams, sizeof(char *));  //must free
    params_size = (int *)  calloc(nparams, sizeof(int));     //must free
    if (!params || !params_size) {
        printf("Memory allocation failure (params/params_size)\n");
        exit(1);
    }

    ptr = params_begin;
    int last_param = nparams - 1;
    for (int i = 0; i < nparams; i++) {
        params[i] = ptr;
        if (i != last_param) {
            end = strchr(ptr, ',');
            params_size[i] = (int) (end - ptr);
            ptr = ++end;
        } else {
            params_size[i] = (int) (params_begin + params_len - ptr);
            break;
        }
    }

    /* n_chunks is the number of media segments to process */
    size_t old_size = strlen(media_playlist);
    size_t new_len = old_size;

    /* only PREFIX="s/" (strlen = 2) has been seen */ 
    new_len += n_chunks * (base_uri_len + params_len + 2 - prefix_len);

    char *prefix = malloc(prefix_len + 1);  // must free
    prefix[prefix_len] = '\0';
    memcpy(prefix, prefix_begin, prefix_len);

    char * new_playlist = (char *) malloc(new_len + 1);
    if (!new_playlist) {
        printf("Memory allocation failure (new_playlist)\n");
        exit(1);
    }
    new_playlist[new_len] = '\0';

    const char *old_pos = media_playlist;
    char *new_pos = new_playlist;
    size_t len = first_chunk - old_pos;

    /* copy header section before chunks */
    size_t written = len;
    assert(written <= new_len);
    memcpy(new_pos, old_pos, len);
    old_pos += len;
    new_pos += len;

    int count = 0;
    while (count < n_chunks) {
        count++;
        /* for each chunk */
        const char *start = strstr(old_pos, prefix);
        assert(start);
        len = start - old_pos;
        /* copy first line of chunk entry */
        written += len;
        assert(written <= new_len);
        memcpy(new_pos, old_pos, len);
        old_pos += len;
        new_pos += len;

        /* copy base uri + '/'  to replace prefix*/
        written += base_uri_len;
        assert(written <= new_len);	
        memcpy(new_pos, base_uri, base_uri_len);
        new_pos += base_uri_len;
        written++;
        assert(written <= new_len);		
        *new_pos = '/';
        new_pos++;
        old_pos += prefix_len;

        /* insert the PARAMS and separators on the slices line  */
        for (int i = 0; i < nparams; i++) {
            if (i != last_param) {
                end = strchr(old_pos, '/');
                assert(end);
                end++;
            } else if (count < n_chunks) {
                end = strstr(old_pos, EXTINF);
                assert(end);
            } else {
                end = media_playlist + strlen(media_playlist);
            }
            written += params_size[i] + 1;
            assert(written <= new_len);
            memcpy(new_pos, params[i], params_size[i]);
            new_pos += params_size[i];
            *new_pos = '/';
            new_pos++;

            len = end - old_pos;
            written += len;
            assert(written <= new_len);
            memcpy (new_pos, old_pos, len);
            new_pos += len;
            old_pos += len;
        }
    }
    assert (count == n_chunks);
    assert (written == new_len);
 
    free (prefix);
    if (params_size) {
        free (params_size);
    }
    if (params) {
        free (params);
    }  
    return new_playlist;
}
