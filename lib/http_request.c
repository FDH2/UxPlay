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
 * modified by fduncanh 2021
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "http_request.h"
#include "llhttp/llhttp.h"

struct http_request_s {
    llhttp_t parser;
    llhttp_settings_t parser_settings;

    bool is_reverse;  // if true, this is a reverse-response from client
    const char *method;
    char *url;
    char protocol[9];

    char **headers;
    int headers_size;
    int headers_index;

    char *data;
    int datalen;

    int complete;
};

static int
on_url(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;
    int urllen = request->url ? strlen(request->url) : 0;

    request->url = realloc(request->url, urllen+length+1);
    assert(request->url);

    request->url[urllen] = '\0';
    strncat(request->url, at, length);

    strncpy(request->protocol, at + length + 1, 8);

    return 0;
}

static int
on_header_field(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    /* Check if our index is a value */
    if (request->headers_index%2 == 1) {
        request->headers_index++;
    }

    /* Allocate space for new field-value pair */
    if (request->headers_index == request->headers_size) {
        request->headers_size += 2;
        request->headers = realloc(request->headers,
                                   request->headers_size*sizeof(char*));
        assert(request->headers);
        request->headers[request->headers_index] = NULL;
        request->headers[request->headers_index+1] = NULL;
    }

    /* Allocate space in the current header string */
    if (request->headers[request->headers_index] == NULL) {
        request->headers[request->headers_index] = calloc(1, length + 1);
    } else {
        request->headers[request->headers_index] = realloc(
                request->headers[request->headers_index],
                strlen(request->headers[request->headers_index]) + length + 1
        );
    }
    assert(request->headers[request->headers_index]);

    strncat(request->headers[request->headers_index], at, length);
    return 0;
}

static int
on_header_value(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    /* Check if our index is a field */
    if (request->headers_index%2 == 0) {
        request->headers_index++;
    }

    /* Allocate space in the current header string */
    if (request->headers[request->headers_index] == NULL) {
        request->headers[request->headers_index] = calloc(1, length + 1);
    } else {
        request->headers[request->headers_index] = realloc(
                request->headers[request->headers_index],
                strlen(request->headers[request->headers_index]) + length + 1
        );
    }
    assert(request->headers[request->headers_index]);

    strncat(request->headers[request->headers_index], at, length);
    return 0;
}

static int
on_body(llhttp_t *parser, const char *at, size_t length)
{
    http_request_t *request = parser->data;

    request->data = realloc(request->data, request->datalen + length);
    assert(request->data);

    memcpy(request->data+request->datalen, at, length);
    request->datalen += length;
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
    request->parser_settings.on_header_field = &on_header_field;
    request->parser_settings.on_header_value = &on_header_value;
    request->parser_settings.on_body = &on_body;
    request->parser_settings.on_message_complete = &on_message_complete;

    llhttp_init(&request->parser, HTTP_REQUEST, &request->parser_settings);
    request->parser.data = request;
    request->is_reverse = false;
    return request;
}

void
http_request_destroy(http_request_t *request)
{
    if (request) {
        free(request->url);
        for (int i = 0; i < request->headers_size; i++) {
            free(request->headers[i]);
        }
        free(request->headers);
        free(request->data);
        free(request);
    }
}

int
http_request_add_data(http_request_t *request, const char *data, int datalen)
{
    assert(request);

    int ret = llhttp_execute(&request->parser, data, datalen);

    /* support for "Upgrade" to reverse http ("PTTH/1.0") protocol */
    llhttp_resume_after_upgrade(&request->parser);

    return ret;
}

int
http_request_is_complete(http_request_t *request)
{
    assert(request);
    return request->complete;
}

int
http_request_has_error(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return 0;
    }
    return (llhttp_get_errno(&request->parser) != HPE_OK);
}

const char *
http_request_get_error_name(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }
    return llhttp_errno_name(llhttp_get_errno(&request->parser));
}

const char *
http_request_get_error_description(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }
    return llhttp_get_error_reason(&request->parser);
}

const char *
http_request_get_method(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }
    return request->method;
}

const char *
http_request_get_url(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }
    return request->url;
}

const char *
http_request_get_protocol(http_request_t *request)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }
    return request->protocol;
}

const char *
http_request_get_header(http_request_t *request, const char *name)
{
    assert(request);
    if (request->is_reverse) {
        return NULL;
    }

    for (int i = 0; i < request->headers_size; i += 2) {
        if (!strcmp(request->headers[i], name)) {
            return request->headers[i+1];
        }
    }
    return NULL;
}

size_t
http_request_header_get_size(http_request_t *request, int *num_fields, size_t *max_field_len, size_t *max_value_len) {
    size_t total = 0;
    if (max_field_len) {
        *max_field_len = 0;
    }
    if (max_value_len) {
        *max_value_len = 0;
    }
    if (num_fields) {
        *num_fields = request->headers_size / 2;
    }
    for (int i = 0; i < request->headers_size; i +=2) {
        size_t len = strlen(request->headers[i]);
        total += len;
        if (i % 2 == 0 && max_field_len && len > *max_field_len) {
            *max_field_len = len;
        } else if (max_value_len && len > *max_value_len) {
            *max_value_len = len;
        }
    }
    return total;
}

const char *
http_request_get_data(http_request_t *request, int *datalen)
{
    assert(request);
    if (datalen) {
        *datalen = request->datalen;
    }
    return request->data;
}

int 
http_request_get_header_string(http_request_t *request, char **header_str)
{
    if(!request || request->headers_size == 0) {
        *header_str = NULL;
        return 0;
    }
    if (request->is_reverse) {
        *header_str = NULL;
        return 0;
    }    
    int len = 0;
    for (int i = 0; i < request->headers_size; i++) {
        len += strlen(request->headers[i]);
        if (i % 2 == 0) {
            len += 2;
        } else {
            len++;
        }
    }
    char *str = (char *) calloc(len+1, sizeof(char));
    assert(str);
    *header_str = str;
    char *p = str;
    int n = len + 1;
    for (int i = 0; i < request->headers_size; i++) {
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
    assert(p == &(str[len]));
    return len;
}

bool http_request_is_reverse(http_request_t *request) {
    return request->is_reverse;
}

void http_request_set_reverse(http_request_t *request) {
    request->is_reverse = true;
}
