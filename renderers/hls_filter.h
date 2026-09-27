/* SPDX-License-Identifier: GPL-3.0-or-later
 * Apply the HLS stream filter (-rpi, -custom) to master playlists that GStreamer fetches itself,
 * as it does when a client hands over a plain http(s) URL (Vimeo, Safari) instead of a playlist. */
#ifndef HLS_FILTER_H
#define HLS_FILTER_H

#include <stdbool.h>
#include <gst/gst.h>
#include "../lib/raop.h"
#include "../lib/logger.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Filter the master playlist of every HLS demuxer this playbin creates. Does nothing for DESKTOP. */
void hls_filter_install(GstElement *playbin, device_profile_t device, bool hw_avc, bool hw_hevc,
                        const char *custom_profile, logger_t *logger);
/* Attach to an HLS demuxer's sink pad; the probe keeps its own copy of the settings. */
void hls_filter_attach(GstPad *sink, device_profile_t device, bool hw_avc, bool hw_hevc,
                       const char *custom_profile, logger_t *logger);
#ifdef __cplusplus
}
#endif
#endif
