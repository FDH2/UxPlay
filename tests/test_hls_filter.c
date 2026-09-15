/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "hls_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

#define HEADER "#EXTM3U\n#EXT-X-VERSION:7\n"
#define AUDIO "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"English\",URI=\"audio.m3u8\"\n"
#define SUBS "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"English\",URI=\"subs.m3u8\"\n"
#define AVC "#EXT-X-STREAM-INF:BANDWIDTH=4655472,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080,AUDIO=\"a\",SUBTITLES=\"s\"\navc.m3u8\n"
#define HEVC "#EXT-X-STREAM-INF:CODECS=\"hvc1.1.6.L150.B0,mp4a.40.2\",RESOLUTION=3840x2160,BANDWIDTH=10000000\nhevc.m3u8\n"
#define VP9 "#EXT-X-STREAM-INF:BANDWIDTH=29687525,CODECS=\"vp09.00.50.08,mp4a.40.2\",RESOLUTION=3840x2160\nvp9.m3u8\n"
#define AV1 "#EXT-X-STREAM-INF:CODECS=\"av01.0.08M.08,mp4a.40.2\",RESOLUTION=1920x1080\nav1.m3u8\n"
#define AUDIO_VARIANT "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\naudio-only.m3u8\n"

static void expect(const char *input, unsigned int w, unsigned int h, unsigned int codecs,
                   hls_filter_result_t status, const char *expected, unsigned int removed) {
    char *output = NULL;
    unsigned int count = 0;
    CHECK(hls_filter_master_playlist(input, w, h, codecs, &output, &count) == status);
    if (status == HLS_FILTER_OK) {
        CHECK(output != NULL);
        CHECK(!strcmp(output, expected));
        CHECK(count == removed);
        free(output);
    } else CHECK(output == NULL);
}

static void test_options(void) {
    unsigned int w = 17, h = 23, codecs = 99;
    CHECK(hls_parse_resolution("1920x1080", &w, &h) && w == 1920 && h == 1080);
    const char *bad_sizes[] = {"", "1920", "1920x", "x1080", "0x1080", "1920x0", "-1x1080",
        "1920X1080", "1920x1080junk", "1920x1080@60", " 1920x1080", "999999999999999999999999x1080"};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(*bad_sizes); i++) {
        CHECK(!hls_parse_resolution(bad_sizes[i], &w, &h));
        CHECK(w == 1920 && h == 1080);
    }
    CHECK(hls_parse_resolution("0", &w, &h) && !w && !h);
    CHECK(hls_parse_codecs("h264:h265", &codecs) && codecs == (HLS_CODEC_H264 | HLS_CODEC_H265));
    const char *bad_codecs[] = {"", "h264:", ":h264", "h264::h265", "vp7", "h264,hevc", "all:h264", "H264"};
    for (size_t i = 0; i < sizeof(bad_codecs) / sizeof(*bad_codecs); i++) {
        CHECK(!hls_parse_codecs(bad_codecs[i], &codecs));
        CHECK(codecs == (HLS_CODEC_H264 | HLS_CODEC_H265));
    }
    CHECK(hls_parse_codecs("vp9:av1", &codecs) && codecs == (HLS_CODEC_VP9 | HLS_CODEC_AV1));
    CHECK(hls_parse_codecs("all", &codecs) && !codecs);
}

static void test_selection(void) {
    const char *input = HEADER AUDIO SUBS VP9 AVC HEVC AV1 AUDIO_VARIANT;
    expect(input, 0, 0, 0, HLS_FILTER_OK, input, 0);
    expect(input, 0, 0, HLS_CODEC_H264 | HLS_CODEC_H265, HLS_FILTER_OK,
           HEADER AUDIO SUBS AVC HEVC AUDIO_VARIANT, 2);
    expect(input, 1920, 1080, 0, HLS_FILTER_OK, HEADER AUDIO SUBS AVC AV1 AUDIO_VARIANT, 2);
    expect(input, 1920, 1080, HLS_CODEC_H264, HLS_FILTER_OK, HEADER AUDIO SUBS AVC AUDIO_VARIANT, 3);
    expect(HEADER VP9 AUDIO_VARIANT, 0, 0, HLS_CODEC_H264, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER AVC, 640, 360, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER AUDIO AUDIO_VARIANT, 640, 360, HLS_CODEC_H264, HLS_FILTER_OK,
           HEADER AUDIO AUDIO_VARIANT, 0);
    /* Codec strings containing commas do not hide the following resolution. */
    expect(HEADER AVC, 1919, 1080, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER AVC, 1920, 1079, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    /* The sibling sample-entry types avc3/hev1 are also supported. */
    const char *aliases = HEADER
        "#EXT-X-STREAM-INF:CODECS=\"avc3.640028,ec-3\",RESOLUTION=1920x1080\na.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"hev1.1.6.L150.B0,mp4a.40.2\",RESOLUTION=3840x2160\nh.m3u8\n";
    expect(aliases, 0, 0, HLS_CODEC_H264 | HLS_CODEC_H265, HLS_FILTER_OK, aliases, 0);
}

static void test_metadata(void) {
    const char *missing_codec = HEADER "#EXT-X-STREAM-INF:RESOLUTION=1920x1080\na.m3u8\n";
    const char *missing_size = HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\"\na.m3u8\n";
    expect(missing_codec, 1920, 1080, 0, HLS_FILTER_OK, missing_codec, 0);
    expect(missing_codec, 0, 0, HLS_CODEC_H264, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(missing_size, 0, 0, HLS_CODEC_H264, HLS_FILTER_OK, missing_size, 0);
    expect(missing_size, 1920, 1080, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc10.fake\"\na.m3u8\n",
           0, 0, HLS_CODEC_H264, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028,vp09.00.50.08\"\na.m3u8\n",
           0, 0, HLS_CODEC_H264, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\",RESOLUTION=999999999999999999999999x1\na.m3u8\n",
           1920, 1080, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    /* Attribute-looking text inside a quoted value is not a real attribute. */
    expect(HEADER "#EXT-X-STREAM-INF:NAME=\"x,RESOLUTION=1x1\",CODECS=\"avc1.640028\"\na.m3u8\n",
           1920, 1080, 0, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\",CODECS=\"vp09.00.50.08\"\na.m3u8\n",
           0, 0, HLS_CODEC_H264, HLS_FILTER_NO_MATCH, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:RESOLUTION=1x1,RESOLUTION=3840x2160\na.m3u8\n",
           1920, 1080, 0, HLS_FILTER_NO_MATCH, NULL, 0);
}

static void test_lines(void) {
    const char *crlf = "#EXTM3U\r\n#EXT-X-STREAM-INF:CODECS=\"avc1.640028\",RESOLUTION=1920x1080\r\n"
                       "# a comment\r\n\r\navc.m3u8";
    expect(crlf, 1920, 1080, HLS_CODEC_H264, HLS_FILTER_OK, crlf, 0);
    expect(HEADER AVC "#EXT-X-STREAM-INF:CODECS=\"vp09.00.50.08\"\n# comment\n\nvp9.m3u8",
           0, 0, HLS_CODEC_H264, HLS_FILTER_OK, HEADER AVC, 1);
    const char *iframe = "#EXT-X-I-FRAME-STREAM-INF:CODECS=\"vp09.00.50.08\",RESOLUTION=3840x2160,URI=\"i,frame.m3u8\"\n";
    char input[2048];
    snprintf(input, sizeof(input), "%s%s%s", HEADER, AVC, iframe);
    expect(input, 1920, 1080, HLS_CODEC_H264, HLS_FILTER_OK, HEADER AVC, 1);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\"\n",
           0, 0, HLS_CODEC_H264, HLS_FILTER_INVALID, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\"\n" AVC,
           0, 0, HLS_CODEC_H264, HLS_FILTER_INVALID, NULL, 0);
    expect(HEADER "#EXT-X-STREAM-INF:CODECS=\"vp09.00.50.08\"\n" AUDIO,
           0, 0, HLS_CODEC_H264, HLS_FILTER_INVALID, NULL, 0);
}

static void test_truncations(void) {
    /* Exercise every truncation boundary with ASan/UBSan, including quoted
     * attribute values, a missing URI, CRLF, and a missing final newline. */
    const char *input = HEADER AUDIO VP9 AVC HEVC;
    size_t length = strlen(input);
    char *copy = malloc(length + 1);
    CHECK(copy != NULL);
    for (size_t n = 0; n <= length; n++) {
        memcpy(copy, input, n);
        copy[n] = '\0';
        char *output;
        unsigned int removed;
        hls_filter_result_t result = hls_filter_master_playlist(copy, 1920, 1080,
            HLS_CODEC_H264 | HLS_CODEC_H265, &output, &removed);
        CHECK(result >= HLS_FILTER_OK && result <= HLS_FILTER_NO_MEMORY);
        if (result == HLS_FILTER_OK) CHECK(strlen(output) <= n);
        else CHECK(output == NULL);
        free(output);
    }
    free(copy);
}

int main(void) {
    test_options();
    test_selection();
    test_metadata();
    test_lines();
    test_truncations();
    puts("HLS filter tests passed");
    return 0;
}
