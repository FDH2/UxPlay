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

/* this file is part of raop.c and should not be included in any other file */

#include "airplay_video.h"
#include "fcup_request.h"

static void
http_handler_server_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                         char **response_data, int *response_datalen)  {

    assert(conn->raop->dnssd);
    int hw_addr_raw_len = 0;
    const char *hw_addr_raw = dnssd_get_hw_addr(conn->raop->dnssd, &hw_addr_raw_len);

    char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
    //int hw_addr_len =
    utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);

    plist_t r_node = plist_new_dict();

    /* first 12 AirPlay features bits (R to L): 0x27F = 0010 0111 1111
     * Only bits 0-6 and bit 9  are set:
     * 0. video supported
     * 1. photo supported
     * 2. video protected wirh FairPlay DRM
     * 3. volume control supported for video
     * 4. HLS supported
     * 5. slideshow supported
     * 6. (unknown)
     * 9. audio supported.
     */
    plist_t features_node = plist_new_uint(0x27F); 
    plist_dict_set_item(r_node, "features", features_node);

    plist_t mac_address_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "macAddress", mac_address_node);

    plist_t model_node = plist_new_string(GLOBAL_MODEL);
    plist_dict_set_item(r_node, "model", model_node);

    plist_t os_build_node = plist_new_string("12B435");
    plist_dict_set_item(r_node, "osBuildVersion", os_build_node);

    plist_t protovers_node = plist_new_string("1.0");
    plist_dict_set_item(r_node, "protovers", protovers_node);

    plist_t source_version_node = plist_new_string(GLOBAL_VERSION);
    plist_dict_set_item(r_node, "srcvers", source_version_node);

    plist_t vv_node = plist_new_uint(strtol(AIRPLAY_VV, NULL, 10));
    plist_dict_set_item(r_node, "vv", vv_node);

    plist_t device_id_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "deviceid", device_id_node);

    plist_to_xml(r_node, response_data, (uint32_t *) response_datalen);

    //assert(*response_datalen == strlen(*response_data));

    /* last character (at *response_data[response_datalen - 1]) is  0x0a = '\n'
     * (*response_data[response_datalen] is '\0').
     * apsdk removes the last "\n" by overwriting it with '\0', and reducing response_datalen by 1. 
     * TODO: check if this is necessary  */
    
    plist_free(r_node);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    free(hw_addr);
}    

static void
http_handler_scrub(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                   char **response_data, int *response_datalen) {
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float scrub_position = 0.0f;
    if (data) {
        data++;
        const char *position = strstr(data, "=") + 1;
        char *end;
        double value = strtod(position, &end);
        if (end && end != position) {
            scrub_position = (float) value;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_scrub: got position = %.6f",
                       scrub_position);	  
        }
    }
    logger_log(conn->raop->logger, LOGGER_DEBUG, "**********************SCRUB %f ***********************",scrub_position);
    conn->raop->callbacks.on_video_scrub(conn->raop->callbacks.cls, scrub_position);
}

static void
http_handler_rate(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float rate_value = 0.0f;
    if (data) {
        data++;
        const char *rate = strstr(data, "=") + 1;
        char *end;
        float value = strtof(rate, &end);
        if (end && end != rate) {
            rate_value =  value;
            logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_rate: got rate = %.6f", rate_value);
        }
    }
    conn->raop->callbacks.on_video_rate(conn->raop->callbacks.cls, rate_value);
}

static void
http_handler_stop(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {
    logger_log(conn->raop->logger, LOGGER_INFO, "client HTTP request POST stop");

    conn->raop->callbacks.on_video_stop(conn->raop->callbacks.cls);
}

/* handles PUT /setProperty http requests from Client to Server */

static void
http_handler_set_property(raop_conn_t *conn,
                          http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {

    const char *url = http_request_get_url(request);
    const char *property = url + strlen("/setProperty?");
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_set_property: %s", property);


    /*  actionAtItemEnd:  values:  
                  0: advance (advance to next item, if there is one)
                  1: pause   (pause playing)
                  2: none    (do nothing)             

        reverseEndTime   (only used when rate < 0) time at which reverse playback ends
        forwardEndTime   (only used when rate > 0) time at which reverse playback ends
        selectedMediaArray contains plist with language choice:
    */

    airplay_video_t *airplay_video = conn->raop->airplay_video[conn->raop->current_video];
    if (!strcmp(property, "selectedMediaArray")) {
        /* verify that this request contains a binary plist*/
        char *header_str = NULL;
        int request_datalen = 0;
        http_request_get_header_string(request, &header_str);
        bool is_plist = strstr(header_str,"apple-binary-plist");
        free(header_str);
        if (!is_plist) {
            logger_log(conn->raop->logger, LOGGER_DEBUG, "POST /setProperty?selectedMediaArray"
                       "does not provide an apple-binary-plist");
            goto post_error;
        }

        const char *request_data = http_request_get_data(request, &request_datalen);
        plist_t req_root_node = NULL;
	plist_from_bin(request_data, request_datalen, &req_root_node);
	plist_t req_value_node = plist_dict_get_item(req_root_node, "value");

        if (!req_value_node || !PLIST_IS_ARRAY(req_value_node)) {	  
            logger_log(conn->raop->logger, LOGGER_INFO, "POST /setProperty?selectedMediaArray"
                   " did not provide expected plist from client");
            goto post_error;
        }

        int count = plist_array_get_size(req_value_node);
	char *name = NULL;
	char *code = NULL;
	char *language_name = NULL;
	char *language_code = NULL;
        for (int i = 0; i < count; i++) {
            plist_t req_value_array_node = plist_array_get_item(req_value_node,i);
            if (!language_name) {
                plist_t req_value_options_name_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsName");
                if (PLIST_IS_STRING(req_value_options_name_node)) {
                    plist_get_string_val(req_value_options_name_node, &name);
                    if (name) {
                        language_name = (char *) calloc(strlen(name) + 1, sizeof(char));
                        memcpy(language_name, name, strlen(name));
                        plist_mem_free(name);
                    }
                }
            }
            if (!language_code) {
		plist_t req_value_options_code_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsUnicodeLanguageIdentifier");
                if (PLIST_IS_STRING(req_value_options_code_node)) {
                    plist_get_string_val(req_value_options_code_node, &code);
                    if (code) {
                        language_code = (char *) calloc(strlen(code) + 1, sizeof(char));
                        memcpy(language_code, code, strlen(code));
                        plist_mem_free(code);
                    }
                }
            }
            if (language_code && language_name) {
                break;
            } else {
	      plist_free (req_value_array_node);
	      continue;
	    }
	}
        plist_free (req_root_node);
	const char *lname = NULL, *lcode = NULL;
        if (language_code) {
	    set_language_code(airplay_video, language_code);
	    lcode = get_language_code(airplay_video);
        }
        if (language_name) {
            set_language_name(airplay_video, language_name);
	    lname = get_language_name(airplay_video);
        }
        logger_log(conn->raop->logger, LOGGER_INFO, "stored language from MediaSelectionOptions: %s \"%s\"", lcode, lname);
    } else if (!strcmp(property, "reverseEndTime") ||
        !strcmp(property, "forwardEndTime") ||
        !strcmp(property, "actionAtItemEnd")) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "property %s is known but unhandled", property);

        plist_t errResponse = plist_new_dict();
        plist_t errCode = plist_new_uint(0);
        plist_dict_set_item(errResponse, "errorCode", errCode);
        plist_to_xml(errResponse, response_data, (uint32_t *) response_datalen);
        plist_free(errResponse);
        http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    } else {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "property %s is unknown, unhandled", property);      
        goto post_error;
    }
    return;
 post_error:
    http_response_add_header(response, "Content-Length", "0");
}

/* handles GET /getProperty http requests from Client to Server.  (not implemented) */

static void
http_handler_get_property(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("getProperty?");
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_get_property: %s (unhandled)", property);
}

/* this request (for a variant FairPlay decryption)  cannot be handled  by UxPlay */
static void
http_handler_fpsetup2(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    logger_log(conn->raop->logger, LOGGER_WARNING, "client HTTP request POST fp-setup2 is unhandled");
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
    int req_datalen;
    const unsigned char *req_data = (unsigned char *) http_request_get_data(request, &req_datalen);
    logger_log(conn->raop->logger, LOGGER_ERR, "only FairPlay version 0x03 is implemented, version is 0x%2.2x",
               req_data[4]);
    http_response_init(response, "HTTP/1.1", 421, "Misdirected Request");
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

typedef struct time_range_s {
  double start;
  double duration;
} time_range_t;

void time_range_to_plist(void *time_ranges, const int n_time_ranges,
		         plist_t time_ranges_node) {
    time_range_t *tr = (time_range_t *) time_ranges;
    for (int i = 0 ; i < n_time_ranges; i++) {
        plist_t time_range_node = plist_new_dict();
        plist_t duration_node = plist_new_real(tr[i].duration);
        plist_dict_set_item(time_range_node, "duration", duration_node);
        plist_t start_node = plist_new_real(tr[i].start);
        plist_dict_set_item(time_range_node, "start", start_node);
        plist_array_append_item(time_ranges_node, time_range_node);
    }
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

int create_playback_info_plist_xml(playback_info_t *playback_info, char **plist_xml) {

    plist_t res_root_node = plist_new_dict();

    plist_t duration_node = plist_new_real(playback_info->duration);
    plist_dict_set_item(res_root_node, "duration", duration_node);

    plist_t position_node = plist_new_real(playback_info->position);
    plist_dict_set_item(res_root_node, "position", position_node);

    plist_t rate_node = plist_new_real(playback_info->rate);
    plist_dict_set_item(res_root_node, "rate", rate_node);

    /* should these be int or bool? */
    plist_t ready_to_play_node = plist_new_uint(playback_info->ready_to_play);
    plist_dict_set_item(res_root_node, "readyToPlay", ready_to_play_node);

    plist_t playback_buffer_empty_node = plist_new_uint(playback_info->playback_buffer_empty);
    plist_dict_set_item(res_root_node, "playbackBufferEmpty", playback_buffer_empty_node);

    plist_t playback_buffer_full_node = plist_new_uint(playback_info->playback_buffer_full);
    plist_dict_set_item(res_root_node, "playbackBufferFull", playback_buffer_full_node);

    plist_t playback_likely_to_keep_up_node = plist_new_uint(playback_info->playback_likely_to_keep_up);
    plist_dict_set_item(res_root_node, "playbackLikelyToKeepUp", playback_likely_to_keep_up_node);

    plist_t loaded_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->loadedTimeRanges, playback_info->num_loaded_time_ranges,
                        loaded_time_ranges_node);
    plist_dict_set_item(res_root_node, "loadedTimeRanges", loaded_time_ranges_node);

    plist_t seekable_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->seekableTimeRanges, playback_info->num_seekable_time_ranges,
                        seekable_time_ranges_node);
    plist_dict_set_item(res_root_node, "seekableTimeRanges", seekable_time_ranges_node);

    int len;
    plist_to_xml(res_root_node, plist_xml, (uint32_t *) &len);
    /* plist_xml is null-terminated, last character is '/n' */

    plist_free(res_root_node);

    return len;
}

/* this handles requests from the Client  for "Playback information" while the Media is playing on the 
   Media Player.  (The Server gets this information by monitoring the Media Player). The Client could use 
   the information to e.g. update  the slider it shows with progress to the player (0%-100%). 
   It does not affect playing of the Media*/

static void
http_handler_playback_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    //const char *session_id = http_request_get_header(request, "X-Apple-Session-ID");
    playback_info_t playback_info;

    playback_info.stallcount = 0;
    //playback_info.playback_buffer_empty = false;   // maybe  need to get this from playbin 
    //playback_info.playback_buffer_full = true;
    //ayback_info.ready_to_play = true; // ???;
    //ayback_info.playback_likely_to_keep_up = true;

    conn->raop->callbacks.on_video_acquire_playback_info(conn->raop->callbacks.cls, &playback_info);
    if (playback_info.duration == -1.0) {
        /* video has finished, reset */
        logger_log(conn->raop->logger, LOGGER_DEBUG, "playback_info not available (finishing)");
        //httpd_remove_known_connections(conn->raop->httpd);
        http_response_set_disconnect(response,1);
        conn->raop->callbacks.video_reset(conn->raop->callbacks.cls, true);
        return;
    } else if (playback_info.position == -1.0) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "playback_info not available");
        return;
    }      

    playback_info.num_loaded_time_ranges = 1; 
    time_range_t time_ranges_loaded[1];
    time_ranges_loaded[0].start = playback_info.position;
    time_ranges_loaded[0].duration = playback_info.duration - playback_info.position;
    playback_info.loadedTimeRanges = (void *) &time_ranges_loaded;

    playback_info.num_seekable_time_ranges = 1;
    time_range_t time_ranges_seekable[1];
    time_ranges_seekable[0].start = 0.0;
    time_ranges_seekable[0].duration = playback_info.position;
    playback_info.seekableTimeRanges = (void *) &time_ranges_seekable;

    *response_datalen =  create_playback_info_plist_xml(&playback_info, response_data);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
}

/* this handles the POST /reverse request from Client to Server on a AirPlay http channel to "Upgrade" 
   to "PTTH/1.0" Reverse HTTP protocol proposed in 2009 Internet-Draft 

          https://datatracker.ietf.org/doc/id/draft-lentczner-rhttp-00.txt .  

   After the Upgrade the channel becomes a reverse http "AirPlay (reversed)" channel for
   http requests from Server to Client.
  */

static void
http_handler_reverse(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen) {

    /* get http socket for send */
    int socket_fd = httpd_get_connection_socket (conn->raop->httpd, (void *) conn);
    if (socket_fd < 0) {
        logger_log(conn->raop->logger, LOGGER_ERR, "fcup_request failed to retrieve socket_fd from httpd");
        /* shut down connection? */
    }
    
    const char *purpose = http_request_get_header(request, "X-Apple-Purpose");
    const char *connection = http_request_get_header(request, "Connection");
    const char *upgrade = http_request_get_header(request, "Upgrade");
    logger_log(conn->raop->logger, LOGGER_INFO, "client requested reverse connection: %s; purpose: %s  \"%s\"",
               connection, upgrade, purpose);

    httpd_set_connection_type(conn->raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);
    int type_PTTH = httpd_count_connection_type(conn->raop->httpd, CONNECTION_TYPE_PTTH);

    if (type_PTTH == 1) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "will use socket %d for %s connections", socket_fd, purpose);
        http_response_init(response, "HTTP/1.1", 101, "Switching Protocols");
        http_response_add_header(response, "Connection", "Upgrade");
        http_response_add_header(response, "Upgrade", "PTTH/1.0");
    } else {
        logger_log(conn->raop->logger, LOGGER_ERR, "multiple TPPH connections (%d) are forbidden", type_PTTH );
    }    
}

/* the POST /action request from Client to Server on the AirPlay http channel follows a POST /event "FCUP Request"
 from Server to Client on the reverse http channel, for a HLS playlist (first the Master Playlist, then the Media Playlists
 listed in the Master Playlist.     The POST /action request contains the playlist requested by the Server in
 the preceding "FCUP Request".   The FCUP Request sequence  continues until all Media Playlists have been obtained by the Server */ 

static void
http_handler_action(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                    char **response_data, int *response_datalen) {

    airplay_video_t *airplay_video = conn->raop->airplay_video[conn->raop->current_video];
    bool data_is_plist = false;
    plist_t req_root_node = NULL;
    uint64_t uint_val;
    int request_id = 0;
    int fcup_response_statuscode = 0;
    bool logger_debug = (logger_get_level(conn->raop->logger) >= LOGGER_DEBUG);

    const char* session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!session_id) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto post_action_error;
    }    
    const char *apple_session_id = get_apple_session_id(airplay_video);
    if (strcmp(session_id, apple_session_id)){
        logger_log(conn->raop->logger, LOGGER_ERR, "X-Apple-Session-ID has changed:\n  was:\"%s\"\n  now:\"%s\"",
                   apple_session_id, session_id);
        goto post_action_error;
    }

    /* verify that this request contains a binary plist*/
    char *header_str = NULL;
    http_request_get_header_string(request, &header_str);
    logger_log(conn->raop->logger, LOGGER_DEBUG, "request header: %s", header_str);
    data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
    free(header_str);
    if (!data_is_plist) {
        logger_log(conn->raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }

    /* extract the root_node  plist */
    int request_datalen = 0;
    const char *request_data = http_request_get_data(request, &request_datalen);
    if (request_datalen == 0) {
        logger_log(conn->raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }
    plist_from_bin(request_data, request_datalen, &req_root_node);

    /* determine type of data */
    plist_t req_type_node = plist_dict_get_item(req_root_node, "type");
    if (!PLIST_IS_STRING(req_type_node)) {
        goto post_action_error;
    }
    
    /* three possible types are known: 
       playlistRemove
       playlistAdd
       unhandledURLRespone
*/
    char *type = NULL;
    plist_get_string_val(req_type_node, &type);
    if (!type) {
        goto post_action_error;
    }
    logger_log(conn->raop->logger, LOGGER_DEBUG, "action type is %s", type);

    /* check that plist structure is as expected*/
    plist_t req_params_node = NULL;
    if (PLIST_IS_DICT (req_root_node)) {
        req_params_node = plist_dict_get_item(req_root_node, "params");
    }
    if (strcmp(type,"playlistInsert") && !PLIST_IS_DICT (req_params_node)) {   //bypass if type=playlistInsert until we have see its plist
        goto post_action_error;
    }
    
    if (!strcmp(type,"playlistRemove")) {
        logger_log(conn->raop->logger, LOGGER_INFO, "unhandled action type playlistRemove (stop playback)");
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node || !PLIST_IS_DICT (req_params_item_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
        char* remove_uuid = NULL;
        plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
        const char *playback_uuid = get_playback_uuid(airplay_video);
        if (remove_uuid) {
            if (strcmp(remove_uuid, playback_uuid)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "uuid of playlist removal action request did not match current playlist:\n"
                           "   current: %s\n   remove: %s", playback_uuid, remove_uuid);
            } else {
                logger_log(conn->raop->logger, LOGGER_DEBUG, "removal_uuid matches playback_uuid\n");
            }
            plist_mem_free (remove_uuid);
        }

    } else if (!strcmp(type, "playlistInsert")) {
        logger_log(conn->raop->logger, LOGGER_ERR, "FIXME: playlist insertion not yet implemented");
        logger_log(conn->raop->logger, LOGGER_INFO, "unhandled action type playlistInsert (add new playback)");

        printf("\n***************FIXME************************\nPlaylist insertion needs more information for it to be implemented:\n"
               "please report following output as an \"Issue\" at http://github.com/FDH2/UxPlay:\n");
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        printf("\n\n%s\n", header_str);
        bool data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
        free(header_str);
        if (data_is_plist) {
            int request_datalen;
            const char *request_data = http_request_get_data(request, &request_datalen);
            plist_t req_root_node = NULL;
            plist_from_bin(request_data, request_datalen, &req_root_node);
            char *plist_xml = NULL;
            uint32_t plist_len = 0;
            plist_to_xml(req_root_node, &plist_xml, &plist_len);
            printf("plist_len = %u\n", plist_len);
            printf("%s\n", plist_xml);
            plist_mem_free(plist_xml);
            exit(0);
        }

    } else if (!strcmp(type, "unhandledURLResponse")) {   
        /* handling type "unhandledURLResponse" (case 1)*/
        uint_val = 0;
        int fcup_response_datalen = 0;

        if  (logger_debug) {
            plist_t req_params_fcup_response_statuscode_node = plist_dict_get_item(req_params_node,
                                                                      "FCUP_Response_StatusCode");
            if (req_params_fcup_response_statuscode_node) {
                plist_get_uint_val(req_params_fcup_response_statuscode_node, &uint_val);
                fcup_response_statuscode = (int) uint_val;
                uint_val = 0;
                logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_StatusCode = %d",
                           fcup_response_statuscode);
            }

            plist_t req_params_fcup_response_requestid_node = plist_dict_get_item(req_params_node,
                                                                     "FCUP_Response_RequestID");
            if (req_params_fcup_response_requestid_node) {
                plist_get_uint_val(req_params_fcup_response_requestid_node, &uint_val);
                request_id = (int) uint_val;
                uint_val = 0;
                logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_RequestID =  %d", request_id);
            }
        }

        plist_t req_params_fcup_response_url_node = plist_dict_get_item(req_params_node, "FCUP_Response_URL");
        if (!PLIST_IS_STRING(req_params_fcup_response_url_node)) {
            goto post_action_error;
        }
        char *fcup_response_url = NULL;
        plist_get_string_val(req_params_fcup_response_url_node, &fcup_response_url);
        if (!fcup_response_url) {
            goto post_action_error;
        }
        logger_log(conn->raop->logger, LOGGER_DEBUG, "FCUP_Response_URL =  %s", fcup_response_url);
	
        plist_t req_params_fcup_response_data_node = plist_dict_get_item(req_params_node, "FCUP_Response_Data");
        if (!PLIST_IS_DATA(req_params_fcup_response_data_node)){
            plist_mem_free(fcup_response_url);
            goto post_action_error;
        }

        uint_val = 0;
        char *fcup_response_data = NULL;    
        plist_get_data_val(req_params_fcup_response_data_node, &fcup_response_data, &uint_val);
        fcup_response_datalen = (int) uint_val;

        char *playlist = NULL;
        if (!fcup_response_data) {
            plist_mem_free(fcup_response_url);
            goto post_action_error;
        } else {
            playlist = (char *) malloc(fcup_response_datalen + 1);
            playlist[fcup_response_datalen] = '\0';
            memcpy(playlist, fcup_response_data, fcup_response_datalen);
            plist_mem_free(fcup_response_data);
        }
        assert(playlist);
        int playlist_len = strlen(playlist);
    
        if (logger_debug) {
            logger_log(conn->raop->logger, LOGGER_DEBUG, "begin FCUP Response data:\n%s\nend FCUP Response data", playlist);
        }

        char *ptr = strstr(fcup_response_url, "/master.m3u8");
        if (ptr) {
            /* this is a master playlist */
            const char *uri_prefix = get_uri_prefix(airplay_video);
            char ** uri_list = NULL;
            int num_uri = 0;
            char *uri_local_prefix = get_uri_local_prefix(airplay_video);
            playlist = select_master_playlist_language(airplay_video, playlist);
            playlist_len = strlen(playlist);
            create_media_uri_table(uri_prefix, playlist, playlist_len, &uri_list, &num_uri);	
            char *new_master = adjust_master_playlist (playlist, playlist_len,  uri_prefix, uri_local_prefix);
            free(playlist);
            store_master_playlist(airplay_video, new_master);
            create_media_data_store(airplay_video, uri_list, num_uri);
            free (uri_list);
            num_uri =  get_num_media_uri(airplay_video);
            set_next_media_uri_id(airplay_video, 0);
        } else {
            /* this is a media playlist */
            float duration = 0.0f;
            int count = analyze_media_playlist(playlist, &duration);
            int uri_num = get_next_media_uri_id(airplay_video);
            --uri_num;    // (next num is current num + 1)
            int ret = store_media_playlist(airplay_video, playlist, &count, &duration, uri_num);
            if (ret == 1) {
                logger_log(conn->raop->logger, LOGGER_DEBUG,"media_playlist is a duplicate: do not store");
            } else if (count) {
                logger_log(conn->raop->logger, LOGGER_DEBUG,
                           "\n%s:\nreceived media playlist has %5d chunks, total duration %9.3f secs\n",
                            fcup_response_url, count, duration);
            }
        }

        plist_mem_free(fcup_response_url);

        int num_uri = get_num_media_uri(airplay_video);
        int uri_num = get_next_media_uri_id(airplay_video);
        if (uri_num <  num_uri) {
            fcup_request((void *) conn, get_media_uri_by_num(airplay_video, uri_num),
                                                             apple_session_id,
                                                             get_next_FCUP_RequestID(airplay_video));
            set_next_media_uri_id(airplay_video, ++uri_num);
        } else {
            char * uri_local_prefix = get_uri_local_prefix(airplay_video);
            conn->raop->callbacks.on_video_play(conn->raop->callbacks.cls,
                                                strcat(uri_local_prefix, "/master.m3u8"),
                                                get_start_position_seconds(airplay_video));
        }


    } else {
        logger_log(conn->raop->logger, LOGGER_INFO, "unknown action type (unhandled)"); 
    }
    plist_mem_free(type);
    plist_free(req_root_node);
    return;

 post_action_error:;
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    plist_mem_free(type);
    if (req_root_node)  {
        plist_free(req_root_node);
    }

}

/* The POST /play request from the Client to Server on the AirPlay http channel contains (among other information)
   the "Content Location" that specifies the HLS Playlists for the video to be streamed, as well as the video 
   "start position in seconds".   Once this request is received by the Sever, the Server sends a POST /event
   "FCUP Request" request to the Client on the reverse http channel, to request the HLS Master Playlist */

static void
http_handler_play(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {

    char* playback_location = NULL;
    char* client_proc_name = NULL;
    plist_t req_root_node = NULL;
    float start_position_seconds = 0.0f;
    bool data_is_binary_plist = false;
    char supported_hls_proc_names[] = "YouTube;";
    airplay_video_t *airplay_video = NULL;
    
    logger_log(conn->raop->logger, LOGGER_DEBUG, "http_handler_play");

    const char* apple_session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!apple_session_id) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto play_error;
    }

    int request_datalen = -1;    
    const char *request_data = http_request_get_data(request, &request_datalen);

    if (request_datalen > 0) {
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "request header:\n%s", header_str);
        data_is_binary_plist = (strstr(header_str, "x-apple-binary-plist") != NULL);
        free (header_str);
    }

    if (!data_is_binary_plist) {
         logger_log(conn->raop->logger, LOGGER_ERR, "Play request Content is not binary_plist (unsupported)");
         goto play_error;
    }

    plist_from_bin(request_data, request_datalen, &req_root_node);

    plist_t req_uuid_node = plist_dict_get_item(req_root_node, "uuid");
    if (!req_uuid_node) {
       goto play_error;
    }
    char* playback_uuid = NULL;
    plist_get_string_val(req_uuid_node, &playback_uuid);

    /* check if playlist is already dowloaded and stored (may have been interruoted by advertisements ) */
#if 0
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("old: airplay_video[%d] %p %s %f\n", i, conn->raop->airplay_video[i],
	       get_playback_uuid(conn->raop->airplay_video[i]),
	       get_duration(conn->raop->airplay_video[i]));
    }
    printf("\n");
    printf("new playback_uuid %s\n\n", playback_uuid);
#endif

    int id = -1;
    id = get_playlist_by_uuid(conn->raop, playback_uuid);
    if (id >= 0) {
        printf("use: airplay_video[%d] %p %s %s\n", id, airplay_video, playback_uuid, get_playback_uuid(airplay_video));
        airplay_video = conn->raop->airplay_video[id];
        assert(airplay_video);
        set_apple_session_id(airplay_video, apple_session_id);      
        char * uri_local_prefix = get_uri_local_prefix(airplay_video);
        conn->raop->callbacks.on_video_play(conn->raop->callbacks.cls,
                                            strcat(uri_local_prefix, "/master.m3u8"),
                                            get_start_position_seconds(airplay_video));
        plist_mem_free(playback_uuid);
        plist_free(req_root_node);
        return;
    }

    /* remove short stort playlists (probably advertisements */
    int count = 0;
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (conn->raop->airplay_video[i]) {
            float duration = get_duration(conn->raop->airplay_video[i]);
                if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS ) {
                    logger_log(conn->raop->logger, LOGGER_INFO,
                              "deleting playlist playback_uuid %s duration (seconds) %f",
                    get_playback_uuid(conn->raop->airplay_video[i]), duration);
                    airplay_video_destroy(conn->raop->airplay_video[i]);
                    conn->raop->airplay_video[i] = NULL;
                } else {
                count++;
                //printf(" %d %d duration %f : keep\n", i, count,  duration);
            }
        }
    }

    /* initialize new airplay_video structure to hold playlist */
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (conn->raop->airplay_video[i]) {
            continue;
        }
        id = i;
        break;
    }
    if (id == -1) {
        logger_log(conn->raop->logger, LOGGER_ERR, "no unused airplay_video structures are available"
                  " MAX_AIRPLAY_VIDEO = %d\n", MAX_AIRPLAY_VIDEO);
        exit(1);
    }

    airplay_video = airplay_video_init(conn->raop, conn->raop->port, conn->raop->lang);
    if (airplay_video) {
        set_playback_uuid(airplay_video, playback_uuid);
        plist_mem_free (playback_uuid);
        conn->raop->current_video = id;
        conn->raop->airplay_video[id] = airplay_video;
        count++;
        //printf("created new airplay_video %p %s\n\n", airplay_video, get_playback_uuid(airplay_video));
    } else {
        logger_log(conn->raop->logger, LOGGER_ERR, "failed to allocate airplay_video[%d]\n", id);
        exit(-1);
    }

    /* ensure that space will always be available for adding future playlists */

    if (count == MAX_AIRPLAY_VIDEO) {
        int next = (id + 1) % (int) MAX_AIRPLAY_VIDEO;
        logger_log(conn->raop->logger, LOGGER_INFO,
                   "deleting playlist playback_uuid %s duration (seconds) %f",
                   get_playback_uuid(conn->raop->airplay_video[next]),
                   get_duration(conn->raop->airplay_video[next]));
        airplay_video_destroy(conn->raop->airplay_video[next]);
        conn->raop->airplay_video[next] = NULL;
    }
#if 0    
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("new: airplay_video[%d] %p %s %f\n", i, conn->raop->airplay_video[i],
	       get_playback_uuid(conn->raop->airplay_video[i]),
	       get_duration(conn->raop->airplay_video[i]));
    }
#endif
    set_apple_session_id(airplay_video, apple_session_id);
	   
    plist_t req_content_location_node = plist_dict_get_item(req_root_node, "Content-Location");
    if (!req_content_location_node) {
        goto play_error;
    } else {
        plist_get_string_val(req_content_location_node, &playback_location);
    }

    plist_t req_client_proc_name_node = plist_dict_get_item(req_root_node, "clientProcName");
    if (!req_client_proc_name_node) {
        goto play_error;
    } else {
        plist_get_string_val(req_client_proc_name_node, &client_proc_name);
        if (!strstr(supported_hls_proc_names, client_proc_name)){
            logger_log(conn->raop->logger, LOGGER_WARNING, "Unsupported HLS streaming format: clientProcName %s not found in supported list: %s",
                       client_proc_name, supported_hls_proc_names);
        }
        plist_mem_free(client_proc_name);
    }

    plist_t req_start_position_seconds_node = plist_dict_get_item(req_root_node, "Start-Position-Seconds");
    if (!req_start_position_seconds_node) {
        logger_log(conn->raop->logger, LOGGER_INFO, "No Start-Position-Seconds in Play request");	    
    } else {
         double start_position = 0.0;
         plist_get_real_val(req_start_position_seconds_node, &start_position);
         start_position_seconds = (float) start_position;
    }
    set_start_position_seconds(airplay_video, (float) start_position_seconds);

    char *ptr = strstr(playback_location, "/master.m3u8");
    if (!ptr) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Content-Location has unsupported form:\n%s\n", playback_location);	    
        goto play_error;
    } else {
        int prefix_len =  (int) (ptr - playback_location);
        char *uri_prefix = (char *) calloc(prefix_len + 1, sizeof(char));
        memcpy(uri_prefix, playback_location, prefix_len);
        set_uri_prefix(airplay_video, uri_prefix);
    }
    set_next_media_uri_id(airplay_video, 0);
    printf("FCUP REQUEST\n");
    fcup_request((void *) conn, playback_location, apple_session_id, get_next_FCUP_RequestID(airplay_video));

    plist_mem_free(playback_location);

    if (req_root_node) {
        plist_free(req_root_node);
    }
    return;

 play_error:;
    plist_mem_free(playback_location);
    if (req_root_node) {
        plist_free(req_root_node);
    }
    logger_log(conn->raop->logger, LOGGER_ERR, "Could not find valid Plist Data for POST/play request, Unhandled");
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    http_response_set_disconnect(response, 1);
    conn->raop->callbacks.conn_reset(conn->raop->callbacks.cls, 2);
}

/* the HLS handler handles http requests GET /[uri] on the HLS channel from the media player to the Server, asking for
   (adjusted) copies of Playlists: first the Master Playlist  (adjusted to change the uri prefix to
   "http://localhost:[port]/.......m3u8"), then the Media Playlists that the media player wishes to use.  
   If the client supplied Media playlists with the "YT-EXT-CONDENSED-URI" header, these must be adjusted into
   the standard uncondensed form before sending with the response.    The uri in the request is  the uri for the
   Media Playlist, taken from the Master Playlist, with the uri prefix removed.  
*/ 

static void
http_handler_hls(raop_conn_t *conn,  http_request_t *request, http_response_t *response,
                 char **response_data, int *response_datalen) {
    airplay_video_t *airplay_video = conn->raop->airplay_video[conn->raop->current_video];
    const char *method = http_request_get_method(request);
    assert (!strcmp(method, "GET"));
    const char *url = http_request_get_url(request);    
    const char* upgrade = http_request_get_header(request, "Upgrade");
    if (upgrade) {
        //don't accept Upgrade: h2c request ?
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(conn->raop->logger, LOGGER_INFO,
                   "%s\nhls upgrade request declined", header_str); 
        free (header_str);
        return;
    }

    if (!strcmp(url, "/master.m3u8")){
        char * master_playlist  = get_master_playlist(airplay_video);
        if (master_playlist) {
            size_t len = strlen(master_playlist);
            char * data = (char *) malloc(len + 1);
            memcpy(data, master_playlist, len);
            data[len] = '\0';
            *response_data = data;
            *response_datalen = (int ) len;
        } else {
            logger_log(conn->raop->logger, LOGGER_ERR,"requested master playlist %s not found", url); 
            *response_datalen = 0;
        }

    } else {
        int chunks;
        float duration;
        char *media_playlist = get_media_playlist(airplay_video, &chunks, &duration, url);
        if (media_playlist) {
            char *data  = adjust_yt_condensed_playlist(media_playlist);
            *response_data = data;
            *response_datalen = strlen(data);
            logger_log(conn->raop->logger, LOGGER_INFO,
                       "Requested media_playlist %s has %5d chunks, total duration %9.3f secs", url, chunks, duration); 
        } else {
            logger_log(conn->raop->logger, LOGGER_ERR,"requested media playlist %s not found", url); 
            *response_datalen = 0;
        }
	    
    }

    http_response_add_header(response, "Access-Control-Allow-Headers", "Content-type");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
    const char *date;
    date = gmt_time_string();
    http_response_add_header(response, "Date", date);
    if (*response_datalen > 0) {
        http_response_add_header(response, "Content-Type", "application/x-mpegURL; charset=utf-8");
    } else if (*response_datalen == 0) {
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
    }
}
