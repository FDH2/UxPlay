/*
 * Test of select_yt_web_manifest_language() (lib/airplay_video.c), which selects the
 * audio language of a YouTube web manifest.   Not built or installed by default:
 *
 *     cmake --build [build_dir] --target test_yt_web_manifest
 *     [build_dir]/test_yt_web_manifest
 *
 * With arguments [file] [lang] [lang_system] it instead filters a master playlist in a file,
 * and prints the result ([lang] can be "", for no requested languages):
 *
 *     [build_dir]/test_yt_web_manifest manifest.m3u8 "" en-GB:en
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "../lib/raop.h"
#include "../lib/logger.h"

/* base64-encoded protobufs: {"acont", "original"}, {"lang", "en-US"},  etc.*/
#define XTAGS_EN "YT-EXT-XTAGS=\"ChEKBWFjb250EghvcmlnaW5hbAoNCgRsYW5nEgVlbi1VUw\""
#define XTAGS_FR "YT-EXT-XTAGS=\"ChQKBWFjb250EgtkdWJiZWQtYXV0bwoNCgRsYW5nEgVmci1GUg\""
#define XTAGS_DE "YT-EXT-XTAGS=\"ChQKBWFjb250EgtkdWJiZWQtYXV0bwoNCgRsYW5nEgVkZS1ERQ\""
#define INF "#EXT-X-STREAM-INF:BANDWIDTH="
#define ATTR ",CODECS=\"mp4a.40.2,avc1.64001F\",RESOLUTION="
#define END ",VIDEO-RANGE=SDR,CLOSED-CAPTIONS=NONE\n"
#define URI "https://manifest.example.com/api/manifest/hls_playlist/"

/* two resolutions, each with dubbed French, original English, no language, dubbed German */
static const char yt_web_manifest[] =
    "#EXTM3U\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "3126828" ATTR "1280x720" END
    URI "720/untagged/index.m3u8\n"
    INF "3126759" ATTR "1280x720,"  XTAGS_DE ",YT-EXT-AUDIO-CONTENT-ID=\"de-DE.10\"" END
    URI "720/de-DE/index.m3u8\n"
    INF "636833" ATTR "640x360,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "360/fr-FR/index.m3u8\n"
    INF "636652" ATTR "640x360,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "360/en-US/index.m3u8\n"
    INF "636650" ATTR "640x360" END
    URI "360/untagged/index.m3u8\n"
    INF "636585" ATTR "640x360,"  XTAGS_DE ",YT-EXT-AUDIO-CONTENT-ID=\"de-DE.10\"" END
    URI "360/de-DE/index.m3u8\n";

/* the same, but the original language is not identified (no YT-EXT-XTAGS) */
static const char no_original_manifest[] =
    "#EXTM3U\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    INF "3127011" ATTR "1280x720,YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n";

/* only one language: nothing to remove */
static const char one_language_manifest[] =
    "#EXTM3U\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "636652" ATTR "640x360,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "360/en-US/index.m3u8\n";

/* relative uri's: a filtered copy could not be played from another location */
static const char relative_uri_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    "720/en-US/index.m3u8\n";

/* not a YouTube web manifest (no YT-EXT-AUDIO-CONTENT-ID) */
static const char other_manifest[] =
    "#EXTM3U\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"English\",LANGUAGE=\"en\",URI=\"audio/en.m3u8\"\n"
    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"French\",LANGUAGE=\"fr\",URI=\"audio/fr.m3u8\"\n"
    INF "3126830" ATTR "1280x720,AUDIO=\"audio\"\n"
    "720/index.m3u8\n"
    INF "636652" ATTR "640x360,AUDIO=\"audio\"\n"
    "360/index.m3u8\n";

/* the expected result for yt_web_manifest when the original language (English) is selected */
static const char yt_web_manifest_en[] =
    "#EXTM3U\n"
    "#EXT-X-INDEPENDENT-SEGMENTS\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "3126828" ATTR "1280x720" END
    URI "720/untagged/index.m3u8\n"
    INF "636652" ATTR "640x360,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "360/en-US/index.m3u8\n"
    INF "636650" ATTR "640x360" END
    URI "360/untagged/index.m3u8\n";

/* a variant stream without a uri line: removal of variant streams could remove the wrong uri */
static const char missing_uri_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "636833" ATTR "640x360,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "360/fr-FR/index.m3u8\n";

/* the original audio is only in variant streams without a language */
static const char untagged_original_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126828" ATTR "1280x720" END
    URI "720/untagged/index.m3u8\n";
static const char untagged_original_manifest_fr[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n";
static const char untagged_original_manifest_original[] =
    "#EXTM3U\n"
    INF "3126828" ATTR "1280x720" END
    URI "720/untagged/index.m3u8\n";

/* the original language has two content id's: the original, and an audio description
   (XTAGS_AD is {"acont", "descriptive"}, {"lang", "en-US"}), listed first, with a higher BANDWIDTH */
#define XTAGS_AD "YT-EXT-XTAGS=\"ChQKBWFjb250EgtkZXNjcmlwdGl2ZQoNCgRsYW5nEgVlbi1VUw\""
static const char two_id_manifest[] =
    "#EXTM3U\n"
    INF "3127050" ATTR "1280x720,"  XTAGS_AD ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.2\"" END
    URI "720/en-US-description/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n";
static const char two_id_manifest_en[] =
    "#EXTM3U\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n";

/* no language is marked as the original: both content id's of the selected language are kept */
static const char two_id_no_original_manifest[] =
    "#EXTM3U\n"
    INF "3127050" ATTR "1280x720,"  XTAGS_AD ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.2\"" END
    URI "720/en-US-description/index.m3u8\n"
    INF "3126830" ATTR "1280x720,YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n";
static const char two_id_no_original_manifest_en[] =
    "#EXTM3U\n"
    INF "3127050" ATTR "1280x720,"  XTAGS_AD ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.2\"" END
    URI "720/en-US-description/index.m3u8\n"
    INF "3126830" ATTR "1280x720,YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n";

/* YT-EXT-XTAGS that are not base64, are truncated, or are not the expected protobuf: the original
   language is not identified, but a language can still be selected */
static const char garbage_xtags_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,YT-EXT-XTAGS=\"!!!!\",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,YT-EXT-XTAGS=\"ChEKBWFjb250Egh\",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "3126759" ATTR "1280x720,YT-EXT-XTAGS=\"/////////w\",YT-EXT-AUDIO-CONTENT-ID=\"de-DE.10\"" END
    URI "720/de-DE/index.m3u8\n";
static const char garbage_xtags_manifest_de[] =
    "#EXTM3U\n"
    INF "3126759" ATTR "1280x720,YT-EXT-XTAGS=\"/////////w\",YT-EXT-AUDIO-CONTENT-ID=\"de-DE.10\"" END
    URI "720/de-DE/index.m3u8\n";

/* the last line is a variant stream with an unterminated attribute, and no uri */
static const char unterminated_quote_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "636833" ATTR "640x360,YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10";

/* an unterminated YT-EXT-AUDIO-CONTENT-ID is not a language (the variant stream is like one without it) */
static const char unterminated_id_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8\n"
    INF "636833" ATTR "640x360,YT-EXT-AUDIO-CONTENT-ID=\"de-DE.10\n"
    URI "360/de-DE/index.m3u8\n";
static const char unterminated_id_manifest_fr[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n";

/* no newline at the end of the last line */
static const char no_final_newline_manifest[] =
    "#EXTM3U\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" END
    URI "720/fr-FR/index.m3u8\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8";
static const char no_final_newline_manifest_en[] =
    "#EXTM3U\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" END
    URI "720/en-US/index.m3u8";

/* CRLF line endings, and a blank line */
#define CRLF_END ",VIDEO-RANGE=SDR,CLOSED-CAPTIONS=NONE\r\n"
static const char crlf_manifest[] =
    "#EXTM3U\r\n"
    "\r\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" CRLF_END
    URI "720/fr-FR/index.m3u8\r\n"
    INF "3126830" ATTR "1280x720,"  XTAGS_EN ",YT-EXT-AUDIO-CONTENT-ID=\"en-US.4\"" CRLF_END
    URI "720/en-US/index.m3u8\r\n"
    INF "3126828" ATTR "1280x720" CRLF_END
    URI "720/untagged/index.m3u8\r\n";
static const char crlf_manifest_fr[] =
    "#EXTM3U\r\n"
    "\r\n"
    INF "3127011" ATTR "1280x720,"  XTAGS_FR ",YT-EXT-AUDIO-CONTENT-ID=\"fr-FR.10\"" CRLF_END
    URI "720/fr-FR/index.m3u8\r\n";

static logger_t *logger = NULL;
static int failures = 0;

static void log_callback(void *cls, int level, const char *msg) {
    fprintf(stderr, "    %s\n", msg);
}

static int count(const char *str, const char *substr) {
    int n = 0;
    while (str && (str = strstr(str, substr))) {
        str += strlen(substr);
        n++;
    }
    return n;
}

/* expected numbers of variant streams of each kind; all 0 if the playlist should be left unchanged */
static void check(const char *name, const char *playlist, const char *lang, const char *lang_system,
                  int n_en, int n_fr, int n_de, int n_untagged) {
    char *filtered = select_yt_web_manifest_language(playlist, lang, lang_system, logger);
    bool unchanged = !(n_en + n_fr + n_de + n_untagged);
    bool ok = false;
    if (!filtered) {
        ok = unchanged;
    } else if (!unchanged) {
        ok = (count(filtered, "CONTENT-ID=\"en-US.4\"") == n_en && count(filtered, "/en-US/") == n_en &&
              count(filtered, "CONTENT-ID=\"fr-FR.10\"") == n_fr && count(filtered, "/fr-FR/") == n_fr &&
              count(filtered, "CONTENT-ID=\"de-DE.10\"") == n_de && count(filtered, "/de-DE/") == n_de &&
              count(filtered, "/untagged/") == n_untagged &&
              count(filtered, "#EXT-X-STREAM-INF:") == n_en + n_fr + n_de + n_untagged &&
              count(filtered, "https://") == n_en + n_fr + n_de + n_untagged &&
              count(filtered, "#EXTM3U\n#EXT-X-INDEPENDENT-SEGMENTS\n") == 1 &&
              filtered[strlen(filtered) - 1] == '\n');
    }
    printf("%s: %s (-lang %s, system %s)\n", ok ? "PASS" : "FAIL", name, lang ? lang : "[none]", lang_system);
    if (!ok) {
        failures++;
        printf("%s\n", filtered ? filtered : "[unchanged]");
    }
    free(filtered);
}

/* expected is the exact result, or NULL if the playlist should be left unchanged */
static void check_exact(const char *name, const char *playlist, const char *lang, const char *lang_system,
                        const char *expected) {
    char *filtered = select_yt_web_manifest_language(playlist, lang, lang_system, logger);
    bool ok = (filtered && expected ? !strcmp(filtered, expected) : !filtered && !expected);
    printf("%s: %s (-lang %s, system %s)\n", ok ? "PASS" : "FAIL", name, lang ? lang : "[none]", lang_system);
    if (!ok) {
        failures++;
        printf("%s\n", filtered ? filtered : "[unchanged]");
    }
    free(filtered);
}

/* a playlist with n languages "l1", "l2", ... (the first one is the original) */
static char *many_languages_manifest(int n) {
    char *playlist = (char *) calloc(n * 512 + 16, sizeof(char));
    char *ptr = playlist + sprintf(playlist, "#EXTM3U\n");
    for (int i = 1; i <= n; i++) {
        ptr += sprintf(ptr, INF "%d" ATTR "1280x720,%sYT-EXT-AUDIO-CONTENT-ID=\"l%d.1\"" END URI "720/l%d/index.m3u8\n",
                       3000000 + i, i == 1 ? XTAGS_EN "," : "", i, i);
    }
    return playlist;
}

static int filter_file(const char *filename, const char *lang, const char *lang_system) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", filename);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    char *playlist = (char *) calloc(len + 1, sizeof(char));
    if (fread(playlist, 1, len, fp) != (size_t) len) {
        fprintf(stderr, "cannot read %s\n", filename);
        return 1;
    }
    fclose(fp);
    char *filtered = select_yt_web_manifest_language(playlist, strlen(lang) ? lang : NULL, lang_system, logger);
    fprintf(stderr, "variant streams: %d of %d kept%s\n", count(filtered ? filtered : playlist, "#EXT-X-STREAM-INF:"),
            count(playlist, "#EXT-X-STREAM-INF:"), filtered ? "" : " (unchanged)");
    printf("%s", filtered ? filtered : playlist);
    free(filtered);
    free(playlist);
    return 0;
}

int main(int argc, char *argv[]) {
    logger = logger_init();
    logger_set_level(logger, LOGGER_INFO);
    logger_set_callback(logger, log_callback, NULL);
    if (argc == 4) {
        return filter_file(argv[1], argv[2], argv[3]);
    }

    /* system languages: untagged variant streams are kept with the original language */
    check("system English", yt_web_manifest, NULL, "en_GB:en", 2, 0, 0, 2);
    check("system English (regional variant)", yt_web_manifest, NULL, "en-GB", 2, 0, 0, 2);
    check("system French", yt_web_manifest, NULL, "fr-CA", 0, 2, 0, 0);
    check("system language not available", yt_web_manifest, NULL, "ja-JP", 2, 0, 0, 2);
    check("system language C", yt_web_manifest, NULL, "C", 2, 0, 0, 2);
    /* requested languages (option -lang) have priority over system languages */
    check("requested French", yt_web_manifest, "fr", "en_GB:en", 0, 2, 0, 0);
    check("requested French (case, underscore)", yt_web_manifest, "FR_fr", "en", 0, 2, 0, 0);
    check("second requested language", yt_web_manifest, "ja:de:fr", "en", 0, 0, 2, 0);
    check("requested languages not available", yt_web_manifest, "ja:ko", "zh-CN", 2, 0, 0, 2);
    /* requested languages need a strict match: "fr-CA" does not match "fr-FR" */
    check("requested regional variant not available", yt_web_manifest, "fr-CA", "de", 0, 0, 2, 0);
    /* cases where the playlist is left unchanged */
    check("no YouTube attributes", other_manifest, "fr", "en", 0, 0, 0, 0);
    check("only one language", one_language_manifest, "fr", "en", 0, 0, 0, 0);
    check("no match, original not identified", no_original_manifest, "ja", "de", 0, 0, 0, 0);
    check("relative uri's", relative_uri_manifest, "fr", "en", 0, 0, 0, 0);
    /* a match does not need the original to be identified */
    check("match, original not identified", no_original_manifest, "fr", "en", 0, 1, 0, 0);

    /* nothing but the unwanted variant streams is removed */
    check_exact("exact result", yt_web_manifest, NULL, "en_GB:en", yt_web_manifest_en);
    check_exact("no final newline", no_final_newline_manifest, NULL, "en", no_final_newline_manifest_en);
    check_exact("CRLF line endings", crlf_manifest, "fr", "en", crlf_manifest_fr);
    /* a variant stream without a uri: the playlist is left unchanged */
    check_exact("variant stream without a uri", missing_uri_manifest, NULL, "en", NULL);
    check_exact("unterminated quote in last line", unterminated_quote_manifest, NULL, "en", NULL);
    check_exact("unterminated language", unterminated_id_manifest, "fr", "en", unterminated_id_manifest_fr);
    /* variant streams without a language are the original, if nothing else is */
    check_exact("original has no language", untagged_original_manifest, NULL, "en",
                untagged_original_manifest_original);
    check_exact("original has no language, match", untagged_original_manifest, NULL, "fr-CA",
                untagged_original_manifest_fr);
    /* the original language also has an audio description */
    check_exact("two content id's, original language", two_id_manifest, NULL, "en", two_id_manifest_en);
    check_exact("two content id's, fallback to original", two_id_manifest, "ja", "de", two_id_manifest_en);
    check_exact("two content id's, original not identified", two_id_no_original_manifest, NULL, "en",
                two_id_no_original_manifest_en);
    /* invalid YT-EXT-XTAGS */
    check_exact("garbage XTAGS, match", garbage_xtags_manifest, "de", "en", garbage_xtags_manifest_de);
    check_exact("garbage XTAGS, no match", garbage_xtags_manifest, "ja", "zh", NULL);
    /* the number of languages is limited (to 64): with more, the playlist is left unchanged */
    char *many_languages = many_languages_manifest(64);
    char *one_language = many_languages_manifest(1);
    check_exact("64 languages", many_languages, "l1", "en", one_language);
    free(many_languages);
    many_languages = many_languages_manifest(65);
    check_exact("65 languages", many_languages, "l1", "en", NULL);
    free(many_languages);
    free(one_language);

    logger_destroy(logger);
    printf("%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
