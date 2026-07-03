/* Unit tests for the AirPlay feature-bitmask construction in lib/dnssd.c.
 * No GStreamer/network dependency: dnssd_t is used as a plain struct here,
 * dnssd_init()/dnssd_private_init() (which touch the mDNS backend) are never
 * called. See specs/no-audio-advertisement.md for the exact bit list and the
 * expected regression values. */

#include "dnssd.h"

#include <stdio.h>
#include <string.h>

/* dnssd.c also defines dnssd_init()/dnssd_destroy(), which call into the
 * backend-specific (lib/mdnsd or lib/dns_sd) dnssd_private_init()/
 * dnssd_private_destroy(). This test never calls dnssd_init()/dnssd_destroy()
 * (it only exercises pure feature-bitmask functions), but the linker still
 * requires these symbols to exist since they're referenced from dnssd.c.
 * These stubs are never actually invoked; they only satisfy the link. */
void *dnssd_private_init(dnssd_t *dnssd_public, int *error) {
    (void) dnssd_public;
    if (error) *error = 0;
    return NULL;
}

void dnssd_private_destroy(void *private) {
    (void) private;
}

static int failures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                               \
        }                                                             \
    } while (0)

/* Baseline with hls_support=h265_support=setup_legacy_pairing=0 is
 * features1=0x527FFEE6, features2=0x0 (verified by hand against the bit
 * table and cross-checked against the commented alternate FEATURES_1 macro
 * in lib/dnssdint.h). Only bits 0, 4, 27 (features1) and 42 (features2,
 * i.e. bit 10 of features2) are expected to move with these three options. */
static uint64_t expected_default_features(int hls_support, int h265_support, int setup_legacy_pairing) {
    uint32_t f1 = 0x527FFEE6u;
    uint32_t f2 = 0x0u;

    if (hls_support) {
        f1 |= (1u << 0);
        f1 |= (1u << 4);
    } else {
        f1 &= ~(1u << 0);
        f1 &= ~(1u << 4);
    }

    if (setup_legacy_pairing) {
        f1 |= (1u << 27);
    } else {
        f1 &= ~((uint32_t)1u << 27);
    }

    if (h265_support) {
        f2 |= (1u << (42 - 32));
    } else {
        f2 &= ~(1u << (42 - 32));
    }

    return (((uint64_t) f2) << 32) | (uint64_t) f1;
}

static void test_default_matches_hand_derived_baseline() {
    dnssd_t d;
    memset(&d, 0, sizeof(d));
    dnssd_set_default_airplay_features(&d, 0, 0, 0);
    CHECK(d.features1 == 0x527FFEE6u, "default features1 == 0x527FFEE6");
    CHECK(d.features2 == 0x0u, "default features2 == 0x0");
    CHECK(dnssd_get_airplay_features(&d) == 0x527FFEE6ULL, "default combined 64-bit value");
}

static void test_hls_h265_legacy_pairing_matrix() {
    int hls, h265, legacy;
    for (hls = 0; hls <= 1; hls++) {
        for (h265 = 0; h265 <= 1; h265++) {
            for (legacy = 0; legacy <= 1; legacy++) {
                dnssd_t d;
                memset(&d, 0, sizeof(d));
                dnssd_set_default_airplay_features(&d, hls, h265, legacy);
                uint64_t got = dnssd_get_airplay_features(&d);
                uint64_t want = expected_default_features(hls, h265, legacy);
                if (got != want) {
                    fprintf(stderr,
                            "FAIL: hls=%d h265=%d legacy=%d: got 0x%llX want 0x%llX (%s:%d)\n",
                            hls, h265, legacy,
                            (unsigned long long) got, (unsigned long long) want,
                            __FILE__, __LINE__);
                    failures++;
                }
            }
        }
    }
}

static void test_no_audio_advertise_clears_expected_bits() {
    dnssd_t d;
    memset(&d, 0, sizeof(d));
    dnssd_set_default_airplay_features(&d, 0, 0, 0);
    dnssd_apply_no_audio_advertise(&d);
    CHECK(d.features1 == 0x5243F4E6u, "no-audio-advertise features1 == 0x5243F4E6");
    CHECK(d.features2 == 0x0u, "no-audio-advertise features2 unchanged (0x0)");

    /* individual bits: 9, 11, 18, 19, 20, 21 must be off; neighboring bits untouched */
    CHECK(((d.features1 >> 9) & 1) == 0, "bit 9 (audio supported) cleared");
    CHECK(((d.features1 >> 11) & 1) == 0, "bit 11 (audio packet redundancy) cleared");
    CHECK(((d.features1 >> 18) & 1) == 0, "bit 18 (audio format 1) cleared");
    CHECK(((d.features1 >> 19) & 1) == 0, "bit 19 (audio format 2) cleared");
    CHECK(((d.features1 >> 20) & 1) == 0, "bit 20 (audio format 3) cleared");
    CHECK(((d.features1 >> 21) & 1) == 0, "bit 21 (audio format 4) cleared");
    CHECK(((d.features1 >> 10) & 1) == 1, "bit 10 (unrelated) left untouched");
    CHECK(((d.features1 >> 22) & 1) == 1, "bit 22 (unrelated) left untouched");
}

static void test_no_audio_advertise_is_idempotent() {
    dnssd_t once, twice;
    memset(&once, 0, sizeof(once));
    memset(&twice, 0, sizeof(twice));

    dnssd_set_default_airplay_features(&once, 0, 0, 0);
    dnssd_apply_no_audio_advertise(&once);

    dnssd_set_default_airplay_features(&twice, 0, 0, 0);
    dnssd_apply_no_audio_advertise(&twice);
    dnssd_apply_no_audio_advertise(&twice);

    CHECK(once.features1 == twice.features1, "applying twice == applying once (features1)");
    CHECK(once.features2 == twice.features2, "applying twice == applying once (features2)");
}

int main() {
    test_default_matches_hand_derived_baseline();
    test_hls_h265_legacy_pairing_matrix();
    test_no_audio_advertise_clears_expected_bits();
    test_no_audio_advertise_is_idempotent();

    if (failures) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("test_dnssd_features: OK\n");
    return 0;
}
