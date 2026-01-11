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

// Server name for window title
static char *g_server_name = NULL;

// Cover art storage
static NSImage *g_cover_art = NULL;
static bool g_showing_cover_art = false;
static id<MTLTexture> g_cover_art_texture = NULL;
static bool g_cover_art_texture_created = false;

// Track metadata for cover art display
static char *g_track_title = NULL;
static char *g_track_artist = NULL;
static char *g_track_album = NULL;

// Idle screen texture
static id<MTLTexture> g_idle_texture = NULL;
static bool g_idle_texture_created = false;

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

#pragma mark - Fullscreen Toggle

static void toggle_fullscreen(void) {
    if (!g_window) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_fullscreen) {
            // Exit fullscreen
            [g_window setStyleMask:NSWindowStyleMaskTitled |
                                   NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskResizable |
                                   NSWindowStyleMaskMiniaturizable];
            [g_window setFrame:g_windowed_frame display:YES animate:YES];
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
            g_paused = !g_paused;
            log_msg(LOGGER_INFO, "Video %s", g_paused ? "paused" : "resumed");
            return;
        } else if (c == 27) { // ESC
            if (g_fullscreen) {
                toggle_fullscreen();
            }
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
- (void)togglePause:(id)sender;
- (void)quit:(id)sender;
@end

@implementation UxPlayMenuHandler

- (void)toggleFullscreen:(id)sender {
    toggle_fullscreen();
}

- (void)togglePause:(id)sender {
    g_paused = !g_paused;
    log_msg(LOGGER_INFO, "Video %s", g_paused ? "paused" : "resumed");
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

@end

static UxPlayMenuHandler *g_menu_handler = NULL;

static void update_window_title(const char *status) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_window) {
            NSString *title;
            if (status && strlen(status) > 0) {
                title = [NSString stringWithFormat:@"%s - %s",
                         g_server_name ?: "UxPlay", status];
            } else {
                title = [NSString stringWithUTF8String:g_server_name ?: "UxPlay"];
            }
            [g_window setTitle:title];
        }
    });
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

        // Draw gradient background (dark gray to darker gray)
        NSGradient *gradient = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:0.15 alpha:1.0]
                                                             endingColor:[NSColor colorWithWhite:0.08 alpha:1.0]];
        [gradient drawInRect:NSMakeRect(0, 0, width, height) angle:90];

        // Draw AirPlay icon (SF Symbol) in center
        CGFloat iconSize = MIN(width, height) * 0.25;
        NSImage *airplayIcon = nil;

        if (@available(macOS 11.0, *)) {
            NSImageSymbolConfiguration *config = [NSImageSymbolConfiguration configurationWithPointSize:iconSize weight:NSFontWeightUltraLight];
            airplayIcon = [NSImage imageWithSystemSymbolName:@"airplayvideo" accessibilityDescription:@"AirPlay"];
            airplayIcon = [airplayIcon imageWithSymbolConfiguration:config];
        }

        if (airplayIcon) {
            NSSize iconActualSize = [airplayIcon size];
            CGFloat iconX = (width - iconActualSize.width) / 2;
            CGFloat iconY = (height - iconActualSize.height) / 2 - height * 0.08;

            // Save graphics state, flip the icon drawing area, then restore
            [NSGraphicsContext saveGraphicsState];
            NSAffineTransform *transform = [NSAffineTransform transform];
            [transform translateXBy:iconX + iconActualSize.width / 2 yBy:iconY + iconActualSize.height / 2];
            [transform scaleXBy:1.0 yBy:-1.0];
            [transform translateXBy:-(iconX + iconActualSize.width / 2) yBy:-(iconY + iconActualSize.height / 2)];
            [transform concat];

            NSRect iconRect = NSMakeRect(iconX, iconY, iconActualSize.width, iconActualSize.height);
            [airplayIcon drawInRect:iconRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:0.6];
            [NSGraphicsContext restoreGraphicsState];
        }

        // Draw "Waiting for AirPlay..." text
        NSString *waitingText = @"Waiting for AirPlay...";
        NSMutableParagraphStyle *paragraphStyle = [[NSMutableParagraphStyle alloc] init];
        [paragraphStyle setAlignment:NSTextAlignmentCenter];

        NSDictionary *textAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:width * 0.035 weight:NSFontWeightLight],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.5],
            NSParagraphStyleAttributeName: paragraphStyle
        };

        NSSize textSize = [waitingText sizeWithAttributes:textAttrs];
        CGFloat textY = height * 0.75 - textSize.height;
        NSRect textRect = NSMakeRect(0, textY, width, textSize.height);
        [waitingText drawInRect:textRect withAttributes:textAttrs];

        // Draw server name below
        if (g_server_name) {
            NSString *serverText = [NSString stringWithUTF8String:g_server_name];
            NSDictionary *serverAttrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:width * 0.025 weight:NSFontWeightLight],
                NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.3],
                NSParagraphStyleAttributeName: paragraphStyle
            };
            NSSize serverSize = [serverText sizeWithAttributes:serverAttrs];
            NSRect serverRect = NSMakeRect(0, textY + textSize.height + 10, width, serverSize.height);
            [serverText drawInRect:serverRect withAttributes:serverAttrs];
        }

        // Draw subtle hint at top
        NSString *hintText = @"Press F for fullscreen  |  Q to quit";
        NSDictionary *hintAttrs = @{
            NSFontAttributeName: [NSFont systemFontOfSize:width * 0.018 weight:NSFontWeightLight],
            NSForegroundColorAttributeName: [[NSColor whiteColor] colorWithAlphaComponent:0.2],
            NSParagraphStyleAttributeName: paragraphStyle
        };
        NSSize hintSize = [hintText sizeWithAttributes:hintAttrs];
        NSRect hintRect = NSMakeRect(0, height * 0.95 - hintSize.height, width, hintSize.height);
        [hintText drawInRect:hintRect withAttributes:hintAttrs];

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
    pthread_mutex_lock(&g_frame_mutex);
    CVPixelBufferRef pixelBuffer = g_pending_frame;
    if (pixelBuffer) {
        CVPixelBufferRetain(pixelBuffer);
    }
    pthread_mutex_unlock(&g_frame_mutex);

    // If showing cover art, display the cover art texture
    if (g_showing_cover_art && g_cover_art_texture_created && g_cover_art_texture) {
        @autoreleasepool {
            id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
            id<CAMetalDrawable> drawable = [view currentDrawable];

            if (drawable && commandBuffer && g_metal_device) {
                // Use MPS scaler for high-quality scaling to fit drawable
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

                // Clear first
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
            }
        }
        if (pixelBuffer) CVPixelBufferRelease(pixelBuffer);
        return;
    }

    // If no frame, show idle screen
    if (!pixelBuffer) {
        @autoreleasepool {
            id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
            id<CAMetalDrawable> drawable = [view currentDrawable];

            if (drawable && commandBuffer && g_metal_device) {
                // Create idle texture if needed
                if (!g_idle_texture_created || !g_idle_texture) {
                    create_idle_texture(drawable.texture.width, drawable.texture.height);
                }

                if (g_idle_texture) {
                    // Blit idle texture to drawable
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
                }
            }
        }
        return;
    }

    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [g_command_queue commandBuffer];
        id<CAMetalDrawable> drawable = [view currentDrawable];

        if (drawable && commandBuffer) {
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

            if (result == kCVReturnSuccess && metalTexture) {
                id<MTLTexture> texture = CVMetalTextureGetTexture(metalTexture);

                // Use Metal Performance Shaders for high-quality scaling
                MPSImageBilinearScale *scaler = [[MPSImageBilinearScale alloc] initWithDevice:g_metal_device];

                // Determine effective dimensions after rotation
                size_t width = srcWidth;
                size_t height = srcHeight;
                bool swapDimensions = (g_rotation == LEFT || g_rotation == RIGHT);
                if (swapDimensions) {
                    width = srcHeight;
                    height = srcWidth;
                }

                // Calculate scale transform to fit drawable while maintaining aspect ratio
                double srcAspect = (double)width / (double)height;
                double dstAspect = (double)drawable.texture.width / (double)drawable.texture.height;

                double baseScaleX, baseScaleY;
                double translateX = 0, translateY = 0;

                if (srcAspect > dstAspect) {
                    // Source is wider - fit to width
                    baseScaleX = (double)drawable.texture.width / (double)width;
                    baseScaleY = baseScaleX;
                    translateY = (drawable.texture.height - height * baseScaleY) / 2.0;
                } else {
                    // Source is taller - fit to height
                    baseScaleY = (double)drawable.texture.height / (double)height;
                    baseScaleX = baseScaleY;
                    translateX = (drawable.texture.width - width * baseScaleX) / 2.0;
                }

                // Apply flip transforms
                // For MPS: negative scale + adjusted translate = flip
                double scaleX = baseScaleX;
                double scaleY = baseScaleY;

                // Apply horizontal flip (HFLIP)
                if (g_flip == HFLIP) {
                    scaleX = -scaleX;
                    translateX = drawable.texture.width - translateX;
                }

                // Apply vertical flip (VFLIP)
                if (g_flip == VFLIP) {
                    scaleY = -scaleY;
                    translateY = drawable.texture.height - translateY;
                }

                // Apply rotation transforms
                // INVERT (180 degrees) = HFLIP + VFLIP
                if (g_rotation == INVERT) {
                    scaleX = -scaleX;
                    scaleY = -scaleY;
                    if (g_flip != HFLIP) {
                        translateX = drawable.texture.width - translateX;
                    }
                    if (g_flip != VFLIP) {
                        translateY = drawable.texture.height - translateY;
                    }
                }

                // Note: LEFT (90 CCW) and RIGHT (90 CW) require texture coordinate rotation
                // which MPS doesn't support directly. For now, log a warning.
                if (swapDimensions) {
                    static bool warned = false;
                    if (!warned) {
                        log_msg(LOGGER_WARNING, "90/270 degree rotation (-vf left/right) not fully supported in native renderer - use -vf invert or hflip/vflip instead");
                        warned = true;
                    }
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

            // Use custom Metal view for double-click handling
            g_metal_view = [[UxPlayMetalView alloc] initWithFrame:frame device:g_metal_device];
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
            g_metal_renderer = nil;
            g_metal_device = nil;
            g_command_queue = nil;
        }
    });
}

#pragma mark - VideoToolbox Decoder

static bool parse_sps_pps(const uint8_t *data, size_t length) {
    // Find SPS and PPS NAL units in the data (supports both H.264 and HEVC)
    // NAL units are separated by 0x00 0x00 0x00 0x01
    //
    // H.264 NAL types (5 bits, mask 0x1F):
    //   7 = SPS, 8 = PPS
    //
    // HEVC NAL types (6 bits, shifted right by 1):
    //   32 = VPS, 33 = SPS, 34 = PPS

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

            i = nal_end;
        } else {
            i++;
        }
    }

    // HEVC requires VPS+SPS+PPS, H.264 requires SPS+PPS
    if (g_is_h265) {
        return (g_vps != NULL && g_sps != NULL && g_pps != NULL);
    } else {
        return (g_sps != NULL && g_pps != NULL);
    }
}

static void create_decoder_session(void) {
    if (g_decoder_session) return;

    OSStatus status;

    if (g_is_h265) {
        // HEVC requires VPS, SPS, and PPS
        if (!g_vps || !g_sps || !g_pps) return;

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
            return;
        }

        if (status != noErr) {
            log_msg(LOGGER_ERR, "Failed to create HEVC format description: %d", (int)status);
            return;
        }
        log_msg(LOGGER_INFO, "Created HEVC format description");
    } else {
        // H.264 requires SPS and PPS
        if (!g_sps || !g_pps) return;

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

        // Update window title to show streaming status
        update_window_title("Streaming");
        g_showing_cover_art = false;

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

// Helper: check if NAL is a parameter set (should be skipped in bitstream)
static bool is_parameter_set_nal(uint8_t first_byte) {
    if (g_is_h265) {
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
    if (!g_decoder_session) return false;

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
            if (nal_size > 0 && !is_parameter_set_nal(data[nal_start])) {
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

            if (nal_size > 0 && !is_parameter_set_nal(data[nal_start])) {
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
    log_msg(LOGGER_INFO, "video_renderer_start called");
    g_running = true;
    g_paused = false;
    log_msg(LOGGER_INFO, "Video renderer started");
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

    // Clear old SPS/PPS/VPS so new stream can set them
    if (g_vps) { free(g_vps); g_vps = NULL; g_vps_size = 0; }
    if (g_sps) { free(g_sps); g_sps = NULL; g_sps_size = 0; }
    if (g_pps) { free(g_pps); g_pps = NULL; g_pps_size = 0; }

    // Reset idle texture so it regenerates at the correct size
    g_idle_texture_created = false;
    g_idle_texture = nil;

    // Reset cover art state
    g_showing_cover_art = false;
    g_cover_art_texture_created = false;
    g_cover_art_texture = nil;
    g_cover_art = nil;

    // Restore window to original size
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_window && !g_fullscreen) {
            NSRect frame = [g_window frame];
            frame.size.width = 1280;
            frame.size.height = 720;
            [g_window setFrame:frame display:YES animate:YES];
        }
    });

    update_window_title("Ready");
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
        // Trigger cover art recreation with updated metadata
        g_cover_art_texture_created = false;
    }
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

    // Clear cover art mode when video frames arrive
    if (g_showing_cover_art) {
        g_showing_cover_art = false;
        log_msg(LOGGER_INFO, "Switching from cover art to video mode");
    }

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

    if (g_vps) { free(g_vps); g_vps = NULL; }
    if (g_sps) { free(g_sps); g_sps = NULL; }
    if (g_pps) { free(g_pps); g_pps = NULL; }
    if (g_server_name) { free(g_server_name); g_server_name = NULL; }
    g_cover_art = nil;
    g_showing_cover_art = false;

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
            if (g_window && !g_fullscreen) {
                NSRect frame = [g_window frame];
                frame.size.width = windowWidth;
                frame.size.height = windowHeight;
                [g_window setFrame:frame display:YES animate:YES];
            }

            // Update window title with track info if available
            if (g_track_title && g_track_artist) {
                char title[256];
                snprintf(title, sizeof(title), "%s - %s", g_track_artist, g_track_title);
                update_window_title(title);
            } else {
                update_window_title("Now Playing");
            }

            // Create composite texture with blurred gradient background
            if (!g_metal_device) return;

            size_t texWidth = (size_t)windowWidth * 2;  // Retina
            size_t texHeight = (size_t)windowHeight * 2;

            // Get CGImage for processing
            CGImageRef cgImage = [image CGImageForProposedRect:NULL context:nil hints:nil];
            if (!cgImage) return;

            // Create CIImage for blur processing
            CIImage *ciImage = [CIImage imageWithCGImage:cgImage];

            // Scale album art to fill the background with extra margin
            CGFloat bgScale = MAX((CGFloat)texWidth / imageSize.width, (CGFloat)texHeight / imageSize.height) * 1.3;
            CIImage *scaledBg = [ciImage imageByApplyingTransform:CGAffineTransformMakeScale(bgScale, bgScale)];

            // Center the scaled background
            CGRect scaledExtent = [scaledBg extent];
            CGFloat offsetX = (texWidth - scaledExtent.size.width) / 2;
            CGFloat offsetY = (texHeight - scaledExtent.size.height) / 2;
            scaledBg = [scaledBg imageByApplyingTransform:CGAffineTransformMakeTranslation(offsetX - scaledExtent.origin.x, offsetY - scaledExtent.origin.y)];

            // IMPORTANT: Clamp BEFORE blur to prevent black edges
            scaledBg = [scaledBg imageByClampingToExtent];

            // Apply strong gaussian blur
            CIFilter *blurFilter = [CIFilter filterWithName:@"CIGaussianBlur"];
            [blurFilter setValue:scaledBg forKey:kCIInputImageKey];
            [blurFilter setValue:@80.0 forKey:kCIInputRadiusKey];
            CIImage *blurredBg = [blurFilter outputImage];

            // Darken the background
            CIFilter *darkenFilter = [CIFilter filterWithName:@"CIColorControls"];
            [darkenFilter setValue:blurredBg forKey:kCIInputImageKey];
            [darkenFilter setValue:@(-0.2) forKey:kCIInputBrightnessKey];
            [darkenFilter setValue:@1.1 forKey:kCIInputSaturationKey];
            blurredBg = [darkenFilter outputImage];

            // Crop to exact size
            blurredBg = [blurredBg imageByCroppingToRect:CGRectMake(0, 0, texWidth, texHeight)];

            // Create context and render background
            CIContext *ciContext = [CIContext contextWithMTLDevice:g_metal_device];
            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();

            // Create bitmap context for final composite
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
                return;
            }

            // Render blurred background
            CGImageRef bgCGImage = [ciContext createCGImage:blurredBg fromRect:CGRectMake(0, 0, texWidth, texHeight)];
            if (bgCGImage) {
                CGContextDrawImage(context, CGRectMake(0, 0, texWidth, texHeight), bgCGImage);
                CGImageRelease(bgCGImage);
            }

            // Calculate album art size and position (centered, raised to make room for text)
            CGFloat artMaxSize = MIN(texWidth, texHeight) * 0.50;
            CGFloat artScale = MIN(artMaxSize / imageSize.width, artMaxSize / imageSize.height);
            CGFloat artWidth = imageSize.width * artScale;
            CGFloat artHeight = imageSize.height * artScale;
            CGFloat artX = (texWidth - artWidth) / 2;
            CGFloat artY = (texHeight - artHeight) / 2 + texHeight * 0.10;  // Raised to make room for text

            // Draw shadow under album art
            CGContextSaveGState(context);
            CGContextSetShadowWithColor(context,
                                        CGSizeMake(0, -25),
                                        50,
                                        [[NSColor colorWithWhite:0 alpha:0.6] CGColor]);

            // Draw album art with rounded corners
            CGFloat cornerRadius = artWidth * 0.04;
            CGRect artRect = CGRectMake(artX, artY, artWidth, artHeight);
            CGPathRef roundedPath = CGPathCreateWithRoundedRect(artRect, cornerRadius, cornerRadius, NULL);
            CGContextAddPath(context, roundedPath);
            CGContextClip(context);
            CGContextDrawImage(context, artRect, cgImage);
            CGPathRelease(roundedPath);
            CGContextRestoreGState(context);

            // Draw track title and artist below the album art using NSGraphicsContext
            NSGraphicsContext *nsContext = [NSGraphicsContext graphicsContextWithCGContext:context flipped:NO];
            [NSGraphicsContext saveGraphicsState];
            [NSGraphicsContext setCurrentContext:nsContext];

            // Create attributed strings for title and artist
            NSString *titleStr = g_track_title ? [NSString stringWithUTF8String:g_track_title] : nil;
            NSString *artistStr = g_track_artist ? [NSString stringWithUTF8String:g_track_artist] : nil;

            if (titleStr || artistStr) {
                // Title style - larger, bold, white
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

                // Artist style - smaller, regular, light gray
                NSDictionary *artistAttrs = @{
                    NSFontAttributeName: [NSFont systemFontOfSize:38 weight:NSFontWeightMedium],
                    NSForegroundColorAttributeName: [NSColor colorWithWhite:0.85 alpha:1.0],
                    NSParagraphStyleAttributeName: centerStyle,
                    NSShadowAttributeName: textShadow
                };

                // Calculate text positions - below album art (in non-flipped coords, Y=0 at bottom)
                // artY is from bottom, album art goes from artY to artY+artHeight
                // Text should be below, so at artY - spacing
                CGFloat textAreaTop = artY - 50;  // 50px below album art bottom
                CGFloat textWidth = texWidth * 0.85;
                CGFloat textX = (texWidth - textWidth) / 2;

                // Draw title first (higher up)
                if (titleStr) {
                    NSSize titleSize = [titleStr sizeWithAttributes:titleAttrs];
                    CGFloat titleY = textAreaTop - titleSize.height;
                    NSRect titleRect = NSMakeRect(textX, titleY, textWidth, titleSize.height + 10);
                    [titleStr drawInRect:titleRect withAttributes:titleAttrs];
                    textAreaTop = titleY - 10;  // Move down for artist
                }

                // Draw artist below title
                if (artistStr) {
                    NSSize artistSize = [artistStr sizeWithAttributes:artistAttrs];
                    CGFloat artistY = textAreaTop - artistSize.height;
                    NSRect artistRect = NSMakeRect(textX, artistY, textWidth, artistSize.height + 10);
                    [artistStr drawInRect:artistRect withAttributes:artistAttrs];
                }
            }

            [NSGraphicsContext restoreGraphicsState];

            CGColorSpaceRelease(colorSpace);
            CGContextRelease(context);

            // Create Metal texture
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                 width:texWidth
                                                                                                height:texHeight
                                                                                             mipmapped:NO];
            descriptor.usage = MTLTextureUsageShaderRead;

            g_cover_art_texture = [g_metal_device newTextureWithDescriptor:descriptor];

            MTLRegion region = MTLRegionMake2D(0, 0, texWidth, texHeight);
            [g_cover_art_texture replaceRegion:region
                                   mipmapLevel:0
                                     withBytes:pixelData
                                   bytesPerRow:bytesPerRow];

            g_cover_art_texture_created = true;
            free(pixelData);

            log_msg(LOGGER_INFO, "Cover art texture created with blurred background (%zux%zu)", texWidth, texHeight);
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
    g_is_h265 = video_is_h265;
    return 0;
}
