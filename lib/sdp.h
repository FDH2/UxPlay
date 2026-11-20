/**
 *  Copyright (C) 2026  Omar Roth
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 */

#ifndef SDP_H
#define SDP_H

typedef struct sdp_audio_s {
    unsigned char ekey[72];
    unsigned char iv[16];
} sdp_audio_t;

enum { SDP_AUDIO_OK = 0, SDP_AUDIO_INVALID = -1, SDP_AUDIO_UNSUPPORTED = -2, SDP_AUDIO_NOMEM = -3 };

/* Read classic RAOP's ALAC format and FairPlay key/IV attributes.
 * The body need not be NUL-terminated. Keys may use unpadded base64.
 * The output is valid only when SDP_AUDIO_OK is returned. */
int sdp_parse_audio(const char *data, int datalen, sdp_audio_t *audio);

#endif
