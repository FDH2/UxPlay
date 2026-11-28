/*
 * UxPlay MPRIS2 support: exposes the AirPlay audio stream (track metadata,
 * cover art, progress, play state) as an org.mpris.MediaPlayer2 player on the
 * D-Bus session bus, so desktop media widgets (GNOME, KDE, playerctl, ...)
 * can show it. Transport buttons are forwarded to the AirPlay client with
 * DACP (the client's _dacp._tcp service is resolved through avahi-daemon).
 *
 * All functions are thread-safe and may be called from RAOP threads.
 * D-Bus traffic itself happens on the default GMainContext.
 *
 * This file is part of UxPlay and is distributed under the GPL v3 or later.
 */

#ifndef UXPLAY_MPRIS_H
#define UXPLAY_MPRIS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* identity: name shown by media widgets (e.g. the UxPlay server name) */
bool mpris_init(const char *identity);
void mpris_shutdown(void);

/* any argument may be NULL */
void mpris_set_metadata(const char *title, const char *artist, const char *album, const char *genre);
void mpris_set_coverart(const void *image, int len);
/* RTP timestamps (44100 Hz) as delivered by the "progress" SET_PARAMETER */
void mpris_set_progress(uint32_t start, uint32_t curr, uint32_t end);
/* cheap; safe to call for every audio packet */
void mpris_set_playing(bool playing);
/* DACP-ID and Active-Remote headers sent by the client */
void mpris_set_remote(const char *dacp_id, const char *active_remote);
/* client disconnected: remove the player from the bus */
void mpris_clear(void);

#ifdef __cplusplus
}
#endif

#endif
