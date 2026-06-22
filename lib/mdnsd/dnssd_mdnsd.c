/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
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
 *
 *=================================================================
 * modified by fduncanh 2022, 2026
 * includes added code authored by kgbook, 2026
 */

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compat.h"
#include "../dnssd.h"
#include "../dnssdint.h"
#include "../utils.h"

#include "mdnsd.h"

typedef struct dnssd_private_s {
    mdnsd_txt_t raop_record;
    mdnsd_txt_t airplay_record;

    char host_name[MDNSD_MAX_NAME];
    char raop_name[MDNSD_MAX_NAME];
    char airplay_name[MDNSD_MAX_NAME];

    unsigned short raop_port;
    unsigned short airplay_port;
    int raop_registered;
    int airplay_registered;

    mdnsd_t *mdnsd;
} dnssd_private_t;

static void dnssd_sanitize_label(char *label)
{
    char *dot = strchr(label, '.');
    if (dot) {
        *dot = '\0';
    }

    for (char *ptr = label; *ptr; ptr++) {
        if (!isalnum((unsigned char) *ptr) && *ptr != '-') {
            *ptr = '-';
        }
    }
}

static void dnssd_make_host_name(char *dst, size_t dst_len)
{
    char host[128];

    if (gethostname(host, sizeof(host)) || !host[0]) {
        snprintf(host, sizeof(host), "UxPlay");
    }
    host[sizeof(host) - 1] = '\0';
    dnssd_sanitize_label(host);
    snprintf(dst, dst_len, "%s.local", host);
}

static int dnssd_prepare_names(dnssd_private_t *dnssd, dnssd_t *dnssd_public)
{
    char hwaddr[2 * MAX_HWADDR_LEN + 1];

    if (utils_hwaddr_raop(hwaddr, sizeof(hwaddr), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        return -1;
    }

    if (snprintf(dnssd->raop_name, sizeof(dnssd->raop_name), "%s@%s._raop._tcp.local",
                 hwaddr, dnssd_public->name) >= (int) sizeof(dnssd->raop_name)) {
        return -1;
    }

    if (snprintf(dnssd->airplay_name, sizeof(dnssd->airplay_name), "%s._airplay._tcp.local",
                 dnssd_public->name) >= (int) sizeof(dnssd->airplay_name)) {
        return -1;
    }

    dnssd_make_host_name(dnssd->host_name, sizeof(dnssd->host_name));
    return 0;
}

static int dnssd_build_raop_txt(dnssd_t *dnssd_public)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    char features[22] = {0};
    const char *pw = "false";
    const char *sf = RAOP_SF;

    switch (dnssd_public->pin_pw) {
    case 1:
        pw = "true";
        sf = "0x8c";
        break;
    case 2:
    case 3:
        pw = "true";
        sf = "0x84";
        break;
    default:
        break;
    }

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);
    dnssd->raop_record.length = 0;

    return mdnsd_txt_add(&dnssd->raop_record, "ch", RAOP_CH) ||
           mdnsd_txt_add(&dnssd->raop_record, "cn", RAOP_CN) ||
           mdnsd_txt_add(&dnssd->raop_record, "da", RAOP_DA) ||
           mdnsd_txt_add(&dnssd->raop_record, "et", RAOP_ET) ||
           mdnsd_txt_add(&dnssd->raop_record, "vv", RAOP_VV) ||
           mdnsd_txt_add(&dnssd->raop_record, "ft", features) ||
           mdnsd_txt_add(&dnssd->raop_record, "am", GLOBAL_MODEL) ||
           mdnsd_txt_add(&dnssd->raop_record, "md", RAOP_MD) ||
           mdnsd_txt_add(&dnssd->raop_record, "rhd", RAOP_RHD) ||
           mdnsd_txt_add(&dnssd->raop_record, "pw", pw) ||
           mdnsd_txt_add(&dnssd->raop_record, "sr", RAOP_SR) ||
           mdnsd_txt_add(&dnssd->raop_record, "ss", RAOP_SS) ||
           mdnsd_txt_add(&dnssd->raop_record, "sv", RAOP_SV) ||
           mdnsd_txt_add(&dnssd->raop_record, "tp", RAOP_TP) ||
           mdnsd_txt_add(&dnssd->raop_record, "txtvers", RAOP_TXTVERS) ||
           mdnsd_txt_add(&dnssd->raop_record, "sf", sf) ||
           mdnsd_txt_add(&dnssd->raop_record, "vs", RAOP_VS) ||
           mdnsd_txt_add(&dnssd->raop_record, "vn", RAOP_VN) ||
           mdnsd_txt_add(&dnssd->raop_record, "pk", dnssd_public->pk);
}

static int dnssd_build_airplay_txt(dnssd_t *dnssd_public)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    char device_id[3 * MAX_HWADDR_LEN];
    char features[22] = {0};

    if (utils_hwaddr_airplay(device_id, sizeof(device_id), dnssd_public->hw_addr, dnssd_public->hw_addr_len) < 0) {
        return -1;
    }

    snprintf(features, sizeof(features), "0x%X,0x%X", dnssd_public->features1, dnssd_public->features2);    
    dnssd->airplay_record.length = 0;

    return mdnsd_txt_add(&dnssd->airplay_record, "deviceid", device_id) ||
           mdnsd_txt_add(&dnssd->airplay_record, "features", features) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pw", dnssd_public->pin_pw ? "true" : "false") ||
           mdnsd_txt_add(&dnssd->airplay_record, "flags", "0x4") ||
           mdnsd_txt_add(&dnssd->airplay_record, "model", GLOBAL_MODEL) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pk", dnssd_public->pk) ||
           mdnsd_txt_add(&dnssd->airplay_record, "pi", AIRPLAY_PI) ||
           mdnsd_txt_add(&dnssd->airplay_record, "srcvers", AIRPLAY_SRCVERS) ||
           mdnsd_txt_add(&dnssd->airplay_record, "vv", AIRPLAY_VV);
}

static void dnssd_fill_airplay_service(dnssd_private_t *dnssd, mdnsd_service_t *service)
{
    memset(service, 0, sizeof(*service));
    snprintf(service->type, sizeof(service->type), "%s", "_airplay._tcp.local");
    snprintf(service->name, sizeof(service->name), "%s", dnssd->airplay_name);
    service->port = dnssd->airplay_port;
    service->txt = dnssd->airplay_record;
    service->registered = dnssd->airplay_registered;
}

static void dnssd_fill_raop_service(dnssd_private_t *dnssd, mdnsd_service_t *service)
{
    memset(service, 0, sizeof(*service));
    snprintf(service->type, sizeof(service->type), "%s", "_raop._tcp.local");
    snprintf(service->name, sizeof(service->name), "%s", dnssd->raop_name);
    service->port = dnssd->raop_port;
    service->txt = dnssd->raop_record;
    service->registered = dnssd->raop_registered;
}

static void dnssd_update_mdnsd(dnssd_private_t *dnssd)
{
    mdnsd_service_t airplay;
    mdnsd_service_t raop;

    dnssd_fill_airplay_service(dnssd, &airplay);
    dnssd_fill_raop_service(dnssd, &raop);
    mdnsd_set_services(dnssd->mdnsd, &airplay, &raop);
}

void *
dnssd_private_init(dnssd_t *dnssd_public, int *error)
{
    dnssd_private_t *dnssd;
    if (error) *error = DNSSD_ERROR_NOERROR;

    dnssd = (dnssd_private_t *) calloc(1, sizeof(dnssd_private_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    if (dnssd_prepare_names(dnssd, dnssd_public)) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_HWADDRLEN;
        return NULL;
    }

    dnssd->mdnsd = mdnsd_init(dnssd->host_name);
    if (!dnssd->mdnsd) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    return (void *) dnssd;
}

void
dnssd_private_destroy(void *private)
{
    if (private) {
        dnssd_private_t *dnssd = (dnssd_private_t *) private;
        mdnsd_destroy(dnssd->mdnsd);
        free(dnssd);
    }
}

int
dnssd_register_raop(dnssd_t *dnssd_public, unsigned short port)
{
    int ret;

    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd_build_raop_txt(dnssd_public)) {
        return -1;
    }

    dnssd->raop_port = port;
    dnssd->raop_registered = 1;
    dnssd_update_mdnsd(dnssd);

    ret = mdnsd_start(dnssd->mdnsd);
    if (!ret) {
        mdnsd_announce(dnssd->mdnsd, MDNSD_TTL_SERVICE);
    }

    return ret;
}

int
dnssd_register_airplay(dnssd_t *dnssd_public, unsigned short port)
{
    int ret;

    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd_build_airplay_txt(dnssd_public)) {
        return -1;
    }

    dnssd->airplay_port = port;
    dnssd->airplay_registered = 1;
    dnssd_update_mdnsd(dnssd);

    ret = mdnsd_start(dnssd->mdnsd);
    if (!ret) {
        mdnsd_announce(dnssd->mdnsd, MDNSD_TTL_SERVICE);
    }

    return ret;
}

const char *
dnssd_get_raop_txt(dnssd_t *dnssd_public, int *length)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;
    
    *length = dnssd->raop_record.length;
    return (const char *) dnssd->raop_record.bytes;
}

const char *
dnssd_get_airplay_txt(dnssd_t *dnssd_public, int *length)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    *length = dnssd->airplay_record.length;
    return (const char *) dnssd->airplay_record.bytes;
}

void
dnssd_unregister_raop(dnssd_t *dnssd_public)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd->raop_registered) {
        mdnsd_service_t service;
        dnssd_fill_raop_service(dnssd, &service);
        mdnsd_goodbye(dnssd->mdnsd, &service);
        dnssd->raop_registered = 0;
        dnssd_update_mdnsd(dnssd);
    }

    if (!dnssd->airplay_registered) {
        mdnsd_stop(dnssd->mdnsd);
    }
}

void
dnssd_unregister_airplay(dnssd_t *dnssd_public)
{
    assert(dnssd_public);
    assert(dnssd_public->dnssd_private);
    dnssd_private_t *dnssd = (dnssd_private_t *) dnssd_public->dnssd_private;

    if (dnssd->airplay_registered) {
        mdnsd_service_t service;
        dnssd_fill_airplay_service(dnssd, &service);
        mdnsd_goodbye(dnssd->mdnsd, &service);
        dnssd->airplay_registered = 0;
        dnssd_update_mdnsd(dnssd);
    }

    if (!dnssd->raop_registered) {
        mdnsd_stop(dnssd->mdnsd);
    }
}

void dnssd_error_text(int *dnssd_error, const  char *appname) {
    printf("*** dnssd_implementation: internal, mdnsd\n");
    printf("setup or start of self-contained mDNSResponder (lib/mdnsd) failed; check UDP port 5353 and multicast access");
    if (*dnssd_error != -1) {
        int sock_err = - *dnssd_error;
        printf("Socket error %d: %s\n", sock_err, SOCKET_ERROR_STRING(sock_err));
    }
}
