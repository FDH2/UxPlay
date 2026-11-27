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
 */
// modified by fduncanh 2026

#ifndef SDP_H
#define SDP_H

typedef struct sdp_s sdp_t;

sdp_t *sdp_init(const char *sdpdata, int sdpdatalen);

const char *sdp_get_version(sdp_t *sdp);
const char *sdp_get_origin(sdp_t *sdp);
const char *sdp_get_session(sdp_t *sdp);
const char *sdp_get_connection(sdp_t *sdp);
const char *sdp_get_time(sdp_t *sdp);
const char *sdp_get_media(sdp_t *sdp);
const char *sdp_get_rtpmap(sdp_t *sdp);
const char *sdp_get_fmtp(sdp_t *sdp);
const char *sdp_get_min_latency(sdp_t *sdp);
int sdp_get_rsaaeskey(sdp_t *sdp, unsigned char *rsaaeskey, size_t len);
int sdp_get_fpaeskey(sdp_t *sdp, unsigned char *fpaeskey, size_t len);
int sdp_get_aesiv(sdp_t *sdp, unsigned char *aesiv, size_t len);

void sdp_destroy(sdp_t *sdp);

#endif
