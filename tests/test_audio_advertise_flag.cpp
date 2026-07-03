/* Unit tests for audio_advertise.h pure decision logic. No GStreamer/network
 * dependency; exercises the predicates that drive --no-audio-advertise and
 * the -a/-as-0 advertisement warning. See specs/no-audio-advertisement.md. */

#include "audio_advertise.h"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_flag_recognition() {
    CHECK(uxplay_is_no_audio_advertise_flag("--no-audio-advertise"), "long flag recognized");
    CHECK(uxplay_is_no_audio_advertise_flag("-naa"), "short alias recognized");
    CHECK(!uxplay_is_no_audio_advertise_flag("-a"), "-a is not the new flag");
    CHECK(!uxplay_is_no_audio_advertise_flag("-as"), "-as is not the new flag");
    CHECK(!uxplay_is_no_audio_advertise_flag("--no-audio-advertised"), "near-miss string rejected");
}

static void test_mode_parsing() {
    bool clear_bits = false, skip_raop = false;

    clear_bits = skip_raop = false;
    CHECK(uxplay_parse_no_audio_advertise_mode("bits", clear_bits, skip_raop), "bits mode parses");
    CHECK(clear_bits && !skip_raop, "bits mode: clear bits only");

    clear_bits = skip_raop = false;
    CHECK(uxplay_parse_no_audio_advertise_mode("raop", clear_bits, skip_raop), "raop mode parses");
    CHECK(!clear_bits && skip_raop, "raop mode: skip raop only");

    clear_bits = skip_raop = false;
    CHECK(uxplay_parse_no_audio_advertise_mode("both", clear_bits, skip_raop), "both mode parses");
    CHECK(clear_bits && skip_raop, "both mode: clear bits and skip raop");

    clear_bits = skip_raop = false;
    CHECK(!uxplay_parse_no_audio_advertise_mode("bogus", clear_bits, skip_raop), "unrecognized token rejected");
}

static void test_should_register_raop() {
    CHECK(uxplay_should_register_raop(false) == true, "raop registered when not skipped");
    CHECK(uxplay_should_register_raop(true) == false, "raop not registered when skipped");
}

static void test_warning_predicates() {
    /* -a alone (or -as 0 alone): use_audio=false, no_audio_advertise=false -> warn */
    CHECK(uxplay_should_warn_audio_advertise(false, false) == true, "-a alone warns");
    /* --no-audio-advertise (any mode): no warning needed, advertisement was narrowed */
    CHECK(uxplay_should_warn_audio_advertise(false, true) == false, "no warning once advertisement narrowed");
    /* neither flag: default use_audio=true -> no warning */
    CHECK(uxplay_should_warn_audio_advertise(true, false) == false, "no warning when audio left on");
    CHECK(uxplay_should_warn_audio_advertise(true, true) == false, "no warning when audio left on (defensive)");

    /* defensive invariant: --no-audio-advertise must force use_audio=false, so this
     * combination should never legitimately occur, but the predicate must still be correct */
    CHECK(uxplay_should_warn_audio_still_playing(true, true) == true, "flags audio still on despite no_audio_advertise");
    CHECK(uxplay_should_warn_audio_still_playing(false, true) == false, "no warning: audio correctly off");
    CHECK(uxplay_should_warn_audio_still_playing(true, false) == false, "no warning: no_audio_advertise not set");
    CHECK(uxplay_should_warn_audio_still_playing(false, false) == false, "no warning: neither set");
}

int main() {
    test_flag_recognition();
    test_mode_parsing();
    test_should_register_raop();
    test_warning_predicates();

    if (failures) {
        std::fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("test_audio_advertise_flag: OK\n");
    return 0;
}
