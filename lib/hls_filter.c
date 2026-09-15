/* HLS master-playlist selection. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "hls_filter.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool equal(const char *first, const char *last, const char *text) {
    return (size_t)(last - first) == strlen(text) && !memcmp(first, text, last - first);
}

static bool number(const char **text, const char *end, unsigned int *value) {
    const char *p = *text;
    unsigned int n = 0;
    if (p == end || *p < '0' || *p > '9') return false;
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p++ - '0');
        if (n > (UINT_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *text = p;
    *value = n;
    return n != 0;
}

static bool resolution(const char *first, const char *last,
                       unsigned int *width, unsigned int *height) {
    unsigned int w, h;
    if (!number(&first, last, &w) || first == last || *first++ != 'x' ||
        !number(&first, last, &h) || first != last) return false;
    *width = w;
    *height = h;
    return true;
}

bool hls_parse_resolution(const char *text, unsigned int *width, unsigned int *height) {
    if (!text) return false;
    if (!strcmp(text, "0")) {
        *width = *height = 0;
        return true;
    }
    return resolution(text, text + strlen(text), width, height);
}

bool hls_parse_codecs(const char *text, unsigned int *codecs) {
    unsigned int mask = 0;
    if (!text || !*text) return false;
    if (!strcmp(text, "all")) {
        *codecs = 0;
        return true;
    }
    while (*text) {
        const char *end = text + strcspn(text, ":");
        if (equal(text, end, "h264")) mask |= HLS_CODEC_H264;
        else if (equal(text, end, "h265")) mask |= HLS_CODEC_H265;
        else if (equal(text, end, "vp9")) mask |= HLS_CODEC_VP9;
        else if (equal(text, end, "av1")) mask |= HLS_CODEC_AV1;
        else return false;
        if (!*end) break;
        text = end + 1;
        if (!*text) return false;
    }
    *codecs = mask;
    return true;
}

/* Attribute values may contain commas inside quotes (notably CODECS and URI).
 * Bound every scan to this line; do not match attribute names inside a value. */
/* 1: present, 0: absent, -1: malformed/ambiguous. */
static int attribute(const char *first, const char *last, const char *name,
                      const char **value, const char **value_end) {
    int found = 0;
    while (first < last) {
        const char *end = first;
        bool quoted = false;
        while (end < last) {
            if (*end == '"') quoted = !quoted;
            if (*end == ',' && !quoted) break;
            end++;
        }
        const char *key = first;
        while (key < end && (*key == ' ' || *key == '\t')) key++;
        const char *eq = memchr(key, '=', end - key);
        if (quoted) return -1;
        if (eq && equal(key, eq, name)) {
            if (found) return -1;
            found = 1;
            *value = eq + 1;
            *value_end = end;
        }
        first = end < last ? end + 1 : last;
    }
    return found;
}

static bool codec_prefix(const char *first, const char *last, const char *name) {
    size_t len = strlen(name);
    return (size_t)(last - first) >= len && !memcmp(first, name, len) &&
           (first + len == last || first[len] == '.');
}

static bool allowed(const char *first, const char *last, unsigned int max_width,
                    unsigned int max_height, unsigned int codecs, bool *audio_only) {
    const char *value, *end;
    unsigned int video_codecs = 0;
    bool known_audio = false, unknown_codec = false;
    if (attribute(first, last, "CODECS", &value, &end) == 1 && end - value >= 2 &&
        *value == '"' && end[-1] == '"') {
        value++;
        end--;
        while (value < end) {
            const char *next = memchr(value, ',', end - value);
            const char *token_end = next ? next : end;
            if (codec_prefix(value, token_end, "avc1") || codec_prefix(value, token_end, "avc3"))
                video_codecs |= HLS_CODEC_H264;
            else if (codec_prefix(value, token_end, "hvc1") || codec_prefix(value, token_end, "hev1"))
                video_codecs |= HLS_CODEC_H265;
            else if (codec_prefix(value, token_end, "vp09")) video_codecs |= HLS_CODEC_VP9;
            else if (codec_prefix(value, token_end, "av01")) video_codecs |= HLS_CODEC_AV1;
            else if (codec_prefix(value, token_end, "mp4a") || equal(value, token_end, "ac-3") ||
                     equal(value, token_end, "ec-3") || equal(value, token_end, "opus") ||
                     equal(value, token_end, "flac") || equal(value, token_end, "alac")) known_audio = true;
            else unknown_codec = true;
            value = next ? next + 1 : end;
            if (next && value == end) unknown_codec = true;
        }
    }
    int has_resolution = attribute(first, last, "RESOLUTION", &value, &end);
    *audio_only = known_audio && !video_codecs && !unknown_codec && !has_resolution;
    if (*audio_only) return true;
    if (codecs && (!video_codecs || unknown_codec || (video_codecs & ~codecs))) return false;
    if (max_width || max_height) {
        unsigned int w, h;
        if (has_resolution != 1 || !resolution(value, end, &w, &h) ||
            w > max_width || h > max_height) return false;
    }
    return true;
}

static const char *line_end(const char *first, const char **end) {
    const char *newline = strchr(first, '\n');
    *end = newline ? newline : first + strlen(first);
    if (*end > first && (*end)[-1] == '\r') (*end)--;
    return newline ? newline + 1 : first + strlen(first);
}

hls_filter_result_t hls_filter_master_playlist(const char *playlist,
    unsigned int max_width, unsigned int max_height, unsigned int codecs,
    char **output, unsigned int *removed) {
    const char stream_tag[] = "#EXT-X-STREAM-INF:";
    const char iframe_tag[] = "#EXT-X-I-FRAME-STREAM-INF:";
    *output = NULL;
    *removed = 0;
    if (!playlist || (!max_width != !max_height)) return HLS_FILTER_INVALID;
    size_t length = strlen(playlist);
    char *result = malloc(length + 1);
    if (!result) return HLS_FILTER_NO_MEMORY;
    if (!max_width && !codecs) {
        memcpy(result, playlist, length + 1);
        *output = result;
        return HLS_FILTER_OK;
    }
    const char *first = playlist;
    char *write = result;
    unsigned int videos = 0, kept_videos = 0;
    while (*first) {
        const char *end;
        const char *next = line_end(first, &end);
        bool stream = (size_t)(end - first) >= sizeof(stream_tag) - 1 &&
                      !memcmp(first, stream_tag, sizeof(stream_tag) - 1);
        bool iframe = (size_t)(end - first) >= sizeof(iframe_tag) - 1 &&
                      !memcmp(first, iframe_tag, sizeof(iframe_tag) - 1);
        bool keep = true;
        if (stream || iframe) {
            bool audio_only;
            keep = allowed(first + (stream ? sizeof(stream_tag) - 1 : sizeof(iframe_tag) - 1),
                           end, max_width, max_height, codecs, &audio_only);
            if (stream) {
                if (!audio_only) {
                    videos++;
                    if (keep) kept_videos++;
                }
                /* A STREAM-INF declaration and its following URI are one unit.
                 * Allow blank lines/comments between them, but not another HLS tag. */
                const char *uri = next;
                bool found_uri = false;
                while (*uri) {
                    const char *uri_end;
                    next = line_end(uri, &uri_end);
                    if (uri_end > uri && *uri != '#') {
                        found_uri = true;
                        break;
                    }
                    if (uri_end - uri >= 4 && !memcmp(uri, "#EXT", 4)) break;
                    uri = next;
                }
                if (!found_uri) {
                    free(result);
                    return HLS_FILTER_INVALID;
                }
            }
            if (!keep) (*removed)++;
        }
        if (keep) {
            memcpy(write, first, next - first);
            write += next - first;
        }
        first = next;
    }
    if (videos && !kept_videos) {
        free(result);
        return HLS_FILTER_NO_MATCH;
    }
    *write = '\0';
    *output = result;
    return HLS_FILTER_OK;
}
