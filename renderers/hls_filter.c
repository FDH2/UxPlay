/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hls_filter.h"
#include <stdlib.h>
#include <string.h>

/* A master is small, but its HTTP response may arrive in many buffers. Bound
 * accumulation independently of Content-Length (which may be absent). */
#define MAX_MANIFEST_SIZE (4 * 1024 * 1024)

typedef struct {
    device_profile_t device;
    bool hw_avc;
    bool hw_hevc;
    char *custom_profile;
    logger_t *logger;
    GByteArray *manifest;
    gboolean forwarding;
    gboolean failed;
} hls_filter_t;

static hls_filter_t *filter_new(device_profile_t device, bool hw_avc, bool hw_hevc,
                                const char *custom_profile, logger_t *logger) {
    hls_filter_t *f = g_new0(hls_filter_t, 1);
    f->device = device;
    f->hw_avc = hw_avc;
    f->hw_hevc = hw_hevc;
    f->custom_profile = g_strdup(custom_profile);
    f->logger = logger;
    f->manifest = g_byte_array_new();
    return f;
}

static void filter_free(gpointer data) {
    hls_filter_t *f = data;
    g_byte_array_unref(f->manifest);
    g_free(f->custom_profile);
    g_free(f);
}

static void filter_error(GstPad *pad, hls_filter_t *f, const char *reason) {
    if (f->failed) return;
    f->failed = TRUE;
    g_byte_array_set_size(f->manifest, 0);
    GstElement *element = gst_pad_get_parent_element(pad);
    if (element) {
        GST_ELEMENT_ERROR(element, STREAM, DEMUX, ("HLS filter: %s", reason), (NULL));
        gst_object_unref(element);
    }
}

static void collect_buffer(GstPad *pad, hls_filter_t *f, GstBuffer *buffer) {
    if (f->failed) return;
    gsize size = gst_buffer_get_size(buffer);
    if (!size) return;
    if (size > MAX_MANIFEST_SIZE - f->manifest->len) {
        filter_error(pad, f, "manifest exceeds 4 MiB");
        return;
    }
    guint offset = f->manifest->len;
    g_byte_array_set_size(f->manifest, offset + size);
    if (gst_buffer_extract(buffer, 0, f->manifest->data + offset, size) != size)
        filter_error(pad, f, "could not read manifest buffer");
}

/* Lines that start with prefix: 0 for a media playlist, which offers no variants to choose between. */
static int count_lines(const char *text, const char *prefix) {
    int count = 0;
    for (const char *line = text; line && *line; ) {
        if (g_str_has_prefix(line, prefix)) count++;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return count;
}

/* At EOS: deliver the complete input to the demuxer, filtered if it is a master playlist. After an error
 * the EOS still passes, so the demuxer's empty input ends while the application handles the bus error. */
static GstPadProbeReturn deliver(GstPad *pad, hls_filter_t *f) {
    gsize len = f->manifest->len;
    if (memchr(f->manifest->data, '\0', len)) {
        filter_error(pad, f, "manifest contains a NUL byte");
        return GST_PAD_PROBE_OK;
    }
    /* filter_master_playlist() takes a malloc'd playlist that ends in '\n', frees it and hands back a new one. */
    char *text = malloc(len + 2);
    if (!text) {
        filter_error(pad, f, "out of memory");
        return GST_PAD_PROBE_OK;
    }
    memcpy(text, f->manifest->data, len);
    text[len] = '\0';
    g_byte_array_set_size(f->manifest, 0);
    int variants = count_lines(text, "#EXT-X-STREAM-INF:");
    if (variants) {
        if (text[len - 1] != '\n') {
            text[len] = '\n';
            text[len + 1] = '\0';
        }
        if (!filter_master_playlist(&text, f->device, f->hw_avc, f->hw_hevc, f->custom_profile)) {
            free(text);
            filter_error(pad, f, "could not filter master playlist");
            return GST_PAD_PROBE_OK;
        }
        int kept = count_lines(text, "#EXT-X-STREAM-INF:");
        if (!kept) {
            free(text);
            filter_error(pad, f, "no variant in the master playlist is within this device profile's limits");
            return GST_PAD_PROBE_OK;
        }
        if (kept < variants && f->logger)
            logger_log(f->logger, LOGGER_INFO, "HLS filter removed %d of %d variant(s) from a master playlist "
                       "fetched by GStreamer", variants - kept, variants);
    }
    gsize size = strlen(text);
    GstBuffer *buffer = gst_buffer_new_wrapped_full(0, text, size, 0, size, text, free);
    f->forwarding = TRUE;
    GstFlowReturn flow = gst_pad_chain(pad, buffer);
    f->forwarding = FALSE;
    if (flow != GST_FLOW_OK) {
        if (flow != GST_FLOW_FLUSHING)
            filter_error(pad, f, "could not deliver filtered manifest");
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn manifest_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    hls_filter_t *f = data;
    /* Re-enter the sink chain once at EOS with the complete, filtered input. */
    if (f->forwarding) return GST_PAD_PROBE_OK;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        collect_buffer(pad, f, GST_PAD_PROBE_INFO_BUFFER(info));
        return GST_PAD_PROBE_DROP;
    }
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        for (guint i = 0; i < gst_buffer_list_length(list); i++)
            collect_buffer(pad, f, gst_buffer_list_get(list, i));
        return GST_PAD_PROBE_DROP;
    }
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_STREAM_START:
    case GST_EVENT_FLUSH_STOP:
        g_byte_array_set_size(f->manifest, 0);
        f->failed = FALSE;
        break;
    case GST_EVENT_EOS:
        /* Even on failure, finish the empty demuxer input: swallowing EOS can leave startup pending while
         * the application handles the bus error. No unfiltered bytes have reached the demuxer. */
        if (f->failed || !f->manifest->len) return GST_PAD_PROBE_OK;
        return deliver(pad, f);
    default:
        break;
    }
    return GST_PAD_PROBE_OK;
}

void hls_filter_attach(GstPad *sink, device_profile_t device, bool hw_avc, bool hw_hevc,
                       const char *custom_profile, logger_t *logger) {
    if (device == DESKTOP) return;
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST |
                      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
                      manifest_probe, filter_new(device, hw_avc, hw_hevc, custom_profile, logger), filter_free);
}

static void element_setup(GstElement *playbin, GstElement *element, gpointer data) {
    (void)playbin;
    hls_filter_t *f = data;
    GstElementFactory *factory = gst_element_get_factory(element);
    if (!factory) return;
    const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (strcmp(name, "hlsdemux2") && strcmp(name, "hlsdemux")) return;
    GstPad *sink = gst_element_get_static_pad(element, "sink");
    if (sink) {
        hls_filter_attach(sink, f->device, f->hw_avc, f->hw_hevc, f->custom_profile, f->logger);
        gst_object_unref(sink);
    }
}

static void closure_free(gpointer data, GClosure *closure) {
    (void)closure;
    filter_free(data);
}

void hls_filter_install(GstElement *playbin, device_profile_t device, bool hw_avc, bool hw_hevc,
                        const char *custom_profile, logger_t *logger) {
    if (device == DESKTOP) return;
    /* Keep HTTP fetching, URI/redirect queries, cookies, and rendition refreshes in GStreamer. Only the
     * initial HLS manifest bytes are changed; relative and signed URLs retain their original base.
     * Non-HLS sources are untouched. Each demuxer owns a copy of the settings, including after a URI
     * change. A master playlist uxplay serves itself (the YouTube handoff) is already filtered, and
     * filtering it again keeps the same variants. */
    g_signal_connect_data(playbin, "element-setup", G_CALLBACK(element_setup),
                          filter_new(device, hw_avc, hw_hevc, custom_profile, logger), closure_free, 0);
}
