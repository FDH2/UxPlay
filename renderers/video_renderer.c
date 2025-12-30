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
static unsigned char X11_search_attempts = 0;
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
static gint64 hls_requested_start_position = 0;
static gint64 hls_seek_start = 0;
static gint64 hls_seek_end = 0;
static gint64 hls_duration = 0;
static gboolean hls_seek_enabled = FALSE;
static gboolean hls_playing = FALSE;
static gboolean hls_buffer_empty = FALSE;
static gboolean hls_buffer_full = FALSE;
static int type_264 = 0;
static int type_265 = 0;
static int type_hls = 0;
static int type_jpeg = 0;

typedef enum {
  //GST_PLAY_FLAG_VIDEO         = (1 << 0),
  //GST_PLAY_FLAG_AUDIO         = (1 << 1),
  //GST_PLAY_FLAG_TEXT          = (1 << 2),
  //GST_PLAY_FLAG_VIS           = (1 << 3),
  //GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  //GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  //GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  //GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  //GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10),
  //GST_PLAY_FLAG_FORCE_FILTERS = (1 << 11),
  //GST_PLAY_FLAG_FORCE_SW_DECODERS = (1 << 12),
} GstPlayFlags;

#define NCODECS  3   /* renderers for h264,h265, and jpeg images */

struct video_renderer_s {
    GstElement *appsrc, *pipeline;
    GstBus *bus;
    const char *codec;
    bool autovideo;
    int id;
    char *uri;
    gboolean eos;
    gint64 duration;
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
static char hls[]  = "hls";
static char jpeg[] = "jpeg";

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

static const char jpeg_caps[]="image/jpeg";
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
    char *token = NULL;
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

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char * rtp_pipeline,
                          const char *decoder, const char *converter, const char *videosink, const char *videosink_options, 
                          bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    GError *error = NULL;
    GstCaps *caps = NULL;
    bool rtp = (bool) strlen(rtp_pipeline);
    hls_video = (uri != NULL);
    /* videosink choices that are auto */
    auto_videosink = (strstr(videosink, "autovideosink") || strstr(videosink, "fpsdisplaysink"));

    logger = render_logger;
    logger_debug = (logger_get_level(logger) >= LOGGER_DEBUG);
    hls_seek_enabled = FALSE;
    hls_playing = FALSE;
    hls_seek_start = -1;
    hls_seek_end = -1;
    hls_duration = -1;
    hls_buffer_empty = TRUE;
    hls_buffer_empty = FALSE;
    type_hls = -1;
    type_264 = -1;
    type_265 = -1;
    type_jpeg = -1;

    /* this call to g_set_application_name makes server_name appear in the  X11 display window title bar, */
    /* (instead of the program name uxplay taken from (argv[0]). It is only set one time. */

    const gchar *appname = g_get_application_name();
    if (!appname || strcmp(appname,server_name))  g_set_application_name(server_name);
    appname = NULL;
    n_renderers = 1;
    /* the renderer for hls video will only be built if a HLS uri is provided in 
     * the call to video_renderer_init, in which case the h264/h265 mirror-mode and jpeg
     * audio-mode renderers will not be built.   This is because it appears that we cannot  
     * put playbin into GST_STATE_READY before knowing the uri (?), so cannot use a
     * unified renderer structure with h264, h265, jpeg and hls  */  
    if (hls_video) {
        type_hls = 0;
    } else {
        type_264 = 0;
        if (h265_support) {
            type_265 = n_renderers++;
        }
        if (coverart_support) {
            type_jpeg = n_renderers++;
        }
    }
    g_assert (n_renderers <= NCODECS);
    for (int i = 0; i < n_renderers; i++) {
        g_assert (i < 3);
        renderer_type[i] = (video_renderer_t *) calloc(1, sizeof(video_renderer_t));
        g_assert(renderer_type[i]);
        renderer_type[i]->autovideo = auto_videosink;
        renderer_type[i]->id = i;
        renderer_type[i]->bus = NULL;
        renderer_type[i]->appsrc = NULL;
        renderer_type[i]->uri = NULL;
        renderer_type[i]->eos = FALSE;
        if (hls_video) {
            renderer_type[i]->uri = (char *) calloc(strlen(uri) + 1, sizeof(char));
            memcpy(renderer_type[i]->uri, uri, strlen(uri));
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
            renderer_type[i]->codec = hls;
            /* if we are not using an autovideosink, build a videosink based on the string "videosink" */
            if (!auto_videosink) { 
                GstElement *playbin_videosink = make_video_sink(videosink, videosink_options);  
                if (!playbin_videosink) {
                    logger_log(logger, LOGGER_ERR, "video_renderer_init: failed to create playbin_videosink");
                } else {
                    logger_log(logger, LOGGER_DEBUG, "video_renderer_init: create playbin_videosink at %p", playbin_videosink);
                    g_object_set(G_OBJECT (renderer_type[i]->pipeline), "video-sink", playbin_videosink, NULL);
                }
            }
            gint flags = 0;
            g_object_get(renderer_type[i]->pipeline, "flags", &flags, NULL);
            flags |= GST_PLAY_FLAG_DOWNLOAD;
            flags |= GST_PLAY_FLAG_BUFFERING;    // set by default in playbin3, but not in playbin2; is it needed?
            g_object_set(renderer_type[i]->pipeline, "flags", flags, NULL);
            //g_object_set (G_OBJECT (renderer_type[i]->pipeline), "uri", uri, NULL);
        } else {
            bool jpeg_pipeline = false;
            if (i == type_264) {
                renderer_type[i]->codec = h264;
                caps = gst_caps_from_string(h264_caps);
            } else if (i == type_265) {
                renderer_type[i]->codec = h265;
                caps = gst_caps_from_string(h265_caps);
            } else if (i == type_jpeg) {
                jpeg_pipeline = true;
                renderer_type[i]->codec = jpeg;
                caps = gst_caps_from_string(jpeg_caps);
            } else {
                g_assert(0);
            }
            GString *launch = g_string_new("appsrc name=video_source ! ");
            if (jpeg_pipeline) {
                g_string_append(launch, "jpegdec ");
            } else {
                g_string_append(launch, "queue ! ");
                g_string_append(launch, parser);
                g_string_append(launch, " ! ");
                if (!rtp) {
                    g_string_append(launch, decoder);
                } else {
                    g_string_append(launch, "rtph264pay ");
                    g_string_append(launch, rtp_pipeline);
                }
            }
            if (!rtp || jpeg_pipeline) {
                g_string_append(launch, " ! ");
                append_videoflip(launch, &videoflip[0], &videoflip[1]);
                g_string_append(launch, converter);
                g_string_append(launch, " ! ");
                g_string_append(launch, "videoscale ! ");
                if (jpeg_pipeline) {
                    g_string_append(launch, " imagefreeze allow-replace=TRUE ! ");
                }
                g_string_append(launch, videosink);
                g_string_append(launch, " name=");
                g_string_append(launch, videosink);
                g_string_append(launch, "_");
                g_string_append(launch, renderer_type[i]->codec);
                g_string_append(launch, videosink_options);
                if (video_sync && !jpeg_pipeline) {
                    g_string_append(launch, " sync=true");
                    sync = true;
                } else {
                    g_string_append(launch, " sync=false");
                    sync = false;
                }
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
                logger_log(logger, LOGGER_ERR, "GStreamer gst_parse_launch failed to create video pipeline %d\n"
                           "*** error message from gst_parse_launch was:\n%s\n"
                           "launch string parsed was \n[%s]", i + 1, error->message, launch->str);
                if (strstr(error->message, "no element")) {
                    logger_log(logger, LOGGER_ERR, "This error usually means that a uxplay option was mistyped\n"
                               "           or some requested part of GStreamer is not installed\n");
                }
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
        /* setting char *x11_display_name to NULL means the value is taken from $DISPLAY in the environment 
         * (a uxplay option to specify a different value is possible)  */
        char *x11_display_name = NULL;
        if (use_x11) {
            if (i == 0) {
                renderer_type[0]->gst_window = (X11_Window_t *) calloc(1, sizeof(X11_Window_t));
                g_assert(renderer_type[0]->gst_window);
                get_X11_Display(renderer_type[0]->gst_window, x11_display_name);
                if (renderer_type[0]->gst_window->display) {
                    renderer_type[0]->use_x11 = true;
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
        renderer_type[i]->bus = gst_element_get_bus(renderer_type[i]->pipeline);	
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_READY);
        GstState state;
        GstStateChangeReturn ret = gst_element_get_state (renderer_type[i]->pipeline, &state, NULL, 100 * GST_MSECOND);
        if (ret == GST_STATE_CHANGE_SUCCESS) {
            logger_log(logger, LOGGER_DEBUG, "Initialized GStreamer video renderer %d", i + 1);
            if (hls_video && i == 0) {
                renderer = renderer_type[i];
            }
        } else {
            logger_log(logger, LOGGER_ERR, "Failed to initialize GStreamer video renderer %d", i + 1);
            logger_log(logger, LOGGER_INFO, "\nPerhaps your GStreamer installation is missing some required plugins,"
                       "\nor your choices of video options (-vs -vd -vc -fs etc.) are incompatible on"
                       "\nthis computer architecture.  (An example: kmssink with fullscreen option -fs"
                       "\nmay work on some systems, but fail on others)");
            exit(1);
        }
    }
}

void video_renderer_pause() {
    if (!renderer) {
        return;
    }
    GstStateChangeReturn ret = gst_element_set_state(renderer->pipeline, GST_STATE_PAUSED);
    logger_log(logger, LOGGER_DEBUG, "video renderer pause: %s", gst_element_state_change_return_get_name(ret));
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
    const gchar *state_name = NULL;
    if (hls_video) {
        g_object_set (G_OBJECT (renderer->pipeline), "uri", renderer->uri, NULL);
        gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
	gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
	state_name = gst_element_state_get_name(state);
	logger_log(logger, LOGGER_DEBUG, "video renderer_start: state %s", state_name);
        return;
    } 
    /* when not hls, start both h264 and h265 pipelines; will shut down the "wrong" one when we know the codec */
    for (int i = 0; i < n_renderers; i++) {
        gst_element_set_state (renderer_type[i]->pipeline, GST_STATE_PAUSED);
        gst_element_get_state(renderer_type[i]->pipeline, &state, NULL, 1000 * GST_MSECOND);
        state_name = gst_element_state_get_name(state);
        logger_log(logger, LOGGER_DEBUG, "video renderer_start: renderer %d %p state %s", i, renderer_type[i], state_name);
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

/* use this to cycle the jpeg renderer to remove expired coverart when no new coverart has replaced it */
int video_renderer_cycle() {
    if (!renderer || !strstr(renderer->codec, jpeg)) {
        return -1;
    }
    GstState state, pending_state, target_state;
    GstStateChangeReturn ret;
    gst_element_get_state(renderer->pipeline, &state, NULL, 0);
    logger_log(logger, LOGGER_DEBUG, "renderer_cycle renderer %p: initial pipeline state is %s", renderer,
               gst_element_state_get_name(state));

    for (int i = 0 ; i < 2; i++) {
        int count = 0;
        if (i == 0 ) {
            target_state = GST_STATE_NULL;
            video_renderer_stop();
        } else {
            target_state = GST_STATE_PLAYING;
            gst_element_set_state (renderer->pipeline, target_state);
        }
        while (state != target_state) {
            ret = gst_element_get_state(renderer->pipeline, &state, &pending_state, 1000 * GST_MSECOND);
            if (ret == GST_STATE_CHANGE_SUCCESS) {
                logger_log(logger, LOGGER_DEBUG, "current pipeline state is %s", gst_element_state_get_name(state));
                if (pending_state != GST_STATE_VOID_PENDING) {
                    logger_log(logger, LOGGER_DEBUG, "pending pipeline state is %s", gst_element_state_get_name(pending_state));
                }
            } else if (ret == GST_STATE_CHANGE_FAILURE) {
                logger_log(logger, LOGGER_ERR, "pipeline %s: state change to %s failed", renderer->codec, gst_element_state_get_name(target_state));
                count++;
                if (count > 10) {
                    return -1;
                }
            } else if (ret == GST_STATE_CHANGE_ASYNC) {
                logger_log(logger, LOGGER_DEBUG, "state change to %s is asynchronous, waiting for completion ...",
                           gst_element_state_get_name(target_state));
            }
        }
    }
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    GstBuffer *buffer = NULL;
    if (type_jpeg == -1) {
        return;
    }
    if (renderer && !strcmp(renderer->codec, jpeg)) {
        buffer = gst_buffer_new_allocate(NULL, *data_len, NULL);
	g_assert(buffer != NULL);
        gst_buffer_fill(buffer, 0, data, *data_len);
        gst_app_src_push_buffer (GST_APP_SRC(renderer->appsrc), buffer);
    }  
}

uint64_t video_renderer_render_buffer(unsigned char* data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    GstBuffer *buffer = NULL;
    GstClockTime pts = (GstClockTime) *ntp_time; /*now in nsecs */
    //GstClockTimeDiff latency = GST_CLOCK_DIFF(gst_element_get_current_clock_time (renderer->appsrc), pts);
    if (sync) {
        if (pts >= gst_video_pipeline_base_time) {
            pts -= gst_video_pipeline_base_time;
        } else {
            // adjust timestamps to be >= gst_video_pipeline_base time
            logger_log(logger, LOGGER_DEBUG, "*** invalid ntp_time < gst_video_pipeline_base_time\n%8.6f ntp_time\n%8.6f base_time",
                       ((double) *ntp_time) / SECOND_IN_NSECS, ((double) gst_video_pipeline_base_time) / SECOND_IN_NSECS);
            return  (uint64_t)  gst_video_pipeline_base_time - pts;
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
        if (!renderer || !(renderer->appsrc)) {
            logger_log(logger, LOGGER_DEBUG, "*** no video renderer found");
            return 0;
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
    return 0;
}

void video_renderer_flush() {
}

void video_renderer_hls_ready() {
    GstState state;
    GstStateChangeReturn ret;
    if (renderer && hls_video) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_hls_ready");
        ret = gst_element_set_state (renderer->pipeline, GST_STATE_READY);
        logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                   gst_element_state_change_return_get_name(ret));
        gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
    }
}

void video_renderer_stop() {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"video_renderer_stop");
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
        //gst_element_set_state (renderer->playbin, GST_STATE_NULL);
     }
}

static void video_renderer_destroy_instance(video_renderer_t *renderer) {
    if (renderer) {
        logger_log(logger, LOGGER_DEBUG,"destroying renderer instance %p codec=%s ", renderer, renderer->codec);
        GstState state;
        GstStateChangeReturn ret;
        gst_element_get_state(renderer->pipeline, &state, NULL, 100 * GST_MSECOND);
        logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        if (state != GST_STATE_NULL) {
            if (!hls_video) {
                gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
            }
            ret = gst_element_set_state (renderer->pipeline, GST_STATE_NULL);
            logger_log(logger, LOGGER_DEBUG,"pipeline_state_change_return: %s",
                       gst_element_state_change_return_get_name(ret));
            gst_element_get_state(renderer->pipeline, &state, NULL, 1000 * GST_MSECOND);
            logger_log(logger, LOGGER_DEBUG,"pipeline state is %s", gst_element_state_get_name(state));
        }
        if (renderer->appsrc) {
            gst_object_unref (renderer->appsrc);
            renderer->appsrc = NULL;
        }
        gst_object_unref(renderer->bus);
        gst_object_unref(renderer->pipeline);
#ifdef X_DISPLAY_FIX
        if (renderer->gst_window){
	  // free_X11_Display(renderer->gst_window);   without this, a memory leak; with it, a coredump
            free(renderer->gst_window);
            renderer->gst_window = NULL;
        }
#endif
        if (renderer->uri) {
            free(renderer->uri);
        }
        free (renderer);
        renderer = NULL;
        logger_log(logger, LOGGER_DEBUG,"renderer destroyed\n");	
    }
}

void video_renderer_destroy() {
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i]) {
            video_renderer_destroy_instance(renderer_type[i]);
        }
    }
}

static void get_stream_status_name(GstStreamStatusType type, char *name, size_t len) {
    switch (type) {
    case GST_STREAM_STATUS_TYPE_CREATE:
        strncpy(name, "CREATE", len);
        return;
    case GST_STREAM_STATUS_TYPE_ENTER:
        strncpy(name, "ENTER", len);
        return;
    case GST_STREAM_STATUS_TYPE_LEAVE:
        strncpy(name, "LEAVE", len);
        return;
    case GST_STREAM_STATUS_TYPE_DESTROY:
        strncpy(name, "DESTROY", len);
        return;
    case GST_STREAM_STATUS_TYPE_START:
        strncpy(name, "START", len);
        return;
    case GST_STREAM_STATUS_TYPE_PAUSE:
        strncpy(name, "PAUSE", len);
        return;
    case GST_STREAM_STATUS_TYPE_STOP:
        strncpy(name, "STOP", len);
        return;
    default:
        strncpy(name, "", len);
        return;
    }
}

static void hls_video_seek_to_start_position(GstElement *pipeline) {
    if (hls_requested_start_position && hls_seek_enabled && hls_requested_start_position >= hls_seek_start
        && hls_requested_start_position  <= hls_seek_end) {
        g_print("***************** seek to hls_requested_start_position %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(hls_requested_start_position));
        if (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
				 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, hls_requested_start_position)) {
            hls_requested_start_position = 0;
        } else {
            g_print("*** seek to requested_start_position failed\n"); 
        }
    } 
}

static gboolean gstreamer_video_pipeline_bus_callback(GstBus *bus, GstMessage *message, void *loop) {
    GstState old_state, new_state;
    const gchar no_state[] = "";
    const gchar *old_state_name = no_state, *new_state_name = no_state;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STATE_CHANGED) {
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        old_state_name = gst_element_state_get_name (old_state);
        new_state_name = gst_element_state_get_name (new_state);
    }

    /* identify which pipeline sent the message */ 
    int type = -1;
    for (int i = 0 ; i < n_renderers ; i ++ ) {
        if (renderer_type[i] && renderer_type[i]->bus == bus) {
            type = i;
            break;
        }
    }

    /* if the bus sending the message is not found, the renderer may already have been destroyed */
    if (type == -1) {
        if (logger_debug) {
            g_print("GStreamer(UNKNOWN, now destroyed?) bus message: %s %s %s %s\n",
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }     
        return TRUE;
    }

    video_renderer_t *renderer = renderer_type[type];

    gint64 pos = -1;
    if (hls_video) {
        gst_element_query_position (renderer->pipeline, GST_FORMAT_TIME, &pos);
        if (hls_requested_start_position && pos >= hls_requested_start_position) {
            hls_requested_start_position = 0;
        }
    }

    if (logger_debug) {
        gchar *name = NULL;
        GstElement *element = NULL;
        gchar type_name[8] = { 0 };
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_STATUS) {
            GstStreamStatusType type;
            gst_message_parse_stream_status(message, &type, &element);
            name = gst_element_get_name(element);
            get_stream_status_name(type, type_name, 8);
            old_state_name = name;
            new_state_name = type_name;
        }
        if (GST_CLOCK_TIME_IS_VALID(pos)) {
            g_print("GStreamer %s  bus message %s %s %s %s; position: %" GST_TIME_FORMAT "\n" ,renderer->codec,
                     GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name, GST_TIME_ARGS(pos));
        } else {
            g_print("GStreamer %s bus message %s %s %s %s\n", renderer->codec,
                    GST_MESSAGE_SRC_NAME(message), GST_MESSAGE_TYPE_NAME(message), old_state_name, new_state_name);
        }
        if (name) {
            g_free(name);
        }
    }

    if (hls_video && !GST_CLOCK_TIME_IS_VALID(hls_duration)) {
        gst_element_query_duration (renderer->pipeline, GST_FORMAT_TIME, &hls_duration);
    }

    if (hls_requested_start_position && hls_seek_enabled) {
        hls_video_seek_to_start_position(renderer->pipeline);
    }

    switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_DURATION:
        hls_duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_BUFFERING:
        if (hls_video) {
            gint percent = -1;
            gst_message_parse_buffering(message, &percent);
            hls_buffer_empty = TRUE;
            hls_buffer_full = FALSE;
            if (percent > 0) {
                hls_buffer_empty = FALSE;
                renderer->buffering_level = percent;
                logger_log(logger, LOGGER_DEBUG, "Buffering :%d percent done", percent);
                if (percent < 100) {
                    gst_element_set_state (renderer->pipeline, GST_STATE_PAUSED);
                } else {
                    hls_buffer_full = TRUE;
                    gst_element_set_state (renderer->pipeline, GST_STATE_PLAYING);
                }
            }
        }
	break;      
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *debug = NULL;
        gboolean flushing = FALSE;
        gboolean closed_window = FALSE;
        gst_message_parse_error (message, &err, &debug);
        logger_log(logger, LOGGER_INFO, "GStreamer error (video): %s %s", GST_MESSAGE_SRC_NAME(message),err->message);
        if (strstr(err->message, "Output window was closed")) {
            closed_window = TRUE;
        }
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
        if (renderer->appsrc) {
            gst_app_src_end_of_stream (GST_APP_SRC(renderer->appsrc));
        }
        if (!hls_video || closed_window) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            g_main_loop_quit( (GMainLoop *) loop);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        /* end-of-stream */
        logger_log(logger, LOGGER_INFO, "GStreamer: End-Of-Stream (video)");
        if (hls_video) {
            gst_bus_set_flushing(bus, TRUE);
            gst_element_set_state (renderer->pipeline, GST_STATE_READY);
            renderer->eos = TRUE;
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (hls_video && strstr(GST_MESSAGE_SRC_NAME(message), "hls-playbin")) {
            GstState old_state, new_state;
            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if (logger_debug) {
            g_print ("****** hls_playbin: Element %s changed state from %s to %s.\n", GST_OBJECT_NAME (message->src),
                     gst_element_state_get_name (old_state),
                     gst_element_state_get_name (new_state));
            } 
            if (new_state != GST_STATE_PLAYING) {
                hls_playing = FALSE;
                break;
            } 
            hls_playing = TRUE;
            GstQuery *query = NULL;
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

            if (hls_requested_start_position && hls_seek_enabled) {
                hls_video_seek_to_start_position(renderer->pipeline);
            }

        }
        if (renderer->autovideo) {
            char *sink = strstr(GST_MESSAGE_SRC_NAME(message), "-actual-sink-");
            if (sink) {
                sink += strlen("-actual-sink-");
                if (strstr(GST_MESSAGE_SRC_NAME(message), renderer->codec)) {
                    logger_log(logger, LOGGER_DEBUG, "GStreamer: automatically-selected videosink"
                               " (renderer %d: %s) is \"%ssink\"", renderer->id + 1,
                               renderer->codec, sink);
#ifdef X_DISPLAY_FIX
                    renderer->use_x11 = (strstr(sink, "ximage") || strstr(sink, "xvimage"));
#endif
                    renderer->autovideo = false;
                }
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
                    const gchar *key = NULL;
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

int video_renderer_choose_codec (bool video_is_jpeg, bool video_is_h265) {
    video_renderer_t *renderer_used = NULL;
    g_assert(!hls_video);
    if (video_is_jpeg) {
        g_assert(type_jpeg != -1);
        renderer_used = renderer_type[type_jpeg];
    } else {
        if (video_is_h265) {
            if (type_265 == -1) {
                logger_log(logger, LOGGER_ERR, "video is h265 but the -h265 option was not used");
                return -1;
            }
            renderer_used = renderer_type[type_265];
        } else {
            g_assert(type_264 != -1);
            renderer_used = renderer_type[type_264];
        }
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
    if (strstr(renderer->codec, h265)) {
        logger_log(logger, LOGGER_INFO, "*** video format is h265 high definition (HD/4K) video %dx%d", width, height);
    }
    /* destroy unused renderers */
    for (int i = 0; i < n_renderers; i++) {
        if (renderer_type[i] == renderer) {
            continue;
        }
	if (renderer_type[i]) {
            video_renderer_t *renderer_unused = renderer_type[i];
            renderer_type[i] = NULL;
            video_renderer_destroy_instance(renderer_unused);
        }
    }
    return 0;
}
    

bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    gint64 pos = 0;
    GstState state;
    *duration = 0.0;
    *position = -1.0;
    *seek_start = 0.0;
    *seek_duration = 0.0;
    *rate = 0.0f;
    if (!renderer) {
        return true;
    }

    if (hls_seek_enabled) {
        *seek_start = ((double) hls_seek_start) / GST_SECOND;
        *seek_duration = ((double) (hls_seek_end - hls_seek_start)) / GST_SECOND;     
    }

    *buffer_empty = (bool) hls_buffer_empty;
    *buffer_full = (bool) hls_buffer_full;
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

    logger_log(logger, LOGGER_DEBUG, "******* video_get_playback_info: position %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " %s rate %f *****",
               GST_TIME_ARGS (pos), GST_TIME_ARGS (hls_duration), gst_element_state_get_name(state), *rate);

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
                                            gstreamer_video_pipeline_bus_callback, (gpointer) loop);    
}

bool video_renderer_eos_watch() {
    if (hls_video && renderer->eos) {
        renderer->eos = FALSE;
	return true;
    }
    return false; 
}
