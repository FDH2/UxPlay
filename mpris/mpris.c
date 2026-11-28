/*
 * UxPlay MPRIS2 support. See mpris.h.
 *
 * This file is part of UxPlay and is distributed under the GPL v3 or later.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include "mpris.h"

#define MPRIS_PATH        "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_IFACE  "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define RTP_RATE 44100

enum { STATUS_STOPPED = 0, STATUS_PAUSED, STATUS_PLAYING };

/* properties that need a PropertiesChanged signal */
enum {
    DIRTY_METADATA = 1 << 0,
    DIRTY_STATUS   = 1 << 1,
    DIRTY_CAPS     = 1 << 2,
};

static const char *introspection_xml =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2'>"
    "    <method name='Raise'/>"
    "    <method name='Quit'/>"
    "    <property name='CanQuit' type='b' access='read'/>"
    "    <property name='CanRaise' type='b' access='read'/>"
    "    <property name='HasTrackList' type='b' access='read'/>"
    "    <property name='Identity' type='s' access='read'/>"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Next'/>"
    "    <method name='Previous'/>"
    "    <method name='Pause'/>"
    "    <method name='PlayPause'/>"
    "    <method name='Stop'/>"
    "    <method name='Play'/>"
    "    <method name='Seek'><arg direction='in' name='Offset' type='x'/></method>"
    "    <method name='SetPosition'>"
    "      <arg direction='in' name='TrackId' type='o'/>"
    "      <arg direction='in' name='Position' type='x'/>"
    "    </method>"
    "    <method name='OpenUri'><arg direction='in' name='Uri' type='s'/></method>"
    "    <signal name='Seeked'><arg name='Position' type='x'/></signal>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "    <property name='Rate' type='d' access='read'/>"
    "    <property name='Metadata' type='a{sv}' access='read'/>"
    "    <property name='Volume' type='d' access='read'/>"
    "    <property name='Position' type='x' access='read'/>"
    "    <property name='MinimumRate' type='d' access='read'/>"
    "    <property name='MaximumRate' type='d' access='read'/>"
    "    <property name='CanGoNext' type='b' access='read'/>"
    "    <property name='CanGoPrevious' type='b' access='read'/>"
    "    <property name='CanPlay' type='b' access='read'/>"
    "    <property name='CanPause' type='b' access='read'/>"
    "    <property name='CanSeek' type='b' access='read'/>"
    "    <property name='CanControl' type='b' access='read'/>"
    "  </interface>"
    "</node>";

/* everything below is protected by "lock" */
static GMutex lock;
static bool enabled = false;
static GDBusConnection *bus = NULL;
static GDBusNodeInfo *node_info = NULL;
static guint reg_root = 0, reg_player = 0;
static guint owner_id = 0;
static char *bus_name = NULL;
static char *identity = NULL;
static char *art_dir = NULL;

static char *title = NULL, *artist = NULL, *album = NULL, *genre = NULL;
static char *art_path = NULL;
static unsigned int art_counter = 0;
static unsigned int track_counter = 0;
static bool have_track = false;
static int status = STATUS_STOPPED;
static gint64 length_us = 0;
static gint64 position_us = 0;     /* position at position_stamp */
static gint64 position_stamp = 0;  /* g_get_monotonic_time() */

static char *dacp_id = NULL, *active_remote = NULL;
static char *dacp_addr = NULL;     /* cached result of the avahi lookup */
static guint16 dacp_port = 0;
static guint32 dacp_scope = 0;

static guint dirty = 0;
static bool update_pending = false;
static bool seeked_pending = false;

static gint atomic_playing = 0;    /* fast path for mpris_set_playing() */
static gint atomic_have_track = 0;

/* ---------- helpers (call with lock held) ---------- */

static gint64 current_position_locked(void) {
    gint64 pos = position_us;
    if (status == STATUS_PLAYING && position_stamp) {
        pos += g_get_monotonic_time() - position_stamp;
    }
    if (length_us > 0 && pos > length_us) {
        pos = length_us;
    }
    return pos < 0 ? 0 : pos;
}

static void set_status_locked(int new_status) {
    if (new_status == status) {
        return;
    }
    position_us = current_position_locked();
    position_stamp = g_get_monotonic_time();
    status = new_status;
    g_atomic_int_set(&atomic_playing, new_status == STATUS_PLAYING);
    dirty |= DIRTY_STATUS;
}

static const char *status_string(int s) {
    switch (s) {
    case STATUS_PLAYING: return "Playing";
    case STATUS_PAUSED:  return "Paused";
    default:             return "Stopped";
    }
}

static GVariant *build_metadata_locked(void) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (have_track) {
        char trackid[64];
        snprintf(trackid, sizeof(trackid), "/org/uxplay/track/%u", track_counter);
        g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(trackid));
        if (title) {
            g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(title));
        }
        if (artist) {
            const char *a[] = { artist, NULL };
            g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(a, -1));
        }
        if (album) {
            g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(album));
        }
        if (genre) {
            const char *g[] = { genre, NULL };
            g_variant_builder_add(&b, "{sv}", "xesam:genre", g_variant_new_strv(g, -1));
        }
        if (length_us > 0) {
            g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(length_us));
        }
        if (art_path) {
            char *uri = g_filename_to_uri(art_path, NULL, NULL);
            if (uri) {
                g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(uri));
                g_free(uri);
            }
        }
    }
    return g_variant_builder_end(&b);
}

static bool can_control_locked(void) {
    return dacp_id != NULL && active_remote != NULL;
}

static void remove_art_locked(void) {
    if (art_path) {
        g_unlink(art_path);
        g_free(art_path);
        art_path = NULL;
    }
}

/* ---------- main-context side ---------- */

static gboolean update_idle(gpointer data) {
    (void) data;
    g_mutex_lock(&lock);
    update_pending = false;
    if (!enabled) {
        g_mutex_unlock(&lock);
        return G_SOURCE_REMOVE;
    }

    if (have_track && owner_id == 0) {
        owner_id = g_bus_own_name_on_connection(bus, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                NULL, NULL, NULL, NULL);
    }

    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    if (dirty & DIRTY_METADATA) {
        g_variant_builder_add(&changed, "{sv}", "Metadata", build_metadata_locked());
    }
    if (dirty & DIRTY_STATUS) {
        g_variant_builder_add(&changed, "{sv}", "PlaybackStatus",
                              g_variant_new_string(status_string(status)));
    }
    if (dirty & DIRTY_CAPS) {
        bool c = can_control_locked();
        g_variant_builder_add(&changed, "{sv}", "CanGoNext", g_variant_new_boolean(c));
        g_variant_builder_add(&changed, "{sv}", "CanGoPrevious", g_variant_new_boolean(c));
        g_variant_builder_add(&changed, "{sv}", "CanPlay", g_variant_new_boolean(c));
        g_variant_builder_add(&changed, "{sv}", "CanPause", g_variant_new_boolean(c));
    }
    bool send_changed = (dirty != 0);
    dirty = 0;
    bool send_seeked = seeked_pending;
    seeked_pending = false;
    gint64 pos = current_position_locked();
    g_mutex_unlock(&lock);

    if (send_changed) {
        g_dbus_connection_emit_signal(bus, NULL, MPRIS_PATH, "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", MPRIS_PLAYER_IFACE, &changed, NULL),
                                      NULL);
    } else {
        g_variant_builder_clear(&changed);
    }
    if (send_seeked) {
        g_dbus_connection_emit_signal(bus, NULL, MPRIS_PATH, MPRIS_PLAYER_IFACE, "Seeked",
                                      g_variant_new("(x)", pos), NULL);
    }
    return G_SOURCE_REMOVE;
}

/* call with lock held */
static void schedule_update_locked(void) {
    if (enabled && !update_pending) {
        update_pending = true;
        g_idle_add(update_idle, NULL);
    }
}

/* ---------- DACP remote control ---------- */

static bool dacp_resolve(const char *id, char **addr, guint16 *port, guint32 *scope) {
    GError *err = NULL;
    GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!sys) {
        g_printerr("mpris: system D-Bus unavailable, cannot resolve DACP service: %s\n", err->message);
        g_error_free(err);
        return false;
    }
    char *name = g_strdup_printf("iTunes_Ctrl_%s", id);
    /* prefer IPv4 (AVAHI_PROTO_INET = 0), then any (AVAHI_PROTO_UNSPEC = -1) */
    const gint32 aprotos[2] = { 0, -1 };
    bool ok = false;
    for (int i = 0; i < 2 && !ok; i++) {
        GVariant *r = g_dbus_connection_call_sync(sys, "org.freedesktop.Avahi", "/",
                                                  "org.freedesktop.Avahi.Server", "ResolveService",
                                                  g_variant_new("(iisssiu)", -1, -1, name, "_dacp._tcp",
                                                                "local", aprotos[i], 0u),
                                                  G_VARIANT_TYPE("(iissssisqaayu)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err);
        if (!r) {
            if (i == 1) {
                g_printerr("mpris: could not resolve %s._dacp._tcp via avahi-daemon: %s\n",
                           name, err->message);
            }
            g_clear_error(&err);
            continue;
        }
        gint32 iface, proto, aproto;
        const char *rname, *rtype, *rdomain, *host, *address;
        guint16 rport;
        guint32 flags;
        GVariant *txt = NULL;
        g_variant_get(r, "(ii&s&s&s&si&sq@aayu)", &iface, &proto, &rname, &rtype, &rdomain,
                      &host, &aproto, &address, &rport, &txt, &flags);
        *addr = g_strdup(address);
        *port = rport;
        *scope = iface > 0 ? (guint32) iface : 0;
        ok = true;
        g_variant_unref(txt);
        g_variant_unref(r);
    }
    g_free(name);
    g_object_unref(sys);
    return ok;
}

static bool dacp_send(const char *addr, guint16 port, guint32 scope, const char *remote, const char *cmd) {
    GError *err = NULL;
    GInetAddress *ia = g_inet_address_new_from_string(addr);
    if (!ia) {
        return false;
    }
    bool link_local = g_inet_address_get_is_link_local(ia) &&
                      g_inet_address_get_family(ia) == G_SOCKET_FAMILY_IPV6;
    GSocketAddress *sa = G_SOCKET_ADDRESS(g_object_new(G_TYPE_INET_SOCKET_ADDRESS,
                                                       "address", ia,
                                                       "port", (guint) port,
                                                       "scope-id", link_local ? scope : 0u,
                                                       NULL));
    g_object_unref(ia);

    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(sa), NULL, &err);
    g_object_unref(sa);
    g_object_unref(client);
    if (!conn) {
        g_printerr("mpris: DACP connect to %s:%u failed: %s\n", addr, port, err->message);
        g_error_free(err);
        return false;
    }

    char *req = g_strdup_printf("GET /ctrl-int/1/%s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "Active-Remote: %s\r\n"
                                "Connection: close\r\n\r\n", cmd, addr, remote);
    bool ok = g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)),
                                        req, strlen(req), NULL, NULL, &err);
    g_free(req);
    char buf[128] = { 0 };
    if (ok) {
        gssize n = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(conn)),
                                       buf, sizeof(buf) - 1, NULL, &err);
        ok = (n > 12 && (strncmp(buf + 9, "200", 3) == 0 || strncmp(buf + 9, "204", 3) == 0));
        if (!ok && n > 0) {
            char *eol = strpbrk(buf, "\r\n");
            if (eol) *eol = '\0';
            g_printerr("mpris: DACP \"%s\" rejected by client: %s\n", cmd, buf);
        }
    }
    if (err) {
        g_printerr("mpris: DACP \"%s\" failed: %s\n", cmd, err->message);
        g_error_free(err);
    }
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_object_unref(conn);
    return ok;
}

static void dacp_worker(gpointer task_data, gpointer user_data) {
    (void) user_data;
    const char *cmd = (const char *) task_data;

    for (int attempt = 0; attempt < 2; attempt++) {
        g_mutex_lock(&lock);
        if (!can_control_locked()) {
            g_mutex_unlock(&lock);
            break;
        }
        char *id = g_strdup(dacp_id);
        char *remote = g_strdup(active_remote);
        char *addr = g_strdup(dacp_addr);
        guint16 port = dacp_port;
        guint32 scope = dacp_scope;
        g_mutex_unlock(&lock);

        if (!addr) {
            if (dacp_resolve(id, &addr, &port, &scope)) {
                g_mutex_lock(&lock);
                if (dacp_id && strcmp(dacp_id, id) == 0) {
                    g_free(dacp_addr);
                    dacp_addr = g_strdup(addr);
                    dacp_port = port;
                    dacp_scope = scope;
                }
                g_mutex_unlock(&lock);
            }
        }
        bool ok = addr && dacp_send(addr, port, scope, remote, cmd);
        g_free(id);
        g_free(remote);
        g_free(addr);
        if (ok) {
            break;
        }
        /* the client may have restarted its DACP server: forget the cached address and retry */
        g_mutex_lock(&lock);
        g_clear_pointer(&dacp_addr, g_free);
        g_mutex_unlock(&lock);
    }
}

/* one worker thread, so commands reach the client in the order they were given */
static GThreadPool *dacp_pool = NULL;

static void dacp_command(const char *cmd) {
    if (dacp_pool) {
        g_thread_pool_push(dacp_pool, (gpointer) cmd, NULL);   /* cmd is a string literal */
    }
}

/* ---------- D-Bus vtables ---------- */

static void root_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                        const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) method; (void) params; (void) data;
    g_dbus_method_invocation_return_value(inv, NULL);   /* Raise, Quit: not supported */
}

static GVariant *root_get(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                          const gchar *prop, GError **error, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) error; (void) data;
    if (!strcmp(prop, "CanQuit") || !strcmp(prop, "CanRaise") || !strcmp(prop, "HasTrackList")) {
        return g_variant_new_boolean(FALSE);
    } else if (!strcmp(prop, "Identity")) {
        g_mutex_lock(&lock);
        GVariant *v = g_variant_new_string(identity ? identity : "UxPlay");
        g_mutex_unlock(&lock);
        return v;
    } else if (!strcmp(prop, "SupportedUriSchemes") || !strcmp(prop, "SupportedMimeTypes")) {
        return g_variant_new_strv(NULL, 0);
    }
    return NULL;
}

static void player_method(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                          const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) params; (void) data;
    const char *cmd = NULL;
    if (!strcmp(method, "Next")) {
        cmd = "nextitem";
    } else if (!strcmp(method, "Previous")) {
        cmd = "previtem";
    } else if (!strcmp(method, "Pause") || !strcmp(method, "Stop")) {
        cmd = "pause";
    } else if (!strcmp(method, "Play")) {
        cmd = "play";
    } else if (!strcmp(method, "PlayPause")) {
        cmd = "playpause";
    }
    /* Seek, SetPosition, OpenUri: not supported (CanSeek is false) */
    if (cmd) {
        dacp_command(cmd);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *player_get(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                            const gchar *prop, GError **error, gpointer data) {
    (void) c; (void) sender; (void) path; (void) iface; (void) error; (void) data;
    GVariant *v = NULL;
    g_mutex_lock(&lock);
    if (!strcmp(prop, "PlaybackStatus")) {
        v = g_variant_new_string(status_string(status));
    } else if (!strcmp(prop, "Rate") || !strcmp(prop, "MinimumRate") ||
               !strcmp(prop, "MaximumRate") || !strcmp(prop, "Volume")) {
        v = g_variant_new_double(1.0);
    } else if (!strcmp(prop, "Metadata")) {
        v = build_metadata_locked();
    } else if (!strcmp(prop, "Position")) {
        v = g_variant_new_int64(current_position_locked());
    } else if (!strcmp(prop, "CanGoNext") || !strcmp(prop, "CanGoPrevious") ||
               !strcmp(prop, "CanPlay") || !strcmp(prop, "CanPause")) {
        v = g_variant_new_boolean(can_control_locked());
    } else if (!strcmp(prop, "CanSeek")) {
        v = g_variant_new_boolean(FALSE);
    } else if (!strcmp(prop, "CanControl")) {
        v = g_variant_new_boolean(TRUE);
    }
    g_mutex_unlock(&lock);
    return v;
}

static const GDBusInterfaceVTable root_vtable = { root_method, root_get, NULL, { 0 } };
static const GDBusInterfaceVTable player_vtable = { player_method, player_get, NULL, { 0 } };

/* ---------- public API ---------- */

static bool str_changed(char **dst, const char *src) {
    if (src && !*src) {
        src = NULL;
    }
    if (g_strcmp0(*dst, src) == 0) {
        return false;
    }
    g_free(*dst);
    *dst = g_strdup(src);
    return true;
}

bool mpris_init(const char *name) {
    GError *err = NULL;
    GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!c) {
        g_printerr("mpris: no D-Bus session bus, media controls disabled (%s)\n", err->message);
        g_error_free(err);
        return false;
    }
    g_mutex_lock(&lock);
    bus = c;
    node_info = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    reg_root = g_dbus_connection_register_object(bus, MPRIS_PATH, node_info->interfaces[0],
                                                 &root_vtable, NULL, NULL, NULL);
    reg_player = g_dbus_connection_register_object(bus, MPRIS_PATH, node_info->interfaces[1],
                                                   &player_vtable, NULL, NULL, NULL);
    dacp_pool = g_thread_pool_new(dacp_worker, NULL, 1, FALSE, NULL);
    identity = g_strdup(name && *name ? name : "UxPlay");
    bus_name = g_strdup_printf("org.mpris.MediaPlayer2.uxplay.instance%d", (int) getpid());
    art_dir = g_build_filename(g_get_user_runtime_dir(), "uxplay", NULL);
    g_mkdir_with_parents(art_dir, 0700);
    enabled = true;
    g_mutex_unlock(&lock);
    return true;
}

void mpris_shutdown(void) {
    g_mutex_lock(&lock);
    if (!enabled) {
        g_mutex_unlock(&lock);
        return;
    }
    enabled = false;
    if (owner_id) {
        g_bus_unown_name(owner_id);
        owner_id = 0;
    }
    GThreadPool *pool = dacp_pool;
    dacp_pool = NULL;
    g_dbus_connection_unregister_object(bus, reg_root);
    g_dbus_connection_unregister_object(bus, reg_player);
    g_dbus_connection_flush_sync(bus, NULL, NULL);
    remove_art_locked();
    if (art_dir) {
        g_rmdir(art_dir);   /* only succeeds if empty, e.g. no other UxPlay instance is using it */
    }
    g_clear_pointer(&art_dir, g_free);
    g_clear_pointer(&node_info, g_dbus_node_info_unref);
    g_clear_object(&bus);
    g_mutex_unlock(&lock);
    if (pool) {
        g_thread_pool_free(pool, TRUE, FALSE);   /* drop queued commands, don't wait */
    }
}

void mpris_set_metadata(const char *t, const char *a, const char *al, const char *g) {
    g_mutex_lock(&lock);
    if (!enabled) {
        g_mutex_unlock(&lock);
        return;
    }
    bool changed = str_changed(&title, t);
    changed = str_changed(&artist, a) || changed;
    changed = str_changed(&album, al) || changed;
    changed = str_changed(&genre, g) || changed;
    if (changed || !have_track) {
        if (changed) {
            track_counter++;
        }
        have_track = true;
        g_atomic_int_set(&atomic_have_track, 1);
        dirty |= DIRTY_METADATA;
        if (status == STATUS_STOPPED) {
            set_status_locked(STATUS_PLAYING);
        }
        schedule_update_locked();
    }
    g_mutex_unlock(&lock);
}

void mpris_set_coverart(const void *image, int len) {
    if (!image || len <= 0) {
        return;
    }
    g_mutex_lock(&lock);
    if (!enabled || !art_dir) {
        g_mutex_unlock(&lock);
        return;
    }
    const unsigned char *p = (const unsigned char *) image;
    const char *ext = (len > 4 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') ? "png" : "jpg";
    /* new file name for every image: media widgets cache artwork by URL */
    char *fname = g_strdup_printf("cover-%d-%u.%s", (int) getpid(), ++art_counter, ext);
    char *path = g_build_filename(art_dir, fname, NULL);
    g_free(fname);
    GError *err = NULL;
    if (g_file_set_contents(path, (const gchar *) image, len, &err)) {
        remove_art_locked();
        art_path = path;
        dirty |= DIRTY_METADATA;
        schedule_update_locked();
    } else {
        g_printerr("mpris: could not write cover art: %s\n", err->message);
        g_error_free(err);
        g_free(path);
    }
    g_mutex_unlock(&lock);
}

void mpris_set_progress(uint32_t start, uint32_t curr, uint32_t end) {
    g_mutex_lock(&lock);
    if (!enabled) {
        g_mutex_unlock(&lock);
        return;
    }
    /* unsigned arithmetic handles RTP timestamp wrap-around */
    gint64 len = (gint64) (uint32_t) (end - start) * G_USEC_PER_SEC / RTP_RATE;
    gint64 pos = (gint64) (uint32_t) (curr - start) * G_USEC_PER_SEC / RTP_RATE;
    if (pos > len) {
        pos = 0;   /* curr before start (e.g. during a buffer pre-fill) */
    }
    gint64 expected = current_position_locked();
    if (len != length_us) {
        length_us = len;
        dirty |= DIRTY_METADATA;
    }
    position_us = pos;
    position_stamp = g_get_monotonic_time();
    if (llabs(expected - pos) > 2 * G_USEC_PER_SEC) {
        seeked_pending = true;
    }
    schedule_update_locked();
    g_mutex_unlock(&lock);
}

void mpris_set_playing(bool playing) {
    if (!g_atomic_int_get(&atomic_have_track) ||
        g_atomic_int_get(&atomic_playing) == (gint) playing) {
        return;   /* fast path: nothing changed */
    }
    g_mutex_lock(&lock);
    if (enabled && have_track) {
        set_status_locked(playing ? STATUS_PLAYING : STATUS_PAUSED);
        schedule_update_locked();
    }
    g_mutex_unlock(&lock);
}

void mpris_set_remote(const char *id, const char *remote) {
    if (!id || !remote) {
        return;
    }
    g_mutex_lock(&lock);
    if (enabled && (g_strcmp0(id, dacp_id) || g_strcmp0(remote, active_remote))) {
        bool had = can_control_locked();
        if (g_strcmp0(id, dacp_id)) {
            g_clear_pointer(&dacp_addr, g_free);
        }
        str_changed(&dacp_id, id);
        str_changed(&active_remote, remote);
        if (had != can_control_locked()) {
            dirty |= DIRTY_CAPS;
            schedule_update_locked();
        }
    }
    g_mutex_unlock(&lock);
}

void mpris_clear(void) {
    g_mutex_lock(&lock);
    if (!enabled) {
        g_mutex_unlock(&lock);
        return;
    }
    have_track = false;
    g_atomic_int_set(&atomic_have_track, 0);
    g_clear_pointer(&title, g_free);
    g_clear_pointer(&artist, g_free);
    g_clear_pointer(&album, g_free);
    g_clear_pointer(&genre, g_free);
    g_clear_pointer(&dacp_id, g_free);
    g_clear_pointer(&active_remote, g_free);
    g_clear_pointer(&dacp_addr, g_free);
    remove_art_locked();
    length_us = 0;
    position_us = 0;
    position_stamp = 0;
    status = STATUS_STOPPED;
    g_atomic_int_set(&atomic_playing, 0);
    dirty = 0;
    seeked_pending = false;
    if (owner_id) {
        g_bus_unown_name(owner_id);   /* the player disappears from media widgets */
        owner_id = 0;
    }
    g_mutex_unlock(&lock);
}
