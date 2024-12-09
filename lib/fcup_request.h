/*
 * Copyright (c) 2022 fduncanh
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

/* this file is part of raop.c via http_handlers.h and should not be included in any other file */


//produces the fcup request plist in xml format as a null-terminated string
char *create_fcup_request(const char *url, int request_id, const char *client_session_id, int *datalen) {
    char *plist_xml = NULL;
    /* values taken from apsdk-public;  */
    /* these seem to be arbitrary choices */
    const int sessionID = 1;
    const int FCUP_Response_ClientInfo = 1;
    const int FCUP_Response_ClientRef = 40030004;

    /* taken from a working AppleTV? */
    const char User_Agent[] = "AppleCoreMedia/1.0.0.11B554a (Apple TV; U; CPU OS 7_0_4 like Mac OS X; en_us";
    
    plist_t req_root_node = plist_new_dict();
    
    plist_t session_id_node = plist_new_uint((int64_t) sessionID);   
    plist_dict_set_item(req_root_node, "sessionID", session_id_node);
    plist_t type_node = plist_new_string("unhandledURLRequest");
    plist_dict_set_item(req_root_node, "type", type_node);

    plist_t fcup_request_node = plist_new_dict();

    plist_t client_info_node = plist_new_uint(FCUP_Response_ClientInfo);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_ClientInfo", client_info_node);
    plist_t client_ref_node = plist_new_uint((int64_t) FCUP_Response_ClientRef);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_ClientRef", client_ref_node);
    plist_t request_id_node = plist_new_uint((int64_t) request_id);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_RequestID", request_id_node);
    plist_t url_node = plist_new_string(url);
    plist_dict_set_item(fcup_request_node, "FCUP_Response_URL", url_node);
    plist_t session_id1_node = plist_new_uint((int64_t) sessionID);    
    plist_dict_set_item(fcup_request_node, "sessionID", session_id1_node);
				    
    plist_t fcup_response_header_node = plist_new_dict();
    plist_t playback_session_id_node = plist_new_string(client_session_id);       
    plist_dict_set_item(fcup_response_header_node, "X-Playback-Session-Id", playback_session_id_node);
    plist_t user_agent_node = plist_new_string(User_Agent);
    plist_dict_set_item(fcup_response_header_node, "User-Agent", user_agent_node);

    plist_dict_set_item(fcup_request_node, "FCUP_Response_Headers", fcup_response_header_node);
    plist_dict_set_item(req_root_node, "request", fcup_request_node);
    
    uint32_t uint_val;
    
    plist_to_xml(req_root_node, &plist_xml, &uint_val);
    *datalen = (int) uint_val;
    plist_free(req_root_node);
    assert(plist_xml[*datalen] == '\0');
    return plist_xml; //needs to be freed after use
}

int fcup_request(void *conn_opaque, const char *media_url, const char *client_session_id, int request_id) {

    raop_conn_t *conn = (raop_conn_t *) conn_opaque;
    int datalen = 0;
    int requestlen;

    int socket_fd = httpd_get_connection_socket_by_type(conn->raop->httpd, CONNECTION_TYPE_PTTH, 1);
    
    logger_log(conn->raop->logger, LOGGER_DEBUG, "fcup_request send socket = %d", socket_fd);
    
    /* create xml plist request data */
    char *plist_xml = create_fcup_request(media_url, request_id, client_session_id, &datalen);

    /* use http_response tools for creating the reverse http request */
    http_response_t *request = http_response_create();
    http_response_reverse_request_init(request, "POST", "/event", "HTTP/1.1");
    http_response_add_header(request, "X-Apple-Session-ID", client_session_id);
    http_response_add_header(request, "Content-Type", "text/x-apple-plist+xml");
    http_response_finish(request, plist_xml, datalen);

    free(plist_xml);

    const char *http_request = http_response_get_data(request, &requestlen); 
    int send_len = send(socket_fd, http_request, requestlen, 0);
    if (send_len < 0) {
        int sock_err = SOCKET_GET_ERROR();
	logger_log(conn->raop->logger, LOGGER_ERR, "fcup_request: send  error %d:%s\n",
		 sock_err, strerror(sock_err));
	http_response_destroy(request);
        /* shut down connection? */
        return -1;
    }

    if (logger_get_level(conn->raop->logger) >= LOGGER_DEBUG) {
      char *request_str =  utils_data_to_text(http_request, requestlen);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "\n%s", request_str);
        free (request_str);
    }
    http_response_destroy(request);
    logger_log(conn->raop->logger, LOGGER_DEBUG,"fcup_request: send sent Request of %d bytes from socket %d\n",
               send_len, socket_fd);
    return 0;
}
