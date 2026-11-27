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
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "sdp.h"
#include "crypto.h"

struct sdp_s {
	char *data;

	/* Actual SDP records */
	const char *version;
	const char *origin;
	const char *connection;
	const char *session;
	const char *time;
	const char *media;

	/* Additional SDP records */
	const char *rtpmap;
	const char *fmtp;
	const char *rsaaeskey;
	const char *fpaeskey;
	const char *aesiv;
	const char *min_latency;
};

static void
parse_sdp_line(sdp_t *sdp, char *line)
{
	int len = strlen(line);
	if (len < 2 || line[1] != '=') {
		return;
	}

	switch (line[0]) {
	case 'v':
		sdp->version = &line[2];
		break;
	case 'o':
		sdp->origin = &line[2];
		break;
	case 's':
		sdp->session = &line[2];
		break;
	case 'c':
		sdp->connection = &line[2];
		break;
	case 't':
		sdp->time = &line[2];
		break;
	case 'm':
		sdp->media = &line[2];
		break;
	case 'a':
		{
			char *key;
			char *value;

			/* Parse key and value */
			key = &line[2];
			value = strstr(line, ":");
			if (!value) break;
			*(value++) = '\0';

			if (!strcmp(key, "rtpmap") && !sdp->rtpmap) {
				sdp->rtpmap = value;
			} else if (!strcmp(key, "fmtp") && !sdp->fmtp) {
				sdp->fmtp = value;
			} else if (!strcmp(key, "rsaaeskey")) {
				sdp->rsaaeskey = value;
			} else if (!strcmp(key, "fpaeskey")) {
				sdp->fpaeskey = value;
			} else if (!strcmp(key, "aesiv")) {
				sdp->aesiv = value;
			} else if (!strcmp(key, "min-latency")) {
				sdp->min_latency = value;
			}
			break;
		}
	}
}

static void
parse_sdp_data(sdp_t *sdp)
{
	int pos, len;

	pos = 0;
	len = strlen(sdp->data);
	while (pos < len) {
		int lfpos;

		/* Find newline in string */
		for (lfpos=pos; sdp->data[lfpos]; lfpos++) {
			if (sdp->data[lfpos] == '\n') {
				break;
			}
		}
		if (sdp->data[lfpos] != '\n') {
			break;
		}

		/* Replace newline with '\0' and parse line */
		sdp->data[lfpos] = '\0';
		if (lfpos > pos && sdp->data[lfpos-1] == '\r') {
			sdp->data[lfpos-1] = '\0';
		}
		parse_sdp_line(sdp, sdp->data+pos);
		pos = lfpos+1;
	}
}

sdp_t *
sdp_init(const char *sdpdata, int sdpdatalen)
{
	sdp_t *sdp;

	sdp = calloc(1, sizeof(sdp_t));
	if (!sdp) {
		return NULL;
	}

	/* Allocate data buffer */
	sdp->data = malloc(sdpdatalen+1);
	if (!sdp->data) {
		free(sdp);
		return NULL;
	}
	memcpy(sdp->data, sdpdata, sdpdatalen);
	sdp->data[sdpdatalen] = '\0';
	parse_sdp_data(sdp);
	return sdp;
}

void
sdp_destroy(sdp_t *sdp)
{
	if (sdp) {
		free(sdp->data);
		free(sdp);
	}
}

const char *
sdp_get_version(sdp_t *sdp)
{
	assert(sdp);

	return sdp->version;
}

const char *
sdp_get_origin(sdp_t *sdp)
{
	assert(sdp);

	return sdp->origin;
}

const char *
sdp_get_session(sdp_t *sdp)
{
	assert(sdp);

	return sdp->session;
}

const char *
sdp_get_connection(sdp_t *sdp)
{
	assert(sdp);

	return sdp->connection;
}

const char *
sdp_get_time(sdp_t *sdp)
{
	assert(sdp);

	return sdp->time;
}

const char *
sdp_get_media(sdp_t *sdp)
{
	assert(sdp);

	return sdp->media;
}

const char *
sdp_get_rtpmap(sdp_t *sdp)
{
	assert(sdp);

	return sdp->rtpmap;
}

const char *
sdp_get_fmtp(sdp_t *sdp)
{
	assert(sdp);

	return sdp->fmtp;
}

const char *
sdp_get_min_latency(sdp_t *sdp)
{
	assert(sdp);

	return sdp->min_latency;
}

int
sdp_get_rsaaeskey(sdp_t *sdp, unsigned char *rsaaeskey, size_t len)
{
	assert(sdp);
	return airplay_b64_decode(sdp->rsaaeskey, rsaaeskey, len);
}

int
sdp_get_fpaeskey(sdp_t *sdp, unsigned char* fpaeskey, size_t len)
{
	assert(sdp);
	return airplay_b64_decode(sdp->fpaeskey, fpaeskey, len);
}

int
sdp_get_aesiv(sdp_t *sdp, unsigned char *aesiv, size_t len)
{
	assert(sdp);
	return airplay_b64_decode(sdp->aesiv, aesiv, len);
}
