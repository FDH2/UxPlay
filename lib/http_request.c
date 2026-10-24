/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
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
 *==================================================================
 * modified by fduncanh 2021, 2026
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "http_request.h"
#include "llhttp/llhttp.h"

#define MAX_EVENT_DATA 4096

struct http_request_s {
    llhttp_t parser;
    llhttp_settings_t parser_settings;
    bool is_reverse;  // if true, this is a reverse-response from client
    const char *method;
    char *url;
    size_t url_len;
    char protocol[9];
    size_t protocol_len;
    size_t version_len;
    char **headers;
    int headers_size;
    int headers_index;

    char *data;
    size_t datalen;
    int complete;
};

static int
on_url(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    char *new_url = realloc(request->url, request->url_len + length + 1); 
    if (!new_url) {
        return HPE_USER; 
    }
    request->url = new_url;
    
    memcpy(request->url + request->url_len, at, length);
    request->url_len += length;
    request->url[request->url_len] = '\0';
    return 0;
}

static int
on_protocol(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    // protocol will be 4 chars ("HTTP" , etc.),
    if (request->protocol_len + length > 4) {
        return HPE_USER; 
    }
    
    memcpy(request->protocol + request->protocol_len, at, length);
    request->protocol_len += length;
    request->protocol[8] = '\0';
    return 0;
}

static int
on_version(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    // version will be 3 chars ("1.1" , etc.),
    if (request->version_len + length  > 3) {
        return HPE_USER; 
    }
    
    memcpy(request->protocol + 5  + request->version_len, at, length);
    request->version_len += length;
    request->protocol[8] = '\0';
    return 0;
}

static int
on_header_field(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    /* Check if our index is a value */
    if (request->headers_index % 2 == 1) {
        request->headers_index++;
    }

    /* Allocate space for new field-value pair */
    if (request->headers_index == request->headers_size) {
        int new_size = request->headers_size + 2;

        char **new_headers = realloc(request->headers, new_size * sizeof(char*));
        if (!new_headers) {
            return HPE_USER;
        }
        request->headers = new_headers;
        request->headers_size = new_size;
	
        request->headers[request->headers_index] = NULL;
        request->headers[request->headers_index + 1] = NULL;
    }

    size_t orig_len = 0;
    
    /* Allocate space in the current header string */
    if (request->headers[request->headers_index] == NULL) {
        request->headers[request->headers_index] = calloc(1, length + 1);
        if (!request->headers[request->headers_index]) {
            return HPE_USER;
        }
    } else {
        orig_len = strlen(request->headers[request->headers_index]);
        char *new_buf =  realloc( request->headers[request->headers_index], orig_len + length + 1);
        if (!new_buf) {
            return HPE_USER;
        }
        request->headers[request->headers_index] = new_buf;
    }

    memcpy(request->headers[request->headers_index] + orig_len, at, length);
    *(request->headers[request->headers_index] + orig_len + length) = '\0';
    return 0;
}

static int
on_header_value(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    /* Check if our index is a field */
    if (request->headers_index % 2 == 0) {
        request->headers_index++;
    }

    if (request->headers_index >= request->headers_size) {
        return HPE_USER;
    }

    size_t orig_len = 0;
    
    /* Allocate space in the current header string */
    if (request->headers[request->headers_index] == NULL) {
        request->headers[request->headers_index] = calloc(1, length + 1);
        if (!request->headers[request->headers_index]) {
            return HPE_USER;
        }
    } else {
        orig_len =  strlen(request->headers[request->headers_index]);

        char *new_buf = realloc(request->headers[request->headers_index], orig_len + length + 1);
        if (!new_buf) {
            return HPE_USER;
        }
        request->headers[request->headers_index] = new_buf;
    }

    memcpy(request->headers[request->headers_index] + orig_len, at, length);
    *(request->headers[request->headers_index] + orig_len + length) = '\0';
    
    return 0;
}

static int
on_body(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    size_t new_size = request->datalen + length + 1;
    char *new_data = realloc(request->data, new_size);
    if (!new_data) {
      return HPE_USER;
    }
    request->data = new_data;

    memcpy(request->data + request->datalen, at, length);
    request->datalen += length;
    request->data[request->datalen] = '\0';

    return 0;
}

static int
on_message_complete(llhttp_t *parser)
{
    http_request_t *request = parser->data;

    request->method = llhttp_method_name(request->parser.method);
    request->complete = 1;
    return 0;
}

http_request_t *
http_request_init(void)
{
    http_request_t *request = calloc(1, sizeof(http_request_t));
    if (!request) {
        return NULL;
    }

    llhttp_settings_init(&request->parser_settings);
    request->parser_settings.on_url = &on_url;
    request->parser_settings.on_protocol = &on_protocol;
    request->parser_settings.on_version = &on_version;
    request->parser_settings.on_header_field = &on_header_field;
    request->parser_settings.on_header_value = &on_header_value;
    request->parser_settings.on_body = &on_body;
    request->parser_settings.on_message_complete = &on_message_complete;

    llhttp_init(&request->parser, HTTP_REQUEST, &request->parser_settings);
    request->parser.data = request;
    assert(sizeof(request->protocol) > 8);
    request->protocol[4] = '/';
    request->data = NULL;
    return request;
}

void
http_request_destroy(http_request_t *request)
{
    if (request) {
        free(request->url);

        if (request->headers) {
            for (int i = 0; i < request->headers_size; i++) {
                if (request->headers[i]) {
                    free(request->headers[i]);
                }
            }
            free(request->headers);
        }
        free(request->data);
        free(request);
    }
}

static size_t
parse_ptth(const char *data, const size_t datalen, size_t *header_len, size_t *body_len)
{
    *header_len = 0;
    *body_len = 0;

    //  skip any leading junk (non-printable ASCII) or whitespace in front of next message
    size_t junk_offset = 0;
    while (junk_offset < datalen && (data[junk_offset] <= 0x20 || data[junk_offset] == 0x7f)) {
        junk_offset++;
    }

    // Adjust parsing frame past the junk
    const char *actual_data = data + junk_offset;
    size_t actual_datalen = datalen - junk_offset;

    const char *header_end = strstr(actual_data, "\r\n\r\n");
    if (!header_end) {
        return 0; // incomplete headers
    }

    size_t hl = (header_end - actual_data) + 4;
    size_t bl = 0;

    const char *cl_ptr = NULL;
    for (size_t i = 0; i < hl - 15; i++) {
        if (strncasecmp(actual_data + i, "Content-Length:", 15) == 0) {
            cl_ptr = actual_data + i;
            break;
        }
    }

    if (cl_ptr) {
        bl = (size_t) strtoul(cl_ptr + 15, NULL, 10);
    }

    size_t total_msg_len = hl + bl;
    if (actual_datalen < total_msg_len) {
        return 0; // incomplete body
    }
    
    *header_len = hl;
    *body_len = bl;

    //  (downstream memmove flushes both message + junk)
    return total_msg_len + junk_offset;
}

int
http_request_add_data(http_request_t *request, const char *data, size_t datalen)
{
    int ret = llhttp_execute(&request->parser, data, datalen);
    if (ret == HPE_PAUSED_UPGRADE) {
        /* support for "Upgrade" to reverse http ("PTTH/1.0") protocol */
        llhttp_resume_after_upgrade(&request->parser);
    }
    return ret;
}

int
http_request_add_event_data(http_request_t *request, const char *data, size_t datalen)
{
    if (request->datalen + datalen >= MAX_EVENT_DATA) {
        return -1;
    }
    memcpy(request->data + request->datalen, data, datalen);
    request->datalen += datalen;
    request->data[request->datalen] = '\0';
    return 0;
}

// any message returned by parse_event_data must be free'd
bool http_request_parse_event_data(http_request_t *request, char **header_ptr, size_t *header_len, char **body_ptr, size_t *body_len) {
    *header_len = 0;
    *body_len = 0;
    *header_ptr = NULL;
    *body_ptr = NULL;

    /* now attempt to parse the reverse-HTTP response, or EVENT message */
    size_t message_len = parse_ptth(request->data, request->datalen, header_len, body_len);
    if (!message_len) {
        return false;
    }
    assert (request->datalen >= message_len);
    assert (message_len == *header_len + *body_len);

    char *local_header = (char *) malloc(*header_len + 1);
    if (!local_header) return false;
    
    memcpy(local_header, request->data, *header_len);
    local_header[*header_len] = '\0';

    char *local_body = NULL;
    if (*body_len) {
        local_body = (char *) malloc(*body_len + 1);
        if (!local_body) {
            free(local_header);
            return false;
        }
        memcpy(local_body, request->data + *header_len, *body_len);
        local_body[*body_len] = '\0';
    }

    size_t remaining_len = request->datalen - message_len;    
    if (remaining_len) {
        memmove(request->data, request->data + message_len, remaining_len);
    }
    request->datalen = remaining_len;
    request->data[request->datalen] = '\0'; 

    *header_ptr = local_header;
    *body_ptr = local_body;

    return true;
}

int http_request_upgrade_data(http_request_t *request)
{
    if (request->data) {
        free(request->data);
        request->data = NULL;
    }
    size_t max_event_data = MAX_EVENT_DATA;
    request->data = (char *) malloc (max_event_data);
    if (!request->data) {
        return -1;
    }
    request->data[0] = '\0';
    request->datalen = 0;
    return 0;
}

int
http_request_is_complete(http_request_t *request)
{
    if (!request){
        return 0;
    }
    return request->complete;
}

int
http_request_has_error(http_request_t *request)
{
    if (!request) {
        return 0;
    }
    return (llhttp_get_errno(&request->parser) != HPE_OK);
}

const char *
http_request_get_error_name(http_request_t *request)
{
    if (!request) {  
        return NULL;
    }
    return llhttp_errno_name(llhttp_get_errno(&request->parser));
}

const char *
http_request_get_error_description(http_request_t *request)
{
    if (!request) {
        return NULL;
    }
    return llhttp_get_error_reason(&request->parser);
}

const char *
http_request_get_method(http_request_t *request)
{
    if (!request || request->is_reverse) {
        return NULL;
    }     
    if (request->is_reverse) {
        return NULL;
    }
    return request->method;
}

const char *
http_request_get_url(http_request_t *request)
{
    if (!request || request->is_reverse) {  
        return NULL;
    }

    if (request->is_reverse) {
        return NULL;
    }
    return request->url;
}

const char *
http_request_get_protocol(http_request_t *request)
{
    if (!request || request->is_reverse) {
        return NULL;
    }
    return request->protocol;
}

const char *
http_request_get_header(http_request_t *request, const char *name)
{

    if (!request || !name || request->is_reverse || !request->headers || request->headers_index < 0) {
        return NULL;
    }

    for (int i = 0; i <= request->headers_index; i += 2) {
        if (request->headers[i] && !strcmp(request->headers[i], name)) {
            return request->headers[i+1];
        }
    }
    return NULL;
}

size_t
http_request_header_get_size(http_request_t *request, int *num_fields, size_t *max_field_len, size_t *max_value_len)
{
    if (num_fields) {
        *num_fields = 0;
    }
    if (max_field_len) {
        *max_field_len = 0;
    }
    if (max_value_len) {
        *max_value_len = 0;
    }

    if (!request || request->is_reverse || request->headers_size == 0 || request->headers_index < 0 || !request->headers) {
        return 0;
    }
    
    size_t total = 0;
    int fields_count = 0;

    for (int i = 0; i <= request->headers_index; i++) {
        if (!request->headers[i]) {
            continue;
        }
	
        size_t len = strlen(request->headers[i]);
        total += len;
        if (i % 2 == 0) {
            // this is a header field
            fields_count++;
            if ( max_field_len && len > *max_field_len) {
                *max_field_len = len;
            }
        } else {
            //this is a header value   
            if (max_value_len && len > *max_value_len) {
                *max_value_len = len;
            }
        }
    }

    if (num_fields) {
        *num_fields = fields_count;
    }

    return total;
}

const char *
http_request_get_data(http_request_t *request, int *datalen)
{
    if (!request || request->is_reverse) {
        return NULL;
    }

    if (datalen) {
        *datalen = request->datalen;
    }
    return request->data;
}

int 
http_request_get_header_string(http_request_t *request, char **header_str)
{
    if (!request || request->is_reverse || request->headers_size == 0 || request->headers_index < 0) {
        *header_str = NULL;
        return 0;
    }

    int len = 0;
    for (int i = 0; i <= request->headers_index; i++) {
        if (request->headers[i]) {
            len += strlen(request->headers[i]);
            if (i % 2 == 0) {
                len += 2;   // ": "
            } else {
                len++;   // "\n""
            }
        }
    }
    char *str = (char *) calloc(len+1, sizeof(char));
    if(!str) {
        *header_str = NULL;
        return -1;
    }
    *header_str = str;
    char *p = str;
    int n = len + 1;
    for (int i = 0; i <= request->headers_index; i++) {
        if (request->headers[i]) {
            int hlen = strlen(request->headers[i]); 
            snprintf(p, n, "%s", request->headers[i]);
            n -= hlen;
            p += hlen;
            if (i % 2 == 0) {
                snprintf(p, n, ": ");
                n -= 2;
                p += 2;
            } else {
                snprintf(p, n, "\n");
                n--;
                p++;
            }
        }
    }
    assert(p == &(str[len]));
    return len;
}

bool http_request_is_reverse(http_request_t *request) {
    return request->is_reverse;
}

void http_request_set_reverse(http_request_t *request) {
    request->is_reverse = true;
}
