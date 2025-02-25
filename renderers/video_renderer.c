/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
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

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include "video_renderer.h"

#define SECOND_IN_NSECS 1000000000UL
#define SECOND_IN_MICROSECS 1000000
#ifdef X_DISPLAY_FIX
#include <gst/video/navigation.h>
#include "x_display_fix.h"
static bool fullscreen = false;
static bool alt_keypress = false;
static unsigned char X11_search_attempts;
#endif

static GstClockTime gst_video_pipeline_base_time = GST_CLOCK_TIME_NONE;
static logger_t *logger = NULL;
static unsigned short width, height, width_source, height_source;  /* not currently used */
static bool first_packet = false;
static bool sync = false;
static bool auto_videosink = true;
static bool hls_video = false;
#ifdef X_DISPLAY_FIX
static bool use_x11 = false;
#endif
static bool logger_debug = false;
static bool video_terminate = false;
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration;
static gboolean hls_seek_enabled;
static gboolean hls_playing;

#define NCODECS  2   /* renderers for h264 and h265 */

struct video_renderer_s {
    GstElement *appsrc, *pipeline;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    gboolean terminate;
    gint buffering_level;
#ifdef  X_DISPLAY_FIX
    bool use_x11;
    const char * server_name;
    X11_Window_t * gst_window;
#endif
};

static video_renderer_t *renderer = NULL;
static video_renderer_t *renderer_type[NCODECS] = {0};
static int n_renderers = NCODECS;
static char h264[] = "h264";
static char h265[] = "h265";
static char hls[] = "hls";

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

/* apple uses colorimetry that is detected as  1:3:7:1           * //previously 1:3:5:1 was seen
 * (not recognized by v4l2 plugin in Gstreamer  < 1.20.4)        *
 * See .../gst-libs/gst/video/video-color.h in gst-plugins-base  *
 * range = 1   -> GST_VIDEO_COLOR_RANGE_0_255      ("full RGB")  * 
 * matrix = 3  -> GST_VIDEO_COLOR_MATRIX_BT709                   *
 * transfer = 7 -> GST_VIDEO_TRANSFER_SRGB                       * // previously GST_VIDEO_TRANSFER_BT709
 * primaries = 1 -> GST_VIDEO_COLOR_PRIMARIES_BT709              *
 * closest used by  GStreamer < 1.20.4 is BT709, 2:3:5:1 with    * // now use sRGB = 1:1:7:1    
 * range = 2 -> GST_VIDEO_COLOR_RANGE_16_235 ("limited RGB")     */  

static const char h264_caps[]="video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[]="video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void video_renderer_size(float *f_width_source, float *f_height_source, float *f_width, float *f_height) {
    width_source = (unsigned short) *f_width_source;
    height_source = (unsigned short) *f_height_source;
    width = (unsigned short) *f_width;
    height = (unsigned short) *f_height;
    logger_log(logger, LOGGER_DEBUG, "begin video stream wxh = %dx%d; source %dx%d", width, height, width_source, height_source);
}

GstElement *make_video_sink(const char *videosink, const char *videosink_options) {
  /* used to build a videosink for playbin, using the user-specified string "videosink" */ 
    GstElement *video_sink = gst_element_factory_make(videosink, "videosink");
    if (!video_sink) {
        return NULL;
    }

    /* process the video_sink_options */
    size_t len = strlen(videosink_options);
    if (!len) {
        return video_sink;
    }

    char *options  = (char *) malloc(len + 1);
    strncpy(options, videosink_options, len + 1);

    /* remove any extension begining with "!" */
    char *end = strchr(options, '!');
    if (end) {   
      *end = '\0';
    }

    /* add any fullscreen options "property=pval" included in string videosink_options*/    
    /* OK to use strtok_r in Windows with MSYS2 (POSIX); use strtok_s for MSVC */
    char *token;
    char *text = options;

    while((token = strtok_r(text, " ", &text))) {
	char *pval = strchr(token, '=');
        if (pval) {
            *pval = '\0';
            pval++;
            const gchar *property_name = (const gchar *) token;
            const gchar *value = (const gchar *) pval;
	    g_print("playbin_videosink property: \"%s\" \"%s\"\n", property_name, value);
	    gst_util_set_object_arg(G_OBJECT (video_sink), property_name, value);
        }
    }
    free(options);
    return video_sink;
}

void  video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    hls_video = (uri != NULL);
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    video_terminate = false;
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;

    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264 and 265 mirror-mode 
     * renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265 and hls  */  
    if (hls_video) {
        n_renderers = 1;
    } else {
        n_renderers = h265_support ? 2 : 1;
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 2);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        if (hls_video) {
            /* use playbin3 to play HLS video: replace "playbin3" by "playbin" to use playbin2 */
            switch (playbin_version)  {
            case 2:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin", "hls-playbin2");
                break;
            case 3:
                renderer_type[i]->pipeline = gst_element_factory_make("playbin3", "hls-playbin3");
                break;
            default:
                logger_log(logger, LOGGER_ERR, "video_renderer_init: invalid playbin version %u", playbin_version);
                g_assert(0);
            }
            logger_log(logger, LOGGER_INFO, "Will use GStreamer playbin version %u to play HLS streamed video", playbin_version);	    
            g_assert(renderer_type[i]->pipeline);
            renderer_type[i]->appsrc = NULL;
	    renderer_type[i]->codec = hls;
            /* if we are not using autovideosink, build a videossink based on the string "videosink" */
            if(strcmp(videosink, "autovideosink")) {
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);

                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }

            g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            switch (i) {
            case 0:
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
                break;
            case 1:
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
                break;
            default:
                g_assert(0);
            }
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
            g_string_append(launch, " name=");
            g_string_append(launch, videosink);
            g_string_append(launch, "_");
            g_string_append(launch, renderer_type[i]->codec);
            g_string_append(launch, videosink_options);
            if (video_sync) {
                g_string_append(launch, " sync=true");
                sync = true;
            } else {
                g_string_append(launch, " sync=false");
                sync = false;
            }

            if (!strcmp(renderer_type[i]->codec, h264)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h265))){
                    pos +=3;
                    *pos = '4';
                }
            } else if (!strcmp(renderer_type[i]->codec, h265)) {
                char *pos = launch->str;
                while ((pos = strstr(pos,h264))){
                    pos +=3;
                    *pos = '5';
                }
            }

            logger_log(logger, LOGGER_DEBUG, "GStreamer video pipeline %d:\n\"%s\"", i + 1, launch->str);
            renderer_type[i]->pipeline = gst_parse_launch(launch->str, &error);
            if (error) {
                g_error ("get_parse_launch error (video) :\n %s\n",error->message);
                g_clear_error (&error);
            }
            g_assert (renderer_type[i]->pipeline);

            GstClock *clock = gst_system_clock_obtain();
            g_object_set(clock, "clock-type", GST_CLOCK_TYPE_REALTIME, NULL);
            gst_pipeline_use_clock(GST_PIPELINE_CAST(renderer_type[i]->pipeline), clock);
            renderer_type[i]->appsrc = gst_bin_get_by_name (GST_BIN (renderer_type[i]->pipeline), "video_source");
            g_assert(renderer_type[i]->appsrc);

            g_object_set(renderer_type[i]->appsrc, "caps", caps, "stream-type", 0, "is-live", TRUE, "format", GST_FORMAT_TIME, NULL);
            g_string_free(launch, TRUE);
            gst_caps_unref(caps);
	    gst_object_unref(clock);
        }	
#ifdef X_DISPLAY_FIX
        use_x11 = (strstr(videosink, "xvimagesink") || strstr(videosink, "ximagesink") || auto_videosink);
        fullscreen = initial_fullscreen;
        renderer_type[i]->server_name = server_name;
        renderer_type[i]->gst_window = NULL;
        renderer_type[i]->use_x11 = false;
        X11_search_attempts = 0;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[i]->use_x11 = true;
                } else {
                    free(renderer_type[0]->gst_window);
                    renderer_type[0]->gst_window = NULL;
                }
            } else if (renderer_type[0]->use_x11) {
                renderer_type[i]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[i]->gst_window);
                memcpy(renderer_type[i]->gst_window, renderer_type[0]->gst_window, sizeof(X11_Window_t));
                renderer_type[i]->use_x11 = true;
            }
        }
#endif
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        if (gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND)) {
            if (state == GST_STATE_READY) {
                logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
                if (hls_video && i == 0) {
                    renderer = renderer_type[i];
                }
            } else {
                logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
        }
    }
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    logger_log(logger, LOGGER_DEBUG, "video renderer paused");
    gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
}

void video_renderer_resume() {
    if (!renderer) {
        return;
    }
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState state;
    /* wait with timeout 100 msec for pipeline to change state from PAUSED to PLAYING */
    gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
    const gchar *state_name = gst_element_state_get_name(state);
    logger_log(logger, LOGGER_DEBUG, "video renderer resumed: state %s", state_name);
    if (renderer->appsrc) {
        gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    }
}

void video_renderer_start() {
    GstState state;
    const gchar *state_name;
    if (hls_video) {
        renderer->bus = gst_element_get_bus(renderer->pipeline);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name= gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
  /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name= gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d state %s", i, state_name);
    }
    renderer = NULL;
    first_packet = true;
#ifdef X_DISPLAY_FIX
    X11_search_attempts = 0;
#endif
}

/* used to find any X11 Window used by the playbin (HLS) pipeline after it starts playing. 
*  if use_x11 is true, called every 100 ms after playbin state is READY until the x11 window is found*/
bool waiting_for_x11_window() {
    if (!hls_video) {
        return false;
    }
#ifdef X_DISPLAY_FIX
    if (use_x11 && renderer->gst_window) {
        get_x_window(renderer->gst_window, renderer->server_name);
        if (!renderer->gst_window->window) {
	    return true;    /* window still not found */
        }
    }
    if (fullscreen) {
         set_fullscreen(renderer->gst_window, &fullscreen);
    }
#endif
    return false;
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
        if (renderer->gst_window && !(renderer->gst_window->window) && renderer->use_x11) {
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
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p", renderer);
        GstState state;
	GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
	logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	    logger_log(logger, LOGGER_DEBUG,"pipeline state change to NULL: %s",
		       gst_element_state_change_return_get_name(ret));
	    gst_element_get_state(renderer->pipeline, NULL, NULL, 1000 * GST_MSECOND);
        }
        gst_object_unref(renderer->bus);
	if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
        }
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

void video_renderer_destroy() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
        }
    }
}

gboolean gstreamer_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {

  /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }
    g_assert(type != -1);

    if (logger_debug) {
        if (hls_video) {
            gint64 pos;
            const gchar no_state[] = "";
            const gchar *old_state_name, *new_state_name;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
                GstState old_state, new_state;
                gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
                old_state_name = gst_element_state_get_name (old_state);
                new_state_name = gst_element_state_get_name (new_state);
            } else {
                 old_state_name = no_state;
                 new_state_name = no_state;
            }
            gst_element_query_position (renderer_type[type]->pipeline, GST_FORMAT_TIME, &pos);
            if (GST_CLOCK_TIME_IS_VALID(pos)) {
                g_print("GStreamer bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n", GST_MESSAGE_SRC_NAME(message),
			GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
            } else {
                g_print("GStreamer bus message %s %s %s %s; position: none\n", GST_MESSAGE_SRC_NAME(message),
			GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
            }
        } else {
            g_print("GStreamer %s bus message: %s %s\n", renderer_type[type]->codec, GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message));
        }
    }

    /* monitor hls video position until seek to hls_start_position is achieved */
    if (hls_video && hls_requested_start_position) {
        if (strstr(GST_MESSAGE_SRC_NAME(message), "sink")) {	  
            gint64 pos;
            if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
                gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
            }
	    gst_element_query_position (renderer_type[type]->pipeline, GST_FORMAT_TIME, &pos);
            //g_print("HLS position %" GST_TIME_FORMAT " requested_start_position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s\n",
            //    GST_TIME_ARGS(pos), GST_TIME_ARGS(hls_requested_start_position), GST_TIME_ARGS(hls_duration),
            //    (hls_seek_enabled ? "seek enabled" : "seek not enabled"));
            if (pos > hls_requested_start_position) {
                hls_requested_start_position = 0;
            }
	    if ( hls_requested_start_position && pos < hls_requested_start_position  && hls_seek_enabled) {
                g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
                if (gst_element_seek_simple (renderer_type[type]->pipeline, GST_FORMAT_TIME,
                                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
                    hls_requested_start_position = 0;
                }
	    }
	}
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
	    if (percent >= 0) {
                renderer_type[type]->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, "Buffering :%u percent done", percent);
                if (percent < 100) {
                    gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_PAUSED);
                } else {
                    gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_PLAYING);
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err;
        gchar *debug;
        gboolean flushing;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error: %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (!hls_video && strstr(err->message,"Internal data stream error")) {
            logger_log(logger, LOGGER_INFO,
                     "*** This is a generic GStreamer error that usually means that GStreamer\n"
                     "*** was unable to construct a working video pipeline.\n\n"
                     "*** If you are letting the default autovideosink select the videosink,\n"
                     "*** GStreamer may be trying to use non-functional hardware h264 video decoding.\n"
                     "*** Try using option -avdec to force software decoding or use -vs <videosink>\n"
                     "*** to select a videosink of your choice (see \"man uxplay\").\n\n"
                     "*** Raspberry Pi models 4B and earlier using Video4Linux2 may need \"-bt709\" uxplay option");
        }
	g_error_free (err);
        g_free (debug);
	if (renderer_type[type]->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer_type[type]->appsrc));
	}
        gst_bus_set_flushing(bus, TRUE);
        gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_READY);
        renderer_type[type]->terminate = TRUE;
        g_main_loop_quit( (GMainLoop *) loop);
        break;
    }
    case GST_MESSAGE_EOS:
      /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream");
	if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer_type[type]->pipeline, GST_STATE_READY);
	    renderer_type[type]->terminate = TRUE;
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && logger_debug && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            if (new_state != GST_STATE_PLAYING) {
                break;
                hls_playing = FALSE;
            }
            hls_playing = TRUE;
            GstQuery *query;
            query = gst_query_new_seeking(GST_FORMAT_TIME);
                if (gst_element_query(renderer->pipeline, query)) {
	        gst_query_parse_seeking (query, NULL, &hls_seek_enabled, &hls_seek_start, &hls_seek_end);
                if (hls_seek_enabled) {
                    g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
			     GST_TIME_ARGS (hls_seek_start), GST_TIME_ARGS (hls_seek_end));
                } else {
                    g_print ("Seeking is DISABLED for this stream.\n");
                }
            } else {
                g_printerr ("Seeking query failed.");
            }
            gst_query_unref (query);
        }
        if (renderer_type[type]->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
		if (strstr(GST_MESSAGE_SRC_NAME(message), renderer_type[type]->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer_type[type]->id + 1,
                               renderer_type[type]->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer_type[type]->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
		    renderer_type[type]->autovideo = false;
                }
            }
        }
        break;
#ifdef  X_DISPLAY_FIX
    case GST_MESSAGE_ELEMENT:
        if (renderer_type[type]->gst_window && renderer_type[type]->gst_window->window) {
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
                                set_fullscreen(renderer_type[type]->gst_window, &fullscreen);
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

int video_renderer_choose_codec (bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    video_renderer_t *renderer_unused = NULL;
    g_assert(!hls_video);
    if (n_renderers == 1) {
        if (video_is_h265) {
            logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
            return -1;
	}
        renderer_used = renderer_type[0];
    } else {
        renderer_used = video_is_h265 ? renderer_type[1] : renderer_type[0];
        renderer_unused = video_is_h265 ? renderer_type[0] : renderer_type[1];
    }
    if (renderer_used == NULL) { 
        return -1;
    } else if (renderer_used == renderer) {
        return 0;
    } else if (renderer) {
        return -1;
    }
    renderer = renderer_used;
    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
    GstState old_state, new_state;
    if (gst_element_get_state(renderer->pipeline, &old_state, &new_state, 100 * GST_MSECOND) == GST_STATE_CHANGE_FAILURE) {
        g_error("video pipeline failed to go into playing state");
	return -1;
    }
    logger_log(logger, LOGGER_DEBUG, "video_pipeline state change from %s to %s\n",
               gst_element_state_get_name (old_state),gst_element_state_get_name (new_state));
    gst_video_pipeline_base_time = gst_element_get_base_time(renderer->appsrc);
    if (renderer_unused) {
       gst_element_set_state (renderer_unused->pipeline, GST_STATE_NULL);
    }
    return 0;
}

unsigned int video_reset_callback(void * loop) {
    if (video_terminate) {
        video_terminate = false;
        if (renderer->appsrc) {
	    gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
	gboolean flushing = TRUE;
        gst_bus_set_flushing(renderer->bus, flushing);
 	gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
	g_main_loop_quit( (GMainLoop *) loop);
    }
    return (unsigned  int) TRUE;
}

bool video_get_playback_info(double *duration, double *position, float *rate) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *rate = 0.0f;
    if (!renderer) {
   
        return true;
    }
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    *rate = 0.0f;
    switch (state) {
    case GST_STATE_PLAYING:
        *rate = 1.0f;
    default:
        break;
    }

    if (!GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        if (!gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration)) {
            return true;
        }
    }
    *duration = ((double) hls_duration) / GST_SECOND;
    if (*duration) {
        if (gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos) &&
                                        GST_CLOCK_TIME_IS_VALID(pos)) {
            *position = ((double) pos) / GST_SECOND;
        }
    }

    logger_log(logger, LOGGER_DEBUG, "********* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s *********",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state));
    return true;
}

void video_renderer_set_start(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    hls_requested_start_position = (gint64) (pos_in_micros * GST_USECOND);
    logger_log(logger, LOGGER_DEBUG, "register HLS video start position %f %lld", position,
    hls_requested_start_position);
}

void video_renderer_seek(float position) {
    int pos_in_micros = (int) (position * SECOND_IN_MICROSECS);
    gint64 seek_position = (gint64) (pos_in_micros * GST_USECOND);
    /* don't seek to within 1  microsecond  of beginning or end of video */
    if (hls_duration < 2000) return;
    seek_position =  seek_position < 1000 ? 1000 : seek_position;
    seek_position =  seek_position > hls_duration  - 1000 ? hls_duration - 1000 : seek_position;
    g_print("SCRUB: seek to %f secs =  %" GST_TIME_FORMAT ", duration = %" GST_TIME_FORMAT "\n", position,
            GST_TIME_ARGS(seek_position),  GST_TIME_ARGS(hls_duration));
    gboolean result = gst_element_seek_simple(renderer->pipeline, GST_FORMAT_TIME,
                                              (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                              seek_position);
    if (result) {
        g_print("seek succeeded\n");
        gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);	
    } else {
        g_print("seek failed\n");
    }
}

unsigned int video_renderer_listen(void *loop, int id) {
    g_assert(id >= 0 && id < n_renderers);
    return (unsigned int) gst_bus_add_watch(renderer_type[id]->bus,(GstBusFunc)
                                            gstreamer_pipeline_bus_callback, (gpointer) loop);    
}
