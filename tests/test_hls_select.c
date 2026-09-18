#include "raop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define HEAD "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",URI=\"audio.m3u8\"\n" \
             "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",URI=\"subs.m3u8\"\n"
#define V(c,r,u) "#EXT-X-STREAM-INF:CODECS=\"" c ",mp4a.40.2\",RESOLUTION=" r ",AUDIO=\"a\",SUBTITLES=\"s\"\n" u "\n"
#define AVC V("avc1.640028", "1920x1080", "avc.m3u8")
#define LOW V("avc1.64001f", "1280x720", "avc-low.m3u8")
#define VP9 V("vp09.00.40.08", "1920x1080", "vp9.m3u8")
#define UHD V("vp09.00.50.08", "3840x2160", "vp9-4k.m3u8")
#define HEVC V("hvc1.1.6.L150.B0", "3840x2160", "hevc.m3u8")
#define AUDIO "#EXT-X-STREAM-INF:CODECS=\"mp4a.40.2\"\naudio-only.m3u8\n"

static void expect(const char *input, const char *option, const char *expected, int removed) {
    hls_codec_t *codecs;
    size_t count;
    CHECK(hls_select_parse(option, &codecs, &count));
    char *copy = malloc(strlen(input) + 1);
    CHECK(copy != NULL);
    strcpy(copy, input);
    CHECK(select_master_playlist_video(copy, codecs, count) == removed);
    CHECK(!strcmp(copy, expected ? expected : input)); /* failures must not modify input */
    free(copy);
    free(codecs);
}

int main(void) {
    const char *both = "avc1@1920x1080:vp09@1920x1080";
    expect(HEAD LOW AVC UHD VP9, "", HEAD LOW AVC UHD VP9, 0);
    expect(HEAD LOW AVC UHD VP9, both, HEAD LOW AVC, 2); /* tied resolution: codec order */
    expect(HEAD LOW AVC UHD VP9, "vp09@1920x1080:avc1@1920x1080", HEAD VP9, 3);
    expect(HEAD LOW VP9, both, HEAD VP9, 1); /* full-HD VP9 beats 720p AVC */
    expect(HEAD VP9, both, HEAD VP9, 0); /* only Full HD VP9 */
    expect(HEAD AVC UHD, both, HEAD AVC, 1); /* out-of-limit VP9 cannot win */
    expect(HEAD AVC HEVC, "avc1@1920x1080:hvc1@3840x2160", HEAD HEVC, 1);
    expect(HEAD AVC HEVC, "avc1@1920x1080:hvc1", HEAD HEVC, 1); /* unbounded */
    expect(HEAD AVC UHD, "avc1@1920x1080", HEAD AVC, 1); /* unlisted codec */
    expect(HEAD UHD, both, NULL, -1);
    expect(HEAD AVC, "avc1@1919x1080", NULL, -1);
    expect(HEAD AVC, "avc1@1920x1079", NULL, -1);
    expect(HEAD AVC VP9 AUDIO, both, HEAD AVC AUDIO, 1); /* preserve audio/subtitle data */
    expect(HEAD UHD AUDIO, both, NULL, -1); /* no silent audio-only fallback */
    expect(HEAD AVC V("avc1.640028", "1920x1080", "other-bitrate.m3u8"), both,
           HEAD AVC V("avc1.640028", "1920x1080", "other-bitrate.m3u8"), 0);
    const char *unknown = HEAD "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\"\nu.m3u8\n";
    expect(unknown, both, NULL, -1); /* size needed for a capped codec */
    expect(unknown, "avc1", unknown, 0);
    expect(HEAD "#EXT-X-STREAM-INF:CODECS=\"avc1\"\nu.m3u8\n" VP9,
           "avc1:vp09@1920x1080", HEAD VP9, 1); /* known size beats unknown */
    expect(HEAD V("avc1", "1920x720", "wide.m3u8") V("vp09", "1280x1080", "tall.m3u8"),
           both, HEAD V("avc1", "1920x720", "wide.m3u8"), 1); /* equal pixel counts */
    expect(HEAD V("avc1", "1080x1920", "portrait.m3u8"), both, NULL, -1);
    expect(HEAD V("avc1", "1080x1920", "portrait.m3u8"), "avc1@1080x1920",
           HEAD V("avc1", "1080x1920", "portrait.m3u8"), 0);
    expect(HEAD "#EXT-X-STREAM-INF:CODECS=\"avc10.fake\",RESOLUTION=1x1\nu.m3u8\n", "avc1", NULL, -1);
    expect(HEAD "#EXT-X-STREAM-INF:CODECS=\"avc1,vp09\",RESOLUTION=1x1\nu.m3u8\n", both, NULL, -1);
    expect(HEAD "#EXT-X-STREAM-INF:CODECS=\"avc1\",CODECS=\"vp09\",RESOLUTION=1x1\nu.m3u8\n", both, NULL, -1);
    expect(HEAD "#EXT-X-STREAM-INF:CODECS=\"avc1\",RESOLUTION=1x1,RESOLUTION=2x2\nu.m3u8\n", "avc1", NULL, -1);
    expect(HEAD "#EXT-X-STREAM-INF:NAME=\"x,RESOLUTION=1x1\",CODECS=\"avc1\"\nu.m3u8\n", both, NULL, -1);
    const char *crlf = "#EXTM3U\r\n#EXT-X-STREAM-INF:CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080\r\n# comment\r\n\r\navc.m3u8";
    expect(crlf, both, crlf, 0);
    expect(HEAD AVC "#EXT-X-STREAM-INF:CODECS=\"vp09\",RESOLUTION=3840x2160\n# comment\n\nvp9.m3u8", both, HEAD AVC, 1);
    expect(HEAD AVC "#EXT-X-I-FRAME-STREAM-INF:CODECS=\"vp09\",RESOLUTION=1920x1080,URI=\"i,frame.m3u8\"\n", both, HEAD AVC, 1);
    const char *iframes = HEAD AVC "#EXT-X-I-FRAME-STREAM-INF:CODECS=\"avc1\",RESOLUTION=1280x720,URI=\"i.m3u8\"\n";
    expect(iframes, both, iframes, 0);
    expect(HEAD AVC "#EXT-X-I-FRAME-STREAM-INF:CODECS=\"avc1\",RESOLUTION=3840x2160,URI=\"i.m3u8\"\n", both, HEAD AVC, 1);
    expect(HEAD AVC "#EXT-X-STREAM-INF:CODECS=\"vp09\"\n", both, NULL, -1);
    expect(HEAD AVC "#EXT-X-STREAM-INF:CODECS=\"vp09\"\n" LOW, both, NULL, -1);
    const char *bad[] = {"vp9", "AVC1", "avc1:", ":avc1", "avc1::vp09", "avc1:avc1", "avc1@",
        "avc1@0x1080", "avc1@1920x0", "avc1@-1x1080", "avc1@1920x1080junk", "avc1@999999999999999999999x1"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); i++) {
        hls_codec_t *codecs; size_t count;
        CHECK(!hls_select_parse(bad[i], &codecs, &count));
        CHECK(codecs == NULL && count == 0);
    }
    /* Every truncation boundary of a realistic playlist, under ASan/UBSan. */
    char truncated[] = HEAD UHD LOW AVC VP9;
    hls_codec_t *codecs; size_t count;
    CHECK(hls_select_parse(both, &codecs, &count));
    for (size_t n = 0; n < sizeof(truncated); n++) {
        char copy[sizeof(truncated)];
        memcpy(copy, truncated, n); copy[n] = '\0';
        int result = select_master_playlist_video(copy, codecs, count);
        CHECK(result >= -1 && strlen(copy) <= n);
        if (result < 0) CHECK(!memcmp(copy, truncated, n));
    }
    free(codecs);
    puts("HLS selection tests passed");
    return 0;
}
