/*
 * Copyright (c) 2019 dsafa22 and 2014 Joakim Plate, modified by Florian Draschbacher,
 * All Rights Reserved.
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
 *=================================================================
 * modified by fduncanh 2021-23
 */

// Some of the code in here comes from https://github.com/juhovh/shairplay/pull/25/files

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#ifdef _WIN32
#define CAST (char *)
#else
#define CAST
#endif

#include "raop.h"
#include "threads.h"
#include "compat.h"
#include "netutils.h"
#include "byteutils.h"
#include "utils.h"

#define SECOND_IN_NSECS 1000000000UL
#define RAOP_NTP_DATA_COUNT   8
#define RAOP_NTP_PHI_PPM   15ull                   // PPM
#define RAOP_NTP_R_RHO   ((1ull    << 32) / 1000u) // packet precision
#define RAOP_NTP_S_RHO   ((1ull    << 32) / 1000u) // system clock precision
#define RAOP_NTP_MAX_DIST ((1500ull << 32) / 1000u) // maximum allowed distance
#define RAOP_NTP_MAX_DISP ((16ull   << 32))         // maximum dispersion

#define RAOP_NTP_CLOCK_BASE (2208988800ull << 32)

typedef struct raop_ntp_data_s {
    uint64_t time; // The local wall clock time at time of ntp packet arrival
    uint64_t dispersion;
    int64_t delay; // The round trip delay
    int64_t offset; // The difference between remote and local wall clock time
} raop_ntp_data_t;

struct raop_ntp_s {
    logger_t *logger;
    raop_callbacks_t callbacks;

    thread_handle_t thread;
    mutex_handle_t run_mutex;

    mutex_handle_t wait_mutex;
    cond_handle_t wait_cond;

    raop_ntp_data_t data[RAOP_NTP_DATA_COUNT];
    int data_index;

    // The clock sync params are periodically updated to the AirPlay client's NTP clock
    mutex_handle_t sync_params_mutex;
    int64_t sync_offset;
    int64_t sync_dispersion;
    int64_t sync_delay;

    // Socket address of the AirPlay client
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    // The remote port of the NTP server on the AirPlay client
    unsigned short timing_rport;

    // The local port of the NTP client on the AirPlay server
    unsigned short timing_lport;

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    // UDP socket
    int tsock;

    timing_protocol_t time_protocol;
    bool client_time_received;

    uint64_t video_arrival_offset;
};

/* for use in syncing audio before a first rtp_sync */
void raop_ntp_set_video_arrival_offset(raop_ntp_t* raop_ntp, const uint64_t *offset) {
    raop_ntp->video_arrival_offset = *offset;
}

uint64_t raop_ntp_get_video_arrival_offset(raop_ntp_t* raop_ntp) {
    return raop_ntp->video_arrival_offset;
}

/*
 * Used for sorting the data array by delay
 */
static int
raop_ntp_compare(const void* av, const void* bv)
{
    const raop_ntp_data_t* a = (const raop_ntp_data_t*)av;
    const raop_ntp_data_t* b = (const raop_ntp_data_t*)bv;
    if (a->delay < b->delay) {
        return -1;
    } else if(a->delay > b->delay) {
        return 1;
    } else {
        return 0;
    }
}

static int
raop_ntp_parse_remote(raop_ntp_t *raop_ntp, const char *remote, int remote_addr_len)
{
    int family = AF_UNSPEC;
    assert(raop_ntp);
    if (remote_addr_len == 4) {
        family = AF_INET;
    } else if (remote_addr_len == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp parse remote ip = %s", remote);
    int ret = netutils_parse_address(family, remote,
                                 &raop_ntp->remote_saddr,
                                 sizeof(raop_ntp->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_ntp->remote_saddr_len = ret;
    return 0;
}

raop_ntp_t *raop_ntp_init(logger_t *logger, raop_callbacks_t *callbacks, const char *remote,
                          int remote_addr_len, unsigned short timing_rport, timing_protocol_t *time_protocol) {
    assert(logger);
    assert(callbacks);

    raop_ntp_t *raop_ntp = calloc(1, sizeof(raop_ntp_t));
    if (!raop_ntp) {
        return NULL;
    }
    raop_ntp->time_protocol = *time_protocol;
    raop_ntp->logger = logger;
    memcpy(&raop_ntp->callbacks, callbacks, sizeof(raop_callbacks_t));    
    raop_ntp->timing_rport = timing_rport;
    raop_ntp->client_time_received = false;

    raop_ntp->video_arrival_offset = 0;

    if (raop_ntp_parse_remote(raop_ntp, remote, remote_addr_len) < 0) {
        free(raop_ntp);
        return NULL;
    }

    // Set port on the remote address struct
    ((struct sockaddr_in *) &raop_ntp->remote_saddr)->sin_port = htons(timing_rport);

    raop_ntp->running = 0;
    raop_ntp->joined = 1;

    uint64_t time = raop_ntp_get_local_time();

    for (int i = 0; i < RAOP_NTP_DATA_COUNT; ++i) {
        raop_ntp->data[i].offset     = 0ll;
        raop_ntp->data[i].delay      = RAOP_NTP_MAX_DISP;
        raop_ntp->data[i].dispersion = RAOP_NTP_MAX_DISP;
        raop_ntp->data[i].time      = time;
    }

    raop_ntp->sync_delay = 0;
    raop_ntp->sync_dispersion = 0;
    raop_ntp->sync_offset = 0;

    MUTEX_CREATE(raop_ntp->run_mutex);
    MUTEX_CREATE(raop_ntp->wait_mutex);
    COND_CREATE(raop_ntp->wait_cond);
    MUTEX_CREATE(raop_ntp->sync_params_mutex);
    return raop_ntp;
}

void
raop_ntp_destroy(raop_ntp_t *raop_ntp)
{
    if (raop_ntp) {
        raop_ntp_stop(raop_ntp);
        MUTEX_DESTROY(raop_ntp->run_mutex);
        MUTEX_DESTROY(raop_ntp->wait_mutex);
        COND_DESTROY(raop_ntp->wait_cond);
        MUTEX_DESTROY(raop_ntp->sync_params_mutex);
        free(raop_ntp);
    }
}

unsigned short raop_ntp_get_port(raop_ntp_t *raop_ntp) {
    return raop_ntp->timing_lport;
}

static int
raop_ntp_init_socket(raop_ntp_t *raop_ntp, int use_ipv6)
{
    assert(raop_ntp);
    unsigned short tport = raop_ntp->timing_lport;
    int tsock = netutils_init_socket(&tport, use_ipv6, 1);

    if (tsock == -1) {
        goto sockets_cleanup;
    }

    // We're calling recvfrom without knowing whether there is any data, so we need a timeout
    uint32_t recv_timeout_msec = 300; 
#ifdef _WIN32
    DWORD tv  = recv_timeout_msec;
#else
    struct timeval tv;
    tv.tv_sec = recv_timeout_msec / (uint32_t) 1000;
    tv.tv_usec = ((uint32_t) 1000) * (recv_timeout_msec % (uint32_t) 1000);
#endif
    if (setsockopt(tsock, SOL_SOCKET, SO_RCVTIMEO, CAST &tv, sizeof(tv)) < 0) {
        goto sockets_cleanup;
    }

    /* Set socket descriptors */
    raop_ntp->tsock = tsock;

    /* Set port values */
    raop_ntp->timing_lport = tport;
    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp local timing port socket %d port UDP %d", tsock, tport);
    return 0;

    sockets_cleanup:
    if (tsock != -1) closesocket(tsock);
    return -1;
}

static void
raop_ntp_flush_socket(int fd)
{
#ifdef _WIN32
#define IOCTL ioctlsocket
    u_long bytes_available = 0;
#else
#define IOCTL ioctl
    int bytes_available = 0;
#endif
    while (IOCTL(fd, FIONREAD, &bytes_available) == 0 && bytes_available > 0)
    {
        // We are guaranteed that we won't block, because bytes are available.
        // Read 1 byte. Extra bytes in the datagram will be discarded.
        char c;
        int result = recvfrom(fd, &c, sizeof(c), 0, NULL, NULL);
        if (result < 0)
        {
            break;
        }
    }
}

static THREAD_RETVAL
raop_ntp_thread(void *arg)
{
    raop_ntp_t *raop_ntp = arg;
    assert(raop_ntp);
    unsigned char response[128] = {0};
    int response_len = 0;
    unsigned char request[32] = {0x80, 0xd2, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    raop_ntp_data_t data_sorted[RAOP_NTP_DATA_COUNT];
    const unsigned  two_pow_n[RAOP_NTP_DATA_COUNT] = {2, 4, 8, 16, 32, 64, 128, 256};
    bool logger_debug = (logger_get_level(raop_ntp->logger) >= LOGGER_DEBUG);
    uint64_t recv_time = 0, client_ref_time = 0;

    while (1) {
        MUTEX_LOCK(raop_ntp->run_mutex);
        if (!raop_ntp->running) {
            MUTEX_UNLOCK(raop_ntp->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_ntp->run_mutex);

        // Flush the socket in case a super delayed response arrived or something
        raop_ntp_flush_socket(raop_ntp->tsock);

        // Send request
        uint64_t send_time = raop_ntp_get_local_time();
        byteutils_put_ntp_timestamp(request, 24, send_time);
        if (recv_time) {
            byteutils_put_long_be(request, 8, client_ref_time);
            byteutils_put_ntp_timestamp(request, 16, recv_time);
        }
        int send_len = sendto(raop_ntp->tsock, (char *)request, sizeof(request), 0,
                              (struct sockaddr *) &raop_ntp->remote_saddr, raop_ntp->remote_saddr_len);
        if (logger_debug) {
            char *str = utils_data_to_string(request, sizeof(request), 16);
            logger_log(raop_ntp->logger, LOGGER_DEBUG, "\nraop_ntp send time type_t=%d packetlen = %d, now = %8.6f\n%s",
                       request[1] &~0x80, sizeof(request), (double) send_time / SECOND_IN_NSECS, str);
            free(str);
        }
        if (send_len < 0) {
            int sock_err = SOCKET_GET_ERROR();
            logger_log(raop_ntp->logger, LOGGER_ERR, "raop_ntp error sending request. Error %d:%s",
                     sock_err, SOCKET_ERROR_STRING(sock_err));
        } else {
            // Read response
            response_len = recvfrom(raop_ntp->tsock, (char *)response, sizeof(response), 0, NULL, NULL);
            if (response_len < 0) {
                char time[30];
                ntp_timestamp_to_time(send_time, time, sizeof(time));
                logger_log(raop_ntp->logger, LOGGER_DEBUG , "raop_ntp receive timeout (request sent %s)", time);
	    } else {
                recv_time = raop_ntp_get_local_time();
                client_ref_time = byteutils_get_long_be(response, 24);
                if (!raop_ntp->client_time_received) {
                    raop_ntp->client_time_received = true;
                }
                //local time of the server when the NTP response packet returns
                int64_t t3 = (int64_t) recv_time;

                // Local time of the server when the NTP request packet leaves the server
                int64_t t0 = (int64_t) byteutils_get_ntp_timestamp(response, 8);

                // Local time of the client when the NTP request packet arrives at the client
                int64_t t1 = (int64_t) raop_remote_timestamp_to_nano_seconds(raop_ntp, byteutils_get_long_be(response, 16));

                // Local time of the client when the response message leaves the client
                int64_t t2 = (int64_t) raop_remote_timestamp_to_nano_seconds(raop_ntp, byteutils_get_long_be(response, 24));

                if (logger_debug) {
                    char *str = utils_data_to_string(response, response_len, 16);                   
                    logger_log(raop_ntp->logger, LOGGER_DEBUG,
                               "raop_ntp receive time type_t=%d packetlen = %d, now = %8.6f t1 = %8.6f, t2 = %8.6f\n%s",
                               response[1] &~0x80, response_len, (double) t3 / SECOND_IN_NSECS, (double) t1 / SECOND_IN_NSECS,
                               (double) t2 / SECOND_IN_NSECS, str); 
                    free(str);
                }
                // The iOS client device sends its time in  seconds relative to an arbitrary Epoch (the last boot).
                // For a little bonus confusion, they add SECONDS_FROM_1900_TO_1970.
                // This means we have to expect some rather huge offset, but its growth or shrink over time should be small.

                raop_ntp->data_index = (raop_ntp->data_index + 1) % RAOP_NTP_DATA_COUNT;
                raop_ntp->data[raop_ntp->data_index].time = t3;
                raop_ntp->data[raop_ntp->data_index].offset     = ((t1 - t0) + (t2 - t3)) / 2;
                raop_ntp->data[raop_ntp->data_index].delay      = ((t3 - t0) - (t2 - t1));
                raop_ntp->data[raop_ntp->data_index].dispersion = RAOP_NTP_R_RHO + RAOP_NTP_S_RHO +  (t3 - t0) * RAOP_NTP_PHI_PPM / SECOND_IN_NSECS;

                // Sort by delay
                memcpy(data_sorted, raop_ntp->data, sizeof(data_sorted));
                qsort(data_sorted, RAOP_NTP_DATA_COUNT, sizeof(data_sorted[0]), raop_ntp_compare);

                uint64_t dispersion = 0ull;
                int64_t offset = data_sorted[0].offset;
                int64_t delay = data_sorted[RAOP_NTP_DATA_COUNT - 1].delay;

                // Calculate dispersion
                for(int i = 0; i < RAOP_NTP_DATA_COUNT; ++i) {
                    unsigned long long disp = raop_ntp->data[i].dispersion + (t3 - raop_ntp->data[i].time) * RAOP_NTP_PHI_PPM / SECOND_IN_NSECS;
                    dispersion += disp / two_pow_n[i];
                }

                MUTEX_LOCK(raop_ntp->sync_params_mutex);

                int64_t correction = offset - raop_ntp->sync_offset;
                raop_ntp->sync_offset = offset;
                raop_ntp->sync_dispersion = dispersion;
                raop_ntp->sync_delay = delay;
                MUTEX_UNLOCK(raop_ntp->sync_params_mutex);

                logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp sync correction = %lld", correction);
            }
        }

        // Sleep for 3 seconds
        struct timespec wait_time;
        MUTEX_LOCK(raop_ntp->wait_mutex);
        clock_gettime(CLOCK_REALTIME, &wait_time);
        wait_time.tv_sec += 3;
        pthread_cond_timedwait(&raop_ntp->wait_cond, &raop_ntp->wait_mutex, &wait_time);
        MUTEX_UNLOCK(raop_ntp->wait_mutex);
    }

    // Ensure running reflects the actual state
    MUTEX_LOCK(raop_ntp->run_mutex);
    raop_ntp->running = false;
    MUTEX_UNLOCK(raop_ntp->run_mutex);

    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp exiting thread");
    return 0;
}

void
raop_ntp_start(raop_ntp_t *raop_ntp, unsigned short *timing_lport)
{
    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp starting time");
    int use_ipv6 = 0;

    assert(raop_ntp);
    assert(timing_lport);

    raop_ntp->timing_lport = *timing_lport;

    MUTEX_LOCK(raop_ntp->run_mutex);
    if (raop_ntp->running || !raop_ntp->joined) {
        MUTEX_UNLOCK(raop_ntp->run_mutex);
        return;
    }

    /* Initialize ports and sockets */
    if (raop_ntp->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    //use_ipv6 = 0;
    if (raop_ntp_init_socket(raop_ntp, use_ipv6) < 0) {
        logger_log(raop_ntp->logger, LOGGER_ERR, "raop_ntp initializing timing socket failed");
        MUTEX_UNLOCK(raop_ntp->run_mutex);
        return;
    }
    *timing_lport = raop_ntp->timing_lport;

    /* Create the thread and initialize running values */
    raop_ntp->running = 1;
    raop_ntp->joined = 0;
    
    THREAD_CREATE(raop_ntp->thread, raop_ntp_thread, raop_ntp);
    MUTEX_UNLOCK(raop_ntp->run_mutex);
}

void
raop_ntp_stop(raop_ntp_t *raop_ntp)
{
    assert(raop_ntp);

    /* Check that we are running and thread is not
     * joined (should never be while still running) */
    MUTEX_LOCK(raop_ntp->run_mutex);
    if (!raop_ntp->running || raop_ntp->joined) {
        MUTEX_UNLOCK(raop_ntp->run_mutex);
        return;
    }
    raop_ntp->running = 0;
    MUTEX_UNLOCK(raop_ntp->run_mutex);

    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp stopping time thread");

    MUTEX_LOCK(raop_ntp->wait_mutex);
    COND_SIGNAL(raop_ntp->wait_cond);
    MUTEX_UNLOCK(raop_ntp->wait_mutex);

    THREAD_JOIN(raop_ntp->thread);

    if (raop_ntp->tsock != -1) {
        closesocket(raop_ntp->tsock);
        raop_ntp->tsock = -1;
    }

    logger_log(raop_ntp->logger, LOGGER_DEBUG, "raop_ntp stopped time thread");

    /* Mark thread as joined */
    MUTEX_LOCK(raop_ntp->run_mutex);
    raop_ntp->joined = 1;
    MUTEX_UNLOCK(raop_ntp->run_mutex);
}

/**
 * Converts from a little endian ntp timestamp to nano seconds since the Unix epoch.
 * Does the same thing as byteutils_get_ntp_timestamp, except its input is an uint64_t
 * and expected to already be in little endian.
 * Please note this just converts to a different representation, the clock remains the
 * same.
 */
uint64_t raop_ntp_timestamp_to_nano_seconds(uint64_t ntp_timestamp, bool account_for_epoch_diff) {
    uint64_t seconds = ((ntp_timestamp >> 32) & 0xffffffff) - (account_for_epoch_diff ? SECONDS_FROM_1900_TO_1970 : 0);
    uint64_t fraction = (ntp_timestamp & 0xffffffff);
    return (seconds * SECOND_IN_NSECS) + ((fraction * SECOND_IN_NSECS) >> 32);
}

uint64_t raop_remote_timestamp_to_nano_seconds(raop_ntp_t *raop_ntp, uint64_t timestamp) {
    uint64_t seconds = ((timestamp >> 32) & 0xffffffff);
    if (raop_ntp->time_protocol == NTP) seconds -= SECONDS_FROM_1900_TO_1970;
    uint64_t fraction = (timestamp & 0xffffffff);
    return (seconds * SECOND_IN_NSECS) + ((fraction * SECOND_IN_NSECS) >> 32);
}
/**
 * Returns the current time in nano seconds according to the local wall clock.
 * The system Unix time is used as the local wall clock.
 */
uint64_t raop_ntp_get_local_time() {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return ((uint64_t) time.tv_nsec) + (uint64_t) time.tv_sec * SECOND_IN_NSECS;
}

/**
 * Returns the current time in nano seconds according to the remote wall clock.
 */
uint64_t raop_ntp_get_remote_time(raop_ntp_t *raop_ntp) {
    if  (!raop_ntp->client_time_received) {
        return 0;
    }
    MUTEX_LOCK(raop_ntp->sync_params_mutex);
    int64_t offset = raop_ntp->sync_offset;
    MUTEX_UNLOCK(raop_ntp->sync_params_mutex);
    return (uint64_t) ((int64_t) raop_ntp_get_local_time() + offset);
}

/**
 * Returns the local wall clock time in nano seconds for the given point in remote clock time
 */
uint64_t raop_ntp_convert_remote_time(raop_ntp_t *raop_ntp, uint64_t remote_time) {
    if  (!raop_ntp->client_time_received) {
        return 0;
    }
    MUTEX_LOCK(raop_ntp->sync_params_mutex);
    int64_t offset = raop_ntp->sync_offset;
    MUTEX_UNLOCK(raop_ntp->sync_params_mutex);
    return (uint64_t) ((int64_t) remote_time - offset);
}

/**
 * Returns the remote wall clock time in nano seconds for the given point in local clock time
 */
uint64_t raop_ntp_convert_local_time(raop_ntp_t *raop_ntp, uint64_t local_time) {
    if  (!raop_ntp->client_time_received) {
        return 0;
    }
    MUTEX_LOCK(raop_ntp->sync_params_mutex);
    int64_t offset = raop_ntp->sync_offset;
    MUTEX_UNLOCK(raop_ntp->sync_params_mutex);
    return (uint64_t) ((int64_t) local_time + offset);
}
