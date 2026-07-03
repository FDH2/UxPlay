/*
 * Pure decision logic for the "--no-audio-advertise" experimental option.
 * See specs/no-audio-advertisement.md for the behavior these functions encode.
 *
 * Kept dependency-free (only <string>) so it can be unit-tested without
 * linking the rest of uxplay.cpp (GStreamer, D-Bus, etc.).
 */

#ifndef UXPLAY_AUDIO_ADVERTISE_H
#define UXPLAY_AUDIO_ADVERTISE_H

#include <string>

/* recognizes "--no-audio-advertise" and its short alias "-naa" */
bool uxplay_is_no_audio_advertise_flag(const std::string &arg);

/*
 * Parses the optional trailing mode token for --no-audio-advertise
 * ("bits" | "raop" | "both"). On success, sets clear_bits and skip_raop and
 * returns true. On an unrecognized token, leaves the outputs untouched and
 * returns false (caller should treat this as a CLI error).
 */
bool uxplay_parse_no_audio_advertise_mode(const std::string &token, bool &clear_bits, bool &skip_raop);

/* whether register_dnssd() should call dnssd_register_raop() */
bool uxplay_should_register_raop(bool naa_skip_raop);

/* "-a"/"-as 0" used without narrowing the advertisement: local audio is off,
 * but a client may still be routed audio it can't hear. */
bool uxplay_should_warn_audio_advertise(bool use_audio, bool no_audio_advertise);

/* defensive/documentation guard: --no-audio-advertise always forces
 * use_audio=false, so this should never be true in practice. */
bool uxplay_should_warn_audio_still_playing(bool use_audio, bool no_audio_advertise);

#endif
