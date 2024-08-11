/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "video_renderer.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#define SECOND_IN_NSECS 1000000000UL
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts; 
#endif

static video_renderer_t *renderer = NULL;
static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif


struct video_renderer_s {
    GstElement *appsrc, *pipeline;
    GstBus *bus;
#ifdef  X_DISPLAY_FIX
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static void append_videoflip (GString *launch, const videoflip_t *flip, const videoflip_t *rot) {
    /* videoflip image transform */
    switch (*flip) {
    case INVERT:
        switch (*rot)  {
        case LEFT:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
	    break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        default:
	    g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_180 ! ");
	    break;
        }
        break;
    case HFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        case RIGHT: 
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_HORIZ ! ");
            break;
        }
        break;
    case VFLIP:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UR_LL ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_UL_LR ! ");
            break;
        default:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_VERT ! ");
	  break;
	}
        break;
    default:
        switch (*rot) {
        case LEFT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90L ! ");
            break;
        case RIGHT:
            g_string_append(launch, "videoflip video-direction=GST_VIDEO_ORIENTATION_90R ! ");
            break;
        default:
            break;
        }
        break;
    }
}

/* apple uses colorimetry=1:3:5:1                                *
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 5 -> GST_VIDEO_TRANSFER_BT709                      *
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    *                            *
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

void  video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                          const char *decoder, const char *converter, const char *videosink, const bool *initial_fullscreen,
                          const bool *video_sync) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    GstClock *clock = gst_system_clock_obtain();
    g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);

    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));
      
    logger = render_logger;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;

    renderer = calloc(1, sizeof(video_renderer_t));
    g_assert(renderer);

    GString *launch = g_string_new("appsrc name=video_source ! ");
    g_string_append(launch, "queue ! ");
    g_string_append(launch, parser);
    g_string_append(launch, " ! ");
    g_string_append(launch, decoder);
    g_string_append(launch, " ! ");
    append_videoflip(launch, &videoflip[0], &videoflip[1]);
    g_string_append(launch, converter);
    g_string_append(launch, " ! ");
    g_string_append(launch, "videoscale ! ");
    g_string_append(launch, videosink);
    if (*video_sync) {
        g_string_append(launch, " sync=true");
        sync = true;
    } else {
        g_string_append(launch, " sync=false");
        sync = false;
    }
    logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline will be:\n\"%s\"", launch->str);
    renderer->pipeline = gst_parse_launch(launch->str, &error);
    if (error) {
        g_error ("get_parse_launch error (video) :\n %s\n",error->message);
        g_clear_error (&error);
    }
    g_assert (renderer->pipeline);
    gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer->pipeline), clock);

    renderer->appsrc = gst_bin_get_by_name (GST_BIN (renderer->pipeline), "video_source");
    g_assert(renderer->appsrc);
    caps = gst_caps_from_string(h264_caps);
    g_object_set(renderer->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
    g_string_free(launch, TRUE);
    gst_caps_unref(caps);
    gst_object_unref(clock);

#ifdef X_DISPLAY_FIX
    use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
    fullscreen = *initial_fullscreen;
    renderer->server_name = server_name;
    renderer->gst_window = NULL;
    X11_search_attempts = 0; 
    if (use_x11) {
        renderer->gst_window = calloc(1, sizeof(X11_Window_t));
        g_assert(renderer->gst_window);
        get_X11_Display(renderer->gst_window);
        if (!renderer->gst_window->display) {
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
    }
#endif
    gst_element_set_state (renderer->pipeline, GST_STATE_READY);
    GstState state;
    if (gst_element_get_state (renderer->pipeline, &state, NULL, 0)) {
        if (state == GST_STATE_READY) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer");
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
        }
    } else {
        logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer");
    }
}

void video_renderer_pause() {
    logger_log(logger, LOGGER_DEBUG, "video renderer paused");
    gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
}

void video_renderer_resume() {
    if (video_renderer_is_paused()) {
        logger_log(logger, LOGGER_DEBUG, "video renderer resumed");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

bool video_renderer_is_paused() {
    GstState state;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    return (state == GST_STATE_PAUSED);
}

void video_renderer_start() {
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

void video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            logger_log(logger, LOGGER_ERR, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return;
        }
    }
    g_assert(data_len != 0);
    /* first four bytes of valid  h264  video data are 0x00, 0x00, 0x00, 0x01.    *
     * nal_count is the number of NAL units in the data: short SPS, PPS, SEI NALs *
     * may  precede a VCL NAL. Each NAL starts with 0x00 0x00 0x00 0x01 and is    *
     * byte-aligned: the first byte of invalid data (decryption failed) is 0x01   */
    if (data[0]) {
        logger_log(logger, LOGGER_ERR, "*** ERROR decryption of video packet failed ");
    } else {
        if (first_packet) {
            logger_log(logger, LOGGER_INFO, "Begin streaming to GStreamer video pipeline");
            first_packet = false;
        }
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
        g_assert(buffer != NULL);
        //g_print("video latency %8.6f\n", (double) latency / SECOND_IN_NSECS);
        if (sync) {
            GST_BUFFER_PTS(buffer) = pts;
        }
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window && !(renderer->gst_window->window) && use_x11) {
            X11_search_attempts++;
            logger_log(logger, LOGGER_DEBUG, "Looking for X11 UxPlay Window, attempt %d", (int) X11_search_attempts);
            get_x_window(renderer->gst_window, renderer->server_name);
	    if (renderer->gst_window->window) {
                logger_log(logger, LOGGER_INFO, "\n*** X11 Windows: Use key F11 or (left Alt)+Enter to toggle full-screen mode\n");
                if (fullscreen) {
                    set_fullscreen(renderer->gst_window, &fullscreen);
                }
            }
        }
#endif
    }
}

void video_renderer_flush() {
}

void video_renderer_stop() {
    if (renderer) {
        gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
    gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
    }
}

void video_renderer_destroy() {
    if (renderer) {
        GstState state;
        gst_element_get_state(renderer->pipeline, &state, NULL, 0);
        if (state != GST_STATE_NULL) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
	    gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        }
        gst_object_unref(renderer->bus);
        gst_object_unref (renderer->appsrc);
        gst_object_unref (renderer->pipeline);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window) {
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        free (renderer);
        renderer = NULL;
    }
}

/* not implemented for gstreamer */
void video_renderer_update_background(int type) {
}

gboolean gstreamer_pipeline_bus_callback(GstBus *bus, GstMessage *message, gpointer loop) {
    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gboolean flushing;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error: %s", err->message);
        if (strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi OS with (unpatched) GStreamer-1.18.4 needs \"-bt709\" uxplay option");
        }
	g_error_free (err);
        g_free (debug);
        gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
	flushing = TRUE;
        gst_bus_set_flushing(bus, flushing);
 	gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	g_main_loop_quit( (GMainLoop *) loop);
        break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
         logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
	//   g_main_loop_quit( (GMainLoop *) loop);
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (auto_videosink) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink is \"%ssink\"", sink);
                auto_videosink = false;
#ifdef X_DISPLAY_FIX
                use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer->gst_window && renderer->gst_window->window) {
            GstNavigationMessageType message_type = gst_navigation_message_get_type (message);
            if (message_type == GST_NAVIGATION_MESSAGE_EVENT) {
                GstEvent *event = NULL;
                if (gst_navigation_message_parse_event (message, &event)) {
                    GstNavigationEventType event_type = gst_navigation_event_get_type (event);
                    const gchar *key;
                    switch (event_type) {
                    case GST_NAVIGATION_EVENT_KEY_PRESS:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if ((strcmp (key, "F11") == 0) || (alt_keypress && strcmp (key, "Return") == 0)) {
                                fullscreen = !(fullscreen);
                                set_fullscreen(renderer->gst_window, &fullscreen);
                            } else if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = true;
                            }
                        }
                        break;
                    case GST_NAVIGATION_EVENT_KEY_RELEASE:
                        if (gst_navigation_event_parse_key_event (event, &key)) {
                            if (strcmp (key, "Alt_L") == 0) {
                                alt_keypress = false;
                            }
                        }
                    default:
                        break;
                    }
                }
                if (event) {
                    gst_event_unref (event);
                }
            }
        }
        break;
#endif
    default:
      /* unhandled message */
        break;
    }
    return TRUE;
}

unsigned int video_renderer_listen(void *loop) {
    return (unsigned int) gst_bus_add_watch(renderer->bus, (GstBusFunc)
                                            gstreamer_pipeline_bus_callback, (gpointer) loop);    
}
