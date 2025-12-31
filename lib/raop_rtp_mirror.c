/*
 * Copyright (c) 2019 dsafa22, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 *==================================================================
 * modified by fduncanh 2021-2023
 */

#include "raop_rtp_mirror.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/tcp.h>
#endif

#include "raop.h"
#include "netutils.h"
#include "compat.h"
#include "logger.h"
#include "byteutils.h"
#include "mirror_buffer.h"
#include "stream.h"
#include "utils.h"
#include "plist/plist.h"

#ifdef _WIN32
#define CAST (char *)
/* are these keepalive settings for WIN32 correct? */
/* (taken from https://github.com/wegank/ludimus)  */
#define TCP_KEEPALIVE 3
#define TCP_KEEPCNT 16
#define TCP_KEEPIDLE TCP_KEEPALIVE
#define TCP_KEEPINTVL 17
#else
#define CAST
#endif

#define SECOND_IN_NSECS 1000000000UL
#define SEC SECOND_IN_NSECS

/* for MacOS, where SOL_TCP and TCP_KEEPIDLE are not defined */
#if !defined(SOL_TCP) && defined(IPPROTO_TCP)
#define SOL_TCP IPPROTO_TCP
#endif
#if !defined(TCP_KEEPIDLE) && defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif

/* for OpenBSD, where TCP_KEEPIDLE and TCP_KEEPALIVE are not defined */
#if !defined(TCP_KEEPIDLE) && !defined(TCP_KEEPALIVE)
#define TCP_KEEPIDLE SO_KEEPALIVE
#endif

//struct h264codec_s {
//    unsigned char compatibility;
//    short pps_size;
//    short sps_size;
//    unsigned char level;
//    unsigned char number_of_pps;
//    unsigned char* picture_parameter_set;
//    unsigned char profile_high;
//    unsigned char reserved_3_and_sps;
//    unsigned char reserved_6_and_nal;
//    unsigned char* sequence_parameter_set;
//    unsigned char version;
//};

struct raop_rtp_mirror_s {
    logger_t *logger;
    raop_callbacks_t callbacks;
    raop_ntp_t *ntp;

    /* mirror buffer for decryption */
    mirror_buffer_t *buffer;

    /* Remote address as sockaddr */
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    int flush;
    thread_handle_t thread_mirror;
    mutex_handle_t run_mutex;

    /* MUTEX LOCKED VARIABLES END */
    int mirror_data_sock;

    unsigned short mirror_data_lport;

     /* switch for displaying client FPS data */
     uint8_t show_client_FPS_data;
};

static int
raop_rtp_mirror_parse_remote(raop_rtp_mirror_t *raop_rtp_mirror, const char *remote, int remotelen)
{
    assert(raop_rtp_mirror);
    int family = AF_UNSPEC;
    if (remotelen == 4) {
        family = AF_INET;
    } else if (remotelen == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror parse remote ip = %s", remote);
    int ret = netutils_parse_address(family, remote,
                                 &raop_rtp_mirror->remote_saddr,
                                 sizeof(raop_rtp_mirror->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_rtp_mirror->remote_saddr_len = ret;
    return 0;
}

#define NO_FLUSH (-42)
raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, raop_ntp_t *ntp,
                                        const char *remote, int remotelen, const unsigned char *aeskey)
{
    assert(logger);
    assert(callbacks);

    raop_rtp_mirror_t *raop_rtp_mirror = calloc(1, sizeof(raop_rtp_mirror_t));
    if (!raop_rtp_mirror) {
        return NULL;
    }
    raop_rtp_mirror->logger = logger;
    raop_rtp_mirror->ntp = ntp;

    memcpy(&raop_rtp_mirror->callbacks, callbacks, sizeof(raop_callbacks_t));
    raop_rtp_mirror->buffer = mirror_buffer_init(logger, aeskey);
    if (!raop_rtp_mirror->buffer) {
        free(raop_rtp_mirror);
        return NULL;
    }
    if (raop_rtp_mirror_parse_remote(raop_rtp_mirror, remote, remotelen) < 0) {
        free(raop_rtp_mirror);
        return NULL;
    }
    raop_rtp_mirror->running = 0;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->flush = NO_FLUSH;

    MUTEX_CREATE(raop_rtp_mirror->run_mutex);
    return raop_rtp_mirror;
}

void
raop_rtp_mirror_init_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t *streamConnectionID)
{
    mirror_buffer_init_aes(raop_rtp_mirror->buffer, streamConnectionID);
}

#define RAOP_PACKET_LEN 32768
/**
 * Mirror
 */
static THREAD_RETVAL
raop_rtp_mirror_thread(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    assert(raop_rtp_mirror);

    int stream_fd = -1;
    unsigned char packet[128] = {0};
    unsigned char* sps_pps = NULL;
    bool prepend_sps_pps = false;
    int sps_pps_len = 0;
    unsigned char* payload = NULL;
    unsigned int readstart = 0;
    bool conn_reset = false;
    uint64_t ntp_timestamp_nal = 0;
    uint64_t ntp_timestamp_raw = 0;
    uint64_t ntp_timestamp_remote = 0;
    uint64_t ntp_timestamp_local  = 0;
    unsigned char nal_start_code[4] = { 0x00, 0x00, 0x00, 0x01 };
    bool logger_debug = (logger_get_level(raop_rtp_mirror->logger) >= LOGGER_DEBUG);
    bool logger_debug_data = (logger_get_level(raop_rtp_mirror->logger) >= LOGGER_DEBUG_DATA);
    bool h265_video = false;
    video_codec_t codec = VIDEO_CODEC_UNKNOWN;
    const char h264[] = "h264";
    const char h265[] = "h265";
    bool unsupported_codec = false;
    bool video_stream_suspended = false;
    bool first_packet = true;
    
    while (1) {
        fd_set rfds;
        struct timeval tv;
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror->running is no longer true");
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

        /* Set timeout valu to 5ms */
        tv.tv_sec = 0;
        tv.tv_usec = 5000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        int nfds = 0;
        if (stream_fd == -1) {
            FD_SET(raop_rtp_mirror->mirror_data_sock, &rfds);
            nfds = raop_rtp_mirror->mirror_data_sock+1;
        } else {
            FD_SET(stream_fd, &rfds);
            nfds = stream_fd+1;
        }
        int ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            int sock_err = SOCKET_GET_ERROR();
            logger_log(raop_rtp_mirror->logger, LOGGER_ERR,
                       "raop_rtp_mirror error in select %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            break;
        }

        if (stream_fd == -1 &&
	    (raop_rtp_mirror && raop_rtp_mirror->mirror_data_sock >= 0) &&
            FD_ISSET(raop_rtp_mirror->mirror_data_sock, &rfds)) {
            struct sockaddr_storage saddr;
            socklen_t saddrlen = 0;
            logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror accepting client");
            saddrlen = sizeof(saddr);
            stream_fd = accept(raop_rtp_mirror->mirror_data_sock, (struct sockaddr *)&saddr, &saddrlen);
            if (stream_fd == -1) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR,
                           "raop_rtp_mirror error in accept %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
                break;
            }

            // We're calling recv for a certain amount of data, so we need a timeout
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 5000;
            if (setsockopt(stream_fd, SOL_SOCKET, SO_RCVTIMEO, CAST &tv, sizeof(tv)) < 0) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR,
                           "raop_rtp_mirror could not set stream socket timeout %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
                break;
            }

            int option = 1;
            if (setsockopt(stream_fd, SOL_SOCKET, SO_KEEPALIVE, CAST &option, sizeof(option)) < 0) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING,
                           "raop_rtp_mirror could not set stream socket keepalive %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            }
            option = 60;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPIDLE, CAST &option, sizeof(option)) < 0) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING,
                           "raop_rtp_mirror could not set stream socket keepalive time %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            }
/* OpenBSD does not have these options.  */
#ifndef __OpenBSD__
            option = 10;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPINTVL, CAST &option, sizeof(option)) < 0) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING,
                           "raop_rtp_mirror could not set stream socket keepalive interval %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            }
            option = 6;
            if (setsockopt(stream_fd, SOL_TCP, TCP_KEEPCNT, CAST &option, sizeof(option)) < 0) {
                int sock_err = SOCKET_GET_ERROR();
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING,
                           "raop_rtp_mirror could not set stream socket keepalive probes %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
            }
#endif /* !__OpenBSD__ */
            readstart = 0;
        }

        if (stream_fd != -1 && FD_ISSET(stream_fd, &rfds)) {

            // The first 128 bytes are some kind of header for the payload that follows
            while (payload == NULL && readstart < 128) {
                unsigned char* pos  = packet + readstart;
                ret = recv(stream_fd, CAST pos, 128 - readstart, 0);
                if (ret <= 0) break;
                readstart = readstart + ret;
            }

            if (payload == NULL && ret == 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                           "raop_rtp_mirror tcp socket was closed by client (recv returned 0); got %d bytes of 128 byte header",readstart);
                FD_CLR(stream_fd, &rfds);
                stream_fd = -1;
                continue;
            } else if (payload == NULL && ret == -1) {
                int sock_err = SOCKET_GET_ERROR();
                if (sock_err == SOCKET_ERRORNAME(EAGAIN) || sock_err == SOCKET_ERRORNAME(EWOULDBLOCK)) continue; // Timeouts can happen even if the connection is fine
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR,
                           "raop_rtp_mirror error  in header recv: %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
                if (sock_err == SOCKET_ERRORNAME(ECONNRESET)) conn_reset = true;; 
                break;
            }

            /*packet[0:3] contains the payload size */
            int payload_size = byteutils_get_int(packet, 0);
            char packet_description[13] = {0};
            char *p = packet_description;
            int n = sizeof(packet_description);
            for (int i = 4; i < 8; i++) {
                snprintf(p, n, "%2.2x ", (unsigned int) packet[i]);
                n -= 3;
                p += 3;
            }
            ntp_timestamp_raw = byteutils_get_long(packet, 8);
            ntp_timestamp_remote = raop_ntp_timestamp_to_nano_seconds(ntp_timestamp_raw, false);
            if (first_packet) {
	        uint64_t offset  = raop_ntp_get_local_time() - ntp_timestamp_remote;
                raop_ntp_set_video_arrival_offset(raop_rtp_mirror->ntp, &offset);
                first_packet = false;
            }

            /* packet[4] + packet[5] identify the payload type:   values seen are:               *
             * 0x00 0x00: encrypted packet containing a non-IDR  type 1 VCL NAL unit             *
             * 0x00 0x10: encrypted packet containing an IDR type 5 VCL NAL unit                 *
             * 0x01 0x00: unencrypted packet containing a type 7 SPS NAL + a type 8 PPS NAL unit *
             * 0x02 0x00: unencrypted packet (old protocol) no payload, sent once every second   *
             * 0x05 0x00  unencrypted packet with a "streaming report", sent once per second.    */

            /* packet[6] + packet[7] may list a payload "option":    values seen are:            *
             * 0x00 0x00 : encrypted and "streaming report" packets                              *
             * 0x1e 0x00 : old protocol (seen in AirMyPC) no-payload once-per-second packets     *
             * 0x16 0x01 : seen in most unencrypted h264  SPS+PPS packets                        *
             * 0x56 0x01 : unencrypted h264 SPS+PPS packets (video stream stops, client sleeps)  *
             * 0x1e 0x01 : unencrypted h265/HEVC SPS+PPS packets                                 
             * 0x5e 0x01 : unencrypted h265 SPS+PPS packets (video stream stops, client sleeps)  */

            /* unencrypted packets with a SPS and a PPS NAL are sent initially, and also when a  *
             * change in video format (e.g. width, height) subsequently occurs. They seem always *
             * to be followed by a packet with a type 5 encrypted IDR VCL NAL, with an identical *
             * timestamp.  On M1/M2 Mac clients, this type 5 NAL is prepended with a type 6 SEI  *
             * NAL unit.  Here we prepend the SPS+PPS NALs to the next encrypted packet, which   *
             * always has the same timestamp, and is (almost?) always an IDR NAL unit.           */

            /* Unencrypted SPS/PPS packets also have image-size data in (parts of) packet[16:127] */

            /* "streaming report" packets have no timestamp in packet[8:15] */

            if (payload == NULL) {
                payload = malloc(payload_size);
                readstart = 0;
            }

            while ((int) readstart < payload_size) {
                // Payload data
                unsigned char *pos = payload + readstart;
                ret = recv(stream_fd, CAST pos, payload_size - readstart, 0);
                if (ret <= 0) break;
                readstart = readstart + ret;
            }

            if (ret == 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror tcp socket was closed by client (recv returned 0)");
                break;
            } else if (ret == -1) {
                int sock_err = SOCKET_GET_ERROR();
                if (sock_err == SOCKET_ERRORNAME(EAGAIN) || sock_err == SOCKET_ERRORNAME(EWOULDBLOCK)) continue; // Timeouts can happen even if the connection is fine
                logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror error in recv: %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
                if (errno == SOCKET_ERRORNAME(ECONNRESET)) conn_reset = true;
                break;
            }

            switch (packet[4]) {
            case  0x00:
                // Normal video data (VCL NAL)

                // Conveniently, the video data is already stamped with the remote wall clock time,
                // so no additional clock syncing needed. The only thing odd here is that the video
                // ntp time stamps don't include the SECONDS_FROM_1900_TO_1970, so it's really just
                // counting nano seconds since last boot.

                ntp_timestamp_local = raop_ntp_convert_remote_time(raop_rtp_mirror->ntp, ntp_timestamp_remote);
                if (logger_debug_data) {
                    uint64_t ntp_now = raop_ntp_get_local_time();
                    int64_t latency = (ntp_timestamp_local ? ((int64_t) ntp_now) - ((int64_t) ntp_timestamp_local) : 0);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                               "raop_rtp video: now = %8.6f, ntp = %8.6f, latency = %9.6f, ts = %8.6f, %s %s, size: %d",
                               (double) ntp_now / SEC, (double) ntp_timestamp_local / SEC, (double) latency / SEC,
                               (double) ntp_timestamp_remote / SEC, packet_description, (h265_video ? h265 : h264), payload_size);
                }

                unsigned char* payload_out;
                unsigned char* payload_decrypted;
                /*
                 * nal_types:1   Coded non-partitioned slice of a non-IDR picture
                 *           5   Coded non-partitioned slice of an IDR picture
                 *           6   Supplemental enhancement information (SEI)
                 *           7   Sequence parameter set (SPS)
                 *           8   Picture parameter set (PPS)
                 *
                 * if a previous unencrypted packet contains an SPS (type 7) and PPS (type 8) NAL which has not 
                 * yet been sent, it should be prepended to the current NAL.    The M1 Macs have increased the h264 level, 
                 * and now the first  encrypted packet after the  unencrypted SPS+PPS packet may also contain a SEI (type 6) NAL 
                 * prepended to its VCL NAL.
                 *
                 * The flag prepend_sps_pps = true will signal that the  previous packet contained a SPS NAL + a PPS NAL, 
                 * that has not yet been sent.   This will trigger prepending it to the current NAL, and the prepend_sps_pps 
                 * flag will be set to false after it has been prepended.  */

                if (prepend_sps_pps & (ntp_timestamp_raw != ntp_timestamp_nal)) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                                   "raop_rtp_mirror: prepended sps_pps timestamp does not match timestamp of "
                                   "video payload\n%llu\n%llu , discarding", ntp_timestamp_raw, ntp_timestamp_nal);
                        free (sps_pps);
                        sps_pps = NULL;
                        prepend_sps_pps = false;
                }
		
                if (prepend_sps_pps) {
                    assert(sps_pps);
                    payload_out = (unsigned char*) malloc(payload_size + sps_pps_len);
                    if (!payload_out) {
                        printf("Memory allocation failed (payload_out)\n");
                        exit(1);
                    }
                    payload_decrypted = payload_out + sps_pps_len;
                    memcpy(payload_out, sps_pps, sps_pps_len);
                    free (sps_pps);
                    sps_pps = NULL;
                } else {
                    payload_out = (unsigned char*)  malloc(payload_size);
                    payload_decrypted = payload_out;
                }
                // Decrypt data: AES-CTR encryption/decryption  does not change the size of the data
                mirror_buffer_decrypt(raop_rtp_mirror->buffer, payload, payload_decrypted, payload_size);

                // It seems the AirPlay protocol prepends NALs with their size, which we're replacing with the 4-byte
                // start code for the NAL Byte-Stream Format.
                bool valid_data = true;
                int nalu_size = 0;
                int nalus_count = 0;
                while (nalu_size < payload_size) {
                    int nc_len = byteutils_get_int_be(payload_decrypted, nalu_size);
                    if (nc_len < 0 || nalu_size + 4 > payload_size) {
                        valid_data = false;
                        break;
                    }
                    memcpy(payload_decrypted + nalu_size, nal_start_code, 4);
                    nalu_size += 4;
                    nalus_count++;
                    /* first bit of h264 nalu MUST be 0 ("forbidden_zero_bit") */
                    if (payload_decrypted[nalu_size] & 0x80) {
                        valid_data = false;
                        break;
                    }
                    int nalu_type = 0;
                    if (h265_video) {
                        nalu_type = payload_decrypted[nalu_size] & 0x7e >> 1;;
                        //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG," h265 video, NALU type %d, size %d", nalu_type, nc_len);
                    } else {
                        nalu_type = payload_decrypted[nalu_size] & 0x1f;
                        int ref_idc = (payload_decrypted[nalu_size] >> 5);
                        switch (nalu_type) {
                        case 14:  /* Prefix NALu , seen before all VCL Nalu's in AirMyPc */
                        case 5:   /*IDR, slice_layer_without_partitioning */
                        case 1:   /*non-IDR, slice_layer_without_partitioning */
                            break;
                        case 2:   /* slice data partition A */
                        case 3:   /* slice data partition B */
                        case 4:   /* slice data partition C */
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                                       "unexpected partitioned VCL NAL unit: nalu_type = %d, ref_idc = %d, nalu_size = %d,"
                                       "processed bytes %d, payloadsize = %d nalus_count = %d",
                                       nalu_type, ref_idc, nc_len, nalu_size, payload_size, nalus_count);
                            break;
                        case 6:
                            if (logger_debug) {
                                char *str = utils_data_to_string(payload_decrypted + nalu_size, nc_len, 16); 
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror SEI NAL size = %d", nc_len);		
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                                           "raop_rtp_mirror h264 Supplemental Enhancement Information:\n%s", str);
                                free(str);
                            }
                            break;
                        case 7:
                            if (logger_debug) {
                                char *str = utils_data_to_string(payload_decrypted + nalu_size, nc_len, 16); 
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror SPS NAL size = %d", nc_len);		
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                                           "raop_rtp_mirror h264 Sequence Parameter Set:\n%s", str);
                                free(str);
                            }
                            break;
                        case 8:
                            if (logger_debug) {
                                char *str = utils_data_to_string(payload_decrypted + nalu_size, nc_len, 16); 
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror PPS NAL size = %d", nc_len);		
                                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                                           "raop_rtp_mirror h264 Picture Parameter Set :\n%s", str);
                                free(str);
                            }
                            break;
                        default:
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                                       "unexpected non-VCL NAL unit: nalu_type = %d, ref_idc = %d, nalu_size = %d,"
                                       "processed bytes %d, payloadsize = %d nalus_count = %d",
                                       nalu_type, ref_idc, nc_len, nalu_size, payload_size, nalus_count);
                             break;
                        }
                    }
                    nalu_size += nc_len;
                }
                if (nalu_size != payload_size) valid_data = false;
                if(!valid_data) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalu marked as invalid");
                    payload_out[0] = 1; /* mark video data as invalid h264 (failed decryption) */
                }

		
                payload_decrypted = NULL;
                video_decode_struct video_data;
                video_data.is_h265 = h265_video;
                video_data.ntp_time_local = ntp_timestamp_local;
                video_data.ntp_time_remote = ntp_timestamp_remote;
                video_data.nal_count = nalus_count;   /*nal_count will be the number of nal units in the packet */
                video_data.data_len = payload_size;
                video_data.data = payload_out;
                if (prepend_sps_pps) {
                    video_data.data_len += sps_pps_len;
                    video_data.nal_count += 2;
                    if (h265_video) {
                        video_data.nal_count++;
                    }
                    prepend_sps_pps =  false;
                }

                raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->ntp, &video_data);
                free(payload_out);
                break;
            case 0x01:
                /* 128-byte observed packet header structure 
                   bytes 0-15: length + timestamp
                   bytes 16-19 float width_source   (value is x.0000, x = unsigned short)
                   bytes 20-23 float height_source  (value is x.0000, x = unsigned short)
                   bytes 24-39 all  0x0
                   bytes 40-43 float width_source   (value is x.0000, x = unsigned short)
                   bytes 44-47 float height_source  (value is x.0000, x = unsigned short)
                   bytes 48-51 ??? float "other_w"  (value seems to be x.0000, x = unsigned short)
                   bytes 48-51 ??? float "other_h"  (value seems to be x.0000, x = unsigned short)
                   bytes 56-59 width 
                   bytes 60-63 height 
                   bytes 64-127 all 0x0 
                */
	      
                // The information in the payload contains an SPS and a PPS NAL
                // The sps_pps is not encrypted
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "\nReceived unencrypted codec packet from client:"
                           " payload_size %d header %s ts_client = %8.6f",
                           payload_size, packet_description, (double) ntp_timestamp_remote / SEC);

                if (packet[6] == 0x56 || packet[6] == 0x5e) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "This packet indicates video stream is stopping");
                }
                if (!video_stream_suspended && (packet[6] == 0x56 || packet[6] == 0x5e)) {
                    video_stream_suspended = true;
                    raop_rtp_mirror->callbacks.video_pause(raop_rtp_mirror->callbacks.cls);
                } else if (video_stream_suspended && (packet[6] == 0x16 || packet[6] == 0x1e)) {
                    raop_rtp_mirror->callbacks.video_resume(raop_rtp_mirror->callbacks.cls);
                    video_stream_suspended = false;
                }

                assert (raop_rtp_mirror->callbacks.video_set_codec);
                ntp_timestamp_nal = ntp_timestamp_raw;

                /* these "floats" are in fact integers that fit into unsigned shorts */
                float width_0 = byteutils_get_float(packet, 16);  
                float height_0 = byteutils_get_float(packet, 20);
                float width_source = byteutils_get_float(packet, 40);    // duplication of width_0
                float height_source = byteutils_get_float(packet, 44);   // duplication of height_0
                float unknown_w = byteutils_get_float(packet, 48);
                float unknown_h = byteutils_get_float(packet, 52);
                float width = byteutils_get_float(packet, 56);
                float height = byteutils_get_float(packet, 60);

                if (width != width_0 || height != height_0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror: Unexpected : data  %f,"
                           " %f != width_source = %f, height_source = %f", width_0, height_0, width_source, height_source);
                }
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror: unidentified extra header data  %f, %f", unknown_w, unknown_h);
                if (raop_rtp_mirror->callbacks.video_report_size) {
                    raop_rtp_mirror->callbacks.video_report_size(raop_rtp_mirror->callbacks.cls, &width_source, &height_source, &width, &height);
                }
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror width_source = %f height_source = %f width = %f height = %f",
                           width_source, height_source, width, height);

                if (payload_size == 0) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror: received type 0x01 packet with no payload:\n"
                               "this indicates non-h264 video but Airplay features bit 42 (SupportsScreenMultiCodec) is not set\n"
                               "use startup option \"-h265\" to set this bit and support h265 (4K) video");
                    unsupported_codec = true;
                    break;
                }
                if (sps_pps) {
                    free(sps_pps);
                    sps_pps = NULL;
                }
                /* test for a H265 VPS/SPS/PPS */
                unsigned char hvc1[] = { 0x68, 0x76, 0x63, 0x31 };

                if (!memcmp(payload + 4, hvc1, 4)) {
                    /* hvc1 HECV detected */
                    if (codec == VIDEO_CODEC_UNKNOWN) {
                        codec = VIDEO_CODEC_H265;
                        h265_video = true;
                        if (raop_rtp_mirror->callbacks.video_set_codec(raop_rtp_mirror->callbacks.cls, codec) < 0) {
                            logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "failed to set video codec as H265 ");
                            /* drop connection */
                            conn_reset = true;
                            break;
                        }
                    } else if (codec != VIDEO_CODEC_H265) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "invalid video codec change to H265: codec was set previously");
                        /* drop connection */
                        conn_reset = true;
                        break;
                    }
                    unsigned char vps_start_code[] = { 0xa0, 0x00, 0x01, 0x00 };
                    unsigned char sps_start_code[] = { 0xa1, 0x00, 0x01, 0x00 };
                    unsigned char pps_start_code[] = { 0xa2, 0x00, 0x01, 0x00 };

                    unsigned char * ptr = payload + 0x75;
 
                    if (memcmp(ptr, vps_start_code, 4)) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "non-conforming HEVC VPS/SPS/PPS payload (VPS)");
                        raop_rtp_mirror->callbacks.video_pause(raop_rtp_mirror->callbacks.cls);
                        break;
                    }
                    short vps_size = byteutils_get_short_be(ptr, 3);
                    ptr += 5;
                    unsigned char *vps = ptr;
                    if (logger_debug) {
                        char *str = utils_data_to_string(vps, vps_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "h265 vps size %d\n%s",vps_size, str);
                        free(str);
                    }
                    ptr += vps_size;
                    if (memcmp(ptr, sps_start_code, 4)) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "non-conforming HEVC VPS/SPS/PPS payload (SPS)");
                        raop_rtp_mirror->callbacks.video_pause(raop_rtp_mirror->callbacks.cls);
                        break;
                    }
                    short sps_size = byteutils_get_short_be(ptr, 3);
                    ptr += 5;
                    unsigned char *sps = ptr;
                    if (logger_debug) {
                        char *str = utils_data_to_string(sps, sps_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "h265 sps size %d\n%s",vps_size, str);
                        free(str);
                    }
                    ptr += sps_size;
                    if (memcmp(ptr, pps_start_code, 4)) {
                       logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "non-conforming HEVC VPS/SPS/PPS payload (PPS)");			
                        raop_rtp_mirror->callbacks.video_pause(raop_rtp_mirror->callbacks.cls);
                        break;
                    }
                    short pps_size = byteutils_get_short_be(ptr, 3);
                    ptr += 5;
                    unsigned char *pps = ptr;
                    if (logger_debug) {
                        char *str = utils_data_to_string(pps, pps_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "h265 pps size %d\n%s",pps_size, str);
                        free(str);
                    }

                    sps_pps_len = vps_size + sps_size + pps_size + 12;
                    sps_pps = (unsigned char*) malloc(sps_pps_len);
                    assert(sps_pps);
                    ptr = sps_pps;
                    memcpy(ptr, nal_start_code, 4);
                    ptr += 4;
                    memcpy(ptr, vps, vps_size);
                    ptr += vps_size;
                    memcpy(ptr, nal_start_code, 4);
                    ptr += 4;
                    memcpy(ptr, sps, sps_size);
                    ptr += sps_size;
                    memcpy(ptr, nal_start_code, 4);
                    ptr += 4;
                    memcpy(ptr, pps, pps_size);
                } else {
                    if (codec == VIDEO_CODEC_UNKNOWN) {
                        codec = VIDEO_CODEC_H264;
                        h265_video = false;
                        if (raop_rtp_mirror->callbacks.video_set_codec(raop_rtp_mirror->callbacks.cls, codec) < 0) {
                            logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "failed to set video codec as H264 ");
                            /* drop connection */
                            conn_reset = true;
                            break;
                        }
                    } else if (codec != VIDEO_CODEC_H264) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "invalid codec change to H264: codec was set previously");
                        /* drop connection */
                        conn_reset = true;
                        break;
                    }
                    short sps_size = byteutils_get_short_be(payload,6);
                    unsigned char *sequence_parameter_set = payload + 8;
                    short pps_size = byteutils_get_short_be(payload, sps_size + 9);
                    unsigned char *picture_parameter_set = payload + sps_size + 11;
                    int data_size = 6;
                    if (logger_debug) {
                        char *str = utils_data_to_string(payload, data_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror: SPS+PPS header size = %d", data_size);		
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror h264 SPS+PPS header:\n%s", str);
                        free(str);
                        str = utils_data_to_string(sequence_parameter_set, sps_size,16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror SPS NAL size = %d",  sps_size);		
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror h264 Sequence Parameter Set:\n%s", str);
                        free(str);
                        str = utils_data_to_string(picture_parameter_set, pps_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror PPS NAL size = %d", pps_size);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror h264 Picture Parameter Set:\n%s", str);
                        free(str);
                    }
                    data_size = payload_size - sps_size - pps_size - 11; 
                    if (data_size > 0 && logger_debug) {
                        char *str = utils_data_to_string (picture_parameter_set + pps_size, data_size, 16);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "remainder size = %d", data_size);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "remainder of SPS+PPS packet:\n%s", str);
                        free(str);
                    } else if (data_size < 0) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, " pps_sps error: packet remainder size = %d < 0", data_size);
                    }

                    // Copy the sps and pps into a buffer to prepend to the next NAL unit.
                    sps_pps_len = sps_size + pps_size + 8;
                    sps_pps = (unsigned char*) malloc(sps_pps_len);
                    assert(sps_pps);
                    memcpy(sps_pps, nal_start_code, 4);
                    memcpy(sps_pps + 4, sequence_parameter_set, sps_size);
                    memcpy(sps_pps + sps_size + 4, nal_start_code, 4); 
                    memcpy(sps_pps + sps_size + 8, payload + sps_size + 11, pps_size);
                }
                prepend_sps_pps = true;
                // h264codec_t h264;
                // h264.version = payload[0];
                // h264.profile_high = payload[1];
                // h264.compatibility = payload[2];
                // h264.level = payload[3];
                // h264.reserved_6_and_nal = payload[4];
                // h264.reserved_3_and_sps = payload[5];
                // h264.sps_size =  sps_size;
                // h264.sequence_parameter_set = malloc(h264.sps_size);
                // memcpy(h264.sequence_parameter_set, sequence_parameter_set, sps_size);
                // h264.number_of_pps = payload[h264.sps_size + 8];
                // h264.pps_size = pps_size;
                // h264.picture_parameter_set = malloc(h264.pps_size);
                // memcpy(h264.picture_parameter_set, picture_parameter_set, pps_size);
                break;
            case 0x02:
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "\nReceived old-protocol once-per-second packet from client:"
                           " payload_size %d header %s ts_raw = %llu", payload_size, packet_description, ntp_timestamp_raw);
                /* "old protocol" (used by AirMyPC), rest of 128-byte  packet is empty  */
                break;
            case 0x05:
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "\nReceived video streaming performance info packet from client:"
                           " payload_size %d header %s ts_raw = %llu", payload_size, packet_description, ntp_timestamp_raw);
                /* payloads with packet[4] = 0x05 have no timestamp, and carry video info from the client as a binary plist *
                 * Sometimes (e.g, when the client has a locked screen), there is a 25kB trailer attached to the packet.    *
                 * This 25000 Byte trailer with unidentified content seems to be the same data each time it is sent.        */

                if (payload_size) {
                    //char *str = utils_data_to_string(packet, 128, 16);
                    //logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "type 5 video packet header:\n%s", str);
                    //free (str);
		    
                    int plist_size = payload_size;
                    if (payload_size > 25000) {
                        plist_size = payload_size - 25000;
                        if (logger_debug) {
                            char *str = utils_data_to_string(payload + plist_size, 16, 16);
                            logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                                       "video_info packet had 25kB trailer; first 16 bytes are:\n%s", str);
                        free(str);
                        }
                    }
                    if (plist_size) {
                        char *plist_xml = NULL;
                        uint32_t plist_len = 0;
                        plist_t root_node = NULL;
                        plist_from_bin((char *) payload, plist_size, &root_node);
                        if (raop_rtp_mirror->callbacks.mirror_video_activity) {
                            double txusage = 0.0;
                            plist_t tx_usage_avg_node = plist_dict_get_item(root_node, "txUsageAvg");
                            plist_get_real_val(tx_usage_avg_node, &txusage);
                            raop_rtp_mirror->callbacks.mirror_video_activity(raop_rtp_mirror->callbacks.cls, &txusage);
                        }
                        if (raop_rtp_mirror->show_client_FPS_data) {
                            plist_to_xml(root_node, &plist_xml, &plist_len);
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "%s", plist_xml);
                            free(plist_xml);
                        }
                    }
                }
                break;
            default:
                logger_log(raop_rtp_mirror->logger, LOGGER_WARNING, "\nReceived unexpected TCP packet from client, "
                           "size %d, %s ts_raw = %llu", payload_size, packet_description, ntp_timestamp_raw);
                break;
            }

            free(payload);
            payload = NULL;
            memset(packet, 0, 128);
            readstart = 0;
            if (unsupported_codec) {
                break;
            }
        }
    }
    /* Close the stream file descriptor */
    if (stream_fd != -1) {
        closesocket(stream_fd);
    }

    // Ensure running reflects the actual state
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->running = false;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror exiting TCP thread");
    if (conn_reset&& raop_rtp_mirror->callbacks.conn_reset) {
        raop_rtp_mirror->callbacks.conn_reset(raop_rtp_mirror->callbacks.cls, 1);
    }

    if (unsupported_codec) {
        closesocket(raop_rtp_mirror->mirror_data_sock);
        raop_rtp_mirror_stop(raop_rtp_mirror);
        raop_rtp_mirror->callbacks.video_reset(raop_rtp_mirror->callbacks.cls, RESET_TYPE_RTP_SHUTDOWN);
    }

    return 0;
}

static int
raop_rtp_mirror_init_socket(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6)
{
    assert(raop_rtp_mirror);

    unsigned short dport = raop_rtp_mirror->mirror_data_lport;
    int dsock = netutils_init_socket(&dport, use_ipv6, 0);
    if (dsock == -1) {
        goto sockets_cleanup;
    }

    /* Listen to the data socket if using TCP */
    if (listen(dsock, 1) < 0) {
        goto sockets_cleanup;
    }

    /* Set socket descriptors */
    raop_rtp_mirror->mirror_data_sock = dsock;

    /* Set port values */
    raop_rtp_mirror->mirror_data_lport = dport;
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror local data port socket %d port TCP %d",
               dsock, dport);
    return 0;

    sockets_cleanup:
    if (dsock != -1) closesocket(dsock);
    return -1;
}

void
raop_rtp_mirror_start(raop_rtp_mirror_t *raop_rtp_mirror, unsigned short *mirror_data_lport,
                      uint8_t show_client_FPS_data)
{
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "raop_rtp_mirror starting mirroring");
    int use_ipv6 = 0;

    assert(raop_rtp_mirror);
    assert(mirror_data_lport);
    raop_rtp_mirror->show_client_FPS_data = show_client_FPS_data;

    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->running || !raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }

    if (raop_rtp_mirror->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    //use_ipv6 = 0;
     
    raop_rtp_mirror->mirror_data_lport = *mirror_data_lport;
    if (raop_rtp_mirror_init_socket(raop_rtp_mirror, use_ipv6) < 0) {
        logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror initializing socket failed");
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    *mirror_data_lport = raop_rtp_mirror->mirror_data_lport;

    /* Create the thread and initialize running values */
    raop_rtp_mirror->running = 1;
    raop_rtp_mirror->joined = 0;

    THREAD_CREATE(raop_rtp_mirror->thread_mirror, raop_rtp_mirror_thread, raop_rtp_mirror);
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror) {
    assert(raop_rtp_mirror);

    /* Check that we are running and thread is not
     * joined (should never be while still running) */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (!raop_rtp_mirror->running || raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    raop_rtp_mirror->running = 0;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    /* Join the thread */
    THREAD_JOIN(raop_rtp_mirror->thread_mirror);

    if (raop_rtp_mirror->mirror_data_sock != -1) {
        closesocket(raop_rtp_mirror->mirror_data_sock);
        raop_rtp_mirror->mirror_data_sock = -1;
    }

    /* Mark thread as joined */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->joined = 1;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror) {
    if (raop_rtp_mirror) {
        raop_rtp_mirror_stop(raop_rtp_mirror);
        MUTEX_DESTROY(raop_rtp_mirror->run_mutex);
        mirror_buffer_destroy(raop_rtp_mirror->buffer);
	free(raop_rtp_mirror);
    }
}
