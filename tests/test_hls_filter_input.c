#include "hls_filter.h"
#include <string.h>

#define CHECK(x) do { if (!(x)) g_error("line %d: %s", __LINE__, #x); } while (0)
#define HEADER "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",URI=\"../audio.m3u8?sig=a,b\"\n"
#define LOW "#EXT-X-STREAM-INF:CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=1280x720\nlow.m3u8?sig=low\n"
#define HD "#EXT-X-STREAM-INF:CODECS=\"hvc1.2.4.L123.90,mp4a.40.2\",RESOLUTION=1920x1080\n../hd.m3u8?sig=hd\n"
#define UHD "#EXT-X-STREAM-INF:CODECS=\"avc1.640034,mp4a.40.2\",RESOLUTION=3840x2160\n4k.m3u8\n"
#define POLICY "AVC:1080,1080;HEVC:1080,1080"

typedef struct {
    GstElement *parent;
    GstPad *src, *sink;
    GString *received;
    guint eos;
} fixture_t;

static GstFlowReturn receive(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    fixture_t *f = gst_pad_get_element_private(pad);
    GstMapInfo map;
    CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ));
    g_string_append_len(f->received, (const char *)map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
}

static gboolean event(GstPad *pad, GstObject *parent, GstEvent *event) {
    fixture_t *f = gst_pad_get_element_private(pad);
    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) f->eos++;
    gst_event_unref(event);
    return TRUE;
}

static void start(fixture_t *f) {
    CHECK(gst_pad_push_event(f->src, gst_event_new_stream_start("manifest")));
    GstCaps *caps = gst_caps_new_empty_simple("application/x-hls");
    CHECK(gst_pad_push_event(f->src, gst_event_new_caps(caps)));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_BYTES);
    CHECK(gst_pad_push_event(f->src, gst_event_new_segment(&segment)));
}

static void init_device(fixture_t *f, device_profile_t device, const char *policy) {
    memset(f, 0, sizeof(*f));
    f->received = g_string_new(NULL);
    f->parent = gst_pipeline_new(NULL);
    f->src = gst_pad_new("src", GST_PAD_SRC);
    f->sink = gst_pad_new("sink", GST_PAD_SINK);
    gst_pad_set_element_private(f->sink, f);
    gst_pad_set_chain_function(f->sink, receive);
    gst_pad_set_event_function(f->sink, event);
    CHECK(gst_element_add_pad(f->parent, f->sink));
    CHECK(gst_pad_set_active(f->src, TRUE));
    CHECK(gst_pad_set_active(f->sink, TRUE));
    CHECK(gst_pad_link(f->src, f->sink) == GST_PAD_LINK_OK);
    char *copy = g_strdup(policy);
    hls_filter_attach(f->sink, device, false, false, copy, NULL);
    g_free(copy); /* the probe must own its settings */
    start(f);
}

static void init(fixture_t *f, const char *policy) {
    init_device(f, *policy ? CUSTOM : DESKTOP, policy);
}

static void finish(fixture_t *f) {
    gst_pad_set_active(f->src, FALSE);
    gst_pad_set_active(f->sink, FALSE);
    gst_pad_unlink(f->src, f->sink);
    gst_object_unref(f->src);
    gst_object_unref(f->parent);
    g_string_free(f->received, TRUE);
}

static GstBuffer *buffer(const char *text, gsize len) {
    GstBuffer *b = gst_buffer_new_allocate(NULL, len, NULL);
    CHECK(gst_buffer_fill(b, 0, text, len) == len);
    return b;
}

static void send(fixture_t *f, const char *text, gsize chunk) {
    for (gsize i = 0, len = strlen(text); i < len; i += chunk)
        CHECK(gst_pad_push(f->src, buffer(text + i, MIN(chunk, len - i))) == GST_FLOW_OK);
}

static void check_error(fixture_t *f) {
    GstBus *bus = gst_element_get_bus(f->parent);
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    CHECK(message != NULL);
    gst_message_unref(message);
    gst_object_unref(bus);
    CHECK(f->received->len == 0 && f->eos == 1);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);
    fixture_t f;
    /* Network chunk boundaries, including inside tags and UTF-8 comments. */
    for (gsize chunk = 1; chunk <= strlen(HEADER LOW HD UHD); chunk++) {
        init(&f, POLICY);
        send(&f, HEADER LOW HD UHD, chunk);
        CHECK(f.received->len == 0);
        CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
        CHECK(!strcmp(f.received->str, HEADER LOW HD) && f.eos == 1);
        finish(&f);
    }
    init(&f, POLICY);
    CHECK(gst_pad_push(f.src, gst_buffer_new()) == GST_FLOW_OK);
    GstBufferList *list = gst_buffer_list_new();
    gst_buffer_list_add(list, buffer(HEADER LOW, strlen(HEADER LOW)));
    gst_buffer_list_add(list, buffer(HD UHD, strlen(HD UHD)));
    CHECK(gst_pad_push_list(f.src, list) == GST_FLOW_OK);
    CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
    CHECK(!strcmp(f.received->str, HEADER LOW HD));
    finish(&f);

    /* A preset profile goes through the same filter: a Pi 5 without hardware decoding keeps software
     * H.264 up to 1080p and HEVC up to 1080p at 30 fps. */
    init_device(&f, PI_5, "");
    send(&f, HEADER LOW HD UHD, 9);
    CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
    CHECK(!strcmp(f.received->str, HEADER LOW HD));
    finish(&f);

    /* Media playlists have no variant alternatives and must remain intact. */
    const char *media = "#EXTM3U\r\n#EXT-X-TARGETDURATION:2\r\n#EXTINF:2,\r\nsegment.ts?sig=1\r\n#EXT-X-ENDLIST\r\n";
    init(&f, POLICY);
    send(&f, media, 7);
    CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
    CHECK(!strcmp(f.received->str, media));
    finish(&f);

    init(&f, "");
    send(&f, HEADER UHD, 3);
    CHECK(!strcmp(f.received->str, HEADER UHD)); /* DESKTOP: no probe, no buffering */
    finish(&f);

    const char *bad[] = {HEADER UHD, HEADER "#EXT-X-STREAM-INF:CODECS=\"hvc1\"\n"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        init(&f, POLICY);
        send(&f, bad[i], 11);
        gst_pad_push_event(f.src, gst_event_new_eos());
        check_error(&f);
        finish(&f);
    }
    init(&f, POLICY);
    CHECK(gst_pad_push(f.src, buffer(HEADER "\0" HD, sizeof(HEADER "\0" HD) - 1)) == GST_FLOW_OK);
    gst_pad_push_event(f.src, gst_event_new_eos());
    check_error(&f);
    finish(&f);

    init(&f, POLICY);
    GstBuffer *large = gst_buffer_new_allocate(NULL, 4 * 1024 * 1024 + 1, NULL);
    CHECK(gst_pad_push(f.src, large) == GST_FLOW_OK);
    gst_pad_push_event(f.src, gst_event_new_eos());
    check_error(&f);
    /* Flush removes both partial bytes and the failed state. */
    CHECK(gst_pad_push_event(f.src, gst_event_new_flush_start()));
    CHECK(gst_pad_push_event(f.src, gst_event_new_flush_stop(TRUE)));
    start(&f);
    send(&f, HEADER HD, 5);
    CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
    CHECK(!strcmp(f.received->str, HEADER HD));
    finish(&f);

    init(&f, POLICY);
    send(&f, HEADER LOW, 2);
    CHECK(gst_pad_push_event(f.src, gst_event_new_flush_start()));
    CHECK(gst_pad_push_event(f.src, gst_event_new_flush_stop(TRUE)));
    start(&f);
    send(&f, HEADER HD, 5);
    CHECK(gst_pad_push_event(f.src, gst_event_new_eos()));
    CHECK(!strcmp(f.received->str, HEADER HD));
    finish(&f);

    init(&f, POLICY);
    send(&f, "#EXTM3U\n#EXT-X-STREAM", 1);
    finish(&f); /* cancellation mid-download */
    g_print("HLS filter input tests passed\n");
    return 0;
}
