/**
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-25 F. Duncanh
 *
 * Native macOS Video Renderer using VideoToolbox + Metal
 * This is an alternative to the GStreamer-based renderer,
 * providing lower latency on macOS by using native Apple frameworks.
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

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include <pthread.h>
#include <stdbool.h>
#include <mach/mach_time.h>

// Define guint for compatibility with the header (GLib type)
typedef unsigned int guint;

#include "video_renderer.h"

// State
static logger_t *g_logger = NULL;
static bool g_initialized = false;
static bool g_running = false;
static bool g_paused = false;
static videoflip_t g_flip = NONE;
static videoflip_t g_rotation = NONE;

// VideoToolbox decoder
static VTDecompressionSessionRef g_decoder_session = NULL;
static CMFormatDescriptionRef g_format_desc = NULL;
static bool g_is_h265 = false;

// SPS/PPS storage for format description
static uint8_t *g_sps = NULL;
static size_t g_sps_size = 0;
static uint8_t *g_pps = NULL;
static size_t g_pps_size = 0;

// Display window and layer
static NSWindow *g_window = NULL;
static MTKView *g_metal_view = NULL;
static id<MTLDevice> g_metal_device = NULL;
static id<MTLCommandQueue> g_command_queue = NULL;
static id<MTLTexture> g_current_texture = NULL;
static CVMetalTextureCacheRef g_texture_cache = NULL;

// Threading
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static CVPixelBufferRef g_pending_frame = NULL;
static uint64_t g_base_time = 0;
static bool g_first_frame = true;

// Statistics for logging
static uint64_t g_frames_received = 0;
static uint64_t g_frames_decoded = 0;
static uint64_t g_frames_rendered = 0;
static uint64_t g_decode_errors = 0;
static uint64_t g_last_stats_time = 0;

// Forward declarations
static void create_decoder_session(void);
static void destroy_decoder_session(void);
static void decompress_callback(void *decompressionOutputRefCon,
                                void *sourceFrameRefCon,
                                OSStatus status,
                                VTDecodeInfoFlags infoFlags,
                                CVImageBufferRef imageBuffer,
                                CMTime presentationTimeStamp,
                                CMTime presentationDuration);

#pragma mark - Logging

static void log_msg(int level, const char *fmt, ...) {
    if (!g_logger) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    logger_log(g_logger, level, "[NATIVE] %s", buffer);
}

static void log_stats(void) {
    uint64_t now = mach_absolute_time();
    // Log stats every ~2 seconds
    if (now - g_last_stats_time > 2000000000ULL) {
        log_msg(LOGGER_INFO, "STATS: recv=%llu decoded=%llu rendered=%llu errors=%llu",
                g_frames_received, g_frames_decoded, g_frames_rendered, g_decode_errors);
        g_last_stats_time = now;
    }
}

#pragma mark - Metal Renderer

@interface MetalRenderer : NSObject <MTKViewDelegate>
@end

@implementation MetalRenderer

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    // Handle resize if needed
}

- (void)drawInMTKView:(MTKView *)view {
    pthread_mutex_lock(&g_frame_mutex);
    CVPixelBufferRef pixelBuffer = g_pending_frame;
    if (pixelBuffer) {
        CVPixelBufferRetain(pixelBuffer);
    }
    pthread_mutex_unlock(&g_frame_mutex);

    if (!pixelBuffer) return;

    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
        id<CAMetalDrawable> drawable = [view currentDrawable];

        if (drawable && commandBuffer) {
            // Create texture from pixel buffer
            size_t width = CVPixelBufferGetWidth(pixelBuffer);
            size_t height = CVPixelBufferGetHeight(pixelBuffer);

            CVMetalTextureRef metalTexture = NULL;
            CVReturn result = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                g_texture_cache,
                pixelBuffer,
                NULL,
                MTLPixelFormatBGRA8Unorm,
                width, height,
                0,
                &metalTexture
            );

            if (result == kCVReturnSuccess && metalTexture) {
                id<MTLTexture> texture = CVMetalTextureGetTexture(metalTexture);

                // Use Metal Performance Shaders for high-quality scaling
                MPSImageBilinearScale *scaler = [[MPSImageBilinearScale alloc] initWithDevice:g_metal_device];

                // Calculate scale transform to fit drawable while maintaining aspect ratio
                double srcAspect = (double)width / (double)height;
                double dstAspect = (double)drawable.texture.width / (double)drawable.texture.height;

                double scaleX, scaleY, translateX = 0, translateY = 0;

                if (srcAspect > dstAspect) {
                    // Source is wider - fit to width
                    scaleX = (double)drawable.texture.width / (double)width;
                    scaleY = scaleX;
                    translateY = (drawable.texture.height - height * scaleY) / 2.0;
                } else {
                    // Source is taller - fit to height
                    scaleY = (double)drawable.texture.height / (double)height;
                    scaleX = scaleY;
                    translateX = (drawable.texture.width - width * scaleX) / 2.0;
                }

                MPSScaleTransform transform = {
                    .scaleX = scaleX,
                    .scaleY = scaleY,
                    .translateX = translateX,
                    .translateY = translateY
                };
                scaler.scaleTransform = &transform;

                // Clear the drawable first (for letterboxing)
                MTLRenderPassDescriptor *clearPass = [MTLRenderPassDescriptor renderPassDescriptor];
                clearPass.colorAttachments[0].texture = drawable.texture;
                clearPass.colorAttachments[0].loadAction = MTLLoadActionClear;
                clearPass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                clearPass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> clearEncoder = [commandBuffer renderCommandEncoderWithDescriptor:clearPass];
                [clearEncoder endEncoding];

                // Scale and blit
                [scaler encodeToCommandBuffer:commandBuffer
                                sourceTexture:texture
                           destinationTexture:drawable.texture];

                [commandBuffer presentDrawable:drawable];
                [commandBuffer commit];

                g_frames_rendered++;
                CFRelease(metalTexture);
            }
        }
    }

    CVPixelBufferRelease(pixelBuffer);
}

@end

static MetalRenderer *g_metal_renderer = NULL;

#pragma mark - Window Management

static void create_window(const char *title) {
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            NSRect frame = NSMakeRect(100, 100, 1280, 720);

            NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                      NSWindowStyleMaskClosable |
                                      NSWindowStyleMaskResizable |
                                      NSWindowStyleMaskMiniaturizable;

            g_window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];

            [g_window setTitle:[NSString stringWithUTF8String:title ?: "UxPlay"]];
            [g_window setReleasedWhenClosed:NO];

            // Create Metal device and view
            g_metal_device = MTLCreateSystemDefaultDevice();
            if (!g_metal_device) {
                log_msg(LOGGER_ERR, "Failed to create Metal device");
                return;
            }

            g_metal_view = [[MTKView alloc] initWithFrame:frame device:g_metal_device];
            g_metal_view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
            g_metal_view.framebufferOnly = NO;
            g_metal_view.preferredFramesPerSecond = 60;

            g_metal_renderer = [[MetalRenderer alloc] init];
            g_metal_view.delegate = g_metal_renderer;

            g_command_queue = [g_metal_device newCommandQueue];

            // Create texture cache
            CVMetalTextureCacheCreate(kCFAllocatorDefault,
                                      NULL,
                                      g_metal_device,
                                      NULL,
                                      &g_texture_cache);

            [g_window setContentView:g_metal_view];
            [g_window makeKeyAndOrderFront:nil];

            log_msg(LOGGER_INFO, "Native macOS window created with Metal rendering");
        }
    });
}

static void destroy_window(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            if (g_window) {
                [g_window close];
                g_window = nil;
            }
            if (g_texture_cache) {
                CFRelease(g_texture_cache);
                g_texture_cache = NULL;
            }
            g_metal_view = nil;
            g_metal_renderer = nil;
            g_metal_device = nil;
            g_command_queue = nil;
        }
    });
}

#pragma mark - VideoToolbox Decoder

static bool parse_sps_pps(const uint8_t *data, size_t length) {
    // Find SPS and PPS NAL units in the data
    // NAL units are separated by 0x00 0x00 0x00 0x01

    size_t i = 0;
    while (i + 4 < length) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            size_t nal_start = i + 4;

            // Find end of this NAL (next start code or end of data)
            size_t nal_end = length;
            for (size_t j = nal_start + 1; j + 3 < length; j++) {
                if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1) {
                    nal_end = j;
                    break;
                }
            }

            size_t nal_size = nal_end - nal_start;
            if (nal_size > 0) {
                uint8_t nal_type = data[nal_start] & 0x1F;

                if (nal_type == 7) { // SPS
                    if (g_sps) free(g_sps);
                    g_sps = malloc(nal_size);
                    memcpy(g_sps, &data[nal_start], nal_size);
                    g_sps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found SPS, size=%zu", nal_size);
                } else if (nal_type == 8) { // PPS
                    if (g_pps) free(g_pps);
                    g_pps = malloc(nal_size);
                    memcpy(g_pps, &data[nal_start], nal_size);
                    g_pps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found PPS, size=%zu", nal_size);
                }
            }

            i = nal_end;
        } else {
            i++;
        }
    }

    return (g_sps != NULL && g_pps != NULL);
}

static void create_decoder_session(void) {
    if (g_decoder_session) return;
    if (!g_sps || !g_pps) return;

    OSStatus status;

    // Create format description from SPS/PPS
    const uint8_t *parameterSetPointers[2] = {g_sps, g_pps};
    const size_t parameterSetSizes[2] = {g_sps_size, g_pps_size};

    status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault,
        2,
        parameterSetPointers,
        parameterSetSizes,
        4,  // NAL unit length field size
        &g_format_desc
    );

    if (status != noErr) {
        log_msg(LOGGER_ERR, "Failed to create format description: %d", (int)status);
        return;
    }

    // Get dimensions
    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(g_format_desc);
    log_msg(LOGGER_INFO, "Video dimensions: %dx%d", dims.width, dims.height);

    // Decoder configuration
    CFMutableDictionaryRef decoder_config = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );

    // Output buffer attributes - request Metal-compatible pixel buffer
    CFMutableDictionaryRef buffer_attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks
    );

    int32_t pixel_format = kCVPixelFormatType_32BGRA;
    CFNumberRef pixel_format_ref = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
    CFDictionarySetValue(buffer_attrs, kCVPixelBufferPixelFormatTypeKey, pixel_format_ref);
    CFRelease(pixel_format_ref);

    // Enable Metal compatibility
    CFDictionarySetValue(buffer_attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);

    // Callback
    VTDecompressionOutputCallbackRecord callback = {
        .decompressionOutputCallback = decompress_callback,
        .decompressionOutputRefCon = NULL
    };

    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        g_format_desc,
        decoder_config,
        buffer_attrs,
        &callback,
        &g_decoder_session
    );

    CFRelease(decoder_config);
    CFRelease(buffer_attrs);

    if (status != noErr) {
        log_msg(LOGGER_ERR, "Failed to create decoder session: %d", (int)status);
        return;
    }

    // Set real-time mode for lowest latency
    VTSessionSetProperty(g_decoder_session,
                         kVTDecompressionPropertyKey_RealTime,
                         kCFBooleanTrue);

    log_msg(LOGGER_INFO, "VideoToolbox decoder session created (hardware accelerated)");
}

static void destroy_decoder_session(void) {
    if (g_decoder_session) {
        VTDecompressionSessionInvalidate(g_decoder_session);
        CFRelease(g_decoder_session);
        g_decoder_session = NULL;
    }
    if (g_format_desc) {
        CFRelease(g_format_desc);
        g_format_desc = NULL;
    }
}

static void decompress_callback(void *decompressionOutputRefCon,
                                void *sourceFrameRefCon,
                                OSStatus status,
                                VTDecodeInfoFlags infoFlags,
                                CVImageBufferRef imageBuffer,
                                CMTime presentationTimeStamp,
                                CMTime presentationDuration) {
    if (status != noErr) {
        g_decode_errors++;
        log_msg(LOGGER_ERR, "VideoToolbox decode error: %d (total errors: %llu)", (int)status, g_decode_errors);
        return;
    }

    if (!imageBuffer) {
        log_msg(LOGGER_WARNING, "Decode callback received NULL imageBuffer");
        return;
    }

    g_frames_decoded++;

    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);

    if (g_frames_decoded == 1) {
        log_msg(LOGGER_INFO, "First frame decoded! Resolution: %zux%zu", width, height);

        // Resize window to match video aspect ratio
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_window) {
                // Scale to fit nicely on screen (max 900 pixels tall for portrait)
                CGFloat scale = 1.0;
                if (height > width) {
                    // Portrait - scale to reasonable height
                    scale = 900.0 / height;
                } else {
                    // Landscape - scale to reasonable width
                    scale = 1200.0 / width;
                }

                CGFloat newWidth = width * scale;
                CGFloat newHeight = height * scale;

                NSRect frame = [g_window frame];
                frame.size.width = newWidth;
                frame.size.height = newHeight;
                [g_window setFrame:frame display:YES animate:YES];

                log_msg(LOGGER_INFO, "Window resized to %.0fx%.0f (scale %.2f)", newWidth, newHeight, scale);
            }
        });
    }

    // Store frame for rendering
    pthread_mutex_lock(&g_frame_mutex);
    if (g_pending_frame) {
        CVPixelBufferRelease(g_pending_frame);
    }
    g_pending_frame = CVPixelBufferRetain(imageBuffer);
    pthread_mutex_unlock(&g_frame_mutex);

    log_stats();
}

static bool decode_frame(const uint8_t *data, size_t length, uint64_t pts) {
    if (!g_decoder_session) return false;

    // Convert Annex-B to AVCC format (replace start codes with length)
    // For simplicity, we'll create a copy with AVCC format

    // Find all NAL units and calculate total size
    size_t avcc_size = 0;
    size_t nal_count = 0;

    for (size_t i = 0; i + 4 < length; ) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            size_t nal_start = i + 4;
            size_t nal_end = length;

            for (size_t j = nal_start; j + 3 < length; j++) {
                if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1) {
                    nal_end = j;
                    break;
                }
            }

            size_t nal_size = nal_end - nal_start;
            uint8_t nal_type = data[nal_start] & 0x1F;

            // Skip SPS/PPS, only include VCL NALs
            if (nal_type != 7 && nal_type != 8) {
                avcc_size += 4 + nal_size;  // 4-byte length + NAL
                nal_count++;
            }

            i = nal_end;
        } else {
            i++;
        }
    }

    if (avcc_size == 0) return false;

    // Create AVCC buffer
    uint8_t *avcc_data = malloc(avcc_size);
    size_t offset = 0;

    for (size_t i = 0; i + 4 < length; ) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            size_t nal_start = i + 4;
            size_t nal_end = length;

            for (size_t j = nal_start; j + 3 < length; j++) {
                if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1) {
                    nal_end = j;
                    break;
                }
            }

            size_t nal_size = nal_end - nal_start;
            uint8_t nal_type = data[nal_start] & 0x1F;

            if (nal_type != 7 && nal_type != 8) {
                // Write length (big-endian)
                avcc_data[offset++] = (nal_size >> 24) & 0xFF;
                avcc_data[offset++] = (nal_size >> 16) & 0xFF;
                avcc_data[offset++] = (nal_size >> 8) & 0xFF;
                avcc_data[offset++] = nal_size & 0xFF;

                // Write NAL data
                memcpy(&avcc_data[offset], &data[nal_start], nal_size);
                offset += nal_size;
            }

            i = nal_end;
        } else {
            i++;
        }
    }

    // Create CMBlockBuffer
    CMBlockBufferRef block_buffer = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        avcc_data,
        avcc_size,
        kCFAllocatorDefault,  // Will free avcc_data
        NULL,
        0,
        avcc_size,
        0,
        &block_buffer
    );

    if (status != noErr) {
        free(avcc_data);
        return false;
    }

    // Create CMSampleBuffer
    CMSampleBufferRef sample_buffer = NULL;
    const size_t sample_sizes[] = {avcc_size};

    CMSampleTimingInfo timing = {
        .duration = CMTimeMake(1, 60),
        .presentationTimeStamp = CMTimeMake(pts, 1000000000),
        .decodeTimeStamp = kCMTimeInvalid
    };

    status = CMSampleBufferCreate(
        kCFAllocatorDefault,
        block_buffer,
        true,
        NULL, NULL,
        g_format_desc,
        1,
        1, &timing,
        1, sample_sizes,
        &sample_buffer
    );

    CFRelease(block_buffer);

    if (status != noErr) {
        return false;
    }

    // Decode
    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression |
                               kVTDecodeFrame_1xRealTimePlayback;
    VTDecodeInfoFlags info_flags = 0;

    status = VTDecompressionSessionDecodeFrame(
        g_decoder_session,
        sample_buffer,
        flags,
        NULL,
        &info_flags
    );

    CFRelease(sample_buffer);

    return (status == noErr);
}

#pragma mark - Public API

void video_renderer_init(logger_t *render_logger, const char *server_name, videoflip_t videoflip[2],
                         const char *parser, const char *rtp_pipeline,
                         const char *decoder, const char *converter, const char *videosink,
                         const char *videosink_options, bool initial_fullscreen, bool video_sync,
                         bool h265_support, bool coverart_support, guint playbin_version, const char *uri) {
    g_logger = render_logger;
    g_flip = videoflip[0];
    g_rotation = videoflip[1];
    g_is_h265 = h265_support;
    g_first_frame = true;
    g_base_time = 0;

    // Create display window
    create_window(server_name);

    g_initialized = true;
    log_msg(LOGGER_INFO, "Native macOS video renderer initialized (VideoToolbox + Metal)");
}

void video_renderer_start(void) {
    g_running = true;
    g_paused = false;
    log_msg(LOGGER_DEBUG, "Video renderer started");
}

void video_renderer_stop(void) {
    g_running = false;
    destroy_decoder_session();
    log_msg(LOGGER_DEBUG, "Video renderer stopped");
}

void video_renderer_pause(void) {
    g_paused = true;
}

void video_renderer_resume(void) {
    g_paused = false;
}

int video_renderer_cycle(void) {
    // Nothing needed - Metal view handles its own render loop
    return 0;
}

bool video_renderer_is_paused(void) {
    return g_paused;
}

uint64_t video_renderer_render_buffer(unsigned char *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    g_frames_received++;

    if (!g_running || g_paused) {
        log_msg(LOGGER_DEBUG, "Renderer not running/paused, dropping frame");
        return 0;
    }

    // Validate data
    if (*data_len < 4) {
        log_msg(LOGGER_ERR, "Video data too short: %d bytes", *data_len);
        return 0;
    }

    if (data[0] != 0) {
        log_msg(LOGGER_ERR, "Invalid video data (decryption failed?) first byte=0x%02x", data[0]);
        return 0;
    }

    // Log first frame info
    if (g_frames_received == 1) {
        log_msg(LOGGER_INFO, "=== FIRST VIDEO FRAME RECEIVED ===");
        log_msg(LOGGER_INFO, "  Size: %d bytes, NAL count: %d", *data_len, *nal_count);
        log_msg(LOGGER_INFO, "  NTP time: %llu", *ntp_time);

        // Log first few bytes
        char hex[64];
        int hex_len = (*data_len > 20) ? 20 : *data_len;
        for (int i = 0; i < hex_len; i++) {
            sprintf(hex + i*3, "%02x ", data[i]);
        }
        log_msg(LOGGER_INFO, "  First bytes: %s", hex);
    }

    // Parse SPS/PPS if we don't have a decoder yet
    if (!g_decoder_session) {
        log_msg(LOGGER_INFO, "No decoder session, looking for SPS/PPS...");
        if (parse_sps_pps(data, *data_len)) {
            log_msg(LOGGER_INFO, "Found SPS/PPS, creating decoder session");
            create_decoder_session();
        } else {
            log_msg(LOGGER_DEBUG, "No SPS/PPS found yet, waiting...");
        }
    }

    // Set base time on first frame
    if (g_first_frame && *ntp_time > 0) {
        g_base_time = *ntp_time;
        g_first_frame = false;
        log_msg(LOGGER_INFO, "Base time set to: %llu", g_base_time);
    }

    // Decode frame
    uint64_t pts = *ntp_time - g_base_time;
    if (!decode_frame(data, *data_len, pts)) {
        log_msg(LOGGER_DEBUG, "Frame decode submission failed (may be waiting for keyframe)");
    }

    return 0;
}

void video_renderer_flush(void) {
    if (g_decoder_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(g_decoder_session);
    }
}

void video_renderer_destroy(void) {
    g_running = false;

    destroy_decoder_session();
    destroy_window();

    if (g_sps) { free(g_sps); g_sps = NULL; }
    if (g_pps) { free(g_pps); g_pps = NULL; }

    pthread_mutex_lock(&g_frame_mutex);
    if (g_pending_frame) {
        CVPixelBufferRelease(g_pending_frame);
        g_pending_frame = NULL;
    }
    pthread_mutex_unlock(&g_frame_mutex);

    g_initialized = false;
    log_msg(LOGGER_INFO, "Native macOS video renderer destroyed");
}

void video_renderer_size(float *width_source, float *height_source, float *width, float *height) {
    // Store size info if needed
}

void video_renderer_hls_ready(void) {
    // HLS not supported in native renderer yet
}

void video_renderer_seek(float position) {
    // HLS seek not supported
}

void video_renderer_set_start(float position) {
    // HLS not supported
}

bool video_renderer_eos_watch(void) {
    return false;
}

unsigned int video_renderer_listen(void *loop, int id) {
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) {
    // JPEG display not implemented yet
}

bool waiting_for_x11_window(void) {
    return false;  // Not applicable on macOS
}

bool video_get_playback_info(double *duration, double *position, double *seek_start,
                             double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    return false;  // HLS not supported
}

int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265) {
    g_is_h265 = video_is_h265;
    return 0;
}
