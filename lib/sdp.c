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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <openssl/evp.h>

#include "sdp.h"
#include "utils.h"

/* SDP uses base64 with optional trailing padding. */
static bool
sdp_decode_base64(const char *value, unsigned char *output, size_t output_len)
{
    char encoded[97];
    unsigned char decoded[72];
    size_t len = strlen(value), padded_len = (len + 3) & ~(size_t)3;
    if (!len || padded_len > sizeof(encoded) - 1 || len % 4 == 1) {
        return false;
    }
    size_t padding_start = strcspn(value, "=");
    for (size_t i = 0; i < padding_start; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            return false;
        }
    }
    for (size_t i = padding_start; i < len; i++) {
        if (value[i] != '=') {
            return false;
        }
    }
    if (padded_len - padding_start > 2) {
        return false;
    }
    memcpy(encoded, value, len);
    memset(encoded + len, '=', padded_len - len);
    encoded[padded_len] = '\0';
    int decoded_len = EVP_DecodeBlock(decoded, (unsigned char *)encoded, (int)padded_len);
    if (decoded_len < 0 || (size_t)decoded_len - (padded_len - padding_start) != output_len) {
        return false;
    }
    memcpy(output, decoded, output_len);
    return true;
}

static bool
sdp_is_supported_alac(const char *rtpmap, const char *fmtp)
{
    unsigned int payload;
    char codec[32], extra;
    if (sscanf(rtpmap, "%3u %31s %c", &payload, codec, &extra) != 2 || payload < 96 || payload > 127 ||
        strcmp(codec, "AppleLossless")) {
        return false;
    }
    /* The renderer's ALAC cookie is fixed to 352 samples, 16-bit stereo,
     * 44.1 kHz. Reject other configurations instead of decoding them incorrectly. */
    const unsigned int format[] = {payload, 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, 44100};
    for (size_t i = 0; i < sizeof(format) / sizeof(format[0]); i++) {
        while (isspace((unsigned char)*fmtp)) {
            fmtp++;
        }
        if (*fmtp < '0' || *fmtp > '9') {
            return false;
        }
        char *end = NULL;
        unsigned long value = strtoul(fmtp, &end, 10);
        if (value != format[i] || (*end && !isspace((unsigned char)*end))) {
            return false;
        }
        fmtp = end;
    }
    while (isspace((unsigned char)*fmtp)) {
        fmtp++;
    }
    return *fmtp == '\0';
}

/* Parse the encrypted ALAC format currently supported by the renderer. */
int
sdp_parse_audio(const char *data, int datalen, sdp_audio_t *audio)
{
    const char *prefixes[] = {"a=rtpmap:", "a=fmtp:", "a=fpaeskey:", "a=aesiv:"};
    char *values[4] = {NULL};
    int result = SDP_AUDIO_INVALID;

    if (!audio || !data || datalen <= 0 || memchr(data, '\0', datalen)) {
        return result;
    }
    char *sdp = malloc((size_t)datalen + 1);
    if (!sdp) {
        return SDP_AUDIO_NOMEM;
    }
    memcpy(sdp, data, datalen);
    sdp[datalen] = '\0';
    char *current = sdp, *line;
    while ((line = utils_strsep(&current, "\r\n"))) {
        for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
            size_t prefix_len = strlen(prefixes[i]);
            if (!strncmp(line, prefixes[i], prefix_len)) {
                if (values[i]) {
                    goto cleanup;
                }
                values[i] = line + prefix_len;
            }
        }
    }
    if (!values[0] || !values[1] || !values[2] || !values[3] ||
        !sdp_decode_base64(values[2], audio->ekey, sizeof(audio->ekey)) ||
        !sdp_decode_base64(values[3], audio->iv, sizeof(audio->iv))) {
        goto cleanup;
    }
    result = sdp_is_supported_alac(values[0], values[1]) ? SDP_AUDIO_OK : SDP_AUDIO_UNSUPPORTED;
cleanup:
    free(sdp);
    return result;
}
