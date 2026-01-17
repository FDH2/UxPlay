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
static gchar *output_filename = NULL;
static int file_count = 0;

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
    gboolean is_h265;
    unsigned char audio_ct;
} mux_renderer_t;

static mux_renderer_t *renderer = NULL;

static const char h264_caps[] = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[] = "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

static const char aac_eld_caps[] = "audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                                "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

void mux_renderer_init(logger_t *render_logger, const char *filename) {
    logger = render_logger;
    if (output_filename) {
        g_free(output_filename);
    }
    output_filename = g_strdup(filename);
    file_count = 0;
    logger_log(logger, LOGGER_INFO, "Mux renderer initialised: %s", filename);
}

void mux_renderer_choose_audio_codec(unsigned char audio_ct) {
    if (renderer && renderer->audio_ct != audio_ct) {
        logger_log(logger, LOGGER_INFO, "Audio codec changed, restarting mux renderer");
        mux_renderer_stop();
    }
    if (!renderer) {
        renderer = g_new0(mux_renderer_t, 1);
        renderer->base_time = GST_CLOCK_TIME_NONE;
        renderer->audio_ct = 8;
    }
    renderer->audio_ct = audio_ct;
    logger_log(logger, LOGGER_DEBUG, "Mux renderer audio codec: ct=%d", audio_ct);
}

void mux_renderer_choose_video_codec(bool is_h265) {
    if (renderer && renderer->pipeline && renderer->is_h265 != is_h265) {
        logger_log(logger, LOGGER_INFO, "Video codec changed, restarting mux renderer");
        mux_renderer_stop();
    }
    if (!renderer) {
        renderer = g_new0(mux_renderer_t, 1);
        renderer->base_time = GST_CLOCK_TIME_NONE;
        renderer->audio_ct = 8;
    }
    renderer->is_h265 = is_h265;
    logger_log(logger, LOGGER_DEBUG, "Mux renderer video codec: h265=%d", is_h265);
    mux_renderer_start();
}

void mux_renderer_start(void) {
    GError *error = NULL;
    gchar *filename;
    GstCaps *video_caps = NULL;
    GstCaps *audio_caps = NULL;
    
    if (!renderer) {
        logger_log(logger, LOGGER_ERR, "Mux renderer not initialised");
        return;
    }
    
    if (renderer->pipeline) {
        logger_log(logger, LOGGER_DEBUG, "Mux renderer already running");
        return;
    }
    
    file_count++;
    filename = g_strdup_printf("%s.%d.mp4", output_filename, file_count);
    
    GString *launch = g_string_new("");
    
    g_string_append(launch, "appsrc name=video_src format=time is-live=true ! queue ! ");
    if (renderer->is_h265) {
        g_string_append(launch, "h265parse ! ");
    } else {
        g_string_append(launch, "h264parse ! ");
    }
    g_string_append(launch, "mux. ");
    
    g_string_append(launch, "appsrc name=audio_src format=time is-live=true ! queue ! ");
    switch (renderer->audio_ct) {
    case 8:
        g_string_append(launch, "avdec_aac ! ");
        break;
    case 2:
        g_string_append(launch, "avdec_alac ! ");
        break;
    default:
        g_string_append(launch, "avdec_aac ! ");
        break;
    }
    g_string_append(launch, "audioconvert ! audioresample ! ");
    g_string_append(launch, "avenc_aac ! aacparse ! mux. ");
    
    g_string_append(launch, "mp4mux name=mux ! filesink name=filesink location=");
    g_string_append(launch, filename);
    
    logger_log(logger, LOGGER_DEBUG, "Mux pipeline: %s", launch->str);
    
    renderer->pipeline = gst_parse_launch(launch->str, &error);
    g_string_free(launch, TRUE);
    
    if (error) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline error: %s", error->message);
        g_clear_error(&error);
        g_free(filename);
        return;
    }
    
    renderer->video_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_src");
    renderer->audio_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_src");
    renderer->filesink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "filesink");
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    
    if (renderer->is_h265) {
        video_caps = gst_caps_from_string(h265_caps);
    } else {
        video_caps = gst_caps_from_string(h264_caps);
    }
    g_object_set(renderer->video_appsrc, "caps", video_caps, NULL);
    gst_caps_unref(video_caps);
    
    switch (renderer->audio_ct) {
    case 8:
        audio_caps = gst_caps_from_string(aac_eld_caps);
        break;
    case 2:
        audio_caps = gst_caps_from_string(alac_caps);
        break;
    default:
        audio_caps = gst_caps_from_string(aac_eld_caps);
        break;
    }
    g_object_set(renderer->audio_appsrc, "caps", audio_caps, NULL);
    gst_caps_unref(audio_caps);
    
    gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    renderer->base_time = GST_CLOCK_TIME_NONE;
    renderer->first_video_time = GST_CLOCK_TIME_NONE;
    renderer->first_audio_time = GST_CLOCK_TIME_NONE;
    renderer->audio_started = FALSE;
    
    logger_log(logger, LOGGER_INFO, "Started recording to: %s", filename);
    g_free(filename);
}

void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time) {
    if (!renderer || !renderer->pipeline || !renderer->video_appsrc) return;
    
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) return;
    
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

void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time) {
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
                logger_log(logger, LOGGER_INFO, "Inserted %.2f seconds of silence before audio", 
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

void mux_renderer_stop(void) {
    if (!renderer || !renderer->pipeline) return;
    
    gst_app_src_end_of_stream(GST_APP_SRC(renderer->video_appsrc));
    gst_app_src_end_of_stream(GST_APP_SRC(renderer->audio_appsrc));
    
    GstMessage *msg = gst_bus_timed_pop_filtered(renderer->bus, 5 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (msg) {
        gst_message_unref(msg);
    }
    
    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    
    gst_object_unref(renderer->video_appsrc);
    renderer->video_appsrc = NULL;
    gst_object_unref(renderer->audio_appsrc);
    renderer->audio_appsrc = NULL;
    gst_object_unref(renderer->filesink);
    renderer->filesink = NULL;
    gst_object_unref(renderer->bus);
    renderer->bus = NULL;
    gst_object_unref(renderer->pipeline);
    renderer->pipeline = NULL;
    
    renderer->base_time = GST_CLOCK_TIME_NONE;
    logger_log(logger, LOGGER_INFO, "Stopped recording");
}

void mux_renderer_destroy(void) {
    mux_renderer_stop();
    if (renderer) {
        g_free(renderer);
        renderer = NULL;
    }
    if (output_filename) {
        g_free(output_filename);
        output_filename = NULL;
    }
}
