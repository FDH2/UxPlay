/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
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

/* based on code from David Ventura https://github.com/DavidVentura/UxPlay */

/* This file should be only included from video_renderer.c as it defines static
 * functions and depends on video_renderer internals */

#ifndef X_DISPLAY_FIX_H
#define X_DISPLAY_FIX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>

struct X11_Window_s {
    Display *display;
    Window window;
} typedef X11_Window_t;

static void get_X11_Display(X11_Window_t * X11, char *display_name) {
    X11->display = XOpenDisplay(display_name);
    X11->window = (Window) NULL;
}

//static void free_X11_Display(X11_Window_t *X11) {
//  if (X11->display) {
//      XCloseDisplay(X11->display);
//        X11->display = NULL;
//	X11->window = (Window) NULL;
//    }
//}
  
static Window enum_windows(const char * str, Display * display, Window window, int depth) {
    char* name = NULL;
    XFetchName(display, window, &name);
    if (name) {
        if (strcmp(str, name) == 0) {
            XFree(name);
            return window;
        }
        XFree(name);
    }

    Window _root, parent;
    Window* children = NULL;
    unsigned int n = 0;
    XQueryTree(display, window, &_root, &parent, &children, &n);
    if (children != NULL) {
        for (int i = 0; i < n; i++) {
            Window w = enum_windows(str, display, children[i], depth + 1);
            if (w) {
                XFree(children);
                return w;
            }
        }
        XFree(children);
    }
    return (Window) NULL;
}

int X11_error_catcher( Display *disp, XErrorEvent *xe ) {
    // do nothing
    return 0;
}

static void get_x_window(X11_Window_t * X11, const char * name) {
    Window root = XDefaultRootWindow(X11->display);
    XSetErrorHandler(X11_error_catcher);
    X11->window  = enum_windows(name, X11->display, root, 0);
    XSetErrorHandler(NULL);
#ifdef ZOOM_WINDOW_NAME_FIX
    if (X11->window) {
        Atom _NET_WM_NAME = XInternAtom(X11->display, "_NET_WM_NAME", 0);
        Atom UTF8_STRING = XInternAtom(X11->display, "UTF8_STRING", 0);
        XChangeProperty(X11->display, X11->window, _NET_WM_NAME, UTF8_STRING, 
                        8, 0, (const unsigned char *) name, strlen(name));
        XSync(X11->display, False);
    }
#endif
}

static void set_fullscreen(X11_Window_t * X11, bool * fullscreen) {
    XClientMessageEvent msg = {
        .type = ClientMessage,
        .display = X11->display,
        .window = X11->window,
        .message_type = XInternAtom(X11->display, "_NET_WM_STATE", True),
        .format = 32,
        .data = { .l = {
                *fullscreen,
                XInternAtom(X11->display, "_NET_WM_STATE_FULLSCREEN", True),
                None,
                0,
                1
            }}
    };
    XSendEvent(X11->display, XRootWindow(X11->display, XDefaultScreen(X11->display)),
               False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*) &msg);
    XSync(X11->display, False);
}

#ifdef __cplusplus
}
#endif

#endif 
