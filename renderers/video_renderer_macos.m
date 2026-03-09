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
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreImage/CoreImage.h>
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

// Current decoded video dimensions (for detecting rotation/resolution changes)
static size_t g_current_width = 0;
static size_t g_current_height = 0;

// SPS/PPS/VPS storage for format description (VPS for HEVC)
static uint8_t *g_vps = NULL;
static size_t g_vps_size = 0;
static uint8_t *g_sps = NULL;
static size_t g_sps_size = 0;
static uint8_t *g_pps = NULL;
static size_t g_pps_size = 0;

// Fullscreen state
static bool g_fullscreen = false;
static bool g_initial_fullscreen = false;
static NSRect g_windowed_frame = {0};

// Always on top state
static bool g_always_on_top = false;

// Manual rotation (CA layer transform, degrees: 0/90/180/270)

// Server name for window title
static char *g_server_name = NULL;
typedef enum {
    WINDOW_TITLE_MODE_NONE = 0,
    WINDOW_TITLE_MODE_READY,
    WINDOW_TITLE_MODE_STREAMING,
    WINDOW_TITLE_MODE_CUSTOM
} window_title_mode_t;
static window_title_mode_t g_window_title_mode = WINDOW_TITLE_MODE_NONE;
static char *g_window_title_custom_status = NULL;

// Cover art storage
static NSImage *g_cover_art = NULL;
static bool g_showing_cover_art = false;
static id<MTLTexture> g_cover_art_texture = NULL;
static bool g_cover_art_texture_created = false;
static bool g_cover_art_texture_rebuild_pending = false;

// Track metadata for cover art display
static char *g_track_title = NULL;
static char *g_track_artist = NULL;
static char *g_track_album = NULL;

// Idle screen texture
static id<MTLTexture> g_idle_texture = NULL;
static bool g_idle_texture_created = false;

// Display window and layer
static NSWindow *g_window = NULL;
static NSView *g_container_view = NULL;        // Outer container (fills window, black bg, clips)
static NSView *g_rotation_host_view = NULL;    // Intermediate view that receives CA rotation transform
static MTKView *g_metal_view = NULL;
static NSArray<NSLayoutConstraint *> *g_rotation_host_fill_constraints = nil;
static NSArray<NSLayoutConstraint *> *g_metal_view_fill_constraints = nil;
static bool g_rotation_host_constraints_active = false;
static id<MTLDevice> g_metal_device = NULL;
static id<MTLCommandQueue> g_command_queue = NULL;
static id<MTLTexture> g_current_texture = NULL;
static CVMetalTextureCacheRef g_texture_cache = NULL;
static id<MTLTexture> g_rotation_texture = NULL;

// Threading
static pthread_mutex_t g_decoder_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static CVPixelBufferRef g_pending_frame = NULL;
static bool g_has_new_frame = false;  // true when decoder has delivered a frame not yet rendered
static dispatch_semaphore_t g_inflight_semaphore = NULL;  // triple-buffer: max 3 in-flight command buffers
static uint64_t g_base_time = 0;
static bool g_first_frame = true;

// Statistics for logging
static uint64_t g_frames_received = 0;
static uint64_t g_frames_decoded = 0;
static uint64_t g_frames_rendered = 0;
static uint64_t g_decode_errors = 0;
static uint64_t g_last_stats_time = 0;
static uint64_t g_last_frame_time = 0;  // mach_absolute_time of last decoded frame
static const double FRAME_STALL_TIMEOUT = 30.0;  // seconds before showing idle screen
static dispatch_source_t g_stall_timer = NULL;
static uint64_t g_last_stall_log_time = 0;  // throttle stall log messages

// Frame pacing
static double g_source_frame_interval = 0.0;  // estimated source frame interval in seconds
static uint64_t g_prev_decode_time = 0;        // mach_absolute_time of previous decoded frame

// Debug info overlay
static bool g_show_fps = false;
static uint64_t g_fps_frame_count = 0;
static uint64_t g_fps_last_time = 0;
static double g_fps_display = 0.0;
static double g_fps_source = 0.0;  // decoded fps
static uint64_t g_fps_decode_count = 0;
static uint64_t g_fps_decode_last_time = 0;

// Forward declarations
static void create_decoder_session(void);
static void destroy_decoder_session(void);
static void layout_video_views_on_main_thread(void);
static void set_video_paused(bool paused, bool should_log);
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

static void set_owned_string(char **slot, const char *value) {
    if (*slot) {
        free(*slot);
        *slot = NULL;
    }
    if (value && value[0] != '\0') {
        *slot = strdup(value);
    }
}

static bool decoder_session_exists(void) {
    bool exists = false;
    pthread_mutex_lock(&g_decoder_mutex);
    exists = (g_decoder_session != NULL);
    pthread_mutex_unlock(&g_decoder_mutex);
    return exists;
}

static void clear_parameter_sets_locked(void) {
    if (g_vps) { free(g_vps); g_vps = NULL; g_vps_size = 0; }
    if (g_sps) { free(g_sps); g_sps = NULL; g_sps_size = 0; }
    if (g_pps) { free(g_pps); g_pps = NULL; g_pps_size = 0; }
}

static void clear_parameter_sets(void) {
    pthread_mutex_lock(&g_decoder_mutex);
    clear_parameter_sets_locked();
    pthread_mutex_unlock(&g_decoder_mutex);
}

#pragma mark - Manual Rotation (Core Animation layer transform, like Reflector 4)

// Manual rotation angle in degrees (0, 90, 180, 270)
static int g_manual_rotation_degrees = 0;
static bool g_manual_rotation_animation_inflight = false;
static uint64_t g_manual_rotation_animation_generation = 0;
static const NSTimeInterval MANUAL_ROTATION_ANIMATION_DURATION = 0.22;

static const char *rotation_degrees_name(int degrees) {
    switch (degrees) {
        case 90:  return "90° CW";
        case 180: return "180°";
        case 270: return "90° CCW";
        default:  return "None";
    }
}

static void get_base_display_dimensions(CGFloat *width, CGFloat *height) {
    CGFloat display_width = (CGFloat)g_current_width;
    CGFloat display_height = (CGFloat)g_current_height;

    if (g_rotation == LEFT || g_rotation == RIGHT) {
        display_width = (CGFloat)g_current_height;
        display_height = (CGFloat)g_current_width;
    }

    if (width) *width = display_width;
    if (height) *height = display_height;
}

static NSSize scaled_content_size_for_display(CGFloat display_width, CGFloat display_height) {
    if (display_width <= 0.0 || display_height <= 0.0) {
        return NSMakeSize(0.0, 0.0);
    }

    CGFloat scale = (display_height > display_width)
        ? 900.0 / display_height
        : 1200.0 / display_width;

    return NSMakeSize(round(display_width * scale), round(display_height * scale));
}

static NSRect window_frame_for_content_size(CGFloat width, CGFloat height) {
    if (!g_window || width <= 0.0 || height <= 0.0) {
        return NSZeroRect;
    }

    NSRect window_frame = [g_window frame];
    NSRect content_rect = [g_window contentRectForFrameRect:window_frame];
    content_rect.size = NSMakeSize(width, height);

    NSRect target_frame = [g_window frameRectForContentRect:content_rect];
    target_frame.origin.x = window_frame.origin.x;
    target_frame.origin.y = window_frame.origin.y + NSHeight(window_frame) - NSHeight(target_frame);
    return target_frame;
}

static void resize_window_content(CGFloat width, CGFloat height, BOOL animate) {
    if (!g_window || g_fullscreen || width <= 0.0 || height <= 0.0) return;
    NSRect target_frame = window_frame_for_content_size(width, height);
    [g_window setFrame:target_frame display:YES animate:animate];
}

static void animate_window_content_resize(CGFloat width, CGFloat height, NSTimeInterval duration) {
    if (!g_window || g_fullscreen || width <= 0.0 || height <= 0.0) return;

    NSRect target_frame = window_frame_for_content_size(width, height);
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration = duration;
        context.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
        [[g_window animator] setFrame:target_frame display:YES];
    } completionHandler:^{
        layout_video_views_on_main_thread();
    }];
}

static CATransform3D rotation_transform_for_degrees(int degrees) {
    if (degrees == 0) {
        return CATransform3DIdentity;
    }

    CGFloat angle = -(CGFloat)degrees * M_PI / 180.0;
    return CATransform3DMakeRotation(angle, 0, 0, 1);
}

static NSRect rotation_host_frame_for_container_bounds(NSRect container_bounds, int degrees) {
    CGFloat container_width = NSWidth(container_bounds);
    CGFloat container_height = NSHeight(container_bounds);
    if (container_width <= 0.0 || container_height <= 0.0) {
        return NSZeroRect;
    }

    if (degrees == 0 || g_current_width == 0 || g_current_height == 0) {
        return container_bounds;
    }

    CGFloat base_width = 0.0;
    CGFloat base_height = 0.0;
    get_base_display_dimensions(&base_width, &base_height);
    if (base_width <= 0.0 || base_height <= 0.0) {
        return container_bounds;
    }

    bool swap_dims = (degrees == 90 || degrees == 270);
    CGFloat fit_scale = swap_dims
        ? fmin(container_width / base_height, container_height / base_width)
        : fmin(container_width / base_width, container_height / base_height);
    CGFloat host_width = round(base_width * fit_scale);
    CGFloat host_height = round(base_height * fit_scale);
    CGFloat host_x = round((container_width - host_width) / 2.0);
    CGFloat host_y = round((container_height - host_height) / 2.0);

    return NSMakeRect(host_x, host_y, host_width, host_height);
}

static void set_rotation_host_fill_constraints_active(BOOL active) {
    if (!g_rotation_host_fill_constraints || g_rotation_host_constraints_active == (bool)active) {
        return;
    }

    if (active) {
        g_rotation_host_view.translatesAutoresizingMaskIntoConstraints = NO;
        [NSLayoutConstraint activateConstraints:g_rotation_host_fill_constraints];
    } else {
        [NSLayoutConstraint deactivateConstraints:g_rotation_host_fill_constraints];
        g_rotation_host_view.translatesAutoresizingMaskIntoConstraints = YES;
    }

    g_rotation_host_constraints_active = active;
}

static void sync_rotation_host_layer_geometry(void) {
    if (!g_rotation_host_view || !g_rotation_host_view.layer) return;

    g_rotation_host_view.layer.anchorPoint = CGPointMake(0.5, 0.5);
    g_rotation_host_view.layer.position = CGPointMake(NSMidX(g_rotation_host_view.frame),
                                                      NSMidY(g_rotation_host_view.frame));
}

static void cancel_manual_rotation_animation_on_main_thread(void) {
    if (!g_rotation_host_view || !g_rotation_host_view.layer) return;

    g_manual_rotation_animation_generation++;
    g_manual_rotation_animation_inflight = false;
    [g_rotation_host_view.layer removeAnimationForKey:@"manualRotationTransform"];
}

static void layout_video_views_on_main_thread(void) {
    if (!g_window || !g_container_view || !g_rotation_host_view || !g_metal_view) return;

    [g_container_view layoutSubtreeIfNeeded];

    NSRect container_bounds = g_container_view.bounds;
    CGFloat container_width = NSWidth(container_bounds);
    CGFloat container_height = NSHeight(container_bounds);
    if (container_width <= 0.0 || container_height <= 0.0) return;

    if (g_manual_rotation_degrees == 0 || g_current_width == 0 || g_current_height == 0) {
        set_rotation_host_fill_constraints_active(YES);
        g_rotation_host_view.layer.transform = CATransform3DIdentity;
        g_rotation_host_view.frame = container_bounds;
        sync_rotation_host_layer_geometry();
        [g_container_view layoutSubtreeIfNeeded];
        return;
    }

    set_rotation_host_fill_constraints_active(NO);
    g_rotation_host_view.autoresizingMask = 0;
    g_rotation_host_view.layer.transform = CATransform3DIdentity;
    g_rotation_host_view.frame = rotation_host_frame_for_container_bounds(container_bounds, g_manual_rotation_degrees);
    sync_rotation_host_layer_geometry();
    g_rotation_host_view.layer.transform = rotation_transform_for_degrees(g_manual_rotation_degrees);
}

static void apply_manual_rotation_on_main_thread(void) {
    if (!g_window || !g_container_view || !g_rotation_host_view || !g_metal_view) return;
    if (g_current_width == 0 || g_current_height == 0) return;

    CGFloat base_width = 0.0;
    CGFloat base_height = 0.0;
    get_base_display_dimensions(&base_width, &base_height);
    if (base_width <= 0.0 || base_height <= 0.0) return;

    bool swap_dims = (g_manual_rotation_degrees == 90 || g_manual_rotation_degrees == 270);
    CGFloat display_width = swap_dims ? base_height : base_width;
    CGFloat display_height = swap_dims ? base_width : base_height;
    NSRect target_container_bounds = g_container_view.bounds;
    NSRect target_window_frame = NSZeroRect;

    if (!g_fullscreen) {
        NSSize target_size = scaled_content_size_for_display(display_width, display_height);
        target_container_bounds = NSMakeRect(0, 0, target_size.width, target_size.height);
        target_window_frame = window_frame_for_content_size(target_size.width, target_size.height);
    }

    cancel_manual_rotation_animation_on_main_thread();

    NSRect target_host_frame = rotation_host_frame_for_container_bounds(target_container_bounds, g_manual_rotation_degrees);
    CATransform3D target_transform = rotation_transform_for_degrees(g_manual_rotation_degrees);
    uint64_t animation_generation = ++g_manual_rotation_animation_generation;
    g_manual_rotation_animation_inflight = true;

    set_rotation_host_fill_constraints_active(NO);
    g_rotation_host_view.autoresizingMask = 0;
    sync_rotation_host_layer_geometry();

    CALayer *host_layer = g_rotation_host_view.layer;
    [host_layer removeAnimationForKey:@"manualRotationTransform"];

    CABasicAnimation *rotation_animation = [CABasicAnimation animationWithKeyPath:@"transform"];
    rotation_animation.fromValue = [NSValue valueWithCATransform3D:host_layer.transform];
    rotation_animation.toValue = [NSValue valueWithCATransform3D:target_transform];
    rotation_animation.duration = MANUAL_ROTATION_ANIMATION_DURATION;
    rotation_animation.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [host_layer addAnimation:rotation_animation forKey:@"manualRotationTransform"];
    host_layer.transform = target_transform;

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration = MANUAL_ROTATION_ANIMATION_DURATION;
        context.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];

        if (!g_fullscreen) {
            [[g_window animator] setFrame:target_window_frame display:YES];
        }

        [[g_rotation_host_view animator] setFrame:target_host_frame];
    } completionHandler:^{
        if (animation_generation != g_manual_rotation_animation_generation) {
            return;
        }

        g_manual_rotation_animation_inflight = false;
        layout_video_views_on_main_thread();
    }];

    NSRect container_bounds = target_container_bounds;
    NSRect host_frame = target_host_frame;
    log_msg(LOGGER_INFO,
            "Manual rotation %s animated: container %.0fx%.0f, host %.0fx%.0f at (%.0f,%.0f)",
            rotation_degrees_name(g_manual_rotation_degrees),
            NSWidth(container_bounds), NSHeight(container_bounds),
            NSWidth(host_frame), NSHeight(host_frame),
            host_frame.origin.x, host_frame.origin.y);
}

static void apply_manual_rotation(void) {
    if ([NSThread isMainThread]) {
        apply_manual_rotation_on_main_thread();
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        apply_manual_rotation_on_main_thread();
    });
}

static void rotate_cw(void) {
    g_manual_rotation_degrees = (g_manual_rotation_degrees + 90) % 360;
    log_msg(LOGGER_INFO, "Manual rotation: %s", rotation_degrees_name(g_manual_rotation_degrees));
    apply_manual_rotation();
}

static void rotate_ccw(void) {
    g_manual_rotation_degrees = (g_manual_rotation_degrees + 270) % 360;
    log_msg(LOGGER_INFO, "Manual rotation: %s", rotation_degrees_name(g_manual_rotation_degrees));
    apply_manual_rotation();
}

static void rotate_reset(void) {
    g_manual_rotation_degrees = 0;
    log_msg(LOGGER_INFO, "Manual rotation reset");
    apply_manual_rotation();
}

#pragma mark - Fullscreen Toggle

static void toggle_fullscreen(void) {
    if (!g_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        cancel_manual_rotation_animation_on_main_thread();
        g_idle_texture_created = false;
        g_idle_texture = nil;
        if (g_fullscreen) {
            // Exit fullscreen
            [g_window setStyleMask:NSWindowStyleMaskTitled |
                                   NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskResizable |
                                   NSWindowStyleMaskMiniaturizable];
            [g_window setFrame:g_windowed_frame display:YES animate:YES];
            [g_window setLevel:g_always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel];
            [NSCursor unhide];
            g_fullscreen = false;
            log_msg(LOGGER_INFO, "Exited fullscreen mode");
        } else {
            // Enter fullscreen
            g_windowed_frame = [g_window frame];
            NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
            NSRect screenFrame = [screen frame];
            [g_window setStyleMask:NSWindowStyleMaskBorderless];
            [g_window setFrame:screenFrame display:YES animate:YES];
            [g_window setLevel:NSScreenSaverWindowLevel];
            [NSCursor hide];
            g_fullscreen = true;
            log_msg(LOGGER_INFO, "Entered fullscreen mode");
        }
    });
}

#pragma mark - Custom Window (Keyboard Handling)

@interface UxPlayWindow : NSWindow
@end

@implementation UxPlayWindow

- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (BOOL)canBecomeMainWindow {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 1) {
        unichar c = [chars characterAtIndex:0];
        if (c == 'f' || c == 'F') {
            toggle_fullscreen();
            return;
        } else if (c == ' ') { // Space - toggle pause
            set_video_paused(!g_paused, true);
            return;
        } else if (c == 27) { // ESC
            if (g_fullscreen) {
                toggle_fullscreen();
            }
            return;
        } else if (c == 'r') {
            rotate_cw();
            return;
        } else if (c == 'R') {
            rotate_ccw();
            return;
        } else if (c == '0') {
            rotate_reset();
            return;
        } else if (c == 'd' || c == 'D') {
            g_show_fps = !g_show_fps;
            log_msg(LOGGER_INFO, "Debug info overlay %s", g_show_fps ? "enabled" : "disabled");
            return;
        } else if (c == 'q' || c == 'Q') {
            // Quit - send SIGTERM to self
            kill(getpid(), SIGTERM);
            return;
        }
    }
    [super keyDown:event];
}

@end

#pragma mark - Custom Metal View (Double-Click Handling)

@interface UxPlayMetalView : MTKView
@end

@implementation UxPlayMetalView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)mouseDown:(NSEvent *)event {
    if ([event clickCount] == 2) {
        toggle_fullscreen();
    }
    [super mouseDown:event];
}

@end

#pragma mark - Menu Bar

@interface UxPlayMenuHandler : NSObject <NSWindowDelegate>
- (void)toggleFullscreen:(id)sender;
- (void)toggleAlwaysOnTop:(id)sender;
- (void)togglePause:(id)sender;
- (void)toggleFPS:(id)sender;
- (void)rotateCW:(id)sender;
- (void)rotateCCW:(id)sender;
- (void)rotateReset:(id)sender;
- (void)quit:(id)sender;
@end

@implementation UxPlayMenuHandler

- (void)toggleFullscreen:(id)sender {
    toggle_fullscreen();
}

- (void)toggleAlwaysOnTop:(id)sender {
    g_always_on_top = !g_always_on_top;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_window && !g_fullscreen) {
            if (g_always_on_top) {
                [g_window setLevel:NSFloatingWindowLevel];
            } else {
                [g_window setLevel:NSNormalWindowLevel];
            }
        }
    });

    // Update menu item checkmark
    NSMenuItem *menuItem = (NSMenuItem *)sender;
    [menuItem setState:g_always_on_top ? NSControlStateValueOn : NSControlStateValueOff];

    log_msg(LOGGER_INFO, "Always on top %s", g_always_on_top ? "enabled" : "disabled");
}

- (void)togglePause:(id)sender {
    set_video_paused(!g_paused, true);
}

- (void)toggleFPS:(id)sender {
    g_show_fps = !g_show_fps;
    NSMenuItem *menuItem = (NSMenuItem *)sender;
    [menuItem setState:g_show_fps ? NSControlStateValueOn : NSControlStateValueOff];
    log_msg(LOGGER_INFO, "Debug info overlay %s", g_show_fps ? "enabled" : "disabled");
}

- (void)rotateCW:(id)sender {
    rotate_cw();
}

- (void)rotateCCW:(id)sender {
    rotate_ccw();
}

- (void)rotateReset:(id)sender {
    rotate_reset();
}

- (void)quit:(id)sender {
    kill(getpid(), SIGTERM);
}

// NSWindowDelegate - handle window close button
- (BOOL)windowShouldClose:(NSWindow *)sender {
    NSArray *screens = [NSScreen screens];
    if ([screens count] > 1) {
        // Multiple displays - warn user
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Quit UxPlay?"];
        [alert setInformativeText:@"You have multiple displays connected. Are you sure you want to quit?"];
        [alert addButtonWithTitle:@"Quit"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert setAlertStyle:NSAlertStyleWarning];

        NSModalResponse response = [alert runModal];
        if (response == NSAlertFirstButtonReturn) {
            kill(getpid(), SIGTERM);
            return YES;
        }
        return NO;
    }

    // Single display - quit immediately
    kill(getpid(), SIGTERM);
    return YES;
}

- (void)windowDidResize:(NSNotification *)notification {
    if (g_manual_rotation_animation_inflight) {
        return;
    }
    layout_video_views_on_main_thread();
}

@end

static UxPlayMenuHandler *g_menu_handler = NULL;

static void apply_window_title_on_main_thread(void) {
    if (!g_window) return;

    const char *base_title = g_server_name ?: "UxPlay";
    NSString *status = nil;

    switch (g_window_title_mode) {
        case WINDOW_TITLE_MODE_READY:
            status = @"Ready";
            break;
        case WINDOW_TITLE_MODE_STREAMING:
            {
                const char *state = g_paused ? "Paused" : "Streaming";
                CGFloat display_width = 0.0;
                CGFloat display_height = 0.0;
                get_base_display_dimensions(&display_width, &display_height);
                double title_fps = g_fps_source;
                if (title_fps <= 0.0 && g_source_frame_interval > 0.0) {
                    title_fps = 1.0 / g_source_frame_interval;
                }

                if (display_width > 0.0 && display_height > 0.0 && title_fps > 0.0) {
                    status = [NSString stringWithFormat:@"%s | %.0fx%.0f | %.1f fps",
                              state, display_width, display_height, title_fps];
                } else if (display_width > 0.0 && display_height > 0.0) {
                    status = [NSString stringWithFormat:@"%s | %.0fx%.0f",
                              state, display_width, display_height];
                } else if (title_fps > 0.0) {
                    status = [NSString stringWithFormat:@"%s | %.1f fps", state, title_fps];
                } else {
                    status = [NSString stringWithUTF8String:state];
                }
            }
            break;
        case WINDOW_TITLE_MODE_CUSTOM:
            if (g_window_title_custom_status && g_window_title_custom_status[0] != '\0') {
                status = [NSString stringWithUTF8String:g_window_title_custom_status];
            }
            break;
        case WINDOW_TITLE_MODE_NONE:
        default:
            status = nil;
            break;
    }

    NSString *title;
    if (status && [status length] > 0) {
        title = [NSString stringWithFormat:@"%s - %@", base_title, status];
    } else {
        title = [NSString stringWithUTF8String:base_title];
    }

    [g_window setTitle:title];
}

static void refresh_window_title(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        apply_window_title_on_main_thread();
    });
}

static void update_window_title(const char *status) {
    if (!status || status[0] == '\0') {
        g_window_title_mode = WINDOW_TITLE_MODE_NONE;
        set_owned_string(&g_window_title_custom_status, NULL);
    } else if (strcmp(status, "Ready") == 0) {
        g_window_title_mode = WINDOW_TITLE_MODE_READY;
        set_owned_string(&g_window_title_custom_status, NULL);
    } else if (strcmp(status, "Streaming") == 0) {
        g_window_title_mode = WINDOW_TITLE_MODE_STREAMING;
        set_owned_string(&g_window_title_custom_status, NULL);
    } else {
        g_window_title_mode = WINDOW_TITLE_MODE_CUSTOM;
        set_owned_string(&g_window_title_custom_status, status);
    }

    refresh_window_title();
}

static void set_video_paused(bool paused, bool should_log) {
    g_paused = paused;
    if (should_log) {
        log_msg(LOGGER_INFO, "Video %s", g_paused ? "paused" : "resumed");
    }
    refresh_window_title();
}

static void create_menu_bar(void) {
    // Create the menu bar
    NSMenu *menuBar = [[NSMenu alloc] init];

    // App menu (UxPlay)
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About UxPlay" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit UxPlay" action:@selector(quit:) keyEquivalent:@"q"];
    [[appMenu itemWithTitle:@"Quit UxPlay"] setTarget:g_menu_handler];
    [appMenuItem setSubmenu:appMenu];
    [menuBar addItem:appMenuItem];

    // View menu
    NSMenuItem *viewMenuItem = [[NSMenuItem alloc] init];
    NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];

    NSMenuItem *fullscreenItem = [[NSMenuItem alloc] initWithTitle:@"Toggle Fullscreen"
                                                            action:@selector(toggleFullscreen:)
                                                     keyEquivalent:@"f"];
    [fullscreenItem setTarget:g_menu_handler];
    [viewMenu addItem:fullscreenItem];

    NSMenuItem *alwaysOnTopItem = [[NSMenuItem alloc] initWithTitle:@"Always on Top"
                                                             action:@selector(toggleAlwaysOnTop:)
                                                      keyEquivalent:@"t"];
    [alwaysOnTopItem setTarget:g_menu_handler];
    [alwaysOnTopItem setState:g_always_on_top ? NSControlStateValueOn : NSControlStateValueOff];
    [viewMenu addItem:alwaysOnTopItem];

    [viewMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *rotateCWItem = [[NSMenuItem alloc] initWithTitle:@"Rotate Clockwise"
                                                          action:@selector(rotateCW:)
                                                   keyEquivalent:@"r"];
    [rotateCWItem setTarget:g_menu_handler];
    [viewMenu addItem:rotateCWItem];

    NSMenuItem *rotateCCWItem = [[NSMenuItem alloc] initWithTitle:@"Rotate Counter-Clockwise"
                                                           action:@selector(rotateCCW:)
                                                    keyEquivalent:@"R"];
    [rotateCCWItem setKeyEquivalentModifierMask:0];
    [rotateCCWItem setTarget:g_menu_handler];
    [viewMenu addItem:rotateCCWItem];

    NSMenuItem *rotateResetItem = [[NSMenuItem alloc] initWithTitle:@"Reset Rotation"
                                                             action:@selector(rotateReset:)
                                                      keyEquivalent:@"0"];
    [rotateResetItem setTarget:g_menu_handler];
    [rotateResetItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [viewMenu addItem:rotateResetItem];

    [viewMenuItem setSubmenu:viewMenu];
    [menuBar addItem:viewMenuItem];

    // Playback menu
    NSMenuItem *playbackMenuItem = [[NSMenuItem alloc] init];
    NSMenu *playbackMenu = [[NSMenu alloc] initWithTitle:@"Playback"];

    NSMenuItem *pauseItem = [[NSMenuItem alloc] initWithTitle:@"Pause/Resume"
                                                       action:@selector(togglePause:)
                                                keyEquivalent:@" "];
    [pauseItem setTarget:g_menu_handler];
    [playbackMenu addItem:pauseItem];

    [playbackMenuItem setSubmenu:playbackMenu];
    [menuBar addItem:playbackMenuItem];

    // Debug menu
    NSMenuItem *debugMenuItem = [[NSMenuItem alloc] init];
    NSMenu *debugMenu = [[NSMenu alloc] initWithTitle:@"Debug"];

    NSMenuItem *fpsItem = [[NSMenuItem alloc] initWithTitle:@"Show Debug Info"
                                                     action:@selector(toggleFPS:)
                                              keyEquivalent:@"d"];
    [fpsItem setTarget:g_menu_handler];
    [fpsItem setState:g_show_fps ? NSControlStateValueOn : NSControlStateValueOff];
    [debugMenu addItem:fpsItem];

    [debugMenuItem setSubmenu:debugMenu];
    [menuBar addItem:debugMenuItem];

    [NSApp setMainMenu:menuBar];
}

#pragma mark - Idle Screen

static void create_idle_texture(size_t width, size_t height) {
    if (!g_metal_device) return;

    @autoreleasepool {
        // Create bitmap context directly with BGRA format for Metal
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        size_t bytesPerRow = width * 4;
        uint8_t *pixelData = (uint8_t *)calloc(height * bytesPerRow, 1);

        CGContextRef cgContext = CGBitmapContextCreate(pixelData,
                                                       width, height,
                                                       8, bytesPerRow,
                                                       colorSpace,
                                                       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(colorSpace);

        if (!cgContext) {
            free(pixelData);
            return;
        }

        // Flip coordinate system so Y=0 is at top (matching Metal's coordinate system)
        CGContextTranslateCTM(cgContext, 0, height);
        CGContextScaleCTM(cgContext, 1.0, -1.0);

        // Create NSGraphicsContext from CGContext for drawing with AppKit
        NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:cgContext flipped:YES];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsContext];

        CGFloat canvasWidth = (CGFloat)width;
        CGFloat canvasHeight = (CGFloat)height;
        CGFloat minDimension = MIN(canvasWidth, canvasHeight);
        CGFloat uiScale = fmin(1.55, fmax(1.0, fmin(canvasWidth / 1680.0, canvasHeight / 1050.0)));

        // Background wash with a soft blue focus so the screen feels alive without looking busy.
        NSGradient *backgroundGradient = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithRed:0.13 green:0.14 blue:0.17 alpha:1.0]
                                                                       endingColor:[NSColor colorWithRed:0.05 green:0.06 blue:0.08 alpha:1.0]];
        [backgroundGradient drawInRect:NSMakeRect(0, 0, canvasWidth, canvasHeight) angle:90];

        NSGradient *ambientGlow = [[NSGradient alloc] initWithColorsAndLocations:
                                   [NSColor colorWithRed:0.34 green:0.45 blue:0.68 alpha:0.22], 0.0,
                                   [NSColor colorWithRed:0.18 green:0.23 blue:0.33 alpha:0.10], 0.35,
                                   [NSColor colorWithWhite:0.0 alpha:0.0], 1.0,
                                   nil];
        NSPoint glowCenter = NSMakePoint(canvasWidth * 0.5, canvasHeight * 0.36);
        [ambientGlow drawFromCenter:glowCenter
                             radius:0.0
                           toCenter:glowCenter
                             radius:minDimension * 0.78
                            options:0];

        NSBezierPath *topVeil = [NSBezierPath bezierPathWithRect:NSMakeRect(0, 0, canvasWidth, canvasHeight * 0.42)];
        [[NSColor colorWithWhite:1.0 alpha:0.025] setFill];
        [topVeil fill];

        NSMutableParagraphStyle *centerStyle = [[NSMutableParagraphStyle alloc] init];
        centerStyle.alignment = NSTextAlignmentCenter;

        CGFloat badgeFontSize = fmax(13.0 * uiScale, fmin(minDimension * 0.020, 18.0 * uiScale));
        NSDictionary *badgeAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:badgeFontSize weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.78],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSString *badgeText = @"AIRPLAY RECEIVER";
        NSSize badgeSize = [badgeText sizeWithAttributes:badgeAttrs];
        NSRect badgeRect = NSMakeRect((canvasWidth - (badgeSize.width + 32.0 * uiScale)) / 2.0,
                                      canvasHeight * 0.15,
                                      badgeSize.width + 32.0 * uiScale,
                                      badgeSize.height + 12.0 * uiScale);
        NSBezierPath *badgePath = [NSBezierPath bezierPathWithRoundedRect:badgeRect
                                                                  xRadius:badgeRect.size.height / 2.0
                                                                  yRadius:badgeRect.size.height / 2.0];
        [[NSColor colorWithWhite:1.0 alpha:0.08] setFill];
        [badgePath fill];
        [[NSColor colorWithWhite:1.0 alpha:0.10] setStroke];
        [badgePath setLineWidth:1.0];
        [badgePath stroke];
        [badgeText drawInRect:NSInsetRect(badgeRect, 0.0, 6.0 * uiScale) withAttributes:badgeAttrs];

        CGFloat plateSize = fmax(176.0 * uiScale, fmin(minDimension * 0.32, 280.0 * uiScale));
        NSRect plateRect = NSMakeRect((canvasWidth - plateSize) / 2.0,
                                      CGRectGetMaxY(badgeRect) + minDimension * 0.035,
                                      plateSize,
                                      plateSize);
        NSBezierPath *platePath = [NSBezierPath bezierPathWithRoundedRect:plateRect
                                                                  xRadius:plateSize * 0.28
                                                                  yRadius:plateSize * 0.28];

        [NSGraphicsContext saveGraphicsState];
        NSShadow *plateShadow = [[NSShadow alloc] init];
        plateShadow.shadowColor = [NSColor colorWithRed:0.02 green:0.03 blue:0.05 alpha:0.48];
        plateShadow.shadowOffset = NSMakeSize(0, 18.0 * uiScale);
        plateShadow.shadowBlurRadius = 36.0 * uiScale;
        [plateShadow set];
        NSGradient *plateGradient = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithRed:0.25 green:0.31 blue:0.44 alpha:0.92]
                                                                  endingColor:[NSColor colorWithRed:0.14 green:0.17 blue:0.25 alpha:0.96]];
        [plateGradient drawInBezierPath:platePath angle:90];
        [NSGraphicsContext restoreGraphicsState];

        [[NSColor colorWithWhite:1.0 alpha:0.12] setStroke];
        [platePath setLineWidth:1.0];
        [platePath stroke];

        NSBezierPath *innerPlatePath = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(plateRect, 10.0 * uiScale, 10.0 * uiScale)
                                                                       xRadius:plateSize * 0.20
                                                                       yRadius:plateSize * 0.20];
        [[NSColor colorWithWhite:1.0 alpha:0.04] setFill];
        [innerPlatePath fill];

        // Custom AirPlay glyph so we control color and weight consistently.
        NSColor *glyphColor = [NSColor colorWithRed:0.95 green:0.97 blue:1.0 alpha:0.94];
        CGFloat screenWidth = plateSize * 0.50;
        CGFloat screenHeight = screenWidth * 0.36;
        CGFloat screenX = NSMidX(plateRect) - screenWidth / 2.0;
        CGFloat screenY = plateRect.origin.y + plateSize * 0.28;
        NSBezierPath *screenPath = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(screenX, screenY, screenWidth, screenHeight)
                                                                   xRadius:screenHeight * 0.18
                                                                   yRadius:screenHeight * 0.18];
        [glyphColor setStroke];
        [screenPath setLineWidth:MAX(2.0, plateSize * 0.026)];
        [screenPath stroke];

        CGFloat triangleWidth = plateSize * 0.24;
        CGFloat triangleHeight = plateSize * 0.17;
        CGFloat triangleCenterX = NSMidX(plateRect);
        CGFloat triangleTopY = screenY + screenHeight + plateSize * 0.08;
        NSBezierPath *trianglePath = [NSBezierPath bezierPath];
        [trianglePath moveToPoint:NSMakePoint(triangleCenterX, triangleTopY)];
        [trianglePath lineToPoint:NSMakePoint(triangleCenterX - triangleWidth / 2.0, triangleTopY + triangleHeight)];
        [trianglePath lineToPoint:NSMakePoint(triangleCenterX + triangleWidth / 2.0, triangleTopY + triangleHeight)];
        [trianglePath closePath];
        [glyphColor setFill];
        [trianglePath fill];

        CGFloat titleFontSize = fmax(48.0 * uiScale, fmin(minDimension * 0.086, 82.0 * uiScale));
        CGFloat subtitleFontSize = fmax(22.0 * uiScale, fmin(minDimension * 0.034, 34.0 * uiScale));
        CGFloat captionFontSize = fmax(11.0 * uiScale, fmin(minDimension * 0.016, 15.0 * uiScale));
        NSString *titleText = @"Ready for AirPlay";
        NSString *subtitleText = @"Choose this receiver in Screen Mirroring to begin streaming.";

        NSShadow *titleShadow = [[NSShadow alloc] init];
        titleShadow.shadowColor = [NSColor colorWithWhite:0.0 alpha:0.55];
        titleShadow.shadowOffset = NSMakeSize(0, 3.0 * uiScale);
        titleShadow.shadowBlurRadius = 18.0 * uiScale;

        NSDictionary *titleAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:titleFontSize weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.95],
            NSParagraphStyleAttributeName: centerStyle,
            NSShadowAttributeName: titleShadow
        };
        NSDictionary *subtitleAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:subtitleFontSize weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.68],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSDictionary *captionAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:captionFontSize weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.48],
            NSParagraphStyleAttributeName: centerStyle
        };

        NSRect titleRect = NSMakeRect(canvasWidth * 0.14,
                                      CGRectGetMaxY(plateRect) + minDimension * 0.055,
                                      canvasWidth * 0.72,
                                      titleFontSize * 1.2);
        [titleText drawInRect:titleRect withAttributes:titleAttrs];

        NSRect subtitleRect = NSMakeRect(canvasWidth * 0.15,
                                         CGRectGetMaxY(titleRect) + minDimension * 0.01,
                                         canvasWidth * 0.70,
                                         subtitleFontSize * 1.6);
        [subtitleText drawWithRect:subtitleRect
                           options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                        attributes:subtitleAttrs];

        NSString *availabilityText = @"AVAILABLE AS";
        NSRect availabilityRect = NSMakeRect(0.0,
                                             CGRectGetMaxY(subtitleRect) + minDimension * 0.028,
                                             canvasWidth,
                                             captionFontSize * 1.2);
        [availabilityText drawInRect:availabilityRect withAttributes:captionAttrs];

        NSString *serverText = g_server_name ? [NSString stringWithUTF8String:g_server_name] : @"This Mac";
        CGFloat serverFontSize = fmax(19.0 * uiScale, fmin(minDimension * 0.030, 28.0 * uiScale));
        NSMutableParagraphStyle *serverStyle = [centerStyle mutableCopy];
        serverStyle.lineBreakMode = NSLineBreakByTruncatingMiddle;
        NSDictionary *serverAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:serverFontSize weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.88],
            NSParagraphStyleAttributeName: serverStyle
        };
        NSRect serverMeasureRect = [serverText boundingRectWithSize:NSMakeSize(canvasWidth * 0.70, CGFLOAT_MAX)
                                                            options:NSStringDrawingUsesLineFragmentOrigin
                                                         attributes:serverAttrs];
        CGFloat serverPillWidth = MIN(canvasWidth * 0.78, MAX(serverMeasureRect.size.width + 44.0 * uiScale, 300.0 * uiScale));
        CGFloat serverPillHeight = MAX(serverMeasureRect.size.height + 18.0 * uiScale, 48.0 * uiScale);
        NSRect serverPillRect = NSMakeRect((canvasWidth - serverPillWidth) / 2.0,
                                           CGRectGetMaxY(availabilityRect) + 10.0,
                                           serverPillWidth,
                                           serverPillHeight);
        NSBezierPath *serverPillPath = [NSBezierPath bezierPathWithRoundedRect:serverPillRect
                                                                       xRadius:serverPillHeight / 2.0
                                                                       yRadius:serverPillHeight / 2.0];
        [[NSColor colorWithWhite:1.0 alpha:0.09] setFill];
        [serverPillPath fill];
        [[NSColor colorWithWhite:1.0 alpha:0.12] setStroke];
        [serverPillPath setLineWidth:1.0];
        [serverPillPath stroke];
        [serverText drawWithRect:NSInsetRect(serverPillRect, 22.0 * uiScale, 11.0 * uiScale)
                         options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                      attributes:serverAttrs];

        CGFloat shortcutPanelWidth = MIN(canvasWidth * 0.86, 1120.0 * uiScale);
        CGFloat shortcutPanelHeight = fmax(112.0 * uiScale, fmin(minDimension * 0.20, 152.0 * uiScale));
        CGFloat shortcutPanelBottomMargin = fmax(18.0 * uiScale, canvasHeight * 0.030);
        CGFloat minimumPillToPanelGap = fmax(32.0 * uiScale, minDimension * 0.036);
        CGFloat minimumPanelY = CGRectGetMaxY(serverPillRect) + minimumPillToPanelGap;
        CGFloat maximumPanelY = canvasHeight - shortcutPanelHeight - shortcutPanelBottomMargin;
        CGFloat shortcutPanelY = fmin(maximumPanelY, fmax(canvasHeight - shortcutPanelHeight - fmax(28.0, canvasHeight * 0.055),
                                                          minimumPanelY));
        NSRect shortcutPanelRect = NSMakeRect((canvasWidth - shortcutPanelWidth) / 2.0,
                                              shortcutPanelY,
                                              shortcutPanelWidth,
                                              shortcutPanelHeight);
        NSBezierPath *shortcutPanelPath = [NSBezierPath bezierPathWithRoundedRect:shortcutPanelRect
                                                                          xRadius:24.0 * uiScale
                                                                          yRadius:24.0 * uiScale];
        [[NSColor colorWithWhite:1.0 alpha:0.08] setFill];
        [shortcutPanelPath fill];
        [[NSColor colorWithWhite:1.0 alpha:0.10] setStroke];
        [shortcutPanelPath setLineWidth:1.0];
        [shortcutPanelPath stroke];

        CGFloat primaryFontSize = fmax(17.0 * uiScale, fmin(minDimension * 0.026, 24.0 * uiScale));
        CGFloat secondaryFontSize = fmax(15.0 * uiScale, fmin(minDimension * 0.022, 20.0 * uiScale));
        NSDictionary *primaryKeyAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:primaryFontSize weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.92],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSDictionary *primaryLabelAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:primaryFontSize weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.70],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSDictionary *secondaryKeyAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:secondaryFontSize weight:NSFontWeightSemibold],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.82],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSDictionary *secondaryLabelAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:secondaryFontSize weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.56],
            NSParagraphStyleAttributeName: centerStyle
        };
        NSDictionary *separatorAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:primaryFontSize weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.26],
            NSParagraphStyleAttributeName: centerStyle
        };

        NSMutableAttributedString *primaryLine = [[NSMutableAttributedString alloc] init];
        NSMutableAttributedString *secondaryLine = [[NSMutableAttributedString alloc] init];
        void (^appendShortcut)(NSMutableAttributedString *, NSDictionary *, NSDictionary *, NSString *, NSString *) =
        ^(NSMutableAttributedString *line, NSDictionary *keyAttributes, NSDictionary *labelAttributes, NSString *key, NSString *label) {
            if ([line length] > 0) {
                [line appendAttributedString:[[NSAttributedString alloc] initWithString:@"   |   " attributes:separatorAttrs]];
            }
            [line appendAttributedString:[[NSAttributedString alloc] initWithString:key attributes:keyAttributes]];
            [line appendAttributedString:[[NSAttributedString alloc] initWithString:@" " attributes:labelAttributes]];
            [line appendAttributedString:[[NSAttributedString alloc] initWithString:label attributes:labelAttributes]];
        };

        appendShortcut(primaryLine, primaryKeyAttrs, primaryLabelAttrs, @"F", @"Fullscreen");
        appendShortcut(primaryLine, primaryKeyAttrs, primaryLabelAttrs, @"Space", @"Pause/Resume");
        appendShortcut(primaryLine, primaryKeyAttrs, primaryLabelAttrs, @"Q", @"Quit");

        appendShortcut(secondaryLine, secondaryKeyAttrs, secondaryLabelAttrs, @"R", @"Rotate CW");
        appendShortcut(secondaryLine, secondaryKeyAttrs, secondaryLabelAttrs, @"Shift+R", @"Rotate CCW");
        appendShortcut(secondaryLine, secondaryKeyAttrs, secondaryLabelAttrs, @"Cmd+0", @"Reset");

        CGFloat panelInsetX = 34.0 * uiScale;
        CGFloat primaryLineHeight = primaryFontSize * 1.35;
        CGFloat secondaryLineHeight = secondaryFontSize * 1.35;
        CGFloat lineGap = 9.0 * uiScale;
        CGFloat primaryLineY = shortcutPanelRect.origin.y + 24.0 * uiScale;
        CGFloat secondaryLineY = primaryLineY + primaryLineHeight + lineGap;
        NSRect primaryLineRect = NSMakeRect(shortcutPanelRect.origin.x + panelInsetX,
                                            primaryLineY,
                                            shortcutPanelRect.size.width - panelInsetX * 2.0,
                                            primaryLineHeight);
        NSRect secondaryLineRect = NSMakeRect(shortcutPanelRect.origin.x + panelInsetX,
                                              secondaryLineY,
                                              shortcutPanelRect.size.width - panelInsetX * 2.0,
                                              secondaryLineHeight);
        [primaryLine drawWithRect:primaryLineRect
                          options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine];
        [secondaryLine drawWithRect:secondaryLineRect
                            options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine];

        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(cgContext);

        // Create Metal texture from the bitmap data
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                             width:width
                                                                                            height:height
                                                                                         mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;

        g_idle_texture = [g_metal_device newTextureWithDescriptor:descriptor];

        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [g_idle_texture replaceRegion:region
                          mipmapLevel:0
                            withBytes:pixelData
                          bytesPerRow:bytesPerRow];

        g_idle_texture_created = true;
        log_msg(LOGGER_DEBUG, "Idle screen texture created (%zux%zu)", width, height);

        free(pixelData);
    }
}

#pragma mark - FPS Overlay

static void update_fps_counters(void) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t now = mach_absolute_time();

    g_fps_frame_count++;
    if (g_fps_last_time == 0) {
        g_fps_last_time = now;
        return;
    }

    double elapsed = (double)(now - g_fps_last_time) * timebase.numer / timebase.denom / 1e9;
    if (elapsed >= 0.5) {
        g_fps_display = g_fps_frame_count / elapsed;
        g_fps_frame_count = 0;
        g_fps_last_time = now;

        // Snapshot decoded fps
        double decode_elapsed = (double)(now - g_fps_decode_last_time) * timebase.numer / timebase.denom / 1e9;
        if (decode_elapsed > 0) {
            g_fps_source = g_fps_decode_count / decode_elapsed;
        }
        g_fps_decode_count = 0;
        g_fps_decode_last_time = now;

        if (g_window_title_mode == WINDOW_TITLE_MODE_STREAMING) {
            refresh_window_title();
        }
    }
}

static void render_fps_overlay(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> drawable) {
    if (!g_show_fps || !commandBuffer || !drawable) return;

    @autoreleasepool {
        size_t overlayW = 360;
        size_t overlayH = 92;
        size_t bytesPerRow = overlayW * 4;
        uint8_t *pixels = (uint8_t *)calloc(overlayH * bytesPerRow, 1);

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(pixels, overlayW, overlayH, 8, bytesPerRow,
                                                  colorSpace,
                                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(colorSpace);
        if (!ctx) { free(pixels); return; }

        // Semi-transparent black background with rounded corners
        CGContextSetRGBFillColor(ctx, 0, 0, 0, 0.6);
        CGRect bgRect = CGRectMake(0, 0, overlayW, overlayH);
        CGPathRef path = CGPathCreateWithRoundedRect(bgRect, 8, 8, NULL);
        CGContextAddPath(ctx, path);
        CGContextFillPath(ctx);
        CGPathRelease(path);

        // Draw text
        NSGraphicsContext *nsCtx = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsCtx];

        // Line 1: Resolution and codec
        NSDictionary *headerAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightBold],
            NSForegroundColorAttributeName: [NSColor colorWithRed:0.4 green:0.8 blue:1.0 alpha:1.0]
        };
        NSString *resText;
        if (g_manual_rotation_degrees != 0) {
            resText = [NSString stringWithFormat:@" %zux%zu  %s  Rot: %s",
                       g_current_width, g_current_height,
                       g_is_h265 ? "H.265/HEVC" : "H.264/AVC",
                       rotation_degrees_name(g_manual_rotation_degrees)];
        } else {
            resText = [NSString stringWithFormat:@" %zux%zu  %s",
                       g_current_width, g_current_height,
                       g_is_h265 ? "H.265/HEVC" : "H.264/AVC"];
        }
        [resText drawAtPoint:NSMakePoint(4, 58) withAttributes:headerAttrs];

        // Line 2: FPS counters
        NSDictionary *fpsAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName: [NSColor colorWithRed:0.2 green:1.0 blue:0.4 alpha:1.0]
        };
        NSString *fpsText = [NSString stringWithFormat:@" Render: %.1f fps  Source: %.1f fps", g_fps_display, g_fps_source];
        [fpsText drawAtPoint:NSMakePoint(4, 38) withAttributes:fpsAttrs];

        // Line 3: Interval / frames / errors
        NSDictionary *subAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: [NSColor colorWithWhite:0.7 alpha:1.0]
        };
        NSString *intervalText = [NSString stringWithFormat:@" Interval: %.1fms  Frames: %llu",
                                  g_source_frame_interval * 1000.0, g_frames_rendered];
        [intervalText drawAtPoint:NSMakePoint(4, 20) withAttributes:subAttrs];

        // Line 4: Decode stats
        NSDictionary *errAttrs = @{
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
            NSForegroundColorAttributeName: g_decode_errors > 0
                ? [NSColor colorWithRed:1.0 green:0.4 blue:0.3 alpha:1.0]
                : [NSColor colorWithWhite:0.7 alpha:1.0]
        };
        NSString *decodeText = [NSString stringWithFormat:@" Decoded: %llu  Errors: %llu  Recv: %llu",
                                g_frames_decoded, g_decode_errors, g_frames_received];
        [decodeText drawAtPoint:NSMakePoint(4, 4) withAttributes:errAttrs];

        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(ctx);

        // Create texture and blit to top-left corner of drawable
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                        width:overlayW
                                                                                       height:overlayH
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> overlayTex = [g_metal_device newTextureWithDescriptor:desc];
        [overlayTex replaceRegion:MTLRegionMake2D(0, 0, overlayW, overlayH)
                      mipmapLevel:0
                        withBytes:pixels
                      bytesPerRow:bytesPerRow];
        free(pixels);

        // Blit to top-left with 10px margin
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        size_t destX = 10;
        size_t destY = drawable.height - overlayH - 10;  // top-left (Metal Y is bottom-up)
        [blit copyFromTexture:overlayTex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(overlayW, overlayH, 1)
                    toTexture:drawable
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(destX, destY, 0)];
        [blit endEncoding];
    }
}

static void update_cover_art_window_title(void) {
    if (g_track_title && g_track_artist) {
        char title[256];
        snprintf(title, sizeof(title), "%s - %s", g_track_artist, g_track_title);
        update_window_title(title);
    } else if (g_track_title) {
        update_window_title(g_track_title);
    } else if (g_track_artist) {
        update_window_title(g_track_artist);
    } else {
        update_window_title("Now Playing");
    }
}

static NSSize cover_art_texture_size_on_main_thread(void) {
    if (g_metal_view) {
        CGSize drawableSize = g_metal_view.drawableSize;
        if (drawableSize.width > 0.0 && drawableSize.height > 0.0) {
            return NSMakeSize(drawableSize.width, drawableSize.height);
        }

        NSRect bounds = [g_metal_view bounds];
        if (bounds.size.width > 0.0 && bounds.size.height > 0.0) {
            CGFloat scale = g_window ? [g_window backingScaleFactor] : 2.0;
            return NSMakeSize(round(bounds.size.width * scale), round(bounds.size.height * scale));
        }
    }

    return NSMakeSize(2560.0, 1440.0);
}

static void rebuild_cover_art_texture_on_main_thread(void) {
    bool had_existing_texture = (g_cover_art_texture_created && g_cover_art_texture);
    bool rebuilt_texture = false;

    do {
        if (!g_showing_cover_art || !g_cover_art || !g_metal_device) {
            break;
        }

        NSImage *image = g_cover_art;
        NSSize imageSize = [image size];
        if (imageSize.width <= 0.0 || imageSize.height <= 0.0) {
            log_msg(LOGGER_ERR, "Invalid cover art image size");
            break;
        }

        NSSize textureSize = cover_art_texture_size_on_main_thread();
        size_t texWidth = (size_t)llround(textureSize.width);
        size_t texHeight = (size_t)llround(textureSize.height);
        if (texWidth == 0 || texHeight == 0) {
            texWidth = 2560;
            texHeight = 1440;
        }

        CGImageRef cgImage = [image CGImageForProposedRect:NULL context:nil hints:nil];
        if (!cgImage) {
            log_msg(LOGGER_ERR, "Failed to get CGImage from cover art");
            break;
        }

        CIImage *ciImage = [CIImage imageWithCGImage:cgImage];
        CGFloat bgScale = MAX((CGFloat)texWidth / imageSize.width, (CGFloat)texHeight / imageSize.height) * 1.3;
        CIImage *scaledBg = [ciImage imageByApplyingTransform:CGAffineTransformMakeScale(bgScale, bgScale)];

        CGRect scaledExtent = [scaledBg extent];
        CGFloat offsetX = (texWidth - scaledExtent.size.width) / 2.0;
        CGFloat offsetY = (texHeight - scaledExtent.size.height) / 2.0;
        scaledBg = [scaledBg imageByApplyingTransform:CGAffineTransformMakeTranslation(offsetX - scaledExtent.origin.x,
                                                                                        offsetY - scaledExtent.origin.y)];
        scaledBg = [scaledBg imageByClampingToExtent];

        CIFilter *blurFilter = [CIFilter filterWithName:@"CIGaussianBlur"];
        [blurFilter setValue:scaledBg forKey:kCIInputImageKey];
        [blurFilter setValue:@80.0 forKey:kCIInputRadiusKey];
        CIImage *blurredBg = [blurFilter outputImage];

        CIFilter *darkenFilter = [CIFilter filterWithName:@"CIColorControls"];
        [darkenFilter setValue:blurredBg forKey:kCIInputImageKey];
        [darkenFilter setValue:@(-0.2) forKey:kCIInputBrightnessKey];
        [darkenFilter setValue:@1.1 forKey:kCIInputSaturationKey];
        blurredBg = [darkenFilter outputImage];
        blurredBg = [blurredBg imageByCroppingToRect:CGRectMake(0, 0, texWidth, texHeight)];

        CIContext *ciContext = [CIContext contextWithMTLDevice:g_metal_device];
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        size_t bytesPerRow = texWidth * 4;
        uint8_t *pixelData = (uint8_t *)calloc(texHeight * bytesPerRow, 1);
        CGContextRef context = CGBitmapContextCreate(pixelData,
                                                     texWidth, texHeight,
                                                     8, bytesPerRow,
                                                     colorSpace,
                                                     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (!context) {
            CGColorSpaceRelease(colorSpace);
            free(pixelData);
            log_msg(LOGGER_ERR, "Failed to create cover art bitmap context");
            break;
        }

        CGImageRef bgCGImage = [ciContext createCGImage:blurredBg fromRect:CGRectMake(0, 0, texWidth, texHeight)];
        if (bgCGImage) {
            CGContextDrawImage(context, CGRectMake(0, 0, texWidth, texHeight), bgCGImage);
            CGImageRelease(bgCGImage);
        }

        CGFloat artMaxSize = MIN(texWidth, texHeight) * 0.50;
        CGFloat artScale = MIN(artMaxSize / imageSize.width, artMaxSize / imageSize.height);
        CGFloat artWidth = imageSize.width * artScale;
        CGFloat artHeight = imageSize.height * artScale;
        CGFloat artX = (texWidth - artWidth) / 2.0;
        CGFloat artY = (texHeight - artHeight) / 2.0 + texHeight * 0.10;

        CGContextSaveGState(context);
        CGContextSetShadowWithColor(context,
                                    CGSizeMake(0, -25),
                                    50,
                                    [[NSColor colorWithWhite:0 alpha:0.6] CGColor]);
        CGFloat cornerRadius = artWidth * 0.04;
        CGRect artRect = CGRectMake(artX, artY, artWidth, artHeight);
        CGPathRef roundedPath = CGPathCreateWithRoundedRect(artRect, cornerRadius, cornerRadius, NULL);
        CGContextAddPath(context, roundedPath);
        CGContextClip(context);
        CGContextDrawImage(context, artRect, cgImage);
        CGPathRelease(roundedPath);
        CGContextRestoreGState(context);

        NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsContext];

        NSString *titleStr = g_track_title ? [NSString stringWithUTF8String:g_track_title] : nil;
        NSString *artistStr = g_track_artist ? [NSString stringWithUTF8String:g_track_artist] : nil;
        if (titleStr || artistStr) {
            NSMutableParagraphStyle *centerStyle = [[NSMutableParagraphStyle alloc] init];
            centerStyle.alignment = NSTextAlignmentCenter;

            NSShadow *textShadow = [[NSShadow alloc] init];
            textShadow.shadowColor = [NSColor colorWithWhite:0 alpha:0.8];
            textShadow.shadowOffset = NSMakeSize(0, -2);
            textShadow.shadowBlurRadius = 8;

            NSDictionary *titleAttrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:52 weight:NSFontWeightBold],
                NSForegroundColorAttributeName: [NSColor whiteColor],
                NSParagraphStyleAttributeName: centerStyle,
                NSShadowAttributeName: textShadow
            };

            NSDictionary *artistAttrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:38 weight:NSFontWeightMedium],
                NSForegroundColorAttributeName: [NSColor colorWithWhite:0.85 alpha:1.0],
                NSParagraphStyleAttributeName: centerStyle,
                NSShadowAttributeName: textShadow
            };

            CGFloat textAreaTop = artY - 50.0;
            CGFloat textWidth = texWidth * 0.85;
            CGFloat textX = (texWidth - textWidth) / 2.0;

            if (titleStr) {
                NSSize titleSize = [titleStr sizeWithAttributes:titleAttrs];
                CGFloat titleY = textAreaTop - titleSize.height;
                NSRect titleRect = NSMakeRect(textX, titleY, textWidth, titleSize.height + 10.0);
                [titleStr drawInRect:titleRect withAttributes:titleAttrs];
                textAreaTop = titleY - 10.0;
            }

            if (artistStr) {
                NSSize artistSize = [artistStr sizeWithAttributes:artistAttrs];
                CGFloat artistY = textAreaTop - artistSize.height;
                NSRect artistRect = NSMakeRect(textX, artistY, textWidth, artistSize.height + 10.0);
                [artistStr drawInRect:artistRect withAttributes:artistAttrs];
            }
        }

        [NSGraphicsContext restoreGraphicsState];
        CGColorSpaceRelease(colorSpace);
        CGContextRelease(context);

        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                             width:texWidth
                                                                                            height:texHeight
                                                                                         mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;

        id<MTLTexture> newTexture = [g_metal_device newTextureWithDescriptor:descriptor];
        if (!newTexture) {
            free(pixelData);
            log_msg(LOGGER_ERR, "Failed to create cover art Metal texture");
            break;
        }

        MTLRegion region = MTLRegionMake2D(0, 0, texWidth, texHeight);
        [newTexture replaceRegion:region
                      mipmapLevel:0
                        withBytes:pixelData
                      bytesPerRow:bytesPerRow];
        free(pixelData);

        g_cover_art_texture = newTexture;
        g_cover_art_texture_created = true;
        rebuilt_texture = true;
        log_msg(LOGGER_INFO, "Cover art texture rebuilt (%zux%zu)", texWidth, texHeight);
    } while (0);

    if (!rebuilt_texture && !had_existing_texture) {
        g_cover_art_texture = nil;
        g_cover_art_texture_created = false;
    }

    g_cover_art_texture_rebuild_pending = false;
}

static void request_cover_art_texture_rebuild(void) {
    if (!g_showing_cover_art || !g_cover_art) return;
    if (g_cover_art_texture_rebuild_pending) return;

    g_cover_art_texture_rebuild_pending = true;
    dispatch_async(dispatch_get_main_queue(), ^{
        rebuild_cover_art_texture_on_main_thread();
    });
}

#pragma mark - Metal Renderer

@interface MetalRenderer : NSObject <MTKViewDelegate>
@end

@implementation MetalRenderer

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    // Recreate idle texture when size changes
    g_idle_texture_created = false;
    g_idle_texture = nil;
}

- (void)drawInMTKView:(MTKView *)view {
    // Grab the latest frame atomically
    pthread_mutex_lock(&g_frame_mutex);
    CVPixelBufferRef pixelBuffer = g_pending_frame;
    bool isNewFrame = g_has_new_frame;
    if (pixelBuffer) {
        CVPixelBufferRetain(pixelBuffer);
    }
    g_has_new_frame = false;
    pthread_mutex_unlock(&g_frame_mutex);

    // Static content (cover art / idle) - only redraw when state changes
    if (g_showing_cover_art) {
        if (!g_cover_art_texture_created || !g_cover_art_texture) {
            if (pixelBuffer) CVPixelBufferRelease(pixelBuffer);
            request_cover_art_texture_rebuild();
            return;
        }

        if (pixelBuffer) CVPixelBufferRelease(pixelBuffer);
        @autoreleasepool {
            dispatch_semaphore_wait(g_inflight_semaphore, DISPATCH_TIME_FOREVER);
            id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
                dispatch_semaphore_signal(g_inflight_semaphore);
            }];
            id<CAMetalDrawable> drawable = [view currentDrawable];

            if (drawable && g_metal_device) {
                MPSImageBilinearScale *scaler = [[MPSImageBilinearScale alloc] initWithDevice:g_metal_device];

                double srcAspect = (double)g_cover_art_texture.width / (double)g_cover_art_texture.height;
                double dstAspect = (double)drawable.texture.width / (double)drawable.texture.height;
                double scaleX, scaleY, translateX = 0, translateY = 0;

                if (srcAspect > dstAspect) {
                    scaleX = (double)drawable.texture.width / (double)g_cover_art_texture.width;
                    scaleY = scaleX;
                    translateY = (drawable.texture.height - g_cover_art_texture.height * scaleY) / 2.0;
                } else {
                    scaleY = (double)drawable.texture.height / (double)g_cover_art_texture.height;
                    scaleX = scaleY;
                    translateX = (drawable.texture.width - g_cover_art_texture.width * scaleX) / 2.0;
                }

                MPSScaleTransform transform = { .scaleX = scaleX, .scaleY = scaleY, .translateX = translateX, .translateY = translateY };
                scaler.scaleTransform = &transform;

                MTLRenderPassDescriptor *clearPass = [MTLRenderPassDescriptor renderPassDescriptor];
                clearPass.colorAttachments[0].texture = drawable.texture;
                clearPass.colorAttachments[0].loadAction = MTLLoadActionClear;
                clearPass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                clearPass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> clearEncoder = [commandBuffer renderCommandEncoderWithDescriptor:clearPass];
                [clearEncoder endEncoding];

                [scaler encodeToCommandBuffer:commandBuffer
                                sourceTexture:g_cover_art_texture
                           destinationTexture:drawable.texture];

                [commandBuffer presentDrawable:drawable];
                [commandBuffer commit];
            } else {
                dispatch_semaphore_signal(g_inflight_semaphore);
            }
        }
        return;
    }

    // No frame available - show idle screen
    if (!pixelBuffer) {
        @autoreleasepool {
            dispatch_semaphore_wait(g_inflight_semaphore, DISPATCH_TIME_FOREVER);
            id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
                dispatch_semaphore_signal(g_inflight_semaphore);
            }];
            id<CAMetalDrawable> drawable = [view currentDrawable];

            if (drawable && g_metal_device) {
                if (!g_idle_texture_created || !g_idle_texture) {
                    create_idle_texture(drawable.texture.width, drawable.texture.height);
                }

                if (g_idle_texture) {
                    id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
                    [blitEncoder copyFromTexture:g_idle_texture
                                     sourceSlice:0
                                     sourceLevel:0
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:MTLSizeMake(g_idle_texture.width, g_idle_texture.height, 1)
                                       toTexture:drawable.texture
                                destinationSlice:0
                                destinationLevel:0
                               destinationOrigin:MTLOriginMake(0, 0, 0)];
                    [blitEncoder endEncoding];

                    [commandBuffer presentDrawable:drawable];
                    [commandBuffer commit];
                } else {
                    dispatch_semaphore_signal(g_inflight_semaphore);
                }
            } else {
                dispatch_semaphore_signal(g_inflight_semaphore);
            }
        }
        return;
    }

    // No new frame since last render - skip GPU work, just release the buffer
    if (!isNewFrame) {
        CVPixelBufferRelease(pixelBuffer);
        return;
    }

    @autoreleasepool {
        dispatch_semaphore_wait(g_inflight_semaphore, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
                (void)cb;
            dispatch_semaphore_signal(g_inflight_semaphore);
        }];
        id<CAMetalDrawable> drawable = [view currentDrawable];

        if (drawable) {
            // Create texture from pixel buffer
            size_t srcWidth = CVPixelBufferGetWidth(pixelBuffer);
            size_t srcHeight = CVPixelBufferGetHeight(pixelBuffer);

            CVMetalTextureRef metalTexture = NULL;
            CVReturn result = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                g_texture_cache,
                pixelBuffer,
                NULL,
                MTLPixelFormatBGRA8Unorm,
                srcWidth, srcHeight,
                0,
                &metalTexture
            );

            if (result != kCVReturnSuccess || !metalTexture) {
                static uint64_t texture_errors = 0;
                if (++texture_errors <= 5 || texture_errors % 100 == 0) {
                    log_msg(LOGGER_ERR, "CVMetalTextureCacheCreateTextureFromImage failed: %d (error count: %llu, size: %zux%zu)",
                            (int)result, texture_errors, srcWidth, srcHeight);
                }
            }
            if (result == kCVReturnSuccess && metalTexture) {
                id<MTLTexture> texture = CVMetalTextureGetTexture(metalTexture);

                // Use Metal Performance Shaders for high-quality scaling
                MPSImageBilinearScale *scaler = [[MPSImageBilinearScale alloc] initWithDevice:g_metal_device];

                // For 90/270 rotation, transpose first then apply flips
                bool needsTranspose = (g_rotation == LEFT || g_rotation == RIGHT);
                id<MTLTexture> sourceForScale = texture;
                size_t effectiveWidth = srcWidth;
                size_t effectiveHeight = srcHeight;

                if (needsTranspose) {
                    // Create/reuse intermediate texture with swapped dimensions
                    if (!g_rotation_texture ||
                        g_rotation_texture.width != srcHeight ||
                        g_rotation_texture.height != srcWidth) {
                        MTLTextureDescriptor *desc = [MTLTextureDescriptor
                            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                        width:srcHeight
                                                       height:srcWidth
                                                    mipmapped:NO];
                        desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                        g_rotation_texture = [g_metal_device newTextureWithDescriptor:desc];
                    }

                    MPSImageTranspose *transpose = [[MPSImageTranspose alloc] initWithDevice:g_metal_device];
                    [transpose encodeToCommandBuffer:commandBuffer
                                       sourceTexture:texture
                                  destinationTexture:g_rotation_texture];

                    sourceForScale = g_rotation_texture;
                    effectiveWidth = srcHeight;
                    effectiveHeight = srcWidth;
                }

                // Calculate scale transform to fit drawable while maintaining aspect ratio
                double srcAspect = (double)effectiveWidth / (double)effectiveHeight;
                double dstAspect = (double)drawable.texture.width / (double)drawable.texture.height;

                double baseScaleX, baseScaleY;
                double translateX = 0, translateY = 0;

                if (srcAspect > dstAspect) {
                    baseScaleX = (double)drawable.texture.width / (double)effectiveWidth;
                    baseScaleY = baseScaleX;
                    translateY = (drawable.texture.height - effectiveHeight * baseScaleY) / 2.0;
                } else {
                    baseScaleY = (double)drawable.texture.height / (double)effectiveHeight;
                    baseScaleX = baseScaleY;
                    translateX = (drawable.texture.width - effectiveWidth * baseScaleX) / 2.0;
                }

                double scaleX = baseScaleX;
                double scaleY = baseScaleY;

                // Compute combined flip flags from user flip + rotation
                bool flipX = false;
                bool flipY = false;

                if (needsTranspose) {
                    // After transpose, specific flips complete the 90/270 rotation.
                    // Combined with user flip to match GStreamer videoflip behavior.
                    if (g_rotation == LEFT) {
                        flipX = (g_flip == VFLIP || g_flip == INVERT);
                        flipY = (g_flip == NONE || g_flip == VFLIP);
                    } else { // RIGHT
                        flipX = (g_flip == NONE || g_flip == HFLIP);
                        flipY = (g_flip == HFLIP || g_flip == INVERT);
                    }
                } else {
                    flipX = (g_flip == HFLIP || g_flip == INVERT);
                    flipY = (g_flip == VFLIP || g_flip == INVERT);
                    if (g_rotation == INVERT) {
                        flipX = !flipX;
                        flipY = !flipY;
                    }
                }

                if (flipX) {
                    scaleX = -scaleX;
                    translateX = drawable.texture.width - translateX;
                }
                if (flipY) {
                    scaleY = -scaleY;
                    translateY = drawable.texture.height - translateY;
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
                                sourceTexture:sourceForScale
                           destinationTexture:drawable.texture];

                // FPS overlay (drawn on top of video)
                update_fps_counters();
                render_fps_overlay(commandBuffer, drawable.texture);

                // Frame pacing: hold each frame for a consistent duration
                if (g_source_frame_interval > 0.0) {
                    [commandBuffer presentDrawable:drawable afterMinimumDuration:g_source_frame_interval];
                } else {
                    [commandBuffer presentDrawable:drawable];
                }
                [commandBuffer commit];

                g_frames_rendered++;
                CFRelease(metalTexture);
            }
        } else {
            dispatch_semaphore_signal(g_inflight_semaphore);
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

            // Use custom window class for keyboard handling
            g_window = [[UxPlayWindow alloc] initWithContentRect:frame
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

            // Subview frames use origin (0,0) relative to their superview
            NSRect viewFrame = NSMakeRect(0, 0, frame.size.width, frame.size.height);

            // Use custom Metal view for double-click handling
            g_metal_view = [[UxPlayMetalView alloc] initWithFrame:viewFrame device:g_metal_device];
            g_metal_view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
            g_metal_view.framebufferOnly = NO;
            g_metal_view.preferredFramesPerSecond = NSScreen.mainScreen.maximumFramesPerSecond;

            // Triple-buffer semaphore for smooth GPU pipelining
            g_inflight_semaphore = dispatch_semaphore_create(3);

            g_metal_renderer = [[MetalRenderer alloc] init];
            g_metal_view.delegate = g_metal_renderer;

            g_command_queue = [g_metal_device newCommandQueue];

            // Create texture cache
            CVMetalTextureCacheCreate(kCFAllocatorDefault,
                                      NULL,
                                      g_metal_device,
                                      NULL,
                                      &g_texture_cache);

            // Container view: fills window, clips children, provides black letterbox background
            g_container_view = [[NSView alloc] initWithFrame:viewFrame];
            g_container_view.wantsLayer = YES;
            g_container_view.layer.backgroundColor = [[NSColor blackColor] CGColor];
            g_container_view.layer.masksToBounds = YES;

            // Rotation host view: receives CA rotation transform, sized to source dimensions
            g_rotation_host_view = [[NSView alloc] initWithFrame:viewFrame];
            g_rotation_host_view.wantsLayer = YES;
            g_rotation_host_view.layer.masksToBounds = NO;
            g_rotation_host_view.layer.anchorPoint = CGPointMake(0.5, 0.5);

            // MTKView always fills the rotation host view
            g_rotation_host_view.translatesAutoresizingMaskIntoConstraints = NO;
            g_metal_view.translatesAutoresizingMaskIntoConstraints = NO;
            [g_rotation_host_view addSubview:g_metal_view];

            g_rotation_host_fill_constraints = @[
                [g_rotation_host_view.leadingAnchor constraintEqualToAnchor:g_container_view.leadingAnchor],
                [g_rotation_host_view.trailingAnchor constraintEqualToAnchor:g_container_view.trailingAnchor],
                [g_rotation_host_view.topAnchor constraintEqualToAnchor:g_container_view.topAnchor],
                [g_rotation_host_view.bottomAnchor constraintEqualToAnchor:g_container_view.bottomAnchor]
            ];
            g_metal_view_fill_constraints = @[
                [g_metal_view.leadingAnchor constraintEqualToAnchor:g_rotation_host_view.leadingAnchor],
                [g_metal_view.trailingAnchor constraintEqualToAnchor:g_rotation_host_view.trailingAnchor],
                [g_metal_view.topAnchor constraintEqualToAnchor:g_rotation_host_view.topAnchor],
                [g_metal_view.bottomAnchor constraintEqualToAnchor:g_rotation_host_view.bottomAnchor]
            ];

            [g_container_view addSubview:g_rotation_host_view];
            [NSLayoutConstraint activateConstraints:g_rotation_host_fill_constraints];
            [NSLayoutConstraint activateConstraints:g_metal_view_fill_constraints];
            g_rotation_host_constraints_active = true;
            [g_window setContentView:g_container_view];
            [g_window makeFirstResponder:g_metal_view];

            // Make app appear in dock and bring window to front
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];
            [g_window makeKeyAndOrderFront:nil];
            [g_window setLevel:NSNormalWindowLevel];

            // Set a default app icon (using system SF Symbol or fallback)
            if (@available(macOS 11.0, *)) {
                NSImage *icon = [NSImage imageWithSystemSymbolName:@"airplayvideo"
                                          accessibilityDescription:@"AirPlay"];
                if (icon) {
                    // Tint the SF Symbol to make it more visible
                    NSImage *tintedIcon = [icon copy];
                    [tintedIcon lockFocus];
                    [[NSColor systemBlueColor] set];
                    NSRect imageRect = NSMakeRect(0, 0, tintedIcon.size.width, tintedIcon.size.height);
                    NSRectFillUsingOperation(imageRect, NSCompositingOperationSourceAtop);
                    [tintedIcon unlockFocus];
                    [NSApp setApplicationIconImage:tintedIcon];
                } else {
                    [NSApp setApplicationIconImage:[NSImage imageNamed:NSImageNameNetwork]];
                }
            } else {
                [NSApp setApplicationIconImage:[NSImage imageNamed:NSImageNameNetwork]];
            }

            // Create menu bar and set window delegate for close button handling
            g_menu_handler = [[UxPlayMenuHandler alloc] init];
            [g_window setDelegate:g_menu_handler];
            create_menu_bar();

            // Apply initial fullscreen if requested via -fs flag
            if (g_initial_fullscreen) {
                g_windowed_frame = frame;
                NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
                NSRect screenFrame = [screen frame];
                [g_window setStyleMask:NSWindowStyleMaskBorderless];
                [g_window setFrame:screenFrame display:YES];
                [g_window setLevel:NSScreenSaverWindowLevel];
                [NSCursor hide];
                g_fullscreen = true;
                log_msg(LOGGER_INFO, "Starting in fullscreen mode");
            }

            layout_video_views_on_main_thread();

            // Start stall detection timer (checks every second)
            g_stall_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(g_stall_timer, dispatch_time(DISPATCH_TIME_NOW, 0), NSEC_PER_SEC, NSEC_PER_SEC / 4);
            dispatch_source_set_event_handler(g_stall_timer, ^{
                if (g_last_frame_time > 0) {
                    mach_timebase_info_data_t timebase;
                    mach_timebase_info(&timebase);
                    double elapsed = (double)(mach_absolute_time() - g_last_frame_time) * timebase.numer / timebase.denom / 1e9;
                    if (elapsed > FRAME_STALL_TIMEOUT) {
                        pthread_mutex_lock(&g_frame_mutex);
                        if (g_pending_frame) {
                            CVPixelBufferRelease(g_pending_frame);
                            g_pending_frame = NULL;
                        }
                        pthread_mutex_unlock(&g_frame_mutex);
                        g_last_frame_time = 0;
                        update_window_title("Ready");
                        uint64_t now = mach_absolute_time();
                        double since_last_log = (g_last_stall_log_time > 0) ?
                            (double)(now - g_last_stall_log_time) * timebase.numer / timebase.denom / 1e9 : 999.0;
                        if (since_last_log > 30.0) {
                            log_msg(LOGGER_INFO, "Stream stalled, returning to idle screen");
                            g_last_stall_log_time = now;
                        } else {
                            log_msg(LOGGER_DEBUG, "Stream stalled, returning to idle screen");
                        }
                    }
                }
            });
            dispatch_resume(g_stall_timer);

            log_msg(LOGGER_INFO, "Native macOS window created with Metal rendering (F=fullscreen, ESC=exit fullscreen, Q=quit)");
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
            g_idle_texture = nil;
            g_idle_texture_created = false;
            g_metal_view = nil;
            g_metal_view_fill_constraints = nil;
            g_rotation_host_fill_constraints = nil;
            g_rotation_host_constraints_active = false;
            g_rotation_host_view = nil;
            g_container_view = nil;
            g_metal_renderer = nil;
            g_metal_device = nil;
            g_command_queue = nil;
            if (g_stall_timer) {
                dispatch_source_cancel(g_stall_timer);
                g_stall_timer = NULL;
            }
        }
    });
}

#pragma mark - VideoToolbox Decoder

// Helper function to find next start code (supports both 3-byte and 4-byte)
// Returns the position after the start code, or length if not found
// Sets *start_code_len to the length of the start code found (3 or 4)
static size_t find_next_start_code(const uint8_t *data, size_t length, size_t start, int *start_code_len) {
    for (size_t i = start; i + 2 < length; i++) {
        // Check for 3-byte start code: 00 00 01
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            // Check if it's actually a 4-byte start code: 00 00 00 01
            if (i > 0 && data[i-1] == 0) {
                // This is part of a 4-byte code, skip
                continue;
            }
            *start_code_len = 3;
            return i + 3;
        }
        // Check for 4-byte start code: 00 00 00 01
        if (i + 3 < length && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
            *start_code_len = 4;
            return i + 4;
        }
    }
    *start_code_len = 0;
    return length;
}

static bool parse_sps_pps(const uint8_t *data, size_t length) {
    // Find SPS and PPS NAL units in the data (supports both H.264 and HEVC)
    // NAL units are separated by start codes: 0x00 0x00 0x01 (3-byte) or 0x00 0x00 0x00 0x01 (4-byte)
    //
    // H.264 NAL types (5 bits, mask 0x1F):
    //   7 = SPS, 8 = PPS
    //
    // HEVC NAL types (6 bits, shifted right by 1):
    //   32 = VPS, 33 = SPS, 34 = PPS

    size_t i = 0;
    int start_code_len = 0;

    // Find first start code
    size_t nal_start = find_next_start_code(data, length, 0, &start_code_len);

    while (nal_start < length) {
        // Find the end of this NAL (next start code or end of data)
        int next_start_code_len = 0;
        size_t next_start = nal_start;
        size_t nal_end = length;

        // Search for next start code
        for (size_t j = nal_start + 1; j + 2 < length; j++) {
            // Check for 3-byte start code
            if (data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) {
                // Check if it's a 4-byte code
                if (j > 0 && data[j-1] == 0) {
                    nal_end = j - 1;  // Exclude the leading 00 of 4-byte code
                } else {
                    nal_end = j;
                }
                next_start = (data[j-1] == 0 && j > 0) ? j - 1 : j;
                break;
            }
        }

        size_t nal_size = nal_end - nal_start;
        if (nal_size > 0) {
            if (g_is_h265) {
                // HEVC: NAL type is in bits 1-6 of first byte
                uint8_t nal_type = (data[nal_start] >> 1) & 0x3F;

                if (nal_type == 32) { // VPS
                    if (g_vps) free(g_vps);
                    g_vps = malloc(nal_size);
                    memcpy(g_vps, &data[nal_start], nal_size);
                    g_vps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found HEVC VPS, size=%zu", nal_size);
                } else if (nal_type == 33) { // SPS
                    if (g_sps) free(g_sps);
                    g_sps = malloc(nal_size);
                    memcpy(g_sps, &data[nal_start], nal_size);
                    g_sps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found HEVC SPS, size=%zu", nal_size);
                } else if (nal_type == 34) { // PPS
                    if (g_pps) free(g_pps);
                    g_pps = malloc(nal_size);
                    memcpy(g_pps, &data[nal_start], nal_size);
                    g_pps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found HEVC PPS, size=%zu", nal_size);
                }
            } else {
                // H.264: NAL type is in bits 0-4
                uint8_t nal_type = data[nal_start] & 0x1F;

                if (nal_type == 7) { // SPS
                    if (g_sps) free(g_sps);
                    g_sps = malloc(nal_size);
                    memcpy(g_sps, &data[nal_start], nal_size);
                    g_sps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found H.264 SPS, size=%zu", nal_size);
                } else if (nal_type == 8) { // PPS
                    if (g_pps) free(g_pps);
                    g_pps = malloc(nal_size);
                    memcpy(g_pps, &data[nal_start], nal_size);
                    g_pps_size = nal_size;
                    log_msg(LOGGER_DEBUG, "Found H.264 PPS, size=%zu", nal_size);
                }
            }
        }

        // Move to next NAL
        if (nal_end >= length) break;
        nal_start = find_next_start_code(data, length, nal_end, &start_code_len);
    }

    // HEVC requires VPS+SPS+PPS, H.264 requires SPS+PPS
    if (g_is_h265) {
        return (g_vps != NULL && g_sps != NULL && g_pps != NULL);
    } else {
        return (g_sps != NULL && g_pps != NULL);
    }
}

static void create_decoder_session(void) {
    pthread_mutex_lock(&g_decoder_mutex);

    if (g_decoder_session) {
        pthread_mutex_unlock(&g_decoder_mutex);
        return;
    }

    OSStatus status;

    if (g_format_desc) {
        CFRelease(g_format_desc);
        g_format_desc = NULL;
    }

    if (g_is_h265) {
        // HEVC requires VPS, SPS, and PPS
        if (!g_vps || !g_sps || !g_pps) {
            pthread_mutex_unlock(&g_decoder_mutex);
            return;
        }

        const uint8_t *parameterSetPointers[3] = {g_vps, g_sps, g_pps};
        const size_t parameterSetSizes[3] = {g_vps_size, g_sps_size, g_pps_size};

        if (@available(macOS 10.13, *)) {
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                kCFAllocatorDefault,
                3,
                parameterSetPointers,
                parameterSetSizes,
                4,  // NAL unit length field size
                NULL,  // extensions
                &g_format_desc
            );
        } else {
            log_msg(LOGGER_ERR, "HEVC decoding requires macOS 10.13 or later");
            pthread_mutex_unlock(&g_decoder_mutex);
            return;
        }

        if (status != noErr) {
            log_msg(LOGGER_ERR, "Failed to create HEVC format description: %d", (int)status);
            pthread_mutex_unlock(&g_decoder_mutex);
            return;
        }
        log_msg(LOGGER_INFO, "Created HEVC format description");
    } else {
        // H.264 requires SPS and PPS
        if (!g_sps || !g_pps) {
            pthread_mutex_unlock(&g_decoder_mutex);
            return;
        }

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
            log_msg(LOGGER_ERR, "Failed to create H.264 format description: %d", (int)status);
            pthread_mutex_unlock(&g_decoder_mutex);
            return;
        }
        log_msg(LOGGER_INFO, "Created H.264 format description");
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

    // Enable Metal compatibility and IOSurface backing (required for CVMetalTextureCache)
    CFDictionarySetValue(buffer_attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    CFDictionaryRef io_surface_props = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(buffer_attrs, kCVPixelBufferIOSurfacePropertiesKey, io_surface_props);
    CFRelease(io_surface_props);

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
        if (g_format_desc) {
            CFRelease(g_format_desc);
            g_format_desc = NULL;
        }
        pthread_mutex_unlock(&g_decoder_mutex);
        return;
    }

    // Set real-time mode for lowest latency
    VTSessionSetProperty(g_decoder_session,
                         kVTDecompressionPropertyKey_RealTime,
                         kCFBooleanTrue);

    log_msg(LOGGER_INFO, "VideoToolbox decoder session created (hardware accelerated)");
    pthread_mutex_unlock(&g_decoder_mutex);
}

static void destroy_decoder_session(void) {
    pthread_mutex_lock(&g_decoder_mutex);

    if (g_decoder_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(g_decoder_session);
        VTDecompressionSessionInvalidate(g_decoder_session);
        CFRelease(g_decoder_session);
        g_decoder_session = NULL;
    }
    if (g_format_desc) {
        CFRelease(g_format_desc);
        g_format_desc = NULL;
    }

    pthread_mutex_unlock(&g_decoder_mutex);
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
    g_fps_decode_count++;
    uint64_t now = mach_absolute_time();
    g_last_frame_time = now;

    // Estimate source frame interval using exponential moving average
    if (g_prev_decode_time > 0) {
        mach_timebase_info_data_t timebase;
        mach_timebase_info(&timebase);
        double interval = (double)(now - g_prev_decode_time) * timebase.numer / timebase.denom / 1e9;
        // Only consider reasonable intervals (8ms to 50ms → 20fps to 120fps)
        if (interval > 0.008 && interval < 0.050) {
            if (g_source_frame_interval <= 0.0) {
                g_source_frame_interval = interval;
            } else {
                // Smooth: 90% old, 10% new to absorb jitter
                g_source_frame_interval = g_source_frame_interval * 0.9 + interval * 0.1;
            }
        }
    }
    g_prev_decode_time = now;

    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);

    // Detect dimension changes (device rotation or resolution change)
    bool initial_dimensions_detected = (g_current_width == 0 || g_current_height == 0);
    bool dimensions_changed = (width != g_current_width || height != g_current_height);
    if (dimensions_changed) {
        log_msg(LOGGER_INFO, "Video dimensions %s: %zux%zu",
                g_current_width == 0 ? "detected" : "changed", width, height);
        g_current_width = width;
        g_current_height = height;
        if (g_window_title_mode == WINDOW_TITLE_MODE_STREAMING) {
            refresh_window_title();
        }
    }

    if (g_frames_decoded == 1) {
        set_video_paused(false, false);
        update_window_title("Streaming");
        g_showing_cover_art = false;
    }

    // Resize window when dimensions change (initial frame or device rotation)
    if (dimensions_changed && g_window) {
        // Reset manual rotation since stream dimensions changed
        g_manual_rotation_degrees = 0;

        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_window) {
                cancel_manual_rotation_animation_on_main_thread();
                CGFloat display_width = 0.0;
                CGFloat display_height = 0.0;
                get_base_display_dimensions(&display_width, &display_height);
                bool animate_window_resize = initial_dimensions_detected && !g_fullscreen;

                if (!g_fullscreen) {
                    NSSize target_size = scaled_content_size_for_display(display_width, display_height);
                    if (animate_window_resize) {
                        animate_window_content_resize(target_size.width, target_size.height, 0.26);
                        log_msg(LOGGER_INFO, "Window content animating to %.0fx%.0f for stream start",
                                target_size.width, target_size.height);
                    } else {
                        resize_window_content(target_size.width, target_size.height, NO);
                        log_msg(LOGGER_INFO, "Window content resized to %.0fx%.0f", target_size.width, target_size.height);
                    }
                }

                if (!animate_window_resize) {
                    layout_video_views_on_main_thread();
                }
            }
        });
    }

    // When paused, keep decoding but don't update the displayed frame
    if (g_paused) {
        return;
    }

    // Store frame for rendering
    pthread_mutex_lock(&g_frame_mutex);
    if (g_pending_frame) {
        CVPixelBufferRelease(g_pending_frame);
    }
    g_pending_frame = CVPixelBufferRetain(imageBuffer);
    g_has_new_frame = true;
    pthread_mutex_unlock(&g_frame_mutex);

    log_stats();
}

// Helper: check if NAL is a parameter set (should be skipped in bitstream)
static bool is_parameter_set_nal(uint8_t first_byte, bool is_h265) {
    if (is_h265) {
        // HEVC: NAL type in bits 1-6
        uint8_t nal_type = (first_byte >> 1) & 0x3F;
        return (nal_type == 32 || nal_type == 33 || nal_type == 34); // VPS, SPS, PPS
    } else {
        // H.264: NAL type in bits 0-4
        uint8_t nal_type = first_byte & 0x1F;
        return (nal_type == 7 || nal_type == 8); // SPS, PPS
    }
}

static bool decode_frame(const uint8_t *data, size_t length, uint64_t pts) {
    bool is_h265 = false;
    pthread_mutex_lock(&g_decoder_mutex);
    is_h265 = g_is_h265;
    pthread_mutex_unlock(&g_decoder_mutex);

    // Convert Annex-B to AVCC/HVCC format (replace start codes with length)
    // For simplicity, we'll create a copy with length-prefixed format

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

            // Skip parameter set NALs (VPS/SPS/PPS), only include VCL NALs
            if (nal_size > 0 && !is_parameter_set_nal(data[nal_start], is_h265)) {
                avcc_size += 4 + nal_size;  // 4-byte length + NAL
                nal_count++;
            }

            i = nal_end;
        } else {
            i++;
        }
    }

    if (avcc_size == 0) return false;

    // Create AVCC/HVCC buffer
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

            if (nal_size > 0 && !is_parameter_set_nal(data[nal_start], is_h265)) {
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

    pthread_mutex_lock(&g_decoder_mutex);
    if (!g_decoder_session || !g_format_desc) {
        pthread_mutex_unlock(&g_decoder_mutex);
        CFRelease(block_buffer);
        return false;
    }

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

    if (status != noErr) {
        pthread_mutex_unlock(&g_decoder_mutex);
        CFRelease(block_buffer);
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

    pthread_mutex_unlock(&g_decoder_mutex);

    CFRelease(block_buffer);
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
    g_initial_fullscreen = initial_fullscreen;
    g_first_frame = true;
    g_base_time = 0;

    // Store server name for window title
    if (g_server_name) free(g_server_name);
    g_server_name = server_name ? strdup(server_name) : strdup("UxPlay");

    // Create display window
    create_window(server_name);

    g_initialized = true;
    log_msg(LOGGER_INFO, "Native macOS video renderer initialized (VideoToolbox + Metal)");
}

void video_renderer_start(void) {
    log_msg(LOGGER_INFO, "video_renderer_start called (initialized=%d, running=%d, decoder=%s)",
            g_initialized, g_running, decoder_session_exists() ? "exists" : "none");
    g_running = true;
    set_video_paused(false, false);
    log_msg(LOGGER_INFO, "Video renderer started and ready for frames");
}

void video_renderer_stop(void) {
    log_msg(LOGGER_INFO, "video_renderer_stop called");
    // Don't set g_running = false - renderer stays active for new connections
    // video_renderer_start is only called once at init, not per-connection
    destroy_decoder_session();

    // Clear pending frame so idle screen is shown
    pthread_mutex_lock(&g_frame_mutex);
    if (g_pending_frame) {
        CVPixelBufferRelease(g_pending_frame);
        g_pending_frame = NULL;
    }
    pthread_mutex_unlock(&g_frame_mutex);

    // Reset state for next stream
    g_first_frame = true;
    g_base_time = 0;
    g_frames_received = 0;
    g_frames_decoded = 0;
    g_frames_rendered = 0;
    g_decode_errors = 0;
    g_source_frame_interval = 0.0;
    g_prev_decode_time = 0;
    g_fps_frame_count = 0;
    g_fps_last_time = 0;
    g_fps_decode_count = 0;
    g_fps_decode_last_time = 0;
    g_current_width = 0;
    g_current_height = 0;
    g_fps_display = 0.0;
    g_fps_source = 0.0;
    set_video_paused(false, false);

    // Clear old SPS/PPS/VPS so new stream can set them
    clear_parameter_sets();

    // Flush texture cache to ensure clean state for new stream
    if (g_texture_cache) {
        CVMetalTextureCacheFlush(g_texture_cache, 0);
    }

    // Clear current texture reference
    g_current_texture = nil;

    // Reset idle texture so it regenerates at the correct size
    g_idle_texture_created = false;
    g_idle_texture = nil;
    g_manual_rotation_degrees = 0;

    // Preserve cover art state - cover art should persist across stream resets
    // It will be cleared when a real video stream starts or the renderer is destroyed
    if (!g_showing_cover_art) {
        // Restore window to original size and ensure it's ready for new connections
        dispatch_async(dispatch_get_main_queue(), ^{
            if (g_window) {
                cancel_manual_rotation_animation_on_main_thread();
                if (!g_fullscreen) {
                    resize_window_content(1280, 720, NO);
                }
                layout_video_views_on_main_thread();
                // Ensure window stays visible and responsive
                [g_window makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            }
        });

        update_window_title("Ready");
    }
    log_msg(LOGGER_INFO, "Video renderer stopped - ready for new stream");
}

void video_renderer_set_device_model(const char *model, const char *name) {
    // Update window title with device name
    if (name) {
        update_window_title(name);
    }
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) {
    // Store track metadata for cover art display
    if (g_track_title) { free(g_track_title); g_track_title = NULL; }
    if (g_track_artist) { free(g_track_artist); g_track_artist = NULL; }
    if (g_track_album) { free(g_track_album); g_track_album = NULL; }

    if (title) g_track_title = strdup(title);
    if (artist) g_track_artist = strdup(artist);
    if (album) g_track_album = strdup(album);

    log_msg(LOGGER_DEBUG, "Track metadata set: %s - %s", artist ?: "Unknown", title ?: "Unknown");

    // If we're showing cover art, recreate the texture with the new metadata
    if (g_showing_cover_art && g_cover_art) {
        request_cover_art_texture_rebuild();
        update_cover_art_window_title();
    }
}

void video_renderer_pause(void) {
    set_video_paused(true, false);
}

void video_renderer_resume(void) {
    set_video_paused(false, false);
}

int video_renderer_cycle(void) {
    // Clear expired cover art - called by uxplay when coverart has expired
    if (g_showing_cover_art) {
        g_showing_cover_art = false;
        g_cover_art_texture_created = false;
        g_cover_art_texture_rebuild_pending = false;
        g_cover_art_texture = nil;
        g_cover_art = nil;
        log_msg(LOGGER_INFO, "Cover art expired, returning to idle screen");
        update_window_title(NULL);
    }
    return 0;
}

bool video_renderer_is_paused(void) {
    return g_paused;
}

uint64_t video_renderer_render_buffer(unsigned char *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    g_frames_received++;

    // Clear cover art mode when video frames arrive
    if (g_showing_cover_art) {
        g_showing_cover_art = false;
        log_msg(LOGGER_INFO, "Switching from cover art to video mode");
    }

    if (!g_running) {
        log_msg(LOGGER_DEBUG, "Renderer not running, dropping frame");
        return 0;
    }

    // Validate data
    if (*data_len < 4) {
        log_msg(LOGGER_ERR, "Video data too short: %d bytes", *data_len);
        return 0;
    }

    // Log first frame info (or first frame after reconnection)
    if (g_frames_received == 1) {
        bool is_h265 = false;

        pthread_mutex_lock(&g_decoder_mutex);
        is_h265 = g_is_h265;
        pthread_mutex_unlock(&g_decoder_mutex);

        log_msg(LOGGER_INFO, "=== FIRST VIDEO FRAME RECEIVED (new stream) ===");
        log_msg(LOGGER_INFO, "  Size: %d bytes, NAL count: %d, Codec: %s",
                *data_len, *nal_count, is_h265 ? "H.265" : "H.264");
        log_msg(LOGGER_INFO, "  NTP time: %llu", *ntp_time);
        pthread_mutex_lock(&g_decoder_mutex);
        log_msg(LOGGER_INFO, "  Decoder session: %s, SPS: %s, PPS: %s, VPS: %s",
                g_decoder_session ? "exists" : "none",
                g_sps ? "yes" : "no",
                g_pps ? "yes" : "no",
                g_vps ? "yes" : "no");
        pthread_mutex_unlock(&g_decoder_mutex);

        // Log first few bytes to help debug
        char hex[64];
        int hex_len = (*data_len > 20) ? 20 : *data_len;
        for (int i = 0; i < hex_len; i++) {
            sprintf(hex + i*3, "%02x ", data[i]);
        }
        log_msg(LOGGER_INFO, "  First bytes: %s", hex);
    }

    // Soft validation - log warning but don't drop frame
    // (some valid frames may not start with 0x00)
    if (data[0] != 0 && g_frames_received <= 5) {
        log_msg(LOGGER_WARNING, "Frame #%llu: first byte=0x%02x (expected 0x00 for start code)",
                g_frames_received, data[0]);
    }

    // Parse SPS/PPS - check every frame for parameter set changes (e.g. device rotation)
    {
        uint8_t *old_sps = NULL;
        size_t old_sps_size = 0;
        bool had_old_sps = false;
        bool have_parameter_sets = false;
        bool sps_changed = false;
        bool decoder_exists = false;
        bool is_h265 = false;
        bool have_vps = false;
        size_t sps_size = 0;
        size_t pps_size = 0;

        pthread_mutex_lock(&g_decoder_mutex);

        is_h265 = g_is_h265;
        had_old_sps = (g_sps != NULL && g_sps_size > 0);
        if (had_old_sps) {
            old_sps_size = g_sps_size;
            old_sps = malloc(old_sps_size);
            if (old_sps) {
                memcpy(old_sps, g_sps, old_sps_size);
            }
        }

        have_parameter_sets = parse_sps_pps(data, *data_len);
        decoder_exists = (g_decoder_session != NULL);
        sps_size = g_sps_size;
        pps_size = g_pps_size;
        have_vps = (g_vps != NULL);

        if (have_parameter_sets) {
            if (!had_old_sps) {
                sps_changed = true;
            } else if (!g_sps || old_sps_size != g_sps_size || !old_sps ||
                       memcmp(old_sps, g_sps, g_sps_size) != 0) {
                sps_changed = true;
            }
        }

        pthread_mutex_unlock(&g_decoder_mutex);

        if (have_parameter_sets) {
            if (!decoder_exists) {
                log_msg(LOGGER_INFO, "Found parameter sets - SPS:%zu bytes, PPS:%zu bytes%s",
                        sps_size, pps_size, is_h265 && have_vps ? ", VPS present" : "");
                log_msg(LOGGER_INFO, "Creating decoder session...");
                create_decoder_session();
                if (decoder_session_exists()) {
                    log_msg(LOGGER_INFO, "Decoder session created successfully");
                } else {
                    log_msg(LOGGER_ERR, "Failed to create decoder session!");
                }
            } else if (sps_changed) {
                log_msg(LOGGER_INFO, "SPS changed (device rotation or resolution change), recreating decoder...");
                destroy_decoder_session();
                create_decoder_session();
                if (decoder_session_exists()) {
                    g_first_frame = true;
                    g_frames_decoded = 0;
                    log_msg(LOGGER_INFO, "Decoder session recreated successfully");
                } else {
                    log_msg(LOGGER_ERR, "Failed to recreate decoder session!");
                }
            }
        } else if (!decoder_exists && g_frames_received == 1) {
            log_msg(LOGGER_INFO, "No SPS/PPS found in first frame, waiting for keyframe...");
        }
        free(old_sps);
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
    pthread_mutex_lock(&g_decoder_mutex);
    if (g_decoder_session) {
        VTDecompressionSessionWaitForAsynchronousFrames(g_decoder_session);
    }
    pthread_mutex_unlock(&g_decoder_mutex);
}

void video_renderer_destroy(void) {
    g_running = false;

    destroy_decoder_session();
    destroy_window();

    clear_parameter_sets();
    if (g_server_name) { free(g_server_name); g_server_name = NULL; }
    if (g_window_title_custom_status) { free(g_window_title_custom_status); g_window_title_custom_status = NULL; }
    g_window_title_mode = WINDOW_TITLE_MODE_NONE;
    g_cover_art = nil;
    g_showing_cover_art = false;
    g_cover_art_texture_created = false;
    g_cover_art_texture_rebuild_pending = false;
    g_cover_art_texture = nil;
    g_rotation_texture = nil;

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
    if (!data || !data_len || *data_len <= 0) return;

    log_msg(LOGGER_INFO, "Displaying cover art (JPEG, %d bytes)", *data_len);

    // Copy data for use in async block
    size_t len = *data_len;
    void *dataCopy = malloc(len);
    memcpy(dataCopy, data, len);

    dispatch_async(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            // Create NSImage from JPEG data
            NSData *imageData = [NSData dataWithBytesNoCopy:dataCopy length:len freeWhenDone:YES];
            NSImage *image = [[NSImage alloc] initWithData:imageData];

            if (!image) {
                log_msg(LOGGER_ERR, "Failed to create image from JPEG data");
                return;
            }

            // Store for rendering
            g_cover_art = image;
            g_showing_cover_art = true;

            // Get cover art size
            NSSize imageSize = [image size];
            if (imageSize.width <= 0 || imageSize.height <= 0) {
                log_msg(LOGGER_ERR, "Invalid image size");
                return;
            }

            // Set window to 16:9 aspect ratio for nice display
            CGFloat windowWidth = 1280;
            CGFloat windowHeight = 720;
            g_manual_rotation_degrees = 0;
            cancel_manual_rotation_animation_on_main_thread();
            if (g_window && !g_fullscreen) {
                resize_window_content(windowWidth, windowHeight, NO);
            }
            layout_video_views_on_main_thread();

            update_cover_art_window_title();
            rebuild_cover_art_texture_on_main_thread();
        }
    });
}

bool waiting_for_x11_window(void) {
    return false;  // Not applicable on macOS
}

bool video_get_playback_info(double *duration, double *position, double *seek_start,
                             double *seek_duration, float *rate, bool *buffer_empty, bool *buffer_full) {
    return false;  // HLS not supported
}

int video_renderer_choose_codec(bool video_is_jpeg, bool video_is_h265) {
    bool codec_changed = false;
    bool had_decoder_session = false;

    pthread_mutex_lock(&g_decoder_mutex);
    codec_changed = (g_is_h265 != video_is_h265);
    g_is_h265 = video_is_h265;
    had_decoder_session = (g_decoder_session != NULL);
    pthread_mutex_unlock(&g_decoder_mutex);

    log_msg(LOGGER_INFO, "Codec selected: %s%s", video_is_h265 ? "H.265/HEVC" : "H.264",
            codec_changed ? " (codec changed, will recreate decoder)" : "");

    // If codec changed and we have an existing decoder, destroy it
    // so a new one will be created with the correct format
    if (codec_changed && had_decoder_session) {
        log_msg(LOGGER_INFO, "Destroying existing decoder session due to codec change");
        destroy_decoder_session();

        // Clear old parameter sets since they're for the wrong codec
        clear_parameter_sets();

        // Reset frame state for new stream
        g_first_frame = true;
        g_frames_decoded = 0;
    }

    return 0;
}
