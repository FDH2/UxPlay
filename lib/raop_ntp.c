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
#include <inttypes.h>

#include "raop.h"
#include "threads.h"
#include "compat.h"
#include "netutils.h"
#include "byteutils.h"
#include "utils.h"

#define USEC_IN_NSECS   1000ULL
#define MSEC_IN_NSECS   1000000ULL
#define SECOND_IN_NSECS 1000000000ULL
#define SECOND_IN_USECS 1000000ULL

#define RAOP_NTP_DATA_COUNT   8
#define RAOP_NTP_PHI_PPM   15e-6                   // 15 PPM
#define RAOP_NTP_R_RHO     0.001                   // packet precision  1ms
#define RAOP_NTP_S_RHO     0.001                   // system clock precision  1ms
#define RAOP_NTP_MAX_DIST  1.5                     // maximum allowed distance  1.5 secs
#define RAOP_NTP_MAX_DISP  16.0                    // maximum dispersion    16 secs.

#define RAOP_NTP_CLOCK_BASE (2208988800ull << 32)

typedef struct raop_ntp_data_s {
    q32_32_t time; // The T4 timestamp  at time of ntp packet arrival
    double dispersion;
    double delay; // The round trip delay
    double offset; // The difference between (adjusted) remote and local wall clock time
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

    bool have_fixed_offset;
    q32_32_t fixed_offset;   // stored in Q32_32 fixed-point format
  
    // The clock sync params are periodically updated to the AirPlay client's NTP clock
    mutex_handle_t sync_params_mutex;
    int64_t sync_offset;
    int64_t sync_dispersion;
    int64_t sync_delay;
    kernel_timestamp_session_t *ntp_session;

    // Socket address of the AirPlay client
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    // The remote port of the NTP server on the AirPlay client
    unsigned short timing_rport;

    // The local port of the NTP client on the AirPlay server
    unsigned short timing_lport;

    timing_protocol_t time_protocol;
    bool client_time_received;

    uint64_t video_arrival_offset;
  
    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    // UDP socket
    int tsock;
};

/* code for recv with kernel timestamp */

#ifdef _WIN32
#ifndef WSA_CMSG_SPACE
#define WSA_CMSG_SPACE(len) (sizeof(struct cmsghdr) + (len))
#endif
static LARGE_INTEGER g_system_qpc_frequency =  {0};
#endif

// Helper to calculate signed difference between two Q32.32 time stamps
double ntp_diff_to_seconds(q32_32_t ntp_end, q32_32_t ntp_start) {
    int64_t signed_diff = (int64_t)(ntp_end - ntp_start);
    return (double)signed_diff / 4294967296.0;
}


void raop_ntp_global_init(void) {
#ifdef _WIN32
    QueryPerformanceFrequency(&g_system_qpc_frequency);
#endif
}

kernel_timestamp_session_t* kernel_timestamp_session_create(int sock_fd) {
    kernel_timestamp_session_t *session = (kernel_timestamp_session_t*)calloc(1, sizeof(kernel_timestamp_session_t));
    if (!session) {
        return NULL;
    }

    session->sock_fd = sock_fd;

#if defined(_WIN32)
    session->qpc_frequency = g_system_qpc_frequency.QuadPart;

    // Anchor this session's QPC baseline immediately at creation
    LARGE_INTEGER qpc_start;
    QueryPerformanceCounter(&qpc_start);
    session->base_qpc_ticks = qpc_start.QuadPart;
    
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t windows_ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    session->base_system_time_us = (windows_ticks - 116444736000000000ULL) / 10ULL;

#if defined(SIO_TIMESTAMPING)  //UCRT64 only, not available on MINGW64
    {
        SOCKET wsock = (SOCKET)sock_fd;
        GUID guid = WSAID_WSARECVMSG;
        DWORD bytes = 0;
        LPFN_WSARECVMSG local_pWSARecvMsg = NULL;

        if (WSAIoctl(wsock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
                     &local_pWSARecvMsg, sizeof(local_pWSARecvMsg), &bytes, NULL, NULL) != SOCKET_ERROR) {
            session->pWSARecvMsg_ptr = (void*)local_pWSARecvMsg;

            TIMESTAMPING_CONFIG config = { .Flags = TIMESTAMPING_FLAG_RX };
            DWORD bytes_returned = 0;

            if (WSAIoctl(wsock, SIO_TIMESTAMPING, &config, sizeof(config),
                         NULL, 0, &bytes_returned, NULL, NULL) == SOCKET_ERROR) {
                session ->pWSARecvMsg_ptr = NULL;
            }	  
        } else {
            session ->pWSARecvMsg_ptr = NULL;
        }
    }
#else
    // legacy MINGW64 fallback (no SIO_TIMEKEEPING kernel timestamping available)
    session->pWSARecvMsg_ptr = NULL;
#endif

#else
    // Non-Wndows POSIX path   (note: could instead use SO_TIMESTAMP_MONOTONIC on macOS)
    {
        int enable_ts = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMP, (const char*)&enable_ts, sizeof(enable_ts));
    }
#endif
    session->kernel_timestamp_timeout = raop_ntp_get_local_time();
    return session;
}

ssize_t kernel_timestamp_session_recv(kernel_timestamp_session_t *session, char *buf, size_t buf_len, 
                                      void *src_addr, int *addrlen, uint64_t *recv_time_kernel, uint64_t *recv_time_clock) {
    if (!session || !buf || buf_len == 0 || !recv_time_kernel || !recv_time_clock) return -1;
    *recv_time_kernel = 0;
    *recv_time_clock = 0;
#ifdef _WIN32
    {
        bool attempt_fallback = true;
#if defined(SIO_TIMESTAMPING) //not defined in legacy MSVCRT systems such as MSYS2 MINGW64
        LPFN_WSARECVMSG pWSARecvMsg = (LPFN_WSARECVMSG)session->pWSARecvMsg_ptr;
        if (pWSARecvMsg != NULL) {
            // Union forces strict compiler alignment bounds for Windows control frames
            union {
                char buf[WSA_CMSG_SPACE(sizeof(UINT64)) + 32];
                struct cmsghdr align;
            } control_un;

            struct sockaddr_storage win_remote_addr = {0};
            INT win_addr_len = sizeof(win_remote_addr);

            WSABUF wsa_buf = {
                .len = (ULONG)buf_len,
                .buf = buf
            };

            WSAMSG wsa_msg = {
                .name = (LPSOCKADDR)&win_remote_addr,
                .namelen = win_addr_len,
                .lpBuffers = &wsa_buf,
                .dwBufferCount = 1,
                .Control.len = sizeof(control_un.buf),
                .Control.buf = control_un.buf
            };

            DWORD bytes_received = 0;
            int sock_err;

            attempt_fallback = false;
            if (pWSARecvMsg((SOCKET)session->sock_fd, &wsa_msg, &bytes_received, NULL, NULL) != SOCKET_ERROR) {
                LARGE_INTEGER qpc_now;
                int64_t default_elapsed_ticks;
                PCMSGHDR cmsg;

                QueryPerformanceCounter(&qpc_now);
                default_elapsed_ticks  = qpc_now.QuadPart - session->base_qpc_ticks;
                *recv_time_clock = (session->base_system_time_us + ((default_elapsed_ticks * 1000000LL) / session->qpc_frequency)) * USEC_IN_NSECS ;

                cmsg = WSA_CMSG_FIRSTHDR(&wsa_msg);
                while (cmsg != NULL) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMP) {
                        UINT64 packet_qpc_ticks = *(UINT64*)WSA_CMSG_DATA(cmsg);
                        if (packet_qpc_ticks > (UINT64)session->base_qpc_ticks) {
                            int64_t packet_elapsed_ticks = (int64_t)packet_qpc_ticks - session->base_qpc_ticks;
                            *recv_time_kernel = (session->base_system_time_us + ((packet_elapsed_ticks * 1000000LL) / session->qpc_frequency)) * USEC_IN_NSECS;
                        }
                        break;
                    }
                    cmsg = WSA_CMSG_NXTHDR(&wsa_msg, cmsg);
                }

                if (src_addr && addrlen) {
                    int copy_len = (wsa_msg.namelen < *addrlen) ? wsa_msg.namelen : *addrlen;
                    memcpy(src_addr, wsa_msg.name, copy_len);
                    *addrlen = wsa_msg.namelen;
                }

                return (ssize_t)bytes_received;
            }

            int sock_err = SOCKET_GET_ERROR();	
            switch (sock_err) {
            case WSATIMEDOUT:
            case WSAECONNRESET:
            case WSAENETRESET:
            case WSACONNABORTED:
                return -1; //Routine network exceptions, exit directly
            default:
                //Unexpected for correctly opened-socket, treat as permanent failure and use fallback
                logger_log(raop_ntp->logger, LOGGER_ERR, "Disabling kernel timestamps due to socket error %d:%s",
                           sock_err, SOCKET_ERROR_STRING(sock_err));
                session->pWSARecvMsg_ptr = NULL;
                attempt_fallback = true;
                break;
            }
        }
#endif
        //Windows fallback path if kernel timestamp could not be extracted; also used on MINGW64 systems
        if (attempt_fallback) {
            int from_len = (src_addr && addrlen) ? *addrlen : sizeof(struct sockaddr_storage);
            struct sockaddr_storage fallback_addr = {0};

            ssize_t n = recvfrom((SOCKET)session->sock_fd, buf, (int)buf_len, 0, 
                                 src_addr ? (struct sockaddr*)src_addr : (struct sockaddr*)&fallback_addr, &from_len);
            if (n >= 0) {
                LARGE_INTEGER qpc_now;
                int64_t elapsed_ticks;
                if (addrlen) {
                    *addrlen = from_len;
                }
                QueryPerformanceCounter(&qpc_now);
                elapsed_ticks = qpc_now.QuadPart - session->base_qpc_ticks;
                *recv_time_clock = (session->base_system_time_us + ((elapsed_ticks * 1000000LL) / session->qpc_frequency)) * USEC_IN_NSECS;
            }
            return n;
        }
        return -1;
    }
#else // non-Windows POSIX path
    {
        struct sockaddr_storage remote_addr = {0};
        struct iovec iov = { .iov_base = buf, .iov_len = buf_len };
    
        // Allocate a union to guarantee strict alignment requirements for the buffer
        union {
            char buf[CMSG_SPACE(sizeof(struct timeval)) + 64]; 
            struct cmsghdr align;
        } control_un;
    
        struct msghdr msg = {
            .msg_name = &remote_addr,
            .msg_namelen = sizeof(remote_addr),
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control_un.buf,
            .msg_controllen = sizeof(control_un.buf)
        };

        ssize_t n = recvmsg(session->sock_fd, &msg, 0);
        if (n < 0) {
            return n;
        }

        // immediate fallback time 
        *recv_time_clock = raop_ntp_get_local_time();

        if (!(msg.msg_flags & MSG_CTRUNC)) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMP) {
                    struct timeval *tv_kernel = (struct timeval *)CMSG_DATA(cmsg);
                    *recv_time_kernel = ((uint64_t)tv_kernel->tv_sec * SECOND_IN_NSECS) + (uint64_t)tv_kernel->tv_usec * USEC_IN_NSECS;
                    break;
                }
            }
        }

        if (src_addr && addrlen) {
            int copy_len = ((int)msg.msg_namelen < *addrlen) ? (int)msg.msg_namelen : *addrlen;
            memcpy(src_addr, msg.msg_name, copy_len);
            *addrlen = (int)msg.msg_namelen; 
        }

        return n;
    }
#endif
}

void kernel_timestamp_session_destroy(kernel_timestamp_session_t *ntp_session) {
    if (ntp_session) {
        CLOSESOCKET(ntp_session->sock_fd);
        free(ntp_session);
    }
}

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
    raop_ntp->have_fixed_offset = false;
    raop_ntp->fixed_offset = 0;

    
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

    raop_ntp->ntp_session = kernel_timestamp_session_create(tsock);
    if (raop_ntp->ntp_session == NULL) {
        logger_log(raop_ntp->logger, LOGGER_ERR, "raop_ntp: Failed to allocate high-precision session context");
        goto sockets_cleanup;
    }
    
    // We're calling recvfrom without knowing whether there is any data, so we need a timeout
    uint32_t recv_timeout_msec = 300; 
#ifdef _WIN32
    DWORD tv  = recv_timeout_msec;
#define CAST (char *)    
#else
    struct timeval tv;
    tv.tv_sec = recv_timeout_msec / (uint32_t) 1000;
    tv.tv_usec = ((uint32_t) 1000) * (recv_timeout_msec % (uint32_t) 1000);
#define CAST
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
    if (raop_ntp->ntp_session != NULL) {
        kernel_timestamp_session_destroy(raop_ntp->ntp_session);
        raop_ntp->ntp_session = NULL;
    } else if (tsock != -1) {
        // Fallback to protect if the handle crashed out before session instantiation
        CLOSESOCKET(tsock);
    }
    return -1;
}

static void
raop_ntp_flush_socket(int fd)
{
#ifdef _WIN32
    u_long bytes_available = 0;
#else
    int bytes_available = 0;
#endif
    while (IOCTLSOCKET(fd, FIONREAD, &bytes_available) == 0 && bytes_available > 0)
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
    const double two_pow_minus_n[RAOP_NTP_DATA_COUNT] = {1.0 / 2.0,  1.0 / 4.0, 1.0 / 8.0, 1.0 / 16.0, 1.0 / 32.0,
                                                         1.0 / 64.0, 1.0 / 128.0, 1.0 / 256.0};
    bool logger_debug = (logger_get_level(raop_ntp->logger) >= LOGGER_DEBUG);
    uint64_t recv_time = 0;

    bool duplicate_ntp_packet = false;
    bool bogus_ntp_packet = false;
    
    /* these are NTP timestamps in Q32.32 form */
    q32_32_t t1_sent = 0, t3_prev = 0, t2_raw = 0, t3_raw = 0;
    q32_32_t t1 = 0, t2 = 0, t3 = 0, t4 = 0;
        
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
	t1_sent = (q32_32_t) byteutils_get_long_be(request, 24);
        int send_len = sendto(raop_ntp->tsock, (char *)request, sizeof(request), 0,
                              (struct sockaddr *) &raop_ntp->remote_saddr, raop_ntp->remote_saddr_len);
        if (logger_debug) {
            char *str = utils_data_to_string(request, sizeof(request), 16);
            logger_log(raop_ntp->logger, LOGGER_DEBUG, "\nraop_ntp send time type_t=%d packetlen = %d, now = %8.6f\n%s",
                       request[1] &~0x80, (int) sizeof(request), (double) send_time / SECOND_IN_NSECS, str);
            free(str);
        }
        if (send_len < 0) {
            int sock_err = SOCKET_GET_ERROR();
            logger_log(raop_ntp->logger, LOGGER_ERR, "raop_ntp error sending request. Error %d:%s",
                     sock_err, SOCKET_ERROR_STRING(sock_err));
        } else {
            // Read response
            uint64_t recv_time_kernel;   // kernel recv timestamp in microsecs * USEC_IN_NSECS
	    uint64_t recv_time_clock;    // userspace recv timestamp in microsecs * USEC_IN_NSECS
            response_len = kernel_timestamp_session_recv(raop_ntp->ntp_session, (char *) response, sizeof(response),
                                                         NULL, NULL, &recv_time_kernel, &recv_time_clock);
	    uint64_t localtime = raop_ntp_get_local_time();

	    if (recv_time_kernel && recv_time_clock) {
                printf("===timestamp timeout  ===%llu\n\n", (unsigned long long) raop_ntp->ntp_session->kernel_timestamp_timeout);
                /* sanity check on kernel timestamp */
                bool sanity = ((int64_t) (recv_time_kernel - send_time) > 0  && (int64_t) (recv_time_clock - recv_time_kernel) > 0); 
                if (!sanity) {
                    raop_ntp->ntp_session->kernel_timestamp_timeout = localtime + ((uint64_t) 16) * SECOND_IN_NSECS;
                }
                if (recv_time_kernel && (int64_t) (recv_time_clock - raop_ntp->ntp_session->kernel_timestamp_timeout) > 0) {
                    recv_time = recv_time_kernel;
                } else {
                    printf("ignoring recv_time_kernel\n");
                    recv_time = recv_time_clock;
                }
   	        printf("===send time (clock)  ===%llu\n", (unsigned long long) send_time);
                printf("===recv time (kernel) ===%llu\n", (unsigned long long) recv_time_kernel);
                printf("===recv time (clock)  ===%llu\n", (unsigned long long) recv_time_clock);
                printf("===recv time          ===%llu\n\n", (unsigned long long) recv_time);	    
            } else if (recv_time_clock) {
                recv_time = recv_time_clock;
            } else {
                recv_time = localtime;
            }

	    if (response_len < 32) {
                char time[30];
                ntp_timestamp_to_time(send_time, time, sizeof(time));
                if (response_len < 0) {
                    logger_log(raop_ntp->logger, LOGGER_DEBUG , "raop_ntp received timeout (request sent %s)", time);
                } else {
                    logger_log(raop_ntp->logger, LOGGER_ERR , "raop_ntp received truncated response (request sent %s, response_len %d < 32)", time, response_len);
                }
            } else {
		// The iOS client device sends its time in  seconds relative to an arbitrary Epoch (the last boot).
                // For a little bonus confusion, they add SECONDS_FROM_1900_TO_1970.
                // To avoid huge offsets, we adjust all remote timestamps (in raw Q32.32 fixed point format) by a fixed offset
                // raop_ntp->fixed offset, determined when the NTP thread receives its first client time signal.
	      
                // Local time of the server when the NTP request packet left the server  (should equal t1_sent) */
                t1 = (q32_32_t) byteutils_get_long_be(response, 8);

	        // Local timestamp of the client when the NTP request packet arrived at the client
                t2_raw = (q32_32_t) byteutils_get_long_be(response, 16);
		/* now adjust t2 to reset client epoch */
                t2 = raop_ntp_adjust_remote_timestamp_offset(raop_ntp, t2_raw);

	        // Local timestamp of the client when the response message left the client  (should not equal t3_prev)
                t3_raw = (q32_32_t) byteutils_get_long_be(response, 24);
		byteutils_put_long_be(request, 8, (uint32_t) t3_raw);   //this is returned to client as client_ref
		/* now adjust t3 to reset client epoch */
		t3 = raop_ntp_adjust_remote_timestamp_offset(raop_ntp, t3_raw);

	        // local timestamp of the server when the response message arrived at the server  
                byteutils_put_ntp_timestamp(request, 16, recv_time);
		t4 = (q32_32_t) byteutils_get_long_be(request, 16);		
		     
                if (logger_debug) {
                    char *str = utils_data_to_string(response, response_len, 16);                   
                    logger_log(raop_ntp->logger, LOGGER_DEBUG,
                               "raop_ntp type_t=%d packetlen = %d\nsent request:      %8.6f\nreceived response: %8.6f"
                               "\nt1 = %" PRIu32 "\nt2 = %" PRIu32 " (%" PRIu32 ")\nt3 = %" PRIu32 " (%" PRIu32 ")\nt4 = %" PRIu32 "\n%s",
                               response[1] &~0x80, response_len,  (double) send_time / SECOND_IN_NSECS, (double) recv_time / SECOND_IN_NSECS,
                               t1, t2, t2_raw, t3, t3_raw, t4, str); 
                    free(str);
                }

		bogus_ntp_packet = (t1 != t1_sent); 
		if (bogus_ntp_packet) {
                logger_log(raop_ntp->logger, LOGGER_INFO , "raop_ntp received NTP packet with invalid server ref: %ull %ull",
                          (unsigned long long) t1_sent, (unsigned long long) t1);
                }

		duplicate_ntp_packet = (t3_raw == t3_prev);
		if (duplicate_ntp_packet) {
                     logger_log(raop_ntp->logger, LOGGER_INFO , "raop_ntp received NTP packet with duplicate t3: %ull %ull",
				(unsigned long long) t3_prev, (unsigned long long) t3_raw);
                }
		t3_prev = t3_raw;

                if (!bogus_ntp_packet && !duplicate_ntp_packet) {
                    /* ntp metrics as seconds (as doubles) */
                    double total_time = ntp_diff_to_seconds(t4,t1);
                    double client_process_time = ntp_diff_to_seconds(t3,t2);
                    double client_to_server = ntp_diff_to_seconds(t2,t1);
                    double server_to_client = ntp_diff_to_seconds(t3,t4);

                    raop_ntp->data_index = (raop_ntp->data_index + 1) % RAOP_NTP_DATA_COUNT;
                    raop_ntp->data[raop_ntp->data_index].time = t4; 
                    raop_ntp->data[raop_ntp->data_index].offset = (client_to_server + server_to_client) /2.0;
                    raop_ntp->data[raop_ntp->data_index].delay  = total_time - client_process_time;
                    raop_ntp->data[raop_ntp->data_index].dispersion = RAOP_NTP_R_RHO + RAOP_NTP_S_RHO + (RAOP_NTP_PHI_PPM  * ntp_diff_to_seconds(t4, t1)); 

                    // Sort by delay
                    memcpy(data_sorted, raop_ntp->data, sizeof(data_sorted));
                    qsort(data_sorted, RAOP_NTP_DATA_COUNT, sizeof(data_sorted[0]), raop_ntp_compare);

                    double dispersion = 0.0;
                    double offset = data_sorted[0].offset;   // take offset from the BEST packet (least delay) in the window
                    double delay = data_sorted[0].delay;     // also take delay from that packet 

                    for(int i = 0; i < RAOP_NTP_DATA_COUNT; ++i) {
                        //skip placeholder slots that have not received real data
                        if (raop_ntp->data[i].delay == RAOP_NTP_MAX_DISP) {
                            continue;
                        }
                        double disp = raop_ntp->data[i].dispersion + ntp_diff_to_seconds(t4, raop_ntp->data[i].time) * RAOP_NTP_PHI_PPM ; 
                        int age = (raop_ntp->data_index - i + RAOP_NTP_DATA_COUNT) % RAOP_NTP_DATA_COUNT;
                        dispersion += disp * two_pow_minus_n[age];
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

    if (raop_ntp->ntp_session != NULL) {
        kernel_timestamp_session_destroy(raop_ntp->ntp_session);
        raop_ntp->ntp_session = NULL;
        raop_ntp->tsock = -1; 
    } else if (raop_ntp->tsock != -1) {
        CLOSESOCKET(raop_ntp->tsock);
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
    uint64_t seconds = (ntp_timestamp >> 32) - (account_for_epoch_diff ? SECONDS_FROM_1900_TO_1970 : 0);
    uint64_t fraction = (ntp_timestamp & 0xffffffff);
    return (seconds * SECOND_IN_NSECS) + ((fraction * SECOND_IN_NSECS) >> 32);
}

uint64_t raop_remote_timestamp_to_nano_seconds(raop_ntp_t *raop_ntp, uint64_t timestamp) {
    uint64_t seconds = (timestamp >> 32);
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

// adjust a client raw timestamp by the fixed Q32.32 offset
q32_32_t raop_ntp_adjust_remote_timestamp_offset(raop_ntp_t *raop_ntp, q32_32_t ntp_timestamp_raw) {
    MUTEX_LOCK(raop_ntp->sync_params_mutex);
    if (!raop_ntp->have_fixed_offset) {
        unsigned char request[8] = {0};
        uint64_t local_time = raop_ntp_get_local_time();
        byteutils_put_ntp_timestamp(request, 0, local_time);
	q32_32_t local_ref_time = byteutils_get_long_be(request,0);      
        raop_ntp->fixed_offset = local_ref_time - ntp_timestamp_raw;
        raop_ntp->have_fixed_offset = true;
    }
    MUTEX_UNLOCK(raop_ntp->sync_params_mutex);
    return ntp_timestamp_raw + raop_ntp->fixed_offset;
}
