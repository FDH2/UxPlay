#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include "mirror_renderer.h"

struct mirror_renderer_s {
    gboolean active, is_audio;
    GstElement *pipeline, *appsrc_video, *appsrc_audio, *textsrc, *volume;
    GstBus *bus;
    const char *codec;
};

static logger_t *logger = NULL;
static char h264[] = "h264";
static char h265[] = "h265";
static char jpeg[] = "jpeg";
static gboolean fix_rgb;
static gboolean show_coverart;
static gboolean render_video = FALSE;
static gboolean render_audio = FALSE;
static gboolean first_packet = TRUE;


static mirror_renderer_t *mirror_renderer_h264 = NULL;
static mirror_renderer_t *mirror_renderer_h265 = NULL;
static mirror_renderer_t *mirror_renderer_alac = NULL;
static mirror_renderer_t *mirror_renderer = NULL;

static GstVideoOrientationMethod video_orientation = GST_VIDEO_ORIENTATION_IDENTITY;


static gboolean check_plugins (void)
{
    GstRegistry *registry = NULL;
    const gchar *needed[] = { "app", "libav", "playback", "autodetect", "videoparsersbad",  NULL};
    const gchar *gst[] = {"plugins-base", "libav", "plugins-base", "plugins-good", "plugins-bad", NULL};
    registry = gst_registry_get ();
    gboolean ret = TRUE;
    for (int i = 0; i < g_strv_length ((gchar **) needed); i++) {
        GstPlugin *plugin = NULL;
        plugin = gst_registry_find_plugin (registry, needed[i]);
        if (!plugin) {
            g_print ("Required gstreamer plugin '%s' not found\n"
                     "Missing plugin is contained in  '[GStreamer 1.x]-%s'\n",needed[i], gst[i]);
            ret = FALSE;
            continue;
        }
        gst_object_unref (plugin);
        plugin = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}

static gboolean check_plugin_feature (const gchar *needed_feature)
{
    GstPluginFeature *plugin_feature = NULL;
    GstRegistry *registry = gst_registry_get ();
    gboolean ret = TRUE;

    plugin_feature = gst_registry_find_feature (registry, needed_feature, GST_TYPE_ELEMENT_FACTORY);
    if (!plugin_feature) {
        g_print ("Required gstreamer libav plugin feature '%s' not found:\n\n"
                 "This may be missing because the FFmpeg package used by GStreamer-1.x-libav is incomplete.\n"
                 "(Some distributions provide an incomplete FFmpeg due to License or Patent issues:\n"
                 "in such cases a complete version for that distribution is usually made available elsewhere)\n",
                 needed_feature);
        ret = FALSE;
    } else {
        gst_object_unref (plugin_feature);
        plugin_feature = NULL;
    }
    if (ret == FALSE) {
        g_print ("\nif the plugin feature is installed, but not found, your gstreamer registry may have been corrupted.\n"
                 "to rebuild it when gstreamer next starts, clear your gstreamer cache with:\n"
                 "\"rm -rf ~/.cache/gstreamer-1.0\"\n\n");
    }
    return ret;
}


bool gstreamer_init(){
    gst_init(NULL,NULL);
    return (bool) check_plugins ();
}





void destroy_mirror_renderer(mirror_renderer_t *renderer) {
    GstState state;
    if (renderer) {
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (state != GST_STATE_NULL) {
            if (renderer->appsrc_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc_video));
            }
            if (renderer->appsrc_audio) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc_audio));
            }
            gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
        }
        gst_object_unref(renderer->bus);
	gst_object_unref(renderer->textsrc);
        gst_object_unref(renderer->pipeline);
        free(renderer);
	renderer = NULL;
      }
}

void reset_mirror_renderers() {
    if (mirror_renderer_h264 && mirror_renderer_h264->active) {
        destroy_mirror_renderer(mirror_renderer_h264);
        mirror_renderer_h264 = create_mirror_renderer(FALSE, FALSE);
    }
    if (mirror_renderer_h265 && mirror_renderer_h265->active) {
        destroy_mirror_renderer(mirror_renderer_h265);
        mirror_renderer_h265 = create_mirror_renderer(TRUE, FALSE);
    }
    if (mirror_renderer_alac && mirror_renderer_alac->active) {
        destroy_mirror_renderer(mirror_renderer_alac);
        mirror_renderer_alac = create_mirror_renderer(FALSE, TRUE);
    }
}




/* Callback to link tsdemux dynamic pads to decoders */
static void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    GstElement *video_queue = GST_ELEMENT(g_object_get_data(G_OBJECT(data), "v_queue"));
    GstElement *audio_queue = GST_ELEMENT(g_object_get_data(G_OBJECT(data), "a_queue"));
    GstPad *sink_pad;

    gchar *name = gst_pad_get_name(pad);
    if (g_str_has_prefix(name, "video")) {
        sink_pad = gst_element_get_static_pad(video_queue, "sink");
    } else if (g_str_has_prefix(name, "audio")) {
        sink_pad = gst_element_get_static_pad(audio_queue, "sink");
    } else {
        g_free(name);
        return;
    }

    gst_pad_link(pad, sink_pad);
    gst_object_unref(sink_pad);
    g_free(name);
}

mirror_renderer_t * create_mirror_renderer(gboolean is_h265, gboolean is_audio) {

    const char aac_eld_caps[] ="audio/mpeg,mpegversion=(int)4,channnels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";
    const char h264_caps[] ="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
    const char h265_caps[] ="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";
    const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channnels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                           "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";
    const char jpeg_caps[]="image/jpeg";
    const char rgb_caps[] = "video/x-raw,colorimetry=sRGB,format=RGB";

    mirror_renderer_t *renderer  = (mirror_renderer_t *) malloc(sizeof(mirror_renderer_t));
    if (renderer) {
      *renderer = (mirror_renderer_t) {0};  //initialize all members of struct to zero of their type
    } else {
      g_printerr("failed to create renderer\n");
      exit(1);
    }
    mirror_renderer->is_audio = is_audio;
    
    GstElement *pipeline = gst_pipeline_new("mirror_pipeline");
    if (pipeline) {
      renderer->pipeline = pipeline;
      renderer->bus = gst_element_get_bus(pipeline);
    } else {
      g_printerr("failed to create pipeline\n");
      exit(1);
    }

    gboolean is_h264 = (!is_h265 && !is_audio);
    if (!is_audio) {
        renderer->codec = (is_h265 ? h265 : h264);
    } else if (show_coverart) {
        renderer->codec = jpeg;
    }

    GstElement *a_queue1 = NULL, *v_queue1 = NULL, *adec = NULL, *vdec = NULL;

    if (render_audio) {
        // audio input sequence (begins at element appsrc_a, ends at element a_queue1)
        GstElement *appsrc_a = gst_element_factory_make("appsrc", "audio-source");
        renderer->appsrc_audio = appsrc_a;
        if (is_audio) {
            g_object_set(G_OBJECT(appsrc_a), "caps", gst_caps_from_string(alac_caps),
                         "stream_type", 0, "is_live", TRUE, "format", GST_FORMAT_TIME, NULL);
        } else {
            g_object_set(G_OBJECT(appsrc_a), "caps", gst_caps_from_string(aac_eld_caps),
                         "stream_type", 0, "is_live", TRUE, "format", GST_FORMAT_TIME, NULL);
        }
        a_queue1 = gst_element_factory_make("queue", NULL);
        if (!is_audio && render_video) {
	  GstElement *a_parse = gst_element_factory_make("aacparse", NULL);
	  GstElement *a_queue0 = gst_element_factory_make("queue", NULL);
            gst_bin_add_many(GST_BIN(pipeline), appsrc_a, a_queue0, a_parse, a_queue1, NULL);
            if (!gst_element_link_many(appsrc_a, a_queue0, a_parse, a_queue1, NULL)) {
                g_printerr("failed to link audio elements (1) %s\n", renderer->codec);
                exit(1);
            }
        } else {
            gst_bin_add_many(GST_BIN(pipeline), appsrc_a, a_queue1, NULL);
            if (!gst_element_link_many(appsrc_a, a_queue1, NULL)) {
                g_printerr("failed to link audio elements (2) %s\n", renderer->codec);
                exit(1);
            }
        }

        // audio output sequence (begins at element adec, ends at element asink)
        if (is_audio) {
	  adec = gst_element_factory_make("avdec_alac", NULL);
        } else {
	  adec = gst_element_factory_make("avdec_aac", NULL);
        }
        GstElement *acon = gst_element_factory_make("audioconvert",NULL);
        GstElement *ares = gst_element_factory_make("audioresample", NULL);
        GstElement *volume = gst_element_factory_make("volume", "volume");
        renderer->volume = volume;
        GstElement *level = gst_element_factory_make("level", NULL);
        GstElement *asink = gst_element_factory_make("autoaudiosink", NULL);
        gst_bin_add_many(GST_BIN(pipeline), adec, acon, ares, volume, level, asink, NULL);
        if (!gst_element_link_many(adec, acon, ares, volume, level, asink, NULL)) {
            g_printerr("failed to link audio elements (3) %s\n", renderer->codec);
            exit(1);
        }	
    }

    if (render_video) {
        // video input sequence (begins at appsrc_v, ends at element v_queue1)
        GstElement *appsrc_v = gst_element_factory_make("appsrc", "video-source");
        renderer->appsrc_video = appsrc_v;
        GstElement *v_parse = NULL;
        if (is_h264) {
            g_object_set(G_OBJECT(appsrc_v), "caps", gst_caps_from_string(h264_caps),
                         "stream_type", 0, "is_live", TRUE, "format", GST_FORMAT_TIME, NULL);
            v_parse = gst_element_factory_make("h264parse", NULL);
        } else if (is_h265) {
            g_object_set(G_OBJECT(appsrc_v), "caps", gst_caps_from_string(h265_caps),
                         "stream_type", 0, "is_live", TRUE, "format", GST_FORMAT_TIME, NULL);	      
            v_parse = gst_element_factory_make("h265parse", NULL);
        } else {
            g_object_set(G_OBJECT(appsrc_v), "caps", gst_caps_from_string(jpeg_caps),
                                  "stream_type", 0, "is_live", TRUE, "format", GST_FORMAT_TIME, NULL);	      
        }
        v_queue1 = gst_element_factory_make("queue", NULL);
        if (v_parse) {
            GstElement *v_queue0 = gst_element_factory_make("queue", NULL);
            gst_bin_add_many(GST_BIN(pipeline), appsrc_v, v_queue0, v_parse, v_queue1);
            if (!gst_element_link_many(appsrc_v, v_queue0, v_parse, v_queue1)) {
                g_printerr("failed to link video elements (1) %s\n", renderer->codec);
                exit(1);
            }
        } else {
            gst_bin_add_many(GST_BIN(pipeline), appsrc_v, v_queue1);
            if (!gst_element_link_many(appsrc_v, v_queue1)) {
                g_printerr("failed to link video elements (2) %s\n", renderer->codec);
                exit(1);
            }
        }

        //video output sequence (begins at element at element vdec, ends at element vsink)
        if (is_audio && show_coverart) {
	  vdec = gst_element_factory_make("jpegdec", NULL);
        } else {
            vdec = gst_element_factory_make("decodebin3", NULL);
        }
        GstElement *vflip = gst_element_factory_make("videoflip", NULL);
        g_object_set(G_OBJECT(vflip), "video-direction", video_orientation, NULL);
        GstElement *vconv = gst_element_factory_make("videoconvert", NULL);
        GstElement *vscale = gst_element_factory_make("videoscale", NULL);
        GstElement *vsink = gst_element_factory_make("autovideosink", NULL);
        gboolean sync = !is_audio;
        g_object_set(G_OBJECT(vsink), "sync", sync, NULL);
        gst_bin_add_many(GST_BIN(pipeline), vdec, vflip, vconv, vscale, NULL);
        if (fix_rgb) {
	  GstElement *vconv1 = gst_element_factory_make("videoconvert", NULL);
            GstElement *rgb_filter = gst_element_factory_make("caps_filter", "rgb_filter");
            g_object_set(G_OBJECT(rgb_filter), "caps", gst_caps_from_string(rgb_caps));
            gst_bin_add_many(GST_BIN(pipeline), vconv1, rgb_filter, NULL);
            if (!gst_element_link_many(vdec, vflip, vconv1, rgb_filter, vconv, vscale, NULL)) {
                g_printerr("failed to link video elements (3) %s\n", renderer->codec);
                exit(1);
            }	
        } else {
            if (!gst_element_link_many(vdec, vflip, vconv, vscale, NULL)) {
                g_printerr("failed to link video elements (4) %s\n", renderer->codec);
                exit(1);
            }		  
        }
        if (is_audio && show_coverart) {
	  GstElement *vfreeze = gst_element_factory_make("imagefreeze", NULL);
            g_object_set(G_OBJECT(vfreeze), "allow-replace", TRUE, NULL);
            GstElement *vtext = gst_element_factory_make("textoverlay", "metadata_overlay");
            gst_bin_add_many(GST_BIN(pipeline), vfreeze, vtext, NULL);
            if (!gst_element_link_many(vscale, vflip, vfreeze, vtext, vsink, NULL)) {
                g_printerr("failed to link video elements (5) %s\n", renderer->codec);
               exit(1);
            }	
            renderer->textsrc = gst_bin_get_by_name(GST_BIN(pipeline), "metadata_overlay");
            g_object_set(G_OBJECT(renderer->textsrc), "text", "", "shaded-background", TRUE, "font-desc", "Sans, 16",  NULL);
        } else {
            if (!gst_element_link_many(vscale, vsink)) {
                g_printerr("failed to link video elements (6) %s\n", renderer->codec);
                exit(1);
            }
        }
    }

    if (!is_audio && render_audio && render_video) {
        GstElement *mux = gst_element_factory_make("mpegtsmux", NULL);
        GstElement *demux = gst_element_factory_make("tsmux", NULL);
        GstElement *a_queue2 = gst_element_factory_make("queue", NULL);
        GstElement *v_queue2 = gst_element_factory_make("queue", NULL);
        gst_bin_add_many(GST_BIN(pipeline), mux, demux, a_queue2, v_queue2, NULL);
        if (!gst_element_link(a_queue1, mux) ||
            !gst_element_link(v_queue1, mux) ||
            !gst_element_link(a_queue2, adec) ||
            !gst_element_link(v_queue2, vdec) ||
            !gst_element_link(mux, demux) ) {
            g_printerr("failed to link mux/demux elements %s\n", renderer->codec);
            exit(1);
        }

        g_object_set_data(G_OBJECT(renderer->pipeline), "v_queue2", v_queue2);
        g_object_set_data(G_OBJECT(renderer->pipeline), "a_queue2", a_queue2);
    } else {
        if (render_audio) {
            if (!gst_element_link(a_queue1, adec)) {
                g_printerr("failed to link audio elements (4) %s\n", renderer->codec);
                exit(1);
            }
        }
        if (render_video) {
	    if (!gst_element_link(v_queue1, vdec)) {
                g_printerr("failed to link video elements (7) %s\n", renderer->codec);
                exit(1);
            }
        }
    }
    return renderer;
}

void set_videoflip(const videoflip_t *flip, const videoflip_t *rot) {
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
            video_orientation = GST_VIDEO_ORIENTATION_90R;
            break;
        case RIGHT:
            video_orientation = GST_VIDEO_ORIENTATION_90L;
            break;
        default:
            video_orientation = GST_VIDEO_ORIENTATION_180;
            break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
             video_orientation = GST_VIDEO_ORIENTATION_UL_LR;
            break;
        case RIGHT:
             video_orientation = GST_VIDEO_ORIENTATION_UR_LL;
            break;
        default:
            video_orientation = GST_VIDEO_ORIENTATION_HORIZ;
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            video_orientation = GST_VIDEO_ORIENTATION_UR_LL;
            break;
        case RIGHT:
            video_orientation = GST_VIDEO_ORIENTATION_UL_LR;
            break;
        default:
            video_orientation = GST_VIDEO_ORIENTATION_VERT;
          break;
        }
        break;
    default:
        switch (*rot) {
        case LEFT:
            video_orientation = GST_VIDEO_ORIENTATION_90L;
            break;
       case RIGHT:
            video_orientation = GST_VIDEO_ORIENTATION_90R;
            break;
        default:
            video_orientation = GST_VIDEO_ORIENTATION_IDENTITY;
            break;
        }
        break;
    }
}


void mirror_renderer_init (logger_t *logger, const char *audiosink, const char* videosink, const char * videosink_options, videoflip_t videoflip[2],
                           bool video_sync, bool audio_sync, bool h265_support, bool coverart_support, bool rgb_fix ) {

    if (!strcmp(videosink, "0")) {
        render_video = FALSE;
    } else {
        render_video = TRUE;
    }

    if (!strcmp(audiosink, "0")) {
        render_audio = FALSE;
    } else {
        render_audio = TRUE;
    }
    set_videoflip(&videoflip[0], &videoflip[1]);
    fix_rgb = (gboolean) rgb_fix;
    show_coverart = (gboolean) coverart_support;


    mirror_renderer_alac = create_mirror_renderer(FALSE, TRUE);
    mirror_renderer_h264 = create_mirror_renderer(FALSE, FALSE);
    if (h265_support) {
        mirror_renderer_h265 = create_mirror_renderer(TRUE, FALSE);
    }
}


uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);                                                                      
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *                                                                                               
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *                                                                                               
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *                                                                                               
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed\n");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        if (!mirror_renderer || !(mirror_renderer->appsrc_video)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);                                                                                                   
        //if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
	//}
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(mirror_renderer->appsrc_video), buffer);
    }
    return 0;
}



void audio_renderer_set_volume(double volume) {
    volume = (volume > 10.0) ? 10.0 : volume;
    volume = (volume < 0.0) ? 0.0 : volume;
    g_object_set(mirror_renderer->volume, "volume", volume, NULL);
}



 void audio_renderer_render_buffer(unsigned char* data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;

    if (!render_audio) return;    /* do nothing unless render_audio == TRUE */

    GstClockTime pts = (GstClockTime) *ntp_time ;    /* now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (audio_renderer->appsrc), pts);                                                                
    if (data_len == 0 || mirror_renderer == NULL) return;

    /* all audio received seems to be either ct = 8 (AAC_ELD 44100/2 spf 460 ) AirPlay Mirror protocol *                                                                          
     * or ct = 2 (ALAC 44100/16/2 spf 352) AirPlay protocol.                                           *                                                                          
     * first byte data[0] of ALAC frame is 0x20,                                                       *                                                                          
     * first byte of AAC_ELD is 0x8c, 0x8d or 0x8e: 0x100011(00,01,10) in modern devices               *                                                                          
     *                   but is 0x80, 0x81 or 0x82: 0x100000(00,01,10) in ios9, ios10 devices          *                                                                          
     * first byte of AAC_LC should be 0xff (ADTS) (but has never been  seen).                          */

    buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
    g_assert(buffer != NULL);
    //g_print("audio latency %8.6f\n", (double) latency / SECOND_IN_NSECS);                                                                                                       
    //if (sync) {
        GST_BUFFER_PTS(buffer) = pts;
    //}
    gst_buffer_fill(buffer, 0, data, *data_len);
    bool valid = false;
    /*AAC-ELD*/
    if (!mirror_renderer->is_audio) {
        switch (data[0]){
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x80:
        case 0x81:
        case 0x82:
            valid = true;
            break;
        default:
            valid = false;
            break;
        }
    } else {
    /*ALAC*/
        valid = (data[0] == 0x20);
    }

    if (valid) {
        gst_app_src_push_buffer(GST_APP_SRC(mirror_renderer->appsrc_audio), buffer);
    } else {
        logger_log(logger, LOGGER_ERR, "*** ERROR invalid  audio frame (compression_type %s ",
                   (mirror_renderer->is_audio ? "alac" : "aac-eld"));
        logger_log(logger, LOGGER_ERR, "***       first byte of invalid frame was  0x%2.2x ", (unsigned int) data[0]);
    }
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (mirror_renderer && !strcmp(mirror_renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(mirror_renderer->appsrc_video), buffer);
    }
}
