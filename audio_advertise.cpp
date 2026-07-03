#include "audio_advertise.h"

bool uxplay_is_no_audio_advertise_flag(const std::string &arg) {
    return arg == "--no-audio-advertise" || arg == "-naa";
}

bool uxplay_parse_no_audio_advertise_mode(const std::string &token, bool &clear_bits, bool &skip_raop) {
    if (token == "bits") {
        clear_bits = true;
        skip_raop = false;
        return true;
    }
    if (token == "raop") {
        clear_bits = false;
        skip_raop = true;
        return true;
    }
    if (token == "both") {
        clear_bits = true;
        skip_raop = true;
        return true;
    }
    return false;
}

bool uxplay_should_register_raop(bool naa_skip_raop) {
    return !naa_skip_raop;
}

bool uxplay_should_warn_audio_advertise(bool use_audio, bool no_audio_advertise) {
    return !use_audio && !no_audio_advertise;
}

bool uxplay_should_warn_audio_still_playing(bool use_audio, bool no_audio_advertise) {
    return no_audio_advertise && use_audio;
}
