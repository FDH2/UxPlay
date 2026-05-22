/**
 *  Copyright (C) 2026  kgbook
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "mdnsd.h"

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define MDNS_MAX_PACKET 1500
#define MDNS_TTL_HOST 120

#define DNS_TYPE_A 1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1
#define DNS_CACHE_FLUSH 0x8000
#define DNS_UNICAST_RESPONSE 0x8000

typedef struct {
    unsigned char bytes[MDNS_MAX_PACKET];
    size_t length;
} mdns_packet_t;

struct mdnsd_s {
    char host_name[MDNSD_MAX_NAME];
    mdnsd_service_t airplay;
    mdnsd_service_t raop;

    uint32_t ipv4_addr;
    int sock_fd;
    int running;
    thread_handle_t thread;
    mutex_handle_t mutex;
};

static int mdns_put_u8(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 1 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) value;
    return 0;
}

static int mdns_put_u16(mdns_packet_t *packet, unsigned int value)
{
    if (packet->length + 2 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_u32(mdns_packet_t *packet, uint32_t value)
{
    if (packet->length + 4 > sizeof(packet->bytes)) {
        return -1;
    }
    packet->bytes[packet->length++] = (unsigned char) ((value >> 24) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 16) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) ((value >> 8) & 0xff);
    packet->bytes[packet->length++] = (unsigned char) (value & 0xff);
    return 0;
}

static int mdns_put_bytes(mdns_packet_t *packet, const void *bytes, size_t length)
{
    if (packet->length + length > sizeof(packet->bytes)) {
        return -1;
    }
    memcpy(packet->bytes + packet->length, bytes, length);
    packet->length += length;
    return 0;
}

static int mdns_put_name(mdns_packet_t *packet, const char *name)
{
    const char *label = name;

    while (label && *label) {
        const char *dot = strchr(label, '.');
        size_t length = dot ? (size_t) (dot - label) : strlen(label);

        if (length > 63 || mdns_put_u8(packet, (unsigned int) length)) {
            return -1;
        }
        if (mdns_put_bytes(packet, label, length)) {
            return -1;
        }
        if (!dot) {
            break;
        }
        label = dot + 1;
    }

    return mdns_put_u8(packet, 0);
}

static int mdns_begin_rr(mdns_packet_t *packet, const char *name, unsigned int type,
                         unsigned int cls, uint32_t ttl, size_t *rdlength_pos)
{
    if (mdns_put_name(packet, name) ||
        mdns_put_u16(packet, type) ||
        mdns_put_u16(packet, cls) ||
        mdns_put_u32(packet, ttl)) {
        return -1;
    }
    *rdlength_pos = packet->length;
    return mdns_put_u16(packet, 0);
}

static void mdns_end_rr(mdns_packet_t *packet, size_t rdlength_pos)
{
    size_t rdlength = packet->length - rdlength_pos - 2;
    packet->bytes[rdlength_pos] = (unsigned char) ((rdlength >> 8) & 0xff);
    packet->bytes[rdlength_pos + 1] = (unsigned char) (rdlength & 0xff);
}

static int mdns_add_ptr(mdns_packet_t *packet, const char *name, const char *ptr,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_PTR, DNS_CLASS_IN, ttl, &rdlength_pos) ||
        mdns_put_name(packet, ptr)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_srv(mdns_packet_t *packet, const char *name, unsigned short port,
                        const char *target, uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_SRV, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, port) ||
        mdns_put_name(packet, target)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_txt(mdns_packet_t *packet, const char *name, const mdnsd_txt_t *txt,
                        uint32_t ttl)
{
    size_t rdlength_pos;
    if (mdns_begin_rr(packet, name, DNS_TYPE_TXT, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, txt->bytes, (size_t) txt->length)) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static int mdns_add_a(mdns_packet_t *packet, const char *name, uint32_t addr,
                      uint32_t ttl)
{
    size_t rdlength_pos;
    if (!addr) {
        return 0;
    }
    if (mdns_begin_rr(packet, name, DNS_TYPE_A, DNS_CLASS_IN | DNS_CACHE_FLUSH,
                      ttl, &rdlength_pos) ||
        mdns_put_bytes(packet, &addr, sizeof(addr))) {
        return -1;
    }
    mdns_end_rr(packet, rdlength_pos);
    return 0;
}

static uint16_t mdns_read_u16(const unsigned char *bytes)
{
    return (uint16_t) (((uint16_t) bytes[0] << 8) | bytes[1]);
}

static int mdns_read_name(const unsigned char *packet, size_t packet_len,
                          size_t *offset, char *name, size_t name_len)
{
    size_t pos = *offset;
    size_t out = 0;
    int jumped = 0;
    int jumps = 0;

    while (pos < packet_len) {
        unsigned int length = packet[pos++];

        if (length == 0) {
            if (!jumped) {
                *offset = pos;
            }
            if (out == 0) {
                if (name_len < 2) {
                    return -1;
                }
                name[out++] = '.';
            }
            name[out] = '\0';
            return 0;
        }

        if ((length & 0xc0) == 0xc0) {
            unsigned int ptr;
            if (pos >= packet_len || ++jumps > 16) {
                return -1;
            }
            ptr = ((length & 0x3f) << 8) | packet[pos++];
            if (ptr >= packet_len) {
                return -1;
            }
            if (!jumped) {
                *offset = pos;
            }
            pos = ptr;
            jumped = 1;
            continue;
        }

        if ((length & 0xc0) || pos + length > packet_len) {
            return -1;
        }
        if (out) {
            if (out + 1 >= name_len) {
                return -1;
            }
            name[out++] = '.';
        }
        if (out + length >= name_len) {
            return -1;
        }
        memcpy(name + out, packet + pos, length);
        out += length;
        pos += length;
    }

    return -1;
}

static int mdns_name_equals(const char *left, const char *right)
{
    while (*left && *right) {
        if (tolower((unsigned char) *left) != tolower((unsigned char) *right)) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int mdns_query_type_matches(uint16_t query_type, uint16_t record_type)
{
    return query_type == record_type || query_type == DNS_TYPE_ANY;
}

static uint32_t mdns_get_default_ipv4(void)
{
    int fd;
    uint32_t addr = 0;
    struct sockaddr_in remote;
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return 0;
    }

    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDR, &remote.sin_addr);

    if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) == 0 &&
        getsockname(fd, (struct sockaddr *) &local, &local_len) == 0) {
        addr = local.sin_addr.s_addr;
    }

    CLOSESOCKET(fd);
    return addr;
}

static int mdns_open_socket(uint32_t iface_addr)
{
    int fd;
    int one = 1;
    unsigned char ttl = 255;
    unsigned char loop = 1;
    struct sockaddr_in addr;
    struct ip_mreq mreq;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        return -SOCKET_GET_ERROR();
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));

    if (iface_addr) {
        struct in_addr iface;
        iface.s_addr = iface_addr;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &iface, sizeof(iface));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        int error = SOCKET_GET_ERROR();
        CLOSESOCKET(fd);
        return -error;
    }

    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, MDNS_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = iface_addr ? iface_addr : htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char *) &mreq, sizeof(mreq)) == -1) {
        int error = SOCKET_GET_ERROR();
        if (iface_addr) {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           (const char *) &mreq, sizeof(mreq)) == 0) {
                return fd;
            }
        }
        CLOSESOCKET(fd);
        return -error;
    }

    return fd;
}

static int mdns_begin_response(mdns_packet_t *packet, size_t *count_pos)
{
    memset(packet, 0, sizeof(*packet));
    if (mdns_put_u16(packet, 0) ||
        mdns_put_u16(packet, 0x8400) ||
        mdns_put_u16(packet, 0)) {
        return -1;
    }
    *count_pos = packet->length;
    return mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0) ||
           mdns_put_u16(packet, 0);
}

static void mdns_finish_response(mdns_packet_t *packet, size_t count_pos,
                                 unsigned int answers)
{
    packet->bytes[count_pos] = (unsigned char) ((answers >> 8) & 0xff);
    packet->bytes[count_pos + 1] = (unsigned char) (answers & 0xff);
}

static int mdns_add_service_records(mdns_packet_t *packet,
                                    const mdnsd_service_t *service,
                                    const char *host_name, uint32_t ttl)
{
    if (!service->registered) {
        return 0;
    }

    return mdns_add_ptr(packet, "_services._dns-sd._udp.local", service->type, ttl) ||
           mdns_add_ptr(packet, service->type, service->name, ttl) ||
           mdns_add_srv(packet, service->name, service->port, host_name,
                        ttl ? MDNS_TTL_HOST : 0) ||
           mdns_add_txt(packet, service->name, &service->txt, ttl);
}

static int mdns_build_response(mdnsd_t *mdnsd, mdns_packet_t *packet,
                               const mdnsd_service_t *service,
                               int include_host, uint32_t ttl)
{
    size_t count_pos;
    unsigned int answers = 0;
    size_t before;

    if (mdns_begin_response(packet, &count_pos)) {
        return -1;
    }

    if (service && service->registered) {
        before = packet->length;
        if (mdns_add_service_records(packet, service, mdnsd->host_name, ttl)) {
            return -1;
        }
        if (packet->length != before) {
            answers += 4;
        }
    }

    if (include_host && mdns_add_a(packet, mdnsd->host_name, mdnsd->ipv4_addr,
                                   ttl ? MDNS_TTL_HOST : 0)) {
        return -1;
    }
    if (include_host && mdnsd->ipv4_addr) {
        answers++;
    }

    mdns_finish_response(packet, count_pos, answers);
    return answers ? 0 : -1;
}

static void mdns_send_packet(mdnsd_t *mdnsd, const mdns_packet_t *packet,
                             const struct sockaddr_in *to)
{
    struct sockaddr_in addr;

    if (mdnsd->sock_fd == -1 || !packet->length) {
        return;
    }

    if (to) {
        addr = *to;
    } else {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(MDNS_PORT);
        inet_pton(AF_INET, MDNS_ADDR, &addr.sin_addr);
    }

    sendto(mdnsd->sock_fd, (const char *) packet->bytes, (int) packet->length, 0,
           (struct sockaddr *) &addr, sizeof(addr));
}

static void mdns_send_service_locked(mdnsd_t *mdnsd, const mdnsd_service_t *service,
                                     int include_host, uint32_t ttl,
                                     const struct sockaddr_in *to)
{
    mdns_packet_t packet;

    if (service->registered &&
        !mdns_build_response(mdnsd, &packet, service, include_host, ttl)) {
        mdns_send_packet(mdnsd, &packet, to);
    }
}

static void mdns_send_response_locked(mdnsd_t *mdnsd, int include_airplay,
                                      int include_raop, int include_host,
                                      uint32_t ttl, const struct sockaddr_in *to)
{
    mdns_packet_t packet;

    if (include_airplay) {
        mdns_send_service_locked(mdnsd, &mdnsd->airplay, include_host, ttl, to);
    }
    if (include_raop) {
        mdns_send_service_locked(mdnsd, &mdnsd->raop, include_host, ttl, to);
    }
    if (include_host && !include_airplay && !include_raop &&
        !mdns_build_response(mdnsd, &packet, NULL, 1, ttl)) {
        mdns_send_packet(mdnsd, &packet, to);
    }
}

static void mdns_announce_locked(mdnsd_t *mdnsd, uint32_t ttl)
{
    mdns_send_response_locked(mdnsd, 1, 1, 1, ttl, NULL);
}

static int mdns_question_matches_service(const mdnsd_t *mdnsd, const char *name,
                                         uint16_t type, int *airplay, int *raop,
                                         int *host)
{
    if (mdns_name_equals(name, "_services._dns-sd._udp.local") &&
        mdns_query_type_matches(type, DNS_TYPE_PTR)) {
        if (mdnsd->airplay.registered) {
            *airplay = 1;
        }
        if (mdnsd->raop.registered) {
            *raop = 1;
        }
        return *airplay || *raop;
    }

    if (mdnsd->airplay.registered &&
        ((mdns_name_equals(name, mdnsd->airplay.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->airplay.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *airplay = 1;
        *host = 1;
        return 1;
    }

    if (mdnsd->raop.registered &&
        ((mdns_name_equals(name, mdnsd->raop.type) &&
          mdns_query_type_matches(type, DNS_TYPE_PTR)) ||
         (mdns_name_equals(name, mdnsd->raop.name) &&
          (mdns_query_type_matches(type, DNS_TYPE_TXT) ||
           mdns_query_type_matches(type, DNS_TYPE_SRV))))) {
        *raop = 1;
        *host = 1;
        return 1;
    }

    if (mdns_name_equals(name, mdnsd->host_name) &&
        mdns_query_type_matches(type, DNS_TYPE_A)) {
        *host = 1;
        return 1;
    }

    return 0;
}

static void mdns_handle_query(mdnsd_t *mdnsd, const unsigned char *bytes,
                              size_t length, const struct sockaddr_in *from)
{
    uint16_t flags;
    uint16_t qdcount;
    size_t offset = 12;
    int include_airplay = 0;
    int include_raop = 0;
    int include_host = 0;
    int unicast_reply = 0;

    if (length < 12) {
        return;
    }

    flags = mdns_read_u16(bytes + 2);
    if (flags & 0x8000) {
        return;
    }

    qdcount = mdns_read_u16(bytes + 4);

    MUTEX_LOCK(mdnsd->mutex);
    for (uint16_t i = 0; i < qdcount; i++) {
        char name[MDNSD_MAX_NAME];
        uint16_t type;
        uint16_t cls;

        if (mdns_read_name(bytes, length, &offset, name, sizeof(name)) ||
            offset + 4 > length) {
            break;
        }
        type = mdns_read_u16(bytes + offset);
        cls = mdns_read_u16(bytes + offset + 2);
        offset += 4;

        if (cls & DNS_UNICAST_RESPONSE) {
            unicast_reply = 1;
        }
        mdns_question_matches_service(mdnsd, name, type, &include_airplay,
                                      &include_raop, &include_host);
    }

    if (include_airplay || include_raop || include_host) {
        mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                  include_host, MDNSD_TTL_SERVICE, NULL);
        if (from && (unicast_reply || from->sin_port != htons(MDNS_PORT))) {
            mdns_send_response_locked(mdnsd, include_airplay, include_raop,
                                      include_host, MDNSD_TTL_SERVICE, from);
        }
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

static THREAD_RETVAL mdns_thread(void *arg)
{
    mdnsd_t *mdnsd = arg;

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int fd;
        int running;
        int ret;

        MUTEX_LOCK(mdnsd->mutex);
        fd = mdnsd->sock_fd;
        running = mdnsd->running;
        MUTEX_UNLOCK(mdnsd->mutex);

        if (!running || fd == -1) {
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            unsigned char buffer[MDNS_MAX_PACKET];
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int received = (int) recvfrom(fd, (char *) buffer, sizeof(buffer), 0,
                                          (struct sockaddr *) &from, &from_len);
            if (received > 0) {
                mdns_handle_query(mdnsd, buffer, (size_t) received, &from);
            }
        }
    }

    return NULL;
}

int mdnsd_txt_add(mdnsd_txt_t *txt, const char *key, const char *value)
{
    char item[256];
    int length = snprintf(item, sizeof(item), "%s=%s", key, value ? value : "");

    if (length < 0 || length > 255 ||
        txt->length + length + 1 > (int) sizeof(txt->bytes)) {
        return -1;
    }

    txt->bytes[txt->length++] = (unsigned char) length;
    memcpy(txt->bytes + txt->length, item, (size_t) length);
    txt->length += length;
    return 0;
}

mdnsd_t *mdnsd_init(const char *host_name)
{
    mdnsd_t *mdnsd = calloc(1, sizeof(*mdnsd));
    if (!mdnsd) {
        return NULL;
    }

    snprintf(mdnsd->host_name, sizeof(mdnsd->host_name), "%s",
             host_name && *host_name ? host_name : "UxPlay.local");
    mdnsd->sock_fd = -1;
    MUTEX_CREATE(mdnsd->mutex);
    return mdnsd;
}

void mdnsd_destroy(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return;
    }

    mdnsd_stop(mdnsd);
    MUTEX_DESTROY(mdnsd->mutex);
    free(mdnsd);
}

int mdnsd_start(mdnsd_t *mdnsd)
{
    if (!mdnsd) {
        return -1;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        MUTEX_UNLOCK(mdnsd->mutex);
        return 0;
    }

    mdnsd->ipv4_addr = mdns_get_default_ipv4();
    mdnsd->sock_fd = mdns_open_socket(mdnsd->ipv4_addr);
    if (mdnsd->sock_fd < 0) {
        int error = mdnsd->sock_fd;
        mdnsd->sock_fd = -1;
        MUTEX_UNLOCK(mdnsd->mutex);
        return error;
    }

    mdnsd->running = 1;
    THREAD_CREATE(mdnsd->thread, mdns_thread, mdnsd);
    if (!mdnsd->thread) {
        CLOSESOCKET(mdnsd->sock_fd);
        mdnsd->sock_fd = -1;
        mdnsd->running = 0;
        MUTEX_UNLOCK(mdnsd->mutex);
        return -1;
    }

    MUTEX_UNLOCK(mdnsd->mutex);
    return 0;
}

void mdnsd_stop(mdnsd_t *mdnsd)
{
    int fd = -1;
    thread_handle_t thread = 0;

    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (mdnsd->running) {
        mdnsd->running = 0;
        fd = mdnsd->sock_fd;
        thread = mdnsd->thread;
        mdnsd->sock_fd = -1;
        mdnsd->thread = 0;
    }
    MUTEX_UNLOCK(mdnsd->mutex);

    if (fd != -1) {
        CLOSESOCKET(fd);
    }
    if (thread) {
        THREAD_JOIN(thread);
    }
}

void mdnsd_set_services(mdnsd_t *mdnsd, const mdnsd_service_t *airplay,
                        const mdnsd_service_t *raop)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    if (airplay) {
        mdnsd->airplay = *airplay;
    } else {
        memset(&mdnsd->airplay, 0, sizeof(mdnsd->airplay));
    }
    if (raop) {
        mdnsd->raop = *raop;
    } else {
        memset(&mdnsd->raop, 0, sizeof(mdnsd->raop));
    }
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_announce(mdnsd_t *mdnsd, uint32_t ttl)
{
    if (!mdnsd) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdns_announce_locked(mdnsd, ttl);
    MUTEX_UNLOCK(mdnsd->mutex);
}

void mdnsd_goodbye(mdnsd_t *mdnsd, const mdnsd_service_t *service)
{
    if (!mdnsd || !service) {
        return;
    }

    MUTEX_LOCK(mdnsd->mutex);
    mdns_send_service_locked(mdnsd, service, 0, 0, NULL);
    MUTEX_UNLOCK(mdnsd->mutex);
}
