/* HLS master-playlist selection. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef HLS_FILTER_H
#define HLS_FILTER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HLS_CODEC_H264 = 1,
    HLS_CODEC_H265 = 2,
    HLS_CODEC_VP9 = 4,
    HLS_CODEC_AV1 = 8
};

typedef enum {
    HLS_FILTER_OK,
    HLS_FILTER_NO_MATCH,
    HLS_FILTER_INVALID,
    HLS_FILTER_NO_MEMORY
} hls_filter_result_t;

/* "0" and "all" respectively disable the limits. Outputs change only on success. */
bool hls_parse_resolution(const char *text, unsigned int *width, unsigned int *height);
bool hls_parse_codecs(const char *text, unsigned int *codecs);

/* Zero dimensions/codecs mean unrestricted. On success, *output is a new,
 * caller-owned string. Missing required video metadata is excluded when a
 * limit is enabled. Audio-only variants and rendition declarations are kept.
 * If every main video variant is excluded, return NO_MATCH rather than silently
 * playing audio only or falling back to an excluded video stream. */
hls_filter_result_t hls_filter_master_playlist(const char *playlist,
    unsigned int max_width, unsigned int max_height, unsigned int codecs,
    char **output, unsigned int *removed);

#ifdef __cplusplus
}
#endif
#endif
