/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified extensively to become 
 * UxPlay - An open-souce AirPlay mirroring server.
 * Modifications Copyright (C) 2021-23 F. Duncanh
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

#include <stddef.h>
#include <cstring>
#include <unistd.h>
#include <ctype.h>
#include <string>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <iterator>
#include <sys/stat.h>
#include <cstdio>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>

#ifdef _WIN32  /*modifications for Windows compilation */
#include <glib.h>
#include <unordered_map>
#include <winsock2.h>
#include <iphlpapi.h>
#include <pthread.h>   //for pthreads in MSYS2 UCRT
#else
#include <csignal>
#include <glib-unix.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <sys/types.h>
#include <pwd.h>
# ifdef __linux__
# include <netpacket/packet.h>
# else
# include <net/if_dl.h>
#  ifdef __OpenBSD__
#  include <err.h>
#  endif
# endif
#endif

#include "lib/raop.h"
#include "lib/stream.h"
#include "lib/logger.h"
#include "lib/dnssd.h"
#include "lib/crypto.h"
#include "renderers/video_renderer.h"
#include "renderers/audio_renderer.h"
#include "renderers/mux_renderer.h"
#ifdef DBUS
#include <dbus/dbus.h>
#endif


#define VERSION "1.73"

#define SECOND_IN_USECS 1000000
#define SECOND_IN_NSECS 1000000000UL
#define DEFAULT_NAME "UxPlay"
#define DEFAULT_DEBUG_LOG false
#define LOWEST_ALLOWED_PORT 1024
#define HIGHEST_PORT 65535
#define MISSED_FEEDBACK_LIMIT 15
#define MIN_PASSWORD_LENGTH 4
#define DEFAULT_PLAYBIN_VERSION 3
#define BT709_FIX "capssetter caps=\"video/x-h264, colorimetry=bt709\""
#define SRGB_FIX  " ! video/x-raw,colorimetry=sRGB,format=RGB  ! "
#ifdef FULL_RANGE_RGB_FIX
  #define DEFAULT_SRGB_FIX true
#else
  #define DEFAULT_SRGB_FIX false
#endif

static std::string server_name = DEFAULT_NAME;
static bool server_name_is_utf8 = false;
static dnssd_t *dnssd = NULL;
static raop_t *raop = NULL;
static logger_t *render_logger = NULL;
static bool audio_sync = false;
static bool video_sync = true;
static int64_t audio_delay_alac = 0;
static int64_t audio_delay_aac = 0;
static bool relaunch_video = false;
static bool reset_loop = false;
static unsigned int open_connections= 0;
static std::string videosink = "autovideosink";
static std::string videosink_options = "";
static videoflip_t videoflip[2] = { NONE , NONE };
static bool use_video = true;
static unsigned char compression_type = 0;
static std::string audiosink = "autoaudiosink";
static int  audiodelay = -1;
static bool use_audio = true;
#if __APPLE__
static bool new_window_closing_behavior = false;
#else
static bool new_window_closing_behavior = true;
#endif
static bool close_window;
static std::string video_parser = "h264parse";
static std::string video_decoder = "decodebin";
static std::string video_converter = "videoconvert";
static bool show_client_FPS_data = false;
static FILE *video_dumpfile = NULL;
static std::string video_dumpfile_name = "videodump";
static int video_dump_limit = 0;
static int video_dumpfile_count = 0;
static int video_dump_count = 0;
static bool dump_video = false;
static unsigned char mark[] = { 0x00, 0x00, 0x00, 0x01 };
static FILE *audio_dumpfile = NULL;
static std::string audio_dumpfile_name = "audiodump";
static int audio_dump_limit = 0;
static int audio_dumpfile_count = 0;
static int audio_dump_count = 0;
static bool dump_audio = false;
static unsigned char audio_type = 0x00;
static unsigned char previous_audio_type = 0x00;
static bool fullscreen = false;
static bool render_coverart = false;
static std::string coverart_filename = "";
static std::string metadata_filename = "";
static bool do_append_hostname = true;
static bool use_random_hw_addr = false;
static unsigned short display[5] = {0}, tcp[3] = {0}, udp[3] = {0};
static bool debug_log = DEFAULT_DEBUG_LOG;
static bool suppress_packet_debug_data = false;
static int log_level = LOGGER_INFO;
static bool bt709_fix = false;
static bool srgb_fix = DEFAULT_SRGB_FIX;
static int nohold = 0;
static bool nofreeze = false;
static unsigned short raop_port;
static unsigned short airplay_port;
static uint64_t remote_clock_offset = 0;
static std::vector<std::string> allowed_clients;
static std::vector<std::string> blocked_clients;
static bool restrict_clients;
static bool setup_legacy_pairing = false;
static unsigned char pin_pw = 0;  /* 0: no client access control; 1: onscreen pin ; 2: require password (same password for all clients)  3: random pw*/
static std::string password = "";
static guint min_password_length = MIN_PASSWORD_LENGTH;
static unsigned short pin = 0;
static std::string keyfile = "";
static std::string mac_address = "";
static std::string dacpfile = "";
static bool registration_list = false;
static std::string pairing_register = "";
static std::vector <std::string> registered_keys;
static double db_low = -30.0;
static double db_high = 0.0;
static bool taper_volume = false;
static double initial_volume = 0.0;
static bool h265_support = false;
static int n_video_renderers = 0;
static int n_audio_renderers = 0;
static bool hls_support = false;
static std::string lang = "";
static std::string url = "";
static guint gst_x11_window_id = 0;
static guint video_eos_watch_id = 0;
static guint progress_id = 0;
static guint gst_hls_position_id = 0;
static bool preserve_connections = false;
static guint missed_feedback_limit = MISSED_FEEDBACK_LIMIT;
static guint missed_feedback = 0;
static guint playbin_version = DEFAULT_PLAYBIN_VERSION;
static bool reset_httpd = false;
static bool monitor_progress = false;
static uint32_t rtptime = 0;
static uint32_t rtptime_start = 0;
static uint32_t rtptime_end = 0;
static uint32_t rtptime_coverart_expired = 0;
static std::string artist;
static std::string coverart_artist;
static std::string ble_filename = "";
static std::string rtp_pipeline = "";
static std::string audio_rtp_pipeline = "";
static GMainLoop *gmainloop = NULL;
static bool mux_to_file = false;
static std::string mux_filename = "recording";

//Support for D-Bus-based screensaver inhibition (org.freedesktop.ScreenSaver) 
static unsigned int scrsv = 0;
#ifdef DBUS 
/* these strings can be changed at startup if a non-conforming Desktop Environmemt is detected */
static std::string dbus_service = "org.freedesktop.ScreenSaver";
static std::string dbus_path = "/org/freedesktop/ScreenSaver";
static std::string dbus_interface = "org.freedesktop.ScreenSaver";
static std::string dbus_inhibit = "Inhibit";
static std::string dbus_uninhibit = "UnInhibit";
static DBusConnection *dbus_connection = NULL;
static dbus_uint32_t dbus_cookie = 0;
static DBusPendingCall *dbus_pending = NULL;
static bool dbus_last_message = false;
static const char *appname = DEFAULT_NAME;
static const char *reason_always = "mirroring client: inhibit always";
static const char *reason_active = "actively receiving video";
static int activity_count;
static float previous_hls_position = 0.0f;
static double activity_threshold = 500000.0;  // threshold for FPSdata item txUsageAvg to classify mirror video as "active"
#define MAX_ACTIVITY_COUNT 60
#endif

/* logging */

static void log(int level, const char* format, ...) {
    va_list vargs;
    if (level > log_level) return;
    switch (level) {
    case 0:
    case 1:
    case 2:
    case 3:
        printf("*** ERROR: ");
        break;
    case 4:
        printf("*** WARNING: ");
        break;
    default:
        break;
    }
    va_start(vargs, format);
    vprintf(format, vargs);
    printf("\n");
    va_end(vargs);
}

#define LOGD(...) log(LOGGER_DEBUG, __VA_ARGS__)
#define LOGI(...) log(LOGGER_INFO, __VA_ARGS__)
#define LOGW(...) log(LOGGER_WARNING, __VA_ARGS__)
#define LOGE(...) log(LOGGER_ERR, __VA_ARGS__)

#ifdef DBUS
static void dbus_screensaver_inhibiter(bool inhibit) {
    g_assert(inhibit != dbus_last_message);
    g_assert(scrsv);
    /* receive reply from previous request, whenever that was sent
     * (may have been sent hours ago ... !) 
     * (code modeled on vlc/modules/misc/inhibit/dbus.c) */
    if (dbus_pending != NULL) {
        DBusMessage *reply;
        dbus_pending_call_block(dbus_pending);
        reply = dbus_pending_call_steal_reply(dbus_pending);
        dbus_pending_call_unref(dbus_pending);
        dbus_pending = NULL;
        if (reply != NULL) {
            if (!dbus_message_get_args(reply, NULL,
                                       DBUS_TYPE_UINT32, &dbus_cookie,
                                       DBUS_TYPE_INVALID)) {
                dbus_cookie = 0;
            }
            dbus_message_unref(reply);
        }
        LOGD("screen_saver: got D-Bus cookie %" PRIu32, (uint32_t) dbus_cookie);
    }
    
    if (!dbus_cookie && !inhibit) {
        return; /* nothing to do */
    }
	  
    /* send request */
    const char *dbus_method = inhibit ? dbus_inhibit.c_str() : dbus_uninhibit.c_str();
    DBusMessage *dbus_message = dbus_message_new_method_call(dbus_service.c_str(),
                                                             dbus_path.c_str(),
                                                             dbus_interface.c_str(),
                                                             dbus_method);
    g_assert (dbus_message);
    
    if (inhibit) {
        dbus_bool_t ret;
        const char *reason = (scrsv == 1) ? reason_active : reason_always;
	
        ret = dbus_message_append_args(dbus_message,
                                       DBUS_TYPE_STRING, &appname,
                                       DBUS_TYPE_STRING, &reason,
                                       DBUS_TYPE_INVALID);
	g_assert(ret);

        ret =  dbus_connection_send_with_reply(dbus_connection, dbus_message, &dbus_pending, -1);
        if (!ret) {
            dbus_pending = NULL;
        }
    } else {
        g_assert(dbus_cookie);
        LOGD("screen_saver: releasing D-Bus cookie %" PRIu32, (uint32_t) dbus_cookie);
        if (dbus_message_append_args(dbus_message,
                                     DBUS_TYPE_UINT32, &dbus_cookie,
                                     DBUS_TYPE_INVALID)
            && dbus_connection_send(dbus_connection, dbus_message, NULL)) {
            dbus_cookie = 0;
        }
    }
    
    dbus_connection_flush(dbus_connection);
    dbus_message_unref(dbus_message);
    dbus_last_message = inhibit;
}
#endif

static bool file_has_write_access (const char * filename) {
    bool exists = false;
    bool write = false;
#ifdef _WIN32
    if ((exists = _access(filename, 0) != -1)) {
        write = (_access(filename, 2) != -1);
    }
#else
    if ((exists = access(filename, F_OK) != -1)) {
        write = (access(filename, W_OK) != -1);
    }
#endif
    if (!exists) {
        FILE *fp = fopen(filename, "w");
        if (fp) {
            write = true;
	    fclose(fp);
	    remove(filename);
        }
    }
    return write;
}

/* 95 byte png file with a 1x1 white square (single pixel): placeholder for coverart*/
static const unsigned char empty_image[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56,
    0xca, 0x00, 0x00, 0x00, 0x03, 0x50, 0x4c, 0x54,  0x45, 0x00, 0x00, 0x00, 0xa7, 0x7a, 0x3d, 0xda,
    0x00, 0x00, 0x00, 0x01, 0x74, 0x52, 0x4e, 0x53,  0x00, 0x40, 0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63,  0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe2,
    0x21, 0xbc, 0x33, 0x00, 0x00, 0x00, 0x00, 0x49,  0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

static size_t write_coverart(const char *filename, const void *image, size_t len) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    size_t count = fwrite(image, 1, len, fp);
    fclose(fp);
    return count;
}

static size_t write_metadata(const char *filename, const char *text) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    size_t count = fwrite(text, sizeof(char), strlen(text) + 1, fp);
    fclose(fp);
    return count;
}

static int write_bledata( const uint32_t *pid, const char *process_name, const char *filename) {
    char name[16] { 0 };
    size_t len = strlen(process_name);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    printf("port %u\n", raop_port);
    size_t count = sizeof(uint16_t) * fwrite(&raop_port, sizeof(uint16_t), 1, fp);
    count += sizeof(uint32_t) * fwrite(pid, sizeof(uint32_t), 1, fp);
    count += sizeof(char) * len * fwrite(process_name, 1, len * sizeof(char), fp);
    fclose(fp);
    return (int) count;
}

static char *create_pin_display(char *pin_str, int margin, int gap) {
    char *ptr;
    char num[2] = { 0 };
    int w = 10;
    int h = 8;
    char digits[10][8][11] = { "0821111380", "2114005113", "1110000111", "1110000111", "1110000111", "1110000111", "5113002114", "0751111470",
                               "0002111000", "0021111000", "0000111000", "0000111000", "0000111000", "0000111000", "0000111000", "0011111110", 
                               "0811112800", "2114005113", "0000000111", "0000082114", "0862111470", "2114700000", "1117000000", "1111111111", 
                               "0821111380", "2114005113", "0000082114", "0000111170", "0000075130", "1110000111", "5113002114", "0751111470", 
                               "0000211110", "0001401110", "0021401110", "0214001110", "2110001110", "1111111111", "0000001110", "0000001110", 
                               "1111111110", "1110000000", "1110000000", "1112111380", "0000075113", "0000000111", "5113002114", "0711114700",  
                               "0821111380", "2114005113", "1110000000", "1112111380", "1114075113", "1110000111", "5113002114", "0751111470", 
                               "1111111111", "0000002114", "0000021140", "0000211400", "0002114000", "0021140000", "0211400000", "2114000000", 
                               "0831111280", "2114002114", "5113802114", "0751111170", "8214775138", "1110000111", "5113002114", "0751111470", 
                               "0821111380", "2114005113", "1110000111", "5113802111", "0751114111", "0000000111", "5113002114", "0751111470"  
                             };

    char pixels[9] = { ' ', '8', 'd', 'b', 'P', 'Y', 'o', '"', '.' };
    /* Ascii art used here is derived from the FIGlet font "collosal" */

    int pin_val = (int) strtoul(pin_str, &ptr, 10);
    if (*ptr) {
        return NULL;
    }
    int len = strlen(pin_str);
    int *pin = (int *) calloc( len, sizeof(int));
    if(!pin) {
        return NULL;
    }

    for (int i = 0; i < len; i++) {
        pin[len - 1 - i] = pin_val % 10;
        pin_val = pin_val / 10;
    }
  
    int size = 4 + h*(margin + len*(w + gap + 1));
    char *pin_image = (char *) calloc(size, sizeof(char));
    if (!pin_image) {
        return NULL;
    }
    char *pos = pin_image;
    snprintf(pos, 2, "\n"); 
    pos++;

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < margin; j++) {
            snprintf(pos, 2,  " ");
            pos++;
        }

        for (int j = 0; j < len; j++) {
            int l = pin[j];
            char *p = digits[l][i];
            for (int k = 0; k < w; k++) {
                char *ptr;
                strncpy(num, p++, 1);
                int r = (int) strtoul(num, &ptr, 10);
                snprintf(pos, 2, "%c", pixels[r]);
                pos++;
            }
            for (int n=0; n < gap ; n++) {
                snprintf(pos, 2, " ");
                pos++;
            }
        }
        snprintf(pos, 2, "\n");
        pos++;
    }
    snprintf(pos, 2, "\n");
    return pin_image;
}

static void dump_audio_to_file(unsigned char *data, int datalen, unsigned char type) {
    if (!audio_dumpfile && audio_type != previous_audio_type) {
        char suffix[20];
        std::string fn = audio_dumpfile_name;
        previous_audio_type = audio_type;
        audio_dumpfile_count++;
        audio_dump_count = 0;
        /* type 0x20 is lossless ALAC, type 0x80 is compressed AAC-ELD, type 0x10 is "other" */
        if (audio_type == 0x20) {
            snprintf(suffix, sizeof(suffix), ".%d.alac", audio_dumpfile_count);
        } else if (audio_type == 0x80) {
            snprintf(suffix, sizeof(suffix), ".%d.aac", audio_dumpfile_count);
        } else {
            snprintf(suffix, sizeof(suffix), ".%d.aud", audio_dumpfile_count);
        }
        fn.append(suffix);
        audio_dumpfile = fopen(fn.c_str(),"w");
        if (audio_dumpfile == NULL) {
            LOGE("could not open file %s for dumping audio frames",fn.c_str());
        }
    }

    if (audio_dumpfile) {
        fwrite(data, 1, datalen, audio_dumpfile);
        if (audio_dump_limit) {
            audio_dump_count++;
            if (audio_dump_count == audio_dump_limit) {
                fclose(audio_dumpfile);
                audio_dumpfile = NULL;
            }          
        }
    }
}

static void dump_video_to_file(unsigned char *data, int datalen) {
    /*  SPS NAL has (data[4] & 0x1f) = 0x07  */
    if ((data[4] & 0x1f) == 0x07  && video_dumpfile && video_dump_limit) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
        video_dumpfile = NULL;
        video_dump_count = 0;                     
    }

    if (!video_dumpfile) {
        std::string fn = video_dumpfile_name;
        if (video_dump_limit) {
            char suffix[20];
            video_dumpfile_count++;
            snprintf(suffix, sizeof(suffix), ".%d", video_dumpfile_count);
            fn.append(suffix);
        }
        fn.append(".h264");
        video_dumpfile = fopen (fn.c_str(),"w");
        if (video_dumpfile == NULL) {
            LOGE("could not open file %s for dumping h264 frames",fn.c_str());
        }
    }

    if (video_dumpfile) {
        if (video_dump_limit == 0) {
            fwrite(data, 1, datalen, video_dumpfile);
        } else if (video_dump_count < video_dump_limit) {
            video_dump_count++;
            fwrite(data, 1, datalen, video_dumpfile);
        }
    }
}

static gboolean feedback_callback(gpointer loop) {
    if (open_connections) {
        if (missed_feedback_limit && missed_feedback > missed_feedback_limit) {
            LOGI("***ERROR lost connection with client (network problem?)");
            LOGI("%u missed client feedback signals exceeds limit of %u", missed_feedback, missed_feedback_limit);
            LOGI("   Sometimes the network connection may recover after a longer delay:\n"
                 "   the default limit n = %d seconds, can be changed with the \"-reset n\" option", MISSED_FEEDBACK_LIMIT);
            if (!nofreeze) {
                close_window = false; /* leave "frozen" window open if reset_video is false */
            }
	    reset_httpd = true;
	    relaunch_video = true;
            g_main_loop_quit((GMainLoop *) loop);
            return TRUE;
        } else if (missed_feedback > 2) {
            LOGE("%u missed client feedback signals (expected every two seconds); client may be offline", missed_feedback);
        }
        missed_feedback++;
    } else {
        missed_feedback = 0;
    }
    return TRUE;
}

static gboolean reset_callback(gpointer loop) {
    if (reset_loop) {
        g_main_loop_quit((GMainLoop *) loop);
    }
    return TRUE;
}

static gboolean x11_window_callback(gpointer loop) {
    /* called while trying to find an x11 window used by playbin (HLS mode) */
    if (waiting_for_x11_window()) {
        return TRUE;
    }
    g_source_remove(gst_x11_window_id);
    gst_x11_window_id = 0;
    return FALSE;
}

/* signals handlers (ctrl-c, etc )*/

static void cleanup();

#ifdef _WIN32
static gboolean handle_signal(gpointer data) {
    relaunch_video = false;
    g_main_loop_quit(gmainloop);
    return G_SOURCE_REMOVE;
}

static BOOL WINAPI CtrlHandler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (gmainloop) {
            g_idle_add(handle_signal, NULL);
            return TRUE;
        } else {  
            cleanup();
            exit(0);
	}
    default:
        return FALSE;
    }
}
#else
static void CtrlHandler(int signum) {
    cleanup();
    exit(0);
}

static gboolean sigint_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean sigterm_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean sighup_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}
#endif

static void display_progress(uint32_t start, uint32_t curr, uint32_t end) {
    if (curr < start || curr > end) {
        return;
    }
    int duration = (int)  (end  - start)/44100;
    int position = (int)  (curr - start)/44100;
    int remain = duration - position;
    printf("audio progress (min:sec): %3d:%2.2d; remaining: %3d:%2.2d; track length %d:%2.2d\r",
           position/60, position%60, remain/60, remain%60, duration/60, duration%60);
    fflush(NULL);
}

static gboolean progress_callback (gpointer loop) {
    if (monitor_progress) {
        if (rtptime_start || rtptime_end) {
            display_progress(rtptime_start, rtptime, rtptime_end);
        }
        if (render_coverart && coverart_artist == "_expired_" && rtptime - rtptime_coverart_expired > 44100 * 5) {
            /* remove any expired coverart still being rendered more than 5 secs after it expired */ 
            coverart_artist.erase();
            video_renderer_cycle();
        }
        return TRUE;
    } else {
        progress_id = 0;
        return FALSE;
    }
}

static gboolean video_eos_watch_callback (gpointer loop) {
    if (video_renderer_eos_watch()) {
        /* HLS video has sent EOS */
        LOGI("hls video has sent EOS");
        video_renderer_hls_ready();
        raop_handle_eos(raop);
    }
    return TRUE;
}

#define MAX_VIDEO_RENDERERS 3
#define MAX_AUDIO_RENDERERS 2
static void main_loop()  {
    guint gst_video_bus_watch_id[MAX_VIDEO_RENDERERS] = { 0 };
    guint gst_audio_bus_watch_id[MAX_AUDIO_RENDERERS] = { 0 };
    GMainLoop *loop = g_main_loop_new(NULL,FALSE);
    relaunch_video = false;
    monitor_progress = false;
    reset_loop = false;
    reset_httpd = false;
    preserve_connections = false;
    n_video_renderers = 0;
    n_audio_renderers = 0;
    if (use_video) {
        n_video_renderers = 1;
        relaunch_video = true;
        if (url.empty()) {
            if (h265_support) {
                n_video_renderers++;
            }
            if (render_coverart) {
                n_video_renderers++;
            }
            /* renderer[0] : h264 video; followed by  h265 video (optional) and jpeg (optional)  */
            gst_x11_window_id = 0;
	    video_eos_watch_id = 0;
        } else {
            /* hls video will be rendered: renderer[0] : hls  */
            url.erase();
            video_eos_watch_id = g_timeout_add(100, (GSourceFunc) video_eos_watch_callback, (gpointer) loop);
            gst_x11_window_id = g_timeout_add(100, (GSourceFunc) x11_window_callback, (gpointer) loop);
        }
        g_assert(n_video_renderers <= MAX_VIDEO_RENDERERS);
        for (int i = 0; i < n_video_renderers; i++) {
            gst_video_bus_watch_id[i] = (guint) video_renderer_listen((void *)loop, i);
        }
    }
    if (use_audio) {
        rtptime_start = 0;
        rtptime_end = 0;
        monitor_progress = true;
        artist.erase();
        coverart_artist.erase();
        progress_id  = g_timeout_add_seconds(1,(GSourceFunc) progress_callback, (gpointer) loop);
        n_audio_renderers = 2;
        g_assert(n_audio_renderers <= MAX_AUDIO_RENDERERS);
        for (int i = 0; i < n_audio_renderers; i++) {
            gst_audio_bus_watch_id[i] = (guint) audio_renderer_listen((void *)loop, i);      
        }
    }

    missed_feedback = 0;
    guint feedback_watch_id = g_timeout_add_seconds(1, (GSourceFunc) feedback_callback, (gpointer) loop);
    guint reset_watch_id = g_timeout_add(100, (GSourceFunc) reset_callback, (gpointer) loop);

#ifdef _WIN32
    gmainloop = loop;
#else 
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    guint sigterm_watch_id = g_unix_signal_add(SIGTERM, (GSourceFunc) sigterm_callback, (gpointer) loop);
    guint sigint_watch_id = g_unix_signal_add(SIGINT, (GSourceFunc) sigint_callback, (gpointer) loop);
    guint sighup_watch_id = g_unix_signal_add(SIGHUP, (GSourceFunc) sigint_callback, (gpointer) loop);
#endif
    g_main_loop_run(loop);

#ifdef _WIN32
    gmainloop = NULL;
#else
    signal(SIGINT, CtrlHandler);  //switch back to non-mainloop CtrlHandler
    signal(SIGTERM, CtrlHandler);
    signal(SIGHUP, CtrlHandler);
    if (sigint_watch_id > 0) g_source_remove(sigint_watch_id);
    if (sigterm_watch_id > 0) g_source_remove(sigterm_watch_id);
    if (sighup_watch_id > 0) g_source_remove(sighup_watch_id);
#endif

    for (int i = 0; i < n_video_renderers; i++) {
        if (gst_video_bus_watch_id[i] > 0) g_source_remove(gst_video_bus_watch_id[i]);
    }
    for (int i = 0; i < n_audio_renderers; i++) {
        if (gst_audio_bus_watch_id[i] > 0) g_source_remove(gst_audio_bus_watch_id[i]);
    }
    if (gst_x11_window_id > 0) g_source_remove(gst_x11_window_id);
    if (reset_watch_id > 0) g_source_remove(reset_watch_id);
    if (progress_id > 0) g_source_remove(progress_id);
    if (video_eos_watch_id > 0) g_source_remove(video_eos_watch_id);
    if (feedback_watch_id > 0) g_source_remove(feedback_watch_id);
    g_main_loop_unref(loop);
}    

static int parse_hw_addr (std::string str, std::vector<char> &hw_addr) {
    for (int i = 0; i < (int) str.length(); i += 3) {
        hw_addr.push_back((char) stol(str.substr(i), NULL, 16));
    }
    return 0;
}

static const char *get_homedir() {
    const char *homedir = getenv("XDG_CONFIG_HOMEDIR");
    if (homedir == NULL) {
        homedir = getenv("HOME");
    }
#ifndef _WIN32
    if (homedir == NULL){
        homedir = getpwuid(getuid())->pw_dir;
    }
#endif
    return homedir;
}

static std::string find_uxplay_config_file() {
    std::string no_config_file = "";
    const char *homedir = NULL;
    const char *uxplayrc = NULL;
    std::string config0, config1, config2;
    struct stat sb;
    uxplayrc = getenv("UXPLAYRC");   /* first look for $UXPLAYRC */
    if (uxplayrc) {
        config0 = uxplayrc;
        if (stat(config0.c_str(), &sb) == 0) return config0;
    }
    homedir = get_homedir();
    if (homedir) {
      config1 = homedir;
      config1.append("/.uxplayrc");
      if (stat(config1.c_str(), &sb) == 0) return config1;  /* look for ~/.uxplayrc */
      config2 = homedir;
      config2.append("/.config/uxplayrc"); /* look for ~/.config/uxplayrc */
      if (stat(config2.c_str(), &sb) == 0) return config2;
    }
    return no_config_file;
}

static std::string find_mac () {
/*  finds the MAC address of a network interface *
 *  in a Windows, Linux, *BSD or macOS system.   */
    std::string mac = "";
    char str[3];
#ifdef _WIN32
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
    if (addresses == NULL) { 					
        return mac;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
        if (addresses == NULL) {
            return mac;
        }
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES address = addresses; address != NULL; address = address->Next) {
            if (address->PhysicalAddressLength != 6                 /* MAC has 6 octets */
                || (address->IfType != 6 && address->IfType != 71)  /* Ethernet or Wireless interface */
                || address->OperStatus != 1) {                      /* interface is up */
                continue;
            }
            mac.erase();
            for (int i = 0; i < 6; i++) {
                snprintf(str, sizeof(str), "%02x", int(address->PhysicalAddress[i]));
                mac = mac + str;
                if (i < 5) mac = mac + ":";
            }
	    break;
        }
    }
    free(addresses);
    return mac;
#else
    struct ifaddrs *ifap, *ifaptr;
    int non_null_octets = 0;
    unsigned char octet[6];
    if (getifaddrs(&ifap) == 0) {
        for(ifaptr = ifap; ifaptr != NULL; ifaptr = ifaptr->ifa_next) {
            if(ifaptr->ifa_addr == NULL) continue;
#ifdef __linux__
            if (ifaptr->ifa_addr->sa_family != AF_PACKET) continue;
            struct sockaddr_ll *s = (struct sockaddr_ll*) ifaptr->ifa_addr;
            for (int i = 0; i < 6; i++) {
                if ((octet[i] = s->sll_addr[i]) != 0) non_null_octets++;
            }
#else    /* macOS and *BSD */
            if (ifaptr->ifa_addr->sa_family != AF_LINK) continue;
            unsigned char *ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) ifaptr->ifa_addr);
            for (int i= 0; i < 6 ; i++) {
                if ((octet[i] = *ptr) != 0) non_null_octets++;
                ptr++;
            }
#endif
            if (non_null_octets) {
                mac.erase();
                for (int i = 0; i < 6 ; i++) {
                    snprintf(str, sizeof(str), "%02x", octet[i]);
                    mac = mac + str;
                    if (i < 5) mac = mac + ":";
                }
                break;
            }
        }
    }
    freeifaddrs(ifap);
#endif
    return mac;
}

static bool validate_mac(char * mac_address) {
    char c;
    if (strlen(mac_address) != 17)  return false;
    for (int i = 0; i < 17; i++) {
        c = *(mac_address + i);
        if (i % 3 == 2) {
            if (c != ':')  return false;
        } else {
            if (c < '0') return false;
            if (c > '9' && c < 'A') return false;
            if (c > 'F' && c < 'a') return false;
            if (c > 'f') return false;
        }
    }
    return true;
}

static std::string random_mac () {
    char str[4];
    unsigned char random[6];
    get_random_bytes(random, sizeof(random));
    /* mark MAC address as locally administered, i.e. random */
    random[0] = random[0] & ~0x01;
    random[0] = random[0] | 0x02;
    snprintf(str,3,"%2.2x", random[0]);
    std::string mac_address(str);
    for (int i = 1; i < 6; i++) {
        snprintf(str,4,":%2.2x", random[i]);
        mac_address = mac_address + str;
    }
    return mac_address;
}

static void print_info (char *name) {
    printf("UxPlay %s: An open-source AirPlay mirroring server.\n", VERSION);
    printf("=========== Website: https://github.com/FDH2/UxPlay ==========\n");
    printf("Usage: %s [-n name] [-s wxh] [-p [n]] [(other options)]\n", name);
    printf("Options:\n");
    printf("-n name   Specify network name of the AirPlay server (UTF-8/ascii)\n");
    printf("-nh       Do not add \"@hostname\" at the end of AirPlay server name\n");
    printf("-h265     Support h265 (4K) video (with h265 versions of h264 plugins)\n");
    printf("-mp4 [fn] Record (non-HLS)audio/video to mp4 file \"fn.[n].[format].mp4\"\n");
    printf("          n=1,2,.. format = H264/5, ALAC/AAC. Default fn=\"recording\"\n");
    printf("-hls [v]  Support HTTP Live Streaming (HLS), Youtube app video only: \n");
    printf("          v = 2 or 3 (default 3) optionally selects video player version\n");
    printf("-lang xx  HLS language preferences (\"fr:es:..\", overrides $LANGUAGE)\n");
    printf("-lang     (or -lang 0): play undubbed HLS version (overrides $LANGUAGE)\n");
    printf("-scrsv n  Screensaver override n: 0=off 1=on during activity 2=always on\n");
    printf("-pin[xxxx]Use a 4-digit pin code to control client access (default: no)\n");
    printf("          default pin is random: optionally use fixed pin xxxx\n");
    printf("-reg [fn] Keep a register in $HOME/.uxplay.register to verify returning\n");
    printf("          client pin-registration; (option: use file \"fn\" for this)\n");
    printf("-pw [pwd] Require use of password to control client access;\n");
    printf("          (with no pwd, pin entry is required at *each* connection.)\n");  
    printf("          (option \"-pw\" after \"-pin\" overrides it, and vice versa)\n");
    printf("-vsync [x]Mirror mode: sync audio to video using timestamps (default)\n");
    printf("          x is optional audio delay: millisecs, decimal, can be neg.\n");
    printf("-vsync no Switch off audio/(server)video timestamp synchronization \n");
    printf("-async [x]Audio-Only mode: sync audio to client video (default: no)\n");
    printf("-async no Switch off audio/(client)video timestamp synchronization\n");
    printf("-db l[:h] Set minimum volume attenuation to l dB (decibels, negative);\n");
    printf("          optional: set maximum to h dB (+ or -) default: -30.0:0.0 dB\n");
    printf("-taper    Use a \"tapered\" AirPlay volume-control profile\n");
    printf("-vol <v>  Set initial audio-streaming volume: range [mute=0.0:1.0=full]\n"); 
    printf("-s wxh[@r]Request to client for video display resolution [refresh_rate]\n"); 
    printf("          default 1920x1080[@60] (or 3840x2160[@60] with -h265 option)\n");
    printf("-o        Set display \"overscanned\" mode on (not usually needed)\n");
    printf("-fs       Full-screen (only with X11, Wayland, VAAPI, D3D11/12, kms)\n");
    printf("-p        Use legacy ports UDP 6000:6001:7011 TCP 7000:7001:7100\n");
    printf("-p n      Use TCP and UDP ports n,n+1,n+2. range %d-%d\n", LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    printf("          use \"-p n1,n2,n3\" to set each port, \"n1,n2\" for n3 = n2+1\n");
    printf("          \"-p tcp n\" or \"-p udp n\" sets TCP or UDP ports separately\n");
    printf("-avdec    Force software h264 video decoding with libav decoder\n"); 
    printf("-vp ...   Choose the GSteamer h264 parser: default \"h264parse\"\n");
    printf("-vd ...   Choose the GStreamer h264 decoder; default \"decodebin\"\n");
    printf("          choices: (software) avdec_h264; (hardware) v4l2h264dec,\n");
    printf("          nvdec, nvh264dec, vaapih264dec, vtdec,etc.\n");
    printf("          choices: avdec_h264,vaapih264dec,nvdec,nvh264dec,v4l2h264dec\n");
    printf("-vc ...   Choose the GStreamer videoconverter; default \"videoconvert\"\n");
    printf("          another choice when using v4l2h264dec: v4l2convert\n");
    printf("-vs ...   Choose the GStreamer videosink; default \"autovideosink\"\n");
    printf("          some choices: ximagesink,xvimagesink,vaapisink,glimagesink,\n");
    printf("          gtksink,waylandsink,kmssink,fbdevsink,osxvideosink,\n");
    printf("          d3d11videosink,d3d12videosink, etc.\n");
    printf("-vs 0     Streamed audio only, with no video display window\n");
    printf("-vrtp pl  Use rtph26[4,5]pay to send decoded video elsewhere: \"pl\"\n");
    printf("          is the remaining pipeline, starting with rtph26*pay options:\n");
    printf("          e.g. \"config-interval=1 ! udpsink host=127.0.0.1 port=5000\"\n");
    printf("          Writes output to \"fn.N.mp4\"\n");
    printf("-v4l2     Use Video4Linux2 for GPU hardware h264 decoding\n");
    printf("-bt709    Sometimes needed for Raspberry Pi models using Video4Linux2 \n");
    printf("-srgb     Display \"Full range\" [0-255] color, not \"Limited Range\"[16-235]\n");
    printf("          This is a workaround for a GStreamer problem, until it is fixed\n");
    printf("-srgb no  Disable srgb option (use when enabled by default: Linux, *BSD)\n");
    printf("-as ...   Choose the GStreamer audiosink; default \"autoaudiosink\"\n");
    printf("          some choices:pulsesink,alsasink,pipewiresink,jackaudiosink,\n");
    printf("          osssink,oss4sink,osxaudiosink,wasapisink,directsoundsink.\n");
    printf("-as 0     (or -a)  Turn audio off, streamed video only\n");
    printf("-artp pl  Use rtpL16pay to send decoded audio elsewhere: \"pl\"\n");
    printf("          is the remaining pipeline, starting with rtpL16pay options:\n");
    printf("          e.g. \"pt=96 ! udpsink host=127.0.0.1 port=5002\"\n");
    printf("-al x     Audio latency in seconds (default 0.25) reported to client.\n");
    printf("-ca [<fn>]In Audio (ALAC) mode, render cover-art [or write to file <fn>]\n");
    printf("-md <fn>  In Airplay Audio (ALAC) mode, write metadata text to file <fn>\n");
    printf("-reset n  Reset after n seconds of client silence (default n=%d, 0=never)\n", MISSED_FEEDBACK_LIMIT);
    printf("-nofreeze Do NOT leave frozen screen in place after reset\n");
    printf("-nc       Do NOT  Close video window when client stops mirroring\n");
    printf("-nc no    Cancel the -nc option (DO close video window) \n");
    printf("-nohold   Drop current connection when new client connects.\n");
    printf("-restrict Restrict clients to those specified by \"-allow <deviceID>\"\n");
    printf("          UxPlay displays deviceID when a client attempts to connect\n");
    printf("          Use \"-restrict no\" for no client restrictions (default)\n");
    printf("-allow <i>Permit deviceID = <i> to connect if restrictions are imposed\n");
    printf("-block <i>Always block connections from deviceID = <i>\n");
    printf("-FPSdata  Show video-streaming performance reports sent by client.\n");
    printf("-fps n    Set maximum allowed streaming framerate, default 30\n");
    printf("-f {H|V|I}Horizontal|Vertical flip, or both=Inversion=rotate 180 deg\n");
    printf("-r {R|L}  Rotate 90 degrees Right (cw) or Left (ccw)\n");
    printf("-m [mac]  Set MAC address (also Device ID);use for concurrent UxPlays\n");
    printf("          if mac xx:xx:xx:xx:xx:xx is not given, a random MAC is used\n");
    printf("-key [fn] Store private key in $HOME/.uxplay.pem (or in file \"fn\")\n");
    printf("-dacp [fn]Export client DACP information to file $HOME/.uxplay.dacp\n");
    printf("          (option to use file \"fn\" instead); used for client remote\n");
    printf("-ble [fn] For BluetoothLE beacon: write data to file ~/.uxplay.ble\n");
    printf("          optional: write to file \"fn\" (\"fn\" = \"off\" to cancel)\n");
    printf("-d [n]    Enable debug logging; optional: n=1 to skip normal packet data\n");
    printf("-vdmp [n] Dump h264 video output to \"fn.h264\"; fn=\"videodump\",change\n");
    printf("          with \"-vdmp [n] filename\". If [n] is given, file fn.x.h264\n");
    printf("          x=1,2,.. opens whenever a new SPS/PPS NAL arrives, and <=n\n");
    printf("          NAL units are dumped.\n");
    printf("-admp [n] Dump audio output to \"fn.x.fmt\", fmt ={aac, alac, aud}, x\n");
    printf("          =1,2,..; fn=\"audiodump\"; change with \"-admp [n] filename\".\n");
    printf("          x increases when audio format changes. If n is given, <= n\n");
    printf("          audio packets are dumped. \"aud\"= unknown format.\n");
    printf("-v        Displays version information\n");
    printf("-h        Displays this help\n");
    printf("-rc fn    Read startup options from file \"fn\" instead of ~/.uxplayrc, etc\n");
    printf("Startup options in $UXPLAYRC, ~/.uxplayrc, or ~/.config/uxplayrc are\n");
    printf("applied first (command-line options may modify them): format is one \n");
    printf("option per line, no initial \"-\"; lines starting with \"#\" are ignored.\n");
}

static bool option_has_value(const int i, const int argc, std::string option, const char *next_arg) {
    if (i >= argc - 1 || next_arg[0] == '-') {
        LOGE("invalid: \"%s\" had no argument", option.c_str());
        return false;
     }
    return true;
}

static bool get_display_settings (std::string value, unsigned short *w, unsigned short *h, unsigned short *r) {
    // assume str  = wxh@r is valid if w and h are positive decimal integers
    // with no more than 4 digits, r < 256 (stored in one byte).
    char *end;
    std::size_t pos = value.find_first_of("x");
    if (pos == std::string::npos) return false;
    std::string str1 = value.substr(pos+1);
    value.erase(pos);
    if (value.length() == 0 || value.length() > 4 || value[0] == '-') return false;
    *w = (unsigned short) strtoul(value.c_str(), &end, 10);
    if (*end || *w == 0)  return false;
    pos = str1.find_first_of("@");
    if(pos != std::string::npos) {
        std::string str2 = str1.substr(pos+1);
        if (str2.length() == 0 || str2.length() > 3 || str2[0] == '-') return false;
        *r = (unsigned short) strtoul(str2.c_str(), &end, 10);
        if (*end || *r == 0 || *r > 255) return false;
        str1.erase(pos);
    }
    if (str1.length() == 0 || str1.length() > 4 || str1[0] == '-') return false;
    *h = (unsigned short) strtoul(str1.c_str(), &end, 10);
    if (*end || *h == 0) return false;
    return true;
}

static bool get_value (const char *str, unsigned int *n) {
    // if n > 0 str must be a positive decimal <= input value *n  
    // if n = 0, str must be a non-negative decimal
    if (strlen(str) == 0 || strlen(str) > 10 || str[0] == '-') return false;
    char *end;
    unsigned long l = strtoul(str, &end, 10);
    if (*end) return false;
    if (*n && (l == 0 || l > *n)) return false;
    *n = (unsigned int) l;
    return true;
}

static bool get_ports (int nports, std::string option, const char * value, unsigned short * const port) {
    /*valid entries are comma-separated values port_1,port_2,...,port_r, 0 < r <= nports */
    /*where ports are distinct, and are in the allowed range.                            */
    /*missed values are consecutive to last given value (at least one value needed).    */
    char *end;
    unsigned long l;
    std::size_t pos;
    std::string val(value), str;
    for (int i = 0; i <= nports ; i++)  {
        if(i == nports) break;
        pos = val.find_first_of(',');
        str = val.substr(0,pos);
        if(str.length() == 0 || str.length() > 5 || str[0] == '-') break;
        l = strtoul(str.c_str(), &end, 10);
        if (*end || l < LOWEST_ALLOWED_PORT || l > HIGHEST_PORT) break;
         *(port + i) = (unsigned short) l;
        for  (int j = 0; j < i ; j++) {
            if( *(port + j) == *(port + i)) break;
        }
        if(pos == std::string::npos) {
            if (nports + *(port + i) > i + 1 + HIGHEST_PORT) break;
            for (int j = i + 1; j < nports; j++) {
                *(port + j) = *(port + j - 1) + 1;
            }
            return true;
        }
        val.erase(0, pos+1);
    }
    LOGE("invalid \"%s %s\", all %d ports must be in range [%d,%d]",
         option.c_str(), value, nports, LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    return false;
}

static bool get_videoflip (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'I':
        *videoflip = INVERT;
        break;
    case 'H':
        *videoflip = HFLIP;
        break;
    case 'V':
        *videoflip = VFLIP;
        break;
    default:
        return false;
    }
    return true;
}

static bool get_videorotate (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'L':
        *videoflip = LEFT;
        break;
    case 'R':
        *videoflip = RIGHT;
        break;
    default:
        return false;
    }
    return true;
}

static void append_hostname(std::string &server_name) {
#ifdef _WIN32   /*modification for compilation on Windows */
    char buffer[256] = "";
    unsigned long size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) {
        std::string name = server_name;
        name.append("@");
        name.append(buffer);
        server_name = name;
    }
#else
    struct utsname buf;
    if (!uname(&buf)) {
        std::string name = server_name;
        name.append("@");
        name.append(buf.nodename);
        server_name = name;
    }
#endif
}

bool is_utf8(const char *string, bool *is_printable_ascii) {
    /* test if C-string is printable ascii or valid UTF-8 (max 4 bytes) */
    if (is_printable_ascii)  {
        *is_printable_ascii = true;
    }
    int len = (int) strlen(string);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char) string[i];
        int n = 0;
        if (0x20 <= c && c <= 0x7e) {
            continue;   //printable ascii, no control characters.
        } else if (is_printable_ascii) {
            *is_printable_ascii = false;
        }
        if (0x00 <= c && c <= 0x7f) {
            continue; //one byte code, 0bbbbbbb
        } else if (c == 0xc0 || c == 0xc1) {
             return false;  //two byte code, invalid start byte
        } else if ((c & 0xe0) == 0xc0) {
            n = 1; //two byte code, 110bbbbb
        } else if (c == 0xe0 && i < len - 1 && (unsigned char) string[i + 1] < 0xa0) {
            return false;  //three byte code, overlong encoding
        } else if (c == 0xed && i < len - 1 && (unsigned char) string[i + 1] > 0x9f) {
            return false;  //three byte code, exclude U+dc00 to U+dfff
        } else if ((c & 0xf0) == 0xe0) {
            n = 2; //three byte code 1110bbbb
        } else if (c >= 0xf5) {
            return false;  //four byte code, invalid start byte
        } else if (c == 0xf0 && i < len - 1 && (unsigned char) string[i + 1] < 0x90) {
            return false;  //four byte code, overlong encoding
        } else if (c == 0xf4 && i < len - 1 && (unsigned char) string[i + 1] > 0x8f) {
            return false;  //four byte code, out of range character (> U+10ffff)
        } else if ((c & 0xf8) == 0xf0) {
            n = 3; //four byte code, 11110bbb
        } else {
            return false;  //more than 4 bytes
        }
        for (int j = 0; j < n && i < len ; j++) { // n bytes matching 10bbbbbb must follow ?
            if ((++i == len) || (((unsigned char) string[i] & 0xc0) != 0x80)) {
                return false;
            }
        }
	
    }
    return true;
}

static void parse_arguments (int argc, char *argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (!is_utf8(argv[i], NULL)) {
            fprintf(stderr,"Error: detected a non-ascii or non-UTF-8 string \"%s\""
                    "while parsing input arguments", argv[i]);
            exit(0);
        }
    }
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            i++;  //specifies startup file: has already been processed
        } else if (arg == "-allow") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            i++;
            allowed_clients.push_back(argv[i]);
        } else if (arg == "-block") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            i++;
            blocked_clients.push_back(argv[i]);    
        } else if (arg == "-restrict") {
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    restrict_clients = false;
                    i++;
                    continue;
                }
	    } 
            restrict_clients = true;
        } else if (arg == "-n") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            bool ascii;
            server_name_is_utf8 = false;
            server_name.erase();
            bool utf8 = is_utf8(argv[++i], &ascii);
            if (!utf8) {
                fprintf(stderr, "invalid (non-UTF-8/ascii) server name in \"-n %s\"", argv[i]);
                exit(1);
            }
            server_name = std::string(argv[i]);
            if (!ascii) {
                server_name_is_utf8 = true;
                printf("WARNING: a non-ascii (UTF-8) server-name \"%s\" was specified:"
                       " ensure correct locale settings to display it\n",server_name.c_str()); 
            }
        } else if (arg == "-nh") {
            do_append_hostname = false;
        } else if (arg == "-async") {
            audio_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    audio_sync = false;
                    i++;
                    continue;
		}
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_alac = n * 1000; /* units are nsecs */
                    } else {
                        fprintf(stderr, "invalid -async %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i] );
                        exit (1);
                    }
                }
            }
        } else if (arg == "-scrsv") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            unsigned int n = 0;
            if (!get_value(argv[++i], &n) || n > 2) {
                fprintf(stderr, "invalid \"-scrsv %s\"; values 0, 1, 2 allowed\n", argv[i]);
                exit(1);
            }
#ifdef DBUS
            scrsv = n;
#else
            fprintf(stderr,"invalid: option \"-scrsv\" is currently only implemented for Linux/*BSD systems with D-Bus service\n");
            exit(1);
#endif
        } else if (arg == "-vsync") {
            video_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    video_sync = false;
                    i++;
                    continue;
                }
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_aac = n * 1000;     /* units are nsecs */
                    } else {
                        fprintf(stderr, "invalid -vsync %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i]);
                        exit (1);
                    }
                }
            }
        } else if (arg == "-s") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            std::string value(argv[++i]);
            if (!get_display_settings(value, &display[0], &display[1], &display[2])) {
                fprintf(stderr, "invalid \"-s %s\"; -s wxh : max w,h=9999; -s wxh@r : max r=255\n",
                        argv[i]);
                exit(1);
            }
        } else if (arg == "-fps") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            unsigned int n = 255;
            if (!get_value(argv[++i], &n)) {
                fprintf(stderr, "invalid \"-fps %s\"; -fps n : max n=255, default n=30\n", argv[i]);
                exit(1);
            }
            display[3] = (unsigned short) n;
        } else if (arg == "-o") {
            display[4] = 1;
        } else if (arg == "-f") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videoflip(argv[++i], &videoflip[0])) {
                fprintf(stderr,"invalid \"-f %s\" , unknown flip type, choices are H, V, I\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-r") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videorotate(argv[++i], &videoflip[1])) {
                fprintf(stderr,"invalid \"-r %s\" , unknown rotation  type, choices are R, L\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-p") {
            if (i == argc - 1 || argv[i + 1][0] == '-') {
                tcp[0] = 7100; tcp[1] = 7000; tcp[2] = 7001;
                udp[0] = 7011; udp[1] = 6001; udp[2] = 6000;
                continue;
            }
            std::string value(argv[++i]);
            if (value == "tcp") {
                arg.append(" tcp");
                if(!get_ports(3, arg, argv[++i], tcp)) exit(1);
            } else if (value == "udp") {
                arg.append( " udp");
                if(!get_ports(3, arg, argv[++i], udp)) exit(1);
            } else {
                if(!get_ports(3, arg, argv[i], tcp)) exit(1);
                for (int j = 0; j < 3; j++) {
                    udp[j] = tcp[j];
                }
            }
        } else if (arg == "-m") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                if (validate_mac(argv[++i])) {
                    mac_address.erase();
                    mac_address = argv[i];
                    use_random_hw_addr = false;
                } else {
                    fprintf(stderr,"invalid mac address \"%s\": address must have form"
                            " \"xx:xx:xx:xx:xx:xx\", x = 0-9, A-F or a-f\n", argv[i]);
                    exit(1);
                }
            } else {
                use_random_hw_addr  = true;
            }
        } else if (arg == "-a") {
            use_audio = false;
        } else if (arg == "-d") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 1;
                if (!get_value(argv[++i], &n)) {
                    fprintf(stderr, "invalid \"-d %s\"; -d n : max n=1 (suppress packet data in debug output)\n", argv[i]);
                    exit(1);
                }
                debug_log = true;
                suppress_packet_debug_data = true;
            } else {
                debug_log = !debug_log;
                suppress_packet_debug_data = false;
            }
        } else if (arg == "-h"  || arg == "--help" || arg == "-?" || arg == "-help") {
            print_info(argv[0]);
            exit(0);
        } else if (arg == "-v") {
            printf("UxPlay version %s; for help, use option \"-h\"\n", VERSION);
            exit(0);
        } else if (arg == "-vp") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_parser.erase();
            video_parser.append(argv[++i]);
        } else if (arg == "-vd") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_decoder.erase();
            video_decoder.append(argv[++i]);
        } else if (arg == "-vc") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_converter.erase();
            video_converter.append(argv[++i]);
        } else if (arg == "-vs") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            videosink.erase();
            videosink.append(argv[++i]);
            std::size_t pos = videosink.find(" ");
            if (pos != std::string::npos) {
                videosink_options.erase();
                videosink_options = videosink.substr(pos);
                videosink.erase(pos);
            }
        } else if (arg == "-as") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            audiosink.erase();
            audiosink.append(argv[++i]);
        } else if (arg == "-t") {
            fprintf(stderr,"The uxplay option \"-t\" has been removed: it was a workaround for an  Avahi issue.\n");
            fprintf(stderr,"The correct solution is to open network port UDP 5353 in the firewall for mDNS queries\n");
            exit(1);
        } else if (arg == "-nc") {
            new_window_closing_behavior = false;
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    new_window_closing_behavior = true;
                    i++;
                    continue;
                }
            }
        } else if (arg == "-avdec") {
            video_parser.erase();
            video_parser = "h264parse";
            video_decoder.erase();
            video_decoder = "avdec_h264";
            video_converter.erase();
            video_converter = "videoconvert";
        } else if (arg == "-v4l2") {
            video_decoder.erase();
            video_decoder = "v4l2h264dec";
            video_converter.erase();
            video_converter = "v4l2convert";
        } else if (arg == "-rpi" || arg == "-rpifb" || arg == "-rpigl" || arg == "-rpiwl") {
            fprintf(stderr,"*** -rpi* options do not apply to Raspberry Pi model 5, and have been removed\n");
            fprintf(stderr,"     For models 3 and 4, use their equivalents, if needed:\n");
            fprintf(stderr,"     -rpi   was equivalent to \"-v4l2\"\n");
            fprintf(stderr,"     -rpifb was equivalent to \"-v4l2 -vs kmssink\"\n");
            fprintf(stderr,"     -rpigl was equivalent to \"-v4l2 -vs glimagesink\"\n");
            fprintf(stderr,"     -rpiwl was equivalent to \"-v4l2 -vs waylandsink\"\n");
            fprintf(stderr,"     Option \"-bt709\" may also be needed for R Pi model 4B and earlier\n");
            exit(1);
        } else if (arg == "-fs" ) {
            fullscreen = true;
        } else if (arg == "-FPSdata") {
            show_client_FPS_data = true;
        } else if (arg == "-reset") {
            /* now using feedback  (every 1 sec ) instead of ntp timeouts (every 3 secs) to detect offline client and reset connections */
            fprintf(stderr,"*** NOTE CHANGE: -reset n now means reset n seconds (not 3n seconds) after client goes offline\n");	  
            missed_feedback_limit = 0;
            if (!get_value(argv[++i], &missed_feedback_limit)) {
                fprintf(stderr, "invalid \"-reset %s\"; -reset n must have n >= 0,  default n = %d seconds\n", argv[i], MISSED_FEEDBACK_LIMIT);
                exit(1);
            }
	} else if (arg == "-vrtp") {
	  if (!option_has_value(i, argc, arg, argv[i+1])) {
	    fprintf(stderr,"option \"-vrtp\" must be followed by a pipeline for sending the video stream:\n"
		    "e.g., \"<rtph26[4,5]pay options> ! udpsink host=127.0.0.1 port -= 5000\"\n");
	    exit(1);
          }
	  rtp_pipeline.erase();
	  rtp_pipeline.append(argv[++i]);
	} else if (arg == "-artp") {
	  if (!option_has_value(i, argc, arg, argv[i+1])) {
	    fprintf(stderr,"option \"-artp\" must be followed by a pipeline for sending the audio stream:\n"
		    "e.g., \"<rtpL16pay options> ! udpsink host=127.0.0.1 port=5002\"\n");
	    exit(1);
          }
	  audio_rtp_pipeline.erase();
	  audio_rtp_pipeline.append(argv[++i]);
	} else if (arg == "-vdmp") {
            dump_video = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        fprintf(stderr, "invalid \"-vdmp 0 %s\"; -vdmp n  needs a non-zero value of n\n", argv[i]);
                        exit(1);
                    }
                    video_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        video_dumpfile_name.erase();
                        video_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    video_dumpfile_name.erase();
                    video_dumpfile_name.append(argv[i]);
                }
                const char *fn = video_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-vdmp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   		
            }
        } else if (arg == "-mp4"){
            mux_to_file = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                mux_filename.erase();
                mux_filename.append(argv[++i]);
                const char *fn = mux_filename.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-mp4 <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }
            }
        } else if (arg == "-admp") {
            dump_audio = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        fprintf(stderr, "invalid \"-admp 0 %s\"; -admp n  needs a non-zero value of n\n", argv[i]);
                        exit(1);
                    }
                    audio_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        audio_dumpfile_name.erase();
                        audio_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    audio_dumpfile_name.erase();
                    audio_dumpfile_name.append(argv[i]);
                }
                const char *fn = audio_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-admp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }
            }
        } else if (arg  == "-ca" ) {
            if (i < argc - 1 && *argv[i+1] != '-') {
                coverart_filename.erase();
                coverart_filename.append(argv[++i]);
                const char *fn = coverart_filename.c_str();
                render_coverart = false;
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-ca <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                render_coverart = true;
            }
        } else if (arg  == "-md" ) {
            if (option_has_value(i, argc, arg, argv[i+1])) {
                metadata_filename.erase();
                metadata_filename.append(argv[++i]);
                const char *fn = metadata_filename.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-md <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                fprintf(stderr,"option -md must be followed by a filename for metadata text output\n");
                exit(1);
            }
        } else if (arg  == "-ble" ) {
            ble_filename.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                i++;
                if (strlen(argv[i]) != 3 || strncmp(argv[i], "off", 3)) { 
                    ble_filename.append(argv[i]);
                    if (!file_has_write_access(argv[i])) {
                        fprintf(stderr, "%s cannot be written to:\noption \"-ble<fn>\" must be to a file with write access\n", argv[i]);
                        exit(1);
                    }
                }
            } else {
                static const char* homedir = get_homedir();
                if (homedir) {
                    ble_filename = homedir;
                    ble_filename.append("/.uxplay.ble");
                    if (!file_has_write_access(ble_filename.c_str())) {
                        fprintf(stderr, "%s cannot be written to\n",ble_filename.c_str()) ;
                        exit(1);
                    }
                } else {
                    fprintf(stderr,"failed to obtain home directory\n");
                    exit(1);
                }
            }
        } else if (arg == "-bt709") {
            bt709_fix = true;
        } else if (arg == "-srgb") {
            srgb_fix = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    srgb_fix = false;
                    i++;
                    continue;
                }
            }
        } else if (arg == "-nohold") {
            nohold = 1;
        } else if (arg == "-al") {
	    int n;
            char *end;
            if (i < argc - 1 && *argv[i+1] != '-') {
                n = (int) (strtof(argv[++i], &end) * SECOND_IN_USECS);
                if (*end == '\0' && n >=0 && n <= 10 * SECOND_IN_USECS) {
                    audiodelay = n;
                    continue;
                }
            }
            fprintf(stderr, "invalid -al %s: value must be a decimal time offset in seconds, range [0,10]\n"
                    "(like 5 or 4.8, which will be converted to a whole number of microseconds)\n", argv[i]);
            exit(1);
        } else if (arg == "-pin") {
            setup_legacy_pairing = true;
            pin_pw = 1;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 9999;
                if (!get_value(argv[++i], &n)) {
                    fprintf(stderr, "invalid \"-pin %s\"; -pin nnnn : max nnnn=9999, (4 digits)\n", argv[i]);
                    exit(1);
                }
                pin = n + 10000;
            }
	} else if (arg == "-reg") {
            registration_list = true;
            pairing_register.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                pairing_register.append(argv[++i]);
                const char * fn = pairing_register.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-reg <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            }
        } else if (arg == "-key") {
            keyfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                keyfile.append(argv[++i]);
                const char * fn = keyfile.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-key <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
	        //                fprintf(stderr, "option \"-key <fn>\" requires a path <fn> to a file for persistent key storage\n");
	        // exit(1);
                keyfile.erase();
                keyfile.append("0");
            }
        } else if (arg == "-pw") {
            setup_legacy_pairing = false;
            if (i < argc - 1 && *argv[i+1] != '-') {
                password.erase();
                password.append(argv[++i]);
                pin_pw = 2;
                if (password.size() < min_password_length) {
                    fprintf(stderr, "invalid client-access password \"%s\": length must be at least %u characters\n", password.c_str(), min_password_length);
                    exit(1);
                }
            } else {
                pin_pw = 3;  //a random password (pin) will be displayed at each connection
            }
        } else if (arg == "-dacp") {
            dacpfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                dacpfile.append(argv[++i]);
                const char *fn = dacpfile.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-dacp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                dacpfile.append(get_homedir());
                dacpfile.append("/.uxplay.dacp");
            }
        } else if (arg == "-taper") {
            taper_volume = true;
        } else if (arg == "-db") {
            bool db_bad = true;
            double db1, db2;
            if ( i < argc -1) {
                char *end1, *end2;
                db1 = strtod(argv[i+1], &end1);
                if (*end1 == ':') {
                    db2 = strtod(++end1, &end2);
                    if ( *end2 == '\0' && end2 > end1  && db1 < 0 && db1 < db2) {
                        db_bad = false;
                    }
                } else  if (*end1 =='\0' && db1 < 0 ) {
                    db_bad = false;
                    db2 = 0.0;
                }
            }
            if (db_bad) {
                fprintf(stderr, "invalid \"-db  %s\": db value must be \"low\" or \"low:high\", low < 0 and high > low are decibel gains\n", argv[i+1]); 
                exit(1);
            }
            i++;
            db_low = db1;
            db_high = db2;
            printf("db range %f:%f\n", db_low, db_high);
        } else if (arg ==  "-vol") {
            bool vol_bad = true;
            if (i < argc - 1) {
                char *end;
                double frac = strtod(argv[i+1], &end);
                if (*end == '\0' && frac >= 0.0 && frac <= 1.0) {
                    if (frac == 0.0) {
                        initial_volume = -144.0;
                    } else if (frac == 1.0) {
                        initial_volume = 0.0;
                    } else {
                        double db_flat = -30.0  + 30.0*frac;
                        //double db = 10.0 * (log10(frac) / log10(2.0));  //tapered 
                        //printf("db %f db_flat %f \n", db, db_flat);
                        //db = (db > db_flat) ? db : db_flat;
                        initial_volume = db_flat;
                    }
                }
                printf("initial_volume attenuation %f db\n", initial_volume);
                vol_bad = false;
            }
            if (vol_bad) {
                fprintf(stderr, "invalid \"-vol %s\", value must be between 0.0 (mute) and 1.0 (full volume)\n", argv[i+1]);
                exit(1);
            }
            i++;
        } else if (arg == "-hls") {
            hls_support = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 3;
                if (!get_value(argv[++i], &n) || playbin_version < 2) {
                    fprintf(stderr, "invalid \"-hls %s\"; -hls n only allows \"playbin\" video player versions 2 or 3\n", argv[i]);
                    exit(1);
                }
                playbin_version = (guint) n;
            }
        } else if (arg == "-lang") {
            lang.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                lang = argv[++i];
            }
        } else if (arg == "-h265") {
            h265_support = true;
        } else if (arg == "-nofreeze") {
            nofreeze = true;
        } else {
            fprintf(stderr, "unknown option %s, stopping (for help use option \"-h\")\n",argv[i]);
            exit(1);
        }
    }
}

static void process_metadata(int count, const char *dmap_tag, const unsigned char* metadata, int datalen, std::string *metadata_text) {
    int dmap_type = 0;
    /* DMAP metadata items can be strings (dmap_type = 9); other types are byte, short, int, long, date, and list.  *
     * The DMAP item begins with a 4-character (4-letter) "dmap_tag" string that identifies the type.               */

    if (debug_log) {
        printf("%d: dmap_tag [%s], %d\n", count, dmap_tag, datalen);
    }

    /* UTF-8 String-type DMAP tags seen in Apple Music Radio are processed here.   *
     * (DMAP tags "asal", "asar", "ascp", "asgn", "minm" ). TODO expand this */  
    
    if (datalen == 0) {
        return;
    }

    if (dmap_tag[0] == 'a' && dmap_tag[1] == 's') {
        dmap_type = 9;
        switch (dmap_tag[2]) {
        case 'a':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Album artist: ");  /*asaa*/
                break;
            case 'l':
                metadata_text->append("Album: ");  /*asal*/
                break;
            case 'r':
                metadata_text->append("Artist: ");  /*asar*/
                if (render_coverart) {
                    artist.erase();
                    artist.append(metadata, metadata + datalen);
                    if (coverart_artist == "_pending_") { 
                        coverart_artist = artist;
                    }
                    if (coverart_artist != "_expired_" && coverart_artist != artist) {
                        coverart_artist = "_expired_";
                        rtptime_coverart_expired = rtptime;
                    }
                }  
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;    
        case 'c':
            switch (dmap_tag[3]) {
            case 'm':
                metadata_text->append("Comment: ");  /*ascm*/
                break;
            case 'n':
                metadata_text->append("Content description: ");  /*ascn*/
                break;
            case 'p':
                metadata_text->append("Composer: ");  /*ascp*/
                break;
            case 't':
                metadata_text->append("Category: ");  /*asct*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        case 's':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Sort Artist: "); /*assa*/
                break;
            case 'c':
                metadata_text->append("Sort Composer: ");  /*assc*/
                break;
            case 'l':
                metadata_text->append("Sort Album artist: ");  /*assl*/
                break;
            case 'n':
                metadata_text->append("Sort Name: ");  /*assn*/
                break;
            case 's':
                metadata_text->append("Sort Series: ");  /*asss*/
                break;
            case 'u':
                metadata_text->append("Sort Album: ");  /*assu*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        default:
	    if (strcmp(dmap_tag, "asdt") == 0) {
                metadata_text->append("Description: ");
            } else if (strcmp (dmap_tag, "asfm") == 0) {
                metadata_text->append("Format: ");
            } else if (strcmp (dmap_tag, "asgn") == 0) {
                metadata_text->append("Genre: ");
            } else if (strcmp (dmap_tag, "asky") == 0) {
                metadata_text->append("Keywords: ");
            } else if (strcmp (dmap_tag, "aslc") == 0) {
                metadata_text->append("Long Content Description: ");
            } else {
                dmap_type = 0;
            }
            break;
        }
    } else if (strcmp (dmap_tag, "minm") == 0) {
        dmap_type = 9;
        metadata_text->append("Title: ");
    }

    if (dmap_type == 9) {
        char *str = (char *) calloc(datalen + 1, sizeof(char));
        if (!str) {
            printf("Memeory allocation failure (str)\n");
            exit(1);
        }
        memcpy(str, metadata, datalen);
        metadata_text->append(str);
        metadata_text->append("\n");
        free(str);
    } else if (debug_log) {
        std::string md = "";
        char hex[4];
        for (int i = 0; i < datalen; i++) {
            if (i > 0 && i % 16 == 0) {
                md.append("\n");
            }
            snprintf(hex, 4, "%2.2x ", (int) metadata[i]);
            md.append(hex);
        }
        LOGI("%s", md.c_str());
    }
}

static int parse_dmap_header(const unsigned char *metadata, char *tag, int *len) {
    const unsigned char *header = metadata;

    bool istag = true;
    for (int i = 0; i < 4; i++) {
        tag[i] =  (char) *header;
        if (!isalpha(tag[i])) {
            istag = false;
        }
        header++;
    }

    *len = 0;
    for (int i = 0; i < 4; i++) {
        *len <<= 8;
        *len += (int) *header;
        header++;
    }
    if (!istag || *len < 0) {
        return 1;
    }
    return 0;
}

static int register_dnssd() {
    int dnssd_error;
    uint64_t features;
    
    dnssd_error = dnssd_register_raop(dnssd, raop_port);
    if (dnssd_error) {
        if (ble_filename.empty()) {
            if (dnssd_error == -65537) {
                LOGE("No DNS-SD Server found (DNSServiceRegister call returned kDNSServiceErr_Unknown)");
            } else if (dnssd_error == -65548) {
                LOGE("DNSServiceRegister call returned kDNSServiceErr_NameConflict");
                LOGI("Is another instance of %s running with the same DeviceID (MAC address) or using same network ports?",
                     DEFAULT_NAME);
                LOGI("Use options -m ... and -p ... to allow multiple instances of %s to run concurrently", DEFAULT_NAME); 
            } else {
                LOGE("dnssd_register_raop failed with error code %d\n"
                     "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
                     "(see Apple's dns_sd.h)", dnssd_error);
            }
            return -3;
        } else {
            LOGI("dnssd_register_raop failed: ignoring because Bluetooth LE service discovery may be available");
        }
    }

    dnssd_error = dnssd_register_airplay(dnssd, airplay_port);
    if (dnssd_error) {
        if (ble_filename.empty()) {
            LOGE("dnssd_register_airplay failed with error code %d\n"
                 "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
                 "(see Apple's dns_sd.h)", dnssd_error);
            return -4;
        } else {
            LOGI("dnssd_register_airplay failed: ignoring because Bluetooth LE service discovery may be available");   
        }
    }

    LOGD("register_dnssd: advertised AirPlay service with \"Features\" code = 0x%llX",
         dnssd_get_airplay_features(dnssd));
    return 0;
}

static void unregister_dnssd() {
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    return;
}

static void stop_dnssd() {
    if (dnssd) {
        unregister_dnssd();
        dnssd_destroy(dnssd);
        dnssd = NULL;
	return;
    }	
}

static int start_dnssd(std::vector<char> hw_addr, std::string name) {
    int dnssd_error;
    if (dnssd) {
        LOGE("start_dnssd error: dnssd != NULL");
        return 2;
    }
    /* pin_pw controls client access
      pin_pw  = 1: client must enter pin displayed onscreen (first access only)
              = 2: client must enter password (same password for all clients)
              = 3: client must enter randoe 4-digit password displayed like an  onscreen pin (every access)
              = 0:  no access control
    */
    dnssd = dnssd_init(name.c_str(), strlen(name.c_str()), hw_addr.data(), hw_addr.size(), &dnssd_error, pin_pw);
    if (dnssd_error) {
        LOGE("Could not initialize dnssd library!: error %d", dnssd_error);
        return 1;
    }

    /* after dnssd starts, reset the default feature set here 
     * (overwrites features set in dnssdint.h)
     * default: FEATURES_1 = 0x5A7FFEE6, FEATURES_2 = 0 */

    dnssd_set_airplay_features(dnssd,  0, 0); // AirPlay video supported 
    dnssd_set_airplay_features(dnssd,  1, 1); // photo supported 
    dnssd_set_airplay_features(dnssd,  2, 1); // video protected with FairPlay DRM 
    dnssd_set_airplay_features(dnssd,  3, 0); // volume control supported for videos

    dnssd_set_airplay_features(dnssd,  4, 0); // http live streaming (HLS) supported
    dnssd_set_airplay_features(dnssd,  5, 1); // slideshow supported 
    dnssd_set_airplay_features(dnssd,  6, 1); // 
    dnssd_set_airplay_features(dnssd,  7, 1); // mirroring supported

    dnssd_set_airplay_features(dnssd,  8, 0); // screen rotation  supported 
    dnssd_set_airplay_features(dnssd,  9, 1); // audio supported 
    dnssd_set_airplay_features(dnssd, 10, 1); //  
    dnssd_set_airplay_features(dnssd, 11, 1); // audio packet redundancy supported

    dnssd_set_airplay_features(dnssd, 12, 1); // FaiPlay secure auth supported 
    dnssd_set_airplay_features(dnssd, 13, 1); // photo preloading  supported 
    dnssd_set_airplay_features(dnssd, 14, 1); // Authentication bit 4:  FairPlay authentication
    dnssd_set_airplay_features(dnssd, 15, 1); // Metadata bit 1 support:   Artwork 

    dnssd_set_airplay_features(dnssd, 16, 1); // Metadata bit 2 support:  Soundtrack  Progress 
    dnssd_set_airplay_features(dnssd, 17, 1); // Metadata bit 0 support:  Text (DAACP) "Now Playing" info.
    dnssd_set_airplay_features(dnssd, 18, 1); // Audio format 1 support:   
    dnssd_set_airplay_features(dnssd, 19, 1); // Audio format 2 support: must be set for AirPlay 2 multiroom audio 

    dnssd_set_airplay_features(dnssd, 20, 1); // Audio format 3 support: must be set for AirPlay 2 multiroom audio 
    dnssd_set_airplay_features(dnssd, 21, 1); // Audio format 4 support:
    dnssd_set_airplay_features(dnssd, 22, 1); // Authentication type 4: FairPlay authentication
    dnssd_set_airplay_features(dnssd, 23, 0); // Authentication type 1: RSA Authentication

    dnssd_set_airplay_features(dnssd, 24, 0); // 
    dnssd_set_airplay_features(dnssd, 25, 1); // 
    dnssd_set_airplay_features(dnssd, 26, 0); // Has Unified Advertiser info
    dnssd_set_airplay_features(dnssd, 27, 1); // Supports Legacy Pairing

    dnssd_set_airplay_features(dnssd, 28, 1); //  
    dnssd_set_airplay_features(dnssd, 29, 0); // 
    dnssd_set_airplay_features(dnssd, 30, 1); // RAOP support: with this bit set, the AirTunes service is not required. 
    dnssd_set_airplay_features(dnssd, 31, 0); // 


    /*  bits 32-63: see  https://emanualcozzi.net/docs/airplay2/features 
    dnssd_set_airplay_features(dnssd, 32, 0); // isCarPlay when ON,; Supports InitialVolume when OFF
    dnssd_set_airplay_features(dnssd, 33, 0); // Supports Air Play Video Play Queue
    dnssd_set_airplay_features(dnssd, 34, 0); // Supports Air Play from cloud (requires that bit 6 is ON)
    dnssd_set_airplay_features(dnssd, 35, 0); // Supports TLS_PSK

    dnssd_set_airplay_features(dnssd, 36, 0); //
    dnssd_set_airplay_features(dnssd, 37, 0); //
    dnssd_set_airplay_features(dnssd, 38, 0); //  Supports Unified Media Control (CoreUtils Pairing and Encryption)
    dnssd_set_airplay_features(dnssd, 39, 0); //

    dnssd_set_airplay_features(dnssd, 40, 0); // Supports Buffered Audio
    dnssd_set_airplay_features(dnssd, 41, 0); // Supports PTP
    dnssd_set_airplay_features(dnssd, 42, 0); // Supports Screen Multi Codec (allows h265 video)
    dnssd_set_airplay_features(dnssd, 43, 0); // Supports System Pairing

    dnssd_set_airplay_features(dnssd, 44, 0); // is AP Valeria Screen Sender
    dnssd_set_airplay_features(dnssd, 45, 0); //
    dnssd_set_airplay_features(dnssd, 46, 0); // Supports HomeKit Pairing and Access Control
    dnssd_set_airplay_features(dnssd, 47, 0); //

    dnssd_set_airplay_features(dnssd, 48, 0); // Supports CoreUtils Pairing and Encryption
    dnssd_set_airplay_features(dnssd, 49, 0); //
    dnssd_set_airplay_features(dnssd, 50, 0); // Metadata bit 3: "Now Playing" info sent by bplist not DAACP test
    dnssd_set_airplay_features(dnssd, 51, 0); // Supports Unified Pair Setup and MFi Authentication

    dnssd_set_airplay_features(dnssd, 52, 0); // Supports Set Peers Extended Message
    dnssd_set_airplay_features(dnssd, 53, 0); //
    dnssd_set_airplay_features(dnssd, 54, 0); // Supports AP Sync
    dnssd_set_airplay_features(dnssd, 55, 0); // Supports WoL

    dnssd_set_airplay_features(dnssd, 56, 0); // Supports Wol
    dnssd_set_airplay_features(dnssd, 57, 0); //
    dnssd_set_airplay_features(dnssd, 58, 0); // Supports Hangdog Remote Control
    dnssd_set_airplay_features(dnssd, 59, 0); // Supports AudioStreamConnection setup

    dnssd_set_airplay_features(dnssd, 60, 0); // Supports Audo Media Data Control         
    dnssd_set_airplay_features(dnssd, 61, 0); // Supports RFC2198 redundancy
    */

    /* needed for HLS video support */
    dnssd_set_airplay_features(dnssd, 0, (int) hls_support);
    dnssd_set_airplay_features(dnssd, 4, (int) hls_support);
    // not sure about this one (bit 8, screen rotation supported):
    //dnssd_set_airplay_features(dnssd, 8, (int) hls_support);
    
    /* needed for h265 video support */
    dnssd_set_airplay_features(dnssd, 42, (int) h265_support);

    /* bit 27 of Features determines whether the AirPlay2 client-pairing protocol will be used (1) or not (0) */
    dnssd_set_airplay_features(dnssd, 27, (int) setup_legacy_pairing);
    return 0;
}

static bool check_client(char *deviceid) {
    bool ret = false;
    int list =  allowed_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,allowed_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

static bool check_blocked_client(char *deviceid) {
    bool ret = false;
    int list =  blocked_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,blocked_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

// Server callbacks

extern "C" void video_reset(void *cls, reset_type_t type) {
    LOGD("video_reset");
    if (use_video) {
        video_renderer_stop();
    }
    if (hls_support && (type == RESET_TYPE_HLS_SHUTDOWN || type == RESET_TYPE_NOHOLD)) { 
        url.erase();
        raop_destroy_airplay_video(raop, -1);
    }
    if (type == RESET_TYPE_HLS_SHUTDOWN) {
        raop_remove_hls_connections(raop);
        preserve_connections = true;
    }
    if (use_video && (type == RESET_TYPE_NOHOLD || type == RESET_TYPE_HLS_EOS)) {
        /* reset the video renderer immediately to avoid a timing issue if we wait for main_loop to reset */ 
        video_renderer_destroy();
        video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(), rtp_pipeline.c_str(),
                            video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                            videosink_options.c_str(), fullscreen, video_sync, h265_support,
                            render_coverart, playbin_version, NULL);
        video_renderer_start();
        close_window = false;  // we already closed the window
    }
    if (type == RESET_TYPE_NOHOLD) {
        preserve_connections = false; //we already closed all other connections
    }
    
    remote_clock_offset = 0;
    relaunch_video = true;
    reset_loop = true;
}

extern "C" int video_set_codec(void *cls, video_codec_t codec) {
    bool video_is_h265 = (codec == VIDEO_CODEC_H265);
    if (mux_to_file) {
        mux_renderer_choose_video_codec(video_is_h265);
    }
    if (!use_video) {
        return 0;
    }
    return video_renderer_choose_codec(false, video_is_h265);
}

extern "C" void display_pin(void *cls, char *pin) {
    int margin = 10;
    int spacing = 3;
    char *image = create_pin_display(pin, margin, spacing);
    if (!image) {
        LOGE("create_pin_display could not create pin image, pin = %s", pin);
    } else {
        LOGI("%s\n",image);     
        free (image);
    }
}

extern "C" const char *passwd(void *cls, int *len){
    if (pin_pw == 2) {
        *len = password.size();
        return password.c_str();
    } else if  (pin_pw == 3) {
        *len = -1;
    } else {
        *len = 0;   /* no password used */
    }
    return NULL;
}

extern "C" void export_dacp(void *cls, const char *active_remote, const char *dacp_id) {
      if (dacpfile.length()) {
        FILE *fp = fopen(dacpfile.c_str(), "w");
        if (fp) {
            fprintf(fp,"%s\n%s\n", dacp_id, active_remote);
            fclose(fp);
        } else {
            LOGE("failed to open DACP export file \"%s\"", dacpfile.c_str());
        }
    }
}

extern "C" void conn_init (void *cls) {
    open_connections++;
    LOGD("Open connections: %i", open_connections);
    //video_renderer_update_background(1);
}

extern "C" void conn_destroy (void *cls) {
    //video_renderer_update_background(-1);
    open_connections--;
    LOGD("Open connections: %i", open_connections);
    if (open_connections == 0) {
        remote_clock_offset = 0;
        if (use_audio) {
            audio_renderer_stop();
        }
        if (dacpfile.length()) {
            remove (dacpfile.c_str());
        }
        if (mux_to_file) {
            mux_renderer_stop();
        }
    }
}

extern "C" void conn_feedback (void *cls) {
    /* received client heartbeat signal: connection still exists */
    missed_feedback = 0;
}

extern "C" void conn_reset (void *cls, int reason) {
    switch (reason) {
    case 1:
        LOGI("*** ERROR lost connection with client (network problem?)");
	break;
    case 2:
        LOGI("*** ERROR Unsupported HLS streaming source: (exit attempt to stream)");
	break;      
    default:
      break;
    }
    
    if (!nofreeze) {
        close_window = false;    /* leave "frozen" window open */
    }
    reset_httpd = true;
    relaunch_video = true;
    reset_loop = true;
}

extern "C" void conn_teardown(void *cls, bool *teardown_96, bool *teardown_110) {
    if (*teardown_110 && close_window) {
        relaunch_video = true;
        reset_loop = true;
    }
}

extern "C" void report_client_request(void *cls, char *deviceid, char * model, char *name, bool * admit) {
    LOGI("connection request from %s (%s) with deviceID = %s\n", name, model, deviceid);
    if (restrict_clients) {
        *admit = check_client(deviceid);
        if (*admit == false) {
            LOGI("client connections have been restricted to those with listed deviceID,\nuse \"-allow %s\" to allow this client to connect.\n",
                 deviceid);
        }
    } else {
        *admit = true;
    }
    if (check_blocked_client(deviceid)) {
        *admit = false;
        LOGI("*** attempt to connect by blocked client (clientID %s): DENIED\n", deviceid);
    }
}

extern "C" void audio_process (void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (dump_audio) {
        dump_audio_to_file(data->data, data->data_len, (data->data)[0] & 0xf0);
    }
    if (mux_to_file) {
        mux_renderer_push_audio(data->data, data->data_len, data->ntp_time_remote);
    }
    if (use_audio) {
        if (!remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            remote_clock_offset = local_time - data->ntp_time_remote;
        }
        data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
        switch (data->ct) {
        case 2:
            /* for progress monitor (ALAC audio only) */
            rtptime = data->rtp_time;
            if (audio_delay_alac) {
                data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote + audio_delay_alac);
            }
            break;
        case 4:
        case 8:
            monitor_progress =  false;
            if (audio_delay_aac) {
                data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote + audio_delay_aac);
            }
            break;
        default:
            break;
        }
        audio_renderer_render_buffer(data->data, &(data->data_len), &(data->seqnum), &(data->ntp_time_remote));
    }
}

extern "C" void video_process (void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    if (dump_video) {
        dump_video_to_file(data->data, data->data_len);
    }
    if (mux_to_file) {
        mux_renderer_push_video(data->data, data->data_len, data->ntp_time_remote);
    }
    if (use_video) {
        if (!remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            remote_clock_offset = local_time - data->ntp_time_remote;
        }
        int count = 0;
        uint64_t pts_mismatch = 0;
        do {
            data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
            pts_mismatch = video_renderer_render_buffer(data->data, &(data->data_len), &(data->nal_count), &(data->ntp_time_remote));
            if (pts_mismatch) {
                LOGI("adjust timestamps by %8.6f secs", (double) pts_mismatch / SECOND_IN_NSECS);
                remote_clock_offset += pts_mismatch;
            }
            count++;
        } while (pts_mismatch && count < 10);
    }
}

#ifdef DBUS
extern "C" void mirror_video_activity  (void *cls, double *txusage) {
    if (scrsv != 1) {
        return;
    }
    if (*txusage > activity_threshold) {
        if (activity_count < MAX_ACTIVITY_COUNT) {
            activity_count++;
        } else if (activity_count == MAX_ACTIVITY_COUNT  && !dbus_last_message) {
	    dbus_screensaver_inhibiter(true);
        }
    } else {
      if (activity_count > 0) {
          activity_count--;
      } else if (activity_count == 0 && dbus_last_message) {
          dbus_screensaver_inhibiter(false);
      }
    }
}
#endif

extern "C" void video_pause (void *cls) {
    if (use_video) {
        video_renderer_pause();
    }
}

extern "C" void video_resume (void *cls) {
    if (use_video) {
        video_renderer_resume();
    }
}


extern "C" void audio_flush (void *cls) {
    if (use_audio) {
        audio_renderer_flush();
    }
}

extern "C" void video_flush (void *cls) {
    if (use_video) {
        video_renderer_flush();
    }
}

extern "C" double audio_set_client_volume(void *cls) {
    return initial_volume;
}

extern "C" void audio_set_volume (void *cls, float volume) {
    double db, db_flat, frac, gst_volume;
    if (!use_audio) {
      return;
    }
    /* convert from AirPlay dB  volume in range {-30dB : 0dB}, to GStreamer volume */
    if (volume == -144.0f) {   /* AirPlay "mute" signal */
        frac = 0.0;
    } else if (volume < -30.0f) {
        LOGE(" invalid AirPlay volume %f", volume);
        frac = 0.0;
    } else if (volume > 0.0f) {
        LOGE(" invalid AirPlay volume %f", volume);
        frac = 1.0;
    } else if (volume == -30.0f) {
        frac = 0.0;
    } else if (volume == 0.0f) {
        frac = 1.0;
    } else {
        frac = (double) ( (30.0f + volume) / 30.0f);
        frac = (frac > 1.0) ? 1.0 : frac;
    }

    /* frac is length of volume slider as fraction of max length */
    /* also (steps/16) where steps is number of discrete steps above mute (16 = full volume) */
    if (frac == 0.0) {
        gst_volume = 0.0;
    } else {
      /* flat rescaling of decibel range from {-30dB : 0dB} to {db_low : db_high} */  
        db_flat = db_low + (db_high-db_low) * frac;
        if (taper_volume) {
            /* taper the volume reduction by the (rescaled) Airplay {-30:0} range so each reduction of
             * the remaining slider length by 50% reduces the perceived volume by 50% (-10dB gain)
             * (This is the "dasl-tapering" scheme offered by shairport-sync) */
            db = db_high + 10.0 * (log10(frac) / log10(2.0));
            db = (db  > db_flat) ? db : db_flat;
        } else {
            db = db_flat;
        }
        /* conversion from (gain) decibels to GStreamer's linear volume scale */
        gst_volume = pow(10.0, 0.05*db);
    }
    audio_renderer_set_volume(gst_volume);
}

extern "C" void audio_get_format (void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    unsigned char type;
    LOGI("ct=%d spf=%d usingScreen=%d isMedia=%d  audioFormat=0x%lx",*ct, *spf, *usingScreen, *isMedia, (unsigned long) *audioFormat);
    switch (*ct) {
    case 2:
        type = 0x20;
        break;
    case 8:
        type = 0x80;
        break;
    default:
        type = 0x10;
        break;
    }
    if (audio_dumpfile && type != audio_type) {
        fclose(audio_dumpfile);
        audio_dumpfile = NULL;
    }
    audio_type = type;
    
    if (use_audio) {
      audio_renderer_start(ct);
    }

    if (mux_to_file) {
        mux_renderer_choose_audio_codec(*ct);
    }

    if (coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), "no data\n");
    }
}

extern "C" void video_report_size(void *cls, float *width_source, float *height_source, float *width, float *height) {
    if (use_video) {
        video_renderer_size(width_source, height_source, width, height);
    }
}

extern "C" void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (buffer && coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), buffer, buflen);
        LOGI("coverart size %d written to %s", buflen,  coverart_filename.c_str());
    } else if (buffer && render_coverart) {
        video_renderer_choose_codec(true, false);  /* video_is_jpeg = true */
        video_renderer_display_jpeg(buffer, &buflen);
        coverart_artist = "_pending_";
    }
}

extern "C" void audio_stop_coverart_rendering(void *cls) {
    if (render_coverart) {
        video_reset(cls, RESET_TYPE_RTP_SHUTDOWN);
    }
}

extern "C" void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    rtptime_start = *start;
    rtptime = *curr;
    rtptime_end = *end;
    display_progress(rtptime_start, rtptime, rtptime_end);
}

extern "C" void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    char dmap_tag[5] = {0x0};
    const unsigned char *metadata = (const  unsigned char *) buffer;
    int datalen;
    int count = 0;

    printf("====================Audio Metadata==================\n");

    if (buflen < 8) {
        LOGE("received invalid metadata, length %d < 8", buflen);
        return;
    } else if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
        LOGE("received invalid metadata, tag [%s]  datalen %d", dmap_tag, datalen);
        return;
    }
    metadata += 8;
    buflen -= 8;

    if (strcmp(dmap_tag, "mlit") != 0 || datalen != buflen) {
        LOGE("received metadata with tag %s, but is not a DMAP listingitem, or datalen = %d !=  buflen %d",
             dmap_tag, datalen, buflen);
        return;
    }
    std::string metadata_text = "";
    while (buflen >= 8) {
        count++;
        if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
            LOGE("received metadata with invalid DMAP header:  tag = [%s],  datalen = %d", dmap_tag, datalen);
            return;
        }
        metadata += 8;
        buflen -= 8;
        process_metadata(count, (const char *) dmap_tag, metadata, datalen, &metadata_text);
        metadata += datalen;
        buflen -= datalen;
    }
    LOGI("%s", metadata_text.c_str());
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), metadata_text.c_str());
    }
    if (buflen != 0) {
        LOGE("%d bytes of metadata were not processed", buflen);
    }
}

extern "C" void register_client(void *cls, const char *device_id, const char *client_pk, const char *client_name) {
    if (!registration_list) {
      /* we are not maintaining a list of registered clients */
        return;
    }
    LOGI("registered new client: %s DeviceID = %s PK = \n%s", client_name, device_id, client_pk);
    registered_keys.push_back(client_pk);
    if (strlen(pairing_register.c_str())) {
        FILE *fp = fopen(pairing_register.c_str(), "a");
        if (fp) {
            fprintf(fp, "%s,%s,%s\n", client_pk, device_id, client_name);
            fclose(fp);
        }
    }
}

extern "C" bool check_register(void *cls, const char *client_pk) {
    if (!registration_list) {
        /* we are not maintaining a list of registered clients */
        return true;
    }
    LOGD("check returning client's pairing registration");
    std::string pk = client_pk;
    if (std::find(registered_keys.rbegin(), registered_keys.rend(), pk) != registered_keys.rend()) {
        LOGD("registration found: PK=%s", client_pk);
        return true;
    } else {
        LOGE("returning client's pairing registration not found: PK=%s", client_pk);
        return false;
    }
}
/* control  callbacks for video player (unimplemented) */

extern "C" void on_video_play(void *cls, const char* location, const float start_position) {
    /* start_position needs to be implemented */
    video_renderer_set_start(start_position);
    url.erase();
    url.append(location);
    relaunch_video = true;
    preserve_connections = true;
    LOGI("********************on_video_play: location = %s*** start position %f ********************", url.c_str(), start_position);
    reset_loop = true;
}

extern "C" void on_video_scrub(void *cls, const float position) {
    LOGI("on_video_scrub: position = %7.5f\n", position);
    video_renderer_seek(position);
}

extern "C" void on_video_rate(void *cls, const float rate) {
    LOGI("on_video_rate = %7.5f\n", rate);
    if (rate == 1.0f) {
        video_renderer_resume();
    } else if (rate ==  0.0f) {
        video_renderer_pause();
    } else  {
        LOGI("on_video_rate: ignoring unexpected value rate = %f\n", rate);
    }
}



extern "C" float on_video_playlist_remove (void *cls) {
    double duration, position, seek_start, seek_end;
    float rate;
    bool buffer_empty, buffer_full;
    LOGI("************************* on_video_playlist_remove\n");
    video_renderer_pause();
    video_get_playback_info(&duration, &position, &seek_start, &seek_end, &rate, &buffer_empty, &buffer_full);
    return (float) position;
}

 extern "C" void on_video_stop(void *cls) {
    LOGI("**************************on_video_stop\n");
    video_renderer_hls_ready();
 }

extern "C" void on_video_acquire_playback_info (void *cls, playback_info_t *playback_info) {
    int buffering_level;
    bool still_playing = video_get_playback_info(&playback_info->duration, &playback_info->position,
                                                 &playback_info->seek_start, &playback_info->seek_duration,
                                                 &playback_info->rate,
                                                 &playback_info->playback_buffer_empty,
                                                 &playback_info->playback_buffer_full);
    playback_info->ready_to_play = true; //?
    playback_info->playback_likely_to_keep_up = true; //?
    
#ifdef DBUS
    /*  this seems to be  called every second for first 900 secs (15 mins?) of HLS video, and subsequently
	at 30 second intervals (use it to signal HLS video activity to  the  DBus screensaver inhibitor) */
    if (scrsv == 1) {
        if (playback_info->position > previous_hls_position && !dbus_last_message) {
            dbus_screensaver_inhibiter(true);
        } else if (playback_info->position == previous_hls_position && dbus_last_message) {
            dbus_screensaver_inhibiter(false);
        }
        previous_hls_position = playback_info->position;
    }
#endif
    
    if (!still_playing) {
        LOGI(" video has finished, %f", playback_info->position);
        playback_info->position = -1.0;
        playback_info->duration = -1.0;
        video_renderer_stop();
    }
}

extern "C" void log_callback (void *cls, int level, const char *msg) {
    switch (level) {
    case LOGGER_DEBUG:
        LOGD("%s", msg);
        break;
    case LOGGER_WARNING:
        LOGW("%s", msg);
        break;
    case LOGGER_INFO:
        LOGI("%s", msg);
        break;
    case LOGGER_ERR:
        LOGE("%s", msg);
        break;
    default:
        break;
    }
}

static int start_raop_server (unsigned short display[5], unsigned short tcp[3], unsigned short udp[3], bool debug_log) {
    raop_callbacks_t raop_cbs;
    memset(&raop_cbs, 0, sizeof(raop_cbs));
    raop_cbs.conn_init = conn_init;
    raop_cbs.conn_destroy = conn_destroy;
    raop_cbs.conn_reset = conn_reset;
    raop_cbs.conn_feedback = conn_feedback;
    raop_cbs.conn_teardown = conn_teardown;
    raop_cbs.audio_process = audio_process;
    raop_cbs.video_process = video_process;
    raop_cbs.audio_flush = audio_flush;
    raop_cbs.video_flush = video_flush;
    raop_cbs.video_pause = video_pause;
    raop_cbs.video_resume = video_resume;
    raop_cbs.audio_set_client_volume = audio_set_client_volume;
    raop_cbs.audio_set_volume = audio_set_volume;
    raop_cbs.audio_get_format = audio_get_format;
    raop_cbs.video_report_size = video_report_size;
    raop_cbs.audio_set_metadata = audio_set_metadata;
    raop_cbs.audio_set_coverart = audio_set_coverart;
    raop_cbs.audio_stop_coverart_rendering = audio_stop_coverart_rendering;
    raop_cbs.audio_set_progress = audio_set_progress;
    raop_cbs.report_client_request = report_client_request;
    raop_cbs.display_pin = display_pin;
    raop_cbs.register_client = register_client;
    raop_cbs.check_register = check_register;
    raop_cbs.passwd = passwd;
    raop_cbs.export_dacp = export_dacp;
    raop_cbs.video_reset = video_reset;
    raop_cbs.video_set_codec = video_set_codec;
#ifdef DBUS
    raop_cbs.mirror_video_activity = mirror_video_activity;
#endif
    raop_cbs.on_video_play = on_video_play;
    raop_cbs.on_video_scrub = on_video_scrub;
    raop_cbs.on_video_rate = on_video_rate;
    raop_cbs.on_video_stop = on_video_stop;
    raop_cbs.on_video_playlist_remove = on_video_playlist_remove;
    raop_cbs.on_video_acquire_playback_info = on_video_acquire_playback_info;

    raop = raop_init(&raop_cbs);
    if (raop == NULL) {
        LOGE("Error initializing raop!");
        return -1;
    }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, log_level);
    /* set nohold = 1 to allow  capture by new client */
    if (raop_init2(raop, nohold, mac_address.c_str(), keyfile.c_str())){
        LOGE("Error initializing raop (2)!");
        free (raop);
        return -1;
    }

    /* write desired display pixel width, pixel height, refresh_rate, max_fps, overscanned.  */
    /* use 0 for default values 1920,1080,60,30,0; these are sent to the Airplay client      */

    if (display[0]) raop_set_plist(raop, "width", (int) display[0]);
    if (display[1]) raop_set_plist(raop, "height", (int) display[1]);
    if (display[2]) raop_set_plist(raop, "refreshRate", (int) display[2]);
    if (display[3]) raop_set_plist(raop, "maxFPS", (int) display[3]);
    if (display[4]) raop_set_plist(raop, "overscanned", (int) display[4]);

    if (show_client_FPS_data) raop_set_plist(raop, "clientFPSdata", 1);
    if (audiodelay >= 0) raop_set_plist(raop, "audio_delay_micros", audiodelay);
    if (pin_pw == 1) raop_set_plist(raop, "pin", (int) pin);
    if (hls_support) raop_set_plist(raop, "hls", 1);

    /* network port selection (ports listed as "0" will be dynamically assigned) */
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    raop_port = raop_get_port(raop);
    raop_start_httpd(raop, &raop_port);
    raop_set_port(raop, raop_port);

    /* use raop_port for airplay_port (instead of tcp[2]) */
    airplay_port = raop_port;

    if (dnssd) {
        raop_set_dnssd(raop, dnssd);
    } else {
        LOGE("raop_set failed to set dnssd");
        return -2;
    }
    return 0;
}

static void stop_raop_server () {
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
    return;
}

static void read_config_file(const char * filename, const char * uxplay_name) {
    std::string config_file = filename;
    std::string option_char = "-";
    std::vector<std::string> options;
    options.push_back(uxplay_name);
    std::ifstream file(config_file);
    if (file.is_open()) {
        fprintf(stdout,"UxPlay: reading configuration from  %s\n", config_file.c_str());
        std::string line;
        while (std::getline(file, line)) {
            if (line[0] == '#') continue;
            //  first process line into separate option items with '\0' as delimiter
            bool is_part_of_item, in_quotes;
            char endchar;
            is_part_of_item = false;
            for (int i = 0; i < (int) line.size(); i++) {
                if (is_part_of_item == false) {
                    if (line[i] == ' ') {
                        line[i] = '\0';
                    } else {
                        // start of new item
                        is_part_of_item = true;
                        switch (line[i]) {
                        case '\'':
                        case '\"':
                            endchar = line[i];
                            line[i] = '\0';
                            in_quotes = true;
                            break;
                        default:
                            in_quotes = false;
		            endchar = ' ';
		            break;
                        }
                    }
                } else {
                    /* previous character was inside this item */
                    if (line[i] == endchar) {
                        if (in_quotes) {
                            /* cases where endchar is inside quoted item */
                            if (i > 0 && line[i - 1] == '\\') continue;
                            if (i + 1 < (int) line.size() && line[i + 1] != ' ') continue;
		        }
                        line[i] =  '\0';
                        is_part_of_item = false;
                    }
                }
            }

            // now tokenize the processed line   
            std::istringstream iss(line);
            std::string token;
            bool first = true;
            while (std::getline(iss, token, '\0')) {
                if (token.size() > 0) {
                    if (first) {
                        options.push_back(option_char + token.c_str());
                        first = false;
                    } else {
                        options.push_back(token.c_str());
                    }
                }
	    }
	}
        file.close();
    } else {
        fprintf(stderr,"UxPlay: failed to open configuration file at %s\n", config_file.c_str());
    }
    if (options.size() > 1) {

        int argc = options.size();
        char **argv = (char **) malloc(sizeof(char*) * argc);
        if (argv == NULL) {
            printf("Memory allocation failure (argV)\n");
            exit(1);
        }
        for (int i = 0; i < argc; i++) {
            argv[i] = (char *) options[i].c_str();
        }
        parse_arguments (argc, argv);
        free (argv);
    }
}

#ifdef GST_MACOS
/* workaround for GStreamer >= 1.22 "Official Builds" on macOS */
#include <TargetConditionals.h>
#include <gst/gstmacos.h>
void real_main (int argc, char *argv[]);

int main (int argc, char *argv[]) {
    LOGI("*=== Using gst_macos_main wrapper for GStreamer >= 1.22 on macOS ===*");
    return  gst_macos_main ((GstMainFunc) real_main, argc, argv , NULL);
}

void real_main (int argc, char *argv[]) {
#else
int main (int argc, char *argv[]) {
#endif
    std::vector<char> server_hw_addr;
    std::string config_file = "";

#ifdef _WIN32
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        LOGE("Could not set control handler");
        exit(1);
    }
#else
    signal(SIGINT, CtrlHandler);
    signal(SIGTERM, CtrlHandler);
    signal(SIGHUP, CtrlHandler);
#endif

#ifdef __OpenBSD__
    if (unveil("/", "rwc") == -1 || unveil(NULL, NULL) == -1) {
        err(1, "unveil");
    }
#endif

#ifdef SUPPRESS_AVAHI_COMPAT_WARNING
    // suppress avahi_compat nag message.  avahi emits a "nag" warning (once)
    // if  getenv("AVAHI_COMPAT_NOWARN") returns null.
    static char avahi_compat_nowarn[] = "AVAHI_COMPAT_NOWARN=1";
    if (!getenv("AVAHI_COMPAT_NOWARN")) putenv(avahi_compat_nowarn);
#endif

    /* for HLS video language preferences */
    char *lang_env = getenv("LANGUAGE");
    if (lang_env && strlen(lang_env)) {
        lang.erase();
        lang = lang_env;
    }
    
    char *rcfile = NULL;
    /* see if option -rc was given */
    for (int i = 1; i < argc ; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            struct stat sb;
            if (i+1 == argc) {
                LOGE ("option -rc requires a filename  (-rc <filename>)");
                exit(1);
            }
            rcfile = argv[i+1];
            if (stat(rcfile, &sb) == -1) {
                LOGE("startup file %s specified by option -rc was not found", rcfile);
                exit(0);
            }
            break;
        }
    }
    if (rcfile) {
        config_file = rcfile;
    } else {	
        config_file = find_uxplay_config_file();
    }
    if (config_file.length()) {
        read_config_file(config_file.c_str(), argv[0]);
    }
    parse_arguments (argc, argv);

    log_level = (debug_log ? LOGGER_DEBUG_DATA : LOGGER_INFO);
    if (debug_log && suppress_packet_debug_data) {
        log_level = LOGGER_DEBUG;
    }

    
#ifdef _WIN32    /*  use utf-8 terminal output; don't buffer stdout in WIN32 when debug_log = false */
    SetConsoleOutputCP(CP_UTF8);
    if (!debug_log) {
        setbuf(stdout, NULL);
    }
#endif

    LOGI("UxPlay %s: An Open-Source AirPlay mirroring and audio-streaming server.", VERSION);

#ifdef DBUS
    if (scrsv) {
        DBusError dbus_error;
        dbus_error_init(&dbus_error);
        dbus_connection = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error);
        if (dbus_error_is_set(&dbus_error)) {
            dbus_error_free(&dbus_error);
            scrsv = 0;
            LOGI ("D-Bus session not found: screensaver inhibition option (\"-scrsv\") will not be active");
        }
    }
    if (scrsv) {
        LOGD ("D-Bus session support is available, connection %p", dbus_connection);
        std::string desktop = getenv("XDG_CURRENT_DESKTOP"); 
        LOGD("Desktop Environment:  %s", desktop.c_str());

        /* if dbus_service, dbus_path, dbus_interface, dbus_inhibit, dbus_uninhibit *
         * in the detected  Desktop Environments are still non-conforming to the    *
         * org.freedesktop.ScreenSaver interface, they can be modifed here          */

        /* some desktop environments (e.g. Xfce 4, Mate) modify the D-Bus service name */
        std::string name;
        if (strstr(desktop.c_str(), "XFCE")) {
            name = "xfce";
        } else if (strstr(desktop.c_str(), "MATE")) {
            name = "mate";
        }
  
        if (!name.empty()) {
            size_t pos;
            std::string replace_word = "freedesktop";
            pos = dbus_service.find(replace_word);
            dbus_service.replace(pos, replace_word.size(), name);
            pos = dbus_path.find(replace_word);
            dbus_path.replace(pos, replace_word.size(), name);
            pos = dbus_interface.find(replace_word);
            dbus_interface.replace(pos, replace_word.size(), name);
        }

        LOGI("Will attempt to use %s (D-Bus screensaver inhibition) %s", dbus_service.c_str(),
             (scrsv == 1 ? "only during screen activity" : "always"));
        if (scrsv == 2) {
            dbus_screensaver_inhibiter(true);
        }
    }
#endif
    if (audiosink == "0") {
        use_audio = false;
        dump_audio = false;
    }
    if (dump_video) {
        if (video_dump_limit > 0) {
             LOGI("dump video using \"-vdmp %d %s\"", video_dump_limit, video_dumpfile_name.c_str());
        } else {
             LOGI("dump video using \"-vdmp %s\"", video_dumpfile_name.c_str());
        }
    }
    if (dump_audio) {
        if (audio_dump_limit > 0) {
            LOGI("dump audio using \"-admp %d %s\"", audio_dump_limit, audio_dumpfile_name.c_str());
        } else {
            LOGI("dump audio using \"-admp %s\"",  audio_dumpfile_name.c_str());
        }
    }

#if __APPLE__
    /* warn about default  use of -nc option on macOS */
    if (!new_window_closing_behavior) {
        LOGI("UxPlay on macOS is using -nc option as workaround for GStreamer problem: use \"-nc no\" to omit workaround");
    }
#endif

    if (videosink == "0") {
        use_video = false;
	videosink.erase();
        videosink.append("fakesink");
	videosink_options.erase();
	LOGI("video_disabled");
        display[3] = 1; /* set fps to 1 frame per sec when no video will be shown */
    }

    if (fullscreen && use_video) {
        if (videosink == "waylandsink" || videosink == "vaapisink") {
            videosink_options.append(" fullscreen=true");
        } else if (videosink == "kmssink") {
            videosink_options.append(" force-modesetting=TRUE ");	  
        }
    }

    if (videosink == "d3d11videosink"  && videosink_options.empty() && use_video) {
        if (fullscreen) {
            videosink_options.append(" fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_PROPERTY fullscreen=TRUE ");
        } else {
            videosink_options.append(" fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER ");
            LOGI("Use Alt-Enter key combination to toggle into/out of full-screen mode");
        }
    }

    if (videosink == "d3d12videosink"  && videosink_options.empty() && use_video) {
        if (fullscreen) {
            videosink_options.append(" fullscreen=TRUE ");
        } else {
            videosink_options.append(" fullscreen-on-alt-enter=TRUE ");
            LOGI("Use Alt-Enter key combination to toggle into/out of full-screen mode");
        }
    } 

    if (bt709_fix && use_video) {
        video_parser.append(" ! ");
        video_parser.append(BT709_FIX);
    }

    if (srgb_fix && use_video) {
        std::string option = video_converter;
        video_converter.append(SRGB_FIX);
        video_converter.append(option);
    }
    
    if (pin_pw == 1 && registration_list) {
        if (pairing_register == "") {
            const char * homedir = get_homedir();
            if (homedir) {
                pairing_register = homedir;
                pairing_register.append("/.uxplay.register");
             }
        }
    }

    /* read in public keys that were previously registered with pair-setup-pin */
    if (pin_pw == 1 && registration_list && strlen(pairing_register.c_str())) {
        size_t len = 0;
        std::string  key;
        int clients = 0;
        std::ifstream file(pairing_register);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                /*32 bytes pk -> base64 -> strlen(pk64) = 44 chars = line[0:43]; add '\0' at line[44] */ 
                line[44] = '\0';
                std::string pk = line.c_str();
                registered_keys.push_back(key.assign(pk));
                clients ++;
            }
            if (clients) {
                LOGI("Register %s lists %d pin-registered clients", pairing_register.c_str(), clients);
            }
            file.close();
        }
    }

    if (pin_pw == 1 && keyfile == "0") {
        const char * homedir = get_homedir();
        if (homedir) {
            keyfile.erase();
            keyfile = homedir;
            keyfile.append("/.uxplay.pem");
        } else {
	    LOGE("could not determine $HOME: public key wiil not be saved, and so will not be persistent");
        }
    }

    if (keyfile != "") {
        LOGI("public key storage (for persistence) is in %s", keyfile.c_str());
    }
    
    if (do_append_hostname) {
        append_hostname(server_name);
    }

    if (!gstreamer_init()) {
        LOGE ("stopping");
        exit (1);
    }

    render_logger = logger_init();
    logger_set_callback(render_logger, log_callback, NULL);
    logger_set_level(render_logger, log_level);

    if (use_audio) {
        audio_renderer_init(render_logger, audiosink.c_str(), &audio_sync, &video_sync, audio_rtp_pipeline.c_str());
    } else {
        LOGI("audio_disabled");
    }
    if (use_video) {
        video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(), rtp_pipeline.c_str(),
                            video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                            videosink_options.c_str(), fullscreen, video_sync, h265_support,
                            render_coverart, playbin_version, NULL);
        video_renderer_start();
#ifdef __OpenBSD__
    } else {
        if (pledge("stdio rpath wpath cpath inet unix prot_exec", NULL) == -1) {
            err(1, "pledge");
        }
#endif
    }

    if (mux_to_file) {
        mux_renderer_init(render_logger, mux_filename.c_str(), use_audio, use_video);
    }

    if (udp[0]) {
        LOGI("using network ports UDP %d %d %d TCP %d %d %d", udp[0], udp[1], udp[2], tcp[0], tcp[1], tcp[2]);
    }

    if (!use_random_hw_addr) {
        if (strlen(mac_address.c_str()) == 0) {
            mac_address = find_mac();
            LOGI("using system MAC address %s",mac_address.c_str());	    
        } else {
            LOGI("using user-set MAC address %s",mac_address.c_str());
        }
    }
    if (mac_address.empty()) {
        mac_address = random_mac();
        LOGI("using randomly-generated MAC address %s",mac_address.c_str());
    }
    parse_hw_addr(mac_address, server_hw_addr);

    if (coverart_filename.length()) {
        LOGI("any AirPlay audio cover-art will be written to file  %s",coverart_filename.c_str());
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }

    if (metadata_filename.length()) {
        LOGI("any AirPlay audio metadata text will be written to file  %s",metadata_filename.c_str());
        write_metadata(metadata_filename.c_str(), "no data\n");
    }

    /* set default resolutions for h264 or h265*/
    if (!display[0] && !display[1]) {
        if (h265_support) {
            display[0] = 3840;
            display[1] = 2160;
        } else {
            display[0] = 1920;
            display[1] = 1080;
        }	  
    }

    if (start_dnssd(server_hw_addr, server_name)) {
        cleanup();
    }
    if (start_raop_server(display, tcp, udp, debug_log)) {
        stop_dnssd();
        cleanup();
    }

    if (lang.length() > 1) {
        raop_set_lang(raop, lang.c_str());
    }
    
#define PID_MAX 4194304 // 2^22
    if (ble_filename.length()) {
#ifdef _WIN32
        DWORD winpid = GetCurrentProcessId();
	uint32_t pid = (uint32_t) winpid;
        g_assert(pid <= PID_MAX);
#else
        pid_t pid = getpid();
        g_assert (pid <= PID_MAX && pid >= 0);
#endif
        write_bledata((uint32_t *) &pid, argv[0], ble_filename.c_str());
        LOGI("Bluetooth LE beacon-based service discovery is possible: PID data written to %s", ble_filename.c_str());
    }
    
    if (register_dnssd()) {
        stop_raop_server();
        stop_dnssd();
        cleanup();
    }
    reconnect:
    compression_type = 0;
    close_window = new_window_closing_behavior;
    main_loop();
    if (relaunch_video) {
        if (reset_httpd) {
            raop_stop_httpd(raop);
        }
        if (use_audio) {
            audio_renderer_stop();
        }
        if (use_video && (close_window || preserve_connections)) {
            video_renderer_destroy();
            if (!preserve_connections) {
                raop_destroy_airplay_video(raop, -1);
                url.erase();
                raop_remove_known_connections(raop);
            }
            const char *uri = (url.empty() ? NULL : url.c_str());
            video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(),rtp_pipeline.c_str(),
                                video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                                videosink_options.c_str(), fullscreen, video_sync, h265_support,
                                render_coverart, playbin_version, uri);
            video_renderer_start();
        }
        if (reset_httpd) {
            unsigned short port = raop_get_port(raop);
            raop_start_httpd(raop, &port);
            raop_set_port(raop, port);
        }
        if (mux_to_file) {
            mux_renderer_stop();
        }
        goto reconnect;
    } else {
        LOGI("Stopping RAOP Server...");
        stop_raop_server();
        stop_dnssd();
    }
    cleanup();
}
 
static void cleanup() {
    if (use_audio) {
        audio_renderer_destroy();
    }
    if (use_video)  {
        video_renderer_destroy();
    }
    logger_destroy(render_logger);
    render_logger = NULL;
    if(audio_dumpfile) {
        fclose(audio_dumpfile);
    }
    if (video_dumpfile) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
    }
    if (coverart_filename.length()) {
        remove (coverart_filename.c_str());
    }
    if (metadata_filename.length()) {
        remove (metadata_filename.c_str());
    }
    if (ble_filename.length()) {
        remove (ble_filename.c_str());
    }
#ifdef DBUS
    if (dbus_connection) {
        LOGD("Ending D-Bus connection %p", dbus_connection);
        if (dbus_last_message) {
            dbus_screensaver_inhibiter(false);
        }
        if (dbus_pending) {
            dbus_pending_call_cancel(dbus_pending);
            dbus_pending_call_unref(dbus_pending);
        }
        dbus_connection_unref(dbus_connection);
    }
#endif
    exit(0);
}
