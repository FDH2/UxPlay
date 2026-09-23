#include "hls_filter.h"
#include <string.h>

/* Invoked by test_hls_http.py against its loopback HTTP server. */
static guint video_buffers, audio_buffers;

static void log_message(void *data, int level, const char *message) {
    g_print("%s\n", message);
}

static void video_handoff(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
    video_buffers++;
}

static void audio_handoff(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data) {
    audio_buffers++;
}

static void source_setup(GstElement *playbin, GstElement *source, gpointer data) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "user-agent"))
        g_object_set(source, "user-agent", "UxPlay-HLS-test", NULL);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    if (argc != 4) return 2;
    /* These optional runtime plugins are not required for a compile-only build. */
    const char *required[] = {"playbin3", "hlsdemux2", "souphttpsrc", "avdec_h264", "avdec_h265", "avdec_aac"};
    for (guint i = 0; i < G_N_ELEMENTS(required); i++) {
        GstElementFactory *factory = gst_element_factory_find(required[i]);
        if (!factory) {
            g_printerr("Missing test plugin: %s\n", required[i]);
            return 77;
        }
        /* playbin3 does not consistently honor FORCE_SW_DECODERS. Prefer
         * libav explicitly in this test process so a hardware decoder cannot
         * negotiate DMA-BUF/VideoMeta with our deliberately plain fakesink. */
        if (g_str_has_prefix(required[i], "avdec_"))
            gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 100);
        gst_object_unref(factory);
    }
    logger_t *logger = logger_init();
    logger_set_callback(logger, log_message, NULL);
    logger_set_level(logger, LOGGER_INFO);
    GstElement *playbin = gst_element_factory_make("playbin3", NULL);
    GstElement *video = gst_element_factory_make("fakesink", NULL);
    GstElement *audio = gst_element_factory_make("fakesink", NULL);
    g_object_set(video, "sync", FALSE, "signal-handoffs", TRUE, NULL);
    g_object_set(audio, "sync", FALSE, "signal-handoffs", TRUE, NULL);
    g_signal_connect(video, "handoff", G_CALLBACK(video_handoff), NULL);
    g_signal_connect(audio, "handoff", G_CALLBACK(audio_handoff), NULL);
    g_signal_connect(playbin, "source-setup", G_CALLBACK(source_setup), NULL);
    /* argv[2]: a -custom profile string; empty means DESKTOP, which installs nothing */
    hls_filter_install(playbin, *argv[2] ? CUSTOM : DESKTOP, false, false, argv[2], logger);
    g_object_set(playbin, "video-sink", video, "audio-sink", audio, NULL);
    /* Demand the highest surviving variant from startup, even for a short clip.
     * Decode the tiny synthetic frames in software regardless of host hardware. */
    g_object_set(playbin, "connection-speed", (guint64)100000, NULL);
    GstBus *bus = gst_element_get_bus(playbin);
    int result = 0;
    /* Reuse one playbin for two URI loads: element-setup and policy lifetimes
     * must survive teardown without keeping the previous manifest. */
    for (int run = 0; run < 2; run++) {
        video_buffers = audio_buffers = 0;
        g_object_set(playbin, "uri", argv[1], NULL);
        gst_element_set_state(playbin, GST_STATE_PLAYING);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        gboolean expect_error = !strcmp(argv[3], "error");
        if (!msg) {
            g_printerr("Playback timed out\n");
            result = 1;
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &error, &debug);
            if (!expect_error || !strstr(error->message, "HLS filter")) {
                g_printerr("%s: %s\n", error->message, debug ? debug : "");
                result = 1;
            }
            g_clear_error(&error);
            g_free(debug);
        } else if (expect_error || !video_buffers || (!audio_buffers && strcmp(argv[3], "video-only"))) {
            g_printerr("Unexpected EOS: video=%u audio=%u\n", video_buffers, audio_buffers);
            result = 1;
        }
        if (msg) gst_message_unref(msg);
        g_print("Session %d: stopping after %u video / %u audio buffers\n", run + 1, video_buffers, audio_buffers);
        gst_element_set_state(playbin, GST_STATE_NULL);
        g_print("Session %d: stopped\n", run + 1);
        if (result) break;
    }
    gst_object_unref(bus);
    gst_object_unref(playbin);
    logger_destroy(logger);
    return result;
}
