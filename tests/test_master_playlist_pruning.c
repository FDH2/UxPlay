#include "raop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define AUDIO_EN "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"en\",URI=\"en.m3u8\",DEFAULT=YES,AUTOSELECT=YES\n"
#define AUDIO_DE "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",LANGUAGE=\"de\",URI=\"de.m3u8\""
#define SUBS_DE "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",LANGUAGE=\"de\",URI=\"subs.m3u8\""
#define YES ",DEFAULT=YES,AUTOSELECT=YES\n"
#define AVC "#EXT-X-STREAM-INF:CODECS=\"avc1\",RESOLUTION=1920x1080\navc.m3u8\n"
#define VP9 "#EXT-X-STREAM-INF:CODECS=\"vp09\",RESOLUTION=1920x1080\nvp9.m3u8\n"

static void expect_pruning(const char *input, const char *language_result,
                           const char *video_result, int removed) {
    hls_codec_t *codecs;
    size_t count;
    CHECK(hls_select_parse("avc1@1920x1080:vp09@1920x1080", &codecs, &count));
    /* Initialization only stores this opaque pointer; these tests do not use RAOP. */
    airplay_video_t *video = airplay_video_init((raop_t *) &count, 7000, "de", "de", "en");
    CHECK(video != NULL);
    char *playlist = malloc(strlen(input) + 1);
    CHECK(playlist != NULL);
    strcpy(playlist, input);
    playlist = select_master_playlist_language(video, playlist);
    CHECK(!strcmp(playlist, language_result));
    CHECK(select_master_playlist_video(playlist, codecs, count) == removed);
    CHECK(!strcmp(playlist, video_result));
    free(playlist);
    airplay_video_destroy(video);
    free(codecs);
}

int main(void) {
    /* Both callers use the same pruning code: LANGUAGE grows attributes and
     * deletes a rendition; video selection then compacts complete variants. */
    expect_pruning("#EXTM3U\n" AUDIO_EN AUDIO_DE "\n" SUBS_DE "\n" VP9 AVC,
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES VP9 AVC,
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES AVC, 1);
    /* A rejected video selection must preserve the LANGUAGE-pruned buffer. */
    expect_pruning("#EXTM3U\n" AUDIO_EN AUDIO_DE "\n" SUBS_DE "\n"
                   "#EXT-X-STREAM-INF:CODECS=\"vp09\",RESOLUTION=3840x2160\nuhd.m3u8\n",
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES
                   "#EXT-X-STREAM-INF:CODECS=\"vp09\",RESOLUTION=3840x2160\nuhd.m3u8\n",
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES
                   "#EXT-X-STREAM-INF:CODECS=\"vp09\",RESOLUTION=3840x2160\nuhd.m3u8\n", -1);
    /* Already selected languages and video require no rewrite. */
    expect_pruning("#EXTM3U\n" AUDIO_DE YES SUBS_DE YES AVC,
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES AVC,
                   "#EXTM3U\n" AUDIO_DE YES SUBS_DE YES AVC, 0);
    puts("Master playlist pruning tests passed");
    return 0;
}
