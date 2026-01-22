/**
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
#include "mux_renderer.h"

#define SECOND_IN_NSECS 1000000000UL

static logger_t *logger = NULL;
static const char *output_filename = NULL;
static int file_count = 0;
static gboolean no_audio = FALSE;
static gboolean no_video = FALSE;
static gboolean audio_is_alac = FALSE;
static gboolean video_is_h265 = FALSE;

typedef struct mux_renderer_s {
    GstElement *pipeline;
    GstElement *video_appsrc;
    GstElement *audio_appsrc;
    GstElement *filesink;
    GstBus *bus;
    GstClockTime base_time;
    GstClockTime first_video_time;
    GstClockTime first_audio_time;
    gboolean audio_started;
    gboolean is_alac;
    gboolean is_h265;
} mux_renderer_t;

static mux_renderer_t *renderer = NULL;

static const char h264_caps[] = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[] = "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

static const char aac_eld_caps[] = "audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                                "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

/* called once when uxplay first starts */
void mux_renderer_init(logger_t *render_logger, const char *filename, bool use_audio, bool use_video) {
    logger = render_logger;
    no_audio = !use_audio;
    no_video = !use_video;
    if (no_audio && no_video) {
        logger_log(logger, LOGGER_INFO, "both audio and video rendering are disabled: nothing to record: (not starting mux renderer)");
        return;
    } else if (no_audio) {
        logger_log(logger, LOGGER_INFO, "audio rendering is disabled: video only will be recorded");
    } else if (no_video) {
        logger_log(logger, LOGGER_INFO, "video rendering is disabled: audio only will be recorded");
    }
    output_filename = filename ;
    file_count = 0;
    logger_log(logger, LOGGER_INFO, "Mux renderer initialized: %s", output_filename);
}

static
void mux_renderer_start(void) {
    GError *error = NULL;
    GstCaps *video_caps = NULL;
    GstCaps *audio_caps = NULL;

    if (renderer && renderer->pipeline) {
        logger_log(logger, LOGGER_DEBUG, "Mux renderer already running");
        return;
    }

    mux_renderer_destroy();
    
    renderer = g_new0(mux_renderer_t, 1);
    renderer->base_time = GST_CLOCK_TIME_NONE;
    renderer->first_video_time = GST_CLOCK_TIME_NONE;
    renderer->first_audio_time = GST_CLOCK_TIME_NONE;
    renderer->audio_started = FALSE;
    renderer->pipeline = NULL;
    renderer->video_appsrc = NULL;
    renderer->audio_appsrc = NULL;
    renderer->is_alac = audio_is_alac;
    renderer->is_h265 = video_is_h265;

    file_count++;
    GString *filename = g_string_new("");
    g_string_append(filename, g_strdup_printf("%s.%d.", output_filename, file_count));
    if (!no_video && !audio_is_alac) {
        if (video_is_h265) {
            g_string_append(filename,"H265.");
        } else {
            g_string_append(filename,"H264.");
        }
    } if (!no_audio) {
        if (audio_is_alac) {
            g_string_append(filename,"ALAC.");
        } else {
            g_string_append(filename,"AAC.");
        }
    }
    g_string_append(filename, "mp4");
    
    GString *launch = g_string_new("");

    if (!no_video && !audio_is_alac) {
        g_string_append(launch, "appsrc name=video_src format=time is-live=true ! queue ! ");
        if (video_is_h265) {
            g_string_append(launch, "h265parse ! ");
        } else {
            g_string_append(launch, "h264parse ! ");
        }
        g_string_append(launch, "mux. ");
    }
    if (!no_audio) {
        g_string_append(launch, "appsrc name=audio_src format=time is-live=true ! queue ! ");
        if (!audio_is_alac ) {
            g_string_append(launch, "aacparse ! queue ! ");
        }
        g_string_append(launch, "mux. ");
    }
    g_string_append(launch, "mp4mux name=mux ! filesink name=filesink location=");
    g_string_append(launch, filename->str);

    logger_log(logger, LOGGER_DEBUG, "created Mux pipeline: %s", launch->str);

    renderer->pipeline = gst_parse_launch(launch->str, &error);

    g_string_free(launch, TRUE);
    if (error) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline error: %s", error->message);
        g_clear_error(&error);
        g_free(filename);
        return;
    }

    if (!no_video && !audio_is_alac) {
        renderer->video_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_src");
        if (renderer->is_h265) {
            video_caps = gst_caps_from_string(h265_caps);
        } else {
            video_caps = gst_caps_from_string(h264_caps);
        }
        g_object_set(renderer->video_appsrc, "caps", video_caps, NULL);
        gst_caps_unref(video_caps);
    }

    if (!no_audio) {
        renderer->audio_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_src");
        if (audio_is_alac) {
            audio_caps = gst_caps_from_string(alac_caps);
        } else {
            audio_caps = gst_caps_from_string(aac_eld_caps);
        }
        g_object_set(renderer->audio_appsrc, "caps", audio_caps, NULL);
        gst_caps_unref(audio_caps);	
    }

    renderer->filesink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "filesink");
    renderer->bus = gst_element_get_bus(renderer->pipeline);

    gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    logger_log(logger, LOGGER_INFO, "Started recording to: %s", filename->str);
    g_string_free(filename, TRUE);
}

/* called by audio_get_format callback in uxplay.cpp, from raop_handlers.h */
void mux_renderer_choose_audio_codec(unsigned char audio_ct) {
    if (no_audio) {
        return;
    }
    audio_is_alac = (audio_ct == 2);
    if (renderer && renderer->is_alac != audio_is_alac) {
        logger_log(logger, LOGGER_DEBUG, "Audio codec changed, recreating mux renderer");
        mux_renderer_destroy();
    }
    if (audio_ct == 2) {
        mux_renderer_start();
    }
}

/* called by video_set_codec calback in uxplay.cpp, from raop_rtp_mirror */
void mux_renderer_choose_video_codec(bool is_h265) {
    video_is_h265 = is_h265;
    if (renderer && renderer->pipeline && renderer->is_h265 != video_is_h265) {
        logger_log(logger, LOGGER_DEBUG, "Video codec changed, recreating mux renderer");
        mux_renderer_destroy();
    }
    logger_log(logger, LOGGER_DEBUG, "Mux renderer video codec: h265=%s", is_h265 ? "true" : "false");
    mux_renderer_start();
}

/* called by video_process callback in uxplay.cpp*/
void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time) {
    if (no_video) {
        return;
    }
    if (!renderer || !renderer->pipeline || !renderer->video_appsrc) return;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) {
        return;
    }
    gst_buffer_fill(buffer, 0, data, data_len);

    if (renderer->base_time == GST_CLOCK_TIME_NONE) {
        renderer->base_time = (GstClockTime)ntp_time;
        renderer->first_video_time = (GstClockTime)ntp_time;
    }

    GstClockTime pts = (GstClockTime)ntp_time - renderer->base_time;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    gst_app_src_push_buffer(GST_APP_SRC(renderer->video_appsrc), buffer);
}

/* called by audio_process callback in uxplay.cpp*/
void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time) {
    if (no_audio) {
        return;
    }
    if (!renderer || !renderer->pipeline || !renderer->audio_appsrc) return;

    if (!renderer->audio_started && renderer->first_video_time != GST_CLOCK_TIME_NONE) {
        renderer->audio_started = TRUE;
        renderer->first_audio_time = (GstClockTime)ntp_time;
        if (renderer->first_audio_time > renderer->first_video_time) {
            GstClockTime silence_duration = renderer->first_audio_time - renderer->first_video_time;
            guint64 num_samples = (silence_duration * 44100) / GST_SECOND;
            gsize silence_size = num_samples * 2 * 2;            
            GstBuffer *silence_buffer = gst_buffer_new_allocate(NULL, silence_size, NULL);
            if (silence_buffer) {
                GstMapInfo map;
                if (gst_buffer_map(silence_buffer, &map, GST_MAP_WRITE)) {
                    memset(map.data, 0, map.size);
                    gst_buffer_unmap(silence_buffer, &map);
                }
                GST_BUFFER_PTS(silence_buffer) = 0;
                GST_BUFFER_DTS(silence_buffer) = 0;
                GST_BUFFER_DURATION(silence_buffer) = silence_duration;
                gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), silence_buffer);
                logger_log(logger, LOGGER_DEBUG, "Inserted %.2f seconds of silence before audio", 
                          (double)silence_duration / GST_SECOND);
            }
        }
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) return;

    gst_buffer_fill(buffer, 0, data, data_len);

    if (renderer->base_time == GST_CLOCK_TIME_NONE) {
        renderer->base_time = (GstClockTime)ntp_time;
    }

    GstClockTime pts = (GstClockTime)ntp_time - renderer->base_time;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;

    gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), buffer);
}

/* called by conn_destroy callback in uxplay.cpp, and when video resets */
void mux_renderer_stop(void) {
    if (!renderer || !renderer->pipeline) return;

    if (renderer->video_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->video_appsrc));
    }
    if (renderer->audio_appsrc) {
        gst_app_src_end_of_stream(GST_APP_SRC(renderer->audio_appsrc));
    }

    GstMessage *msg = gst_bus_timed_pop_filtered(renderer->bus, 5 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
        gst_message_unref(msg);
    }

    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);

    if (renderer->video_appsrc) {
        gst_object_unref(renderer->video_appsrc);
        renderer->video_appsrc = NULL;
    }
    if (renderer->audio_appsrc) {
        gst_object_unref(renderer->audio_appsrc);
        renderer->audio_appsrc = NULL;
    }
    gst_object_unref(renderer->filesink);
    renderer->filesink = NULL;
    gst_object_unref(renderer->bus);
    renderer->bus = NULL;
    gst_object_unref(renderer->pipeline);
    renderer->pipeline = NULL;

    renderer->base_time = GST_CLOCK_TIME_NONE;
    logger_log(logger, LOGGER_INFO, "Stopped recording");
    audio_is_alac = FALSE;
    video_is_h265 = FALSE;
}

void mux_renderer_destroy(void) {
    mux_renderer_stop();
    if (renderer) {
        g_free(renderer);
        renderer = NULL;
    }
}
