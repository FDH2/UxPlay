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
 * modified by fduncanh 2022
 */


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <ctype.h>

#include "dnssd.h"

#include "dnssdint.h"
#include "utils.h"


dnssd_t *
dnssd_init(const char* name, int name_len, const char* hw_addr, int hw_addr_len, unsigned char pin_pw, int *error)
{
    /* pin_pw = 0: no pin or password
                1: use onscreen pin for client access control
                2 or 3: require password for client access control  
    */
    const char *dot_local = ".local";
  
    if (error) *error = DNSSD_ERROR_NOERROR;
  
    /* first verify that  name is a null-terminated string with strlen = name_len, and is not ".local" */
    if (*(name + name_len) != '\0' || name_len != (int) strlen(name) || !strcmp(name, dot_local)) {
        if (error) *error = DNSSD_ERROR_BADNAME;
        return NULL;
    }

    /* use only that part of name before any substring ".local"
       (this fixes  namespace issues when a custom mDNSResponder is used on macOS) */
    const char *dot_local_start = strstr(name, dot_local);
    if (dot_local_start) {
        name_len = dot_local_start - name;
    }

    dnssd_t *dnssd = (dnssd_t *) calloc(1, sizeof(dnssd_t));
    if (!dnssd) {
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    dnssd->pin_pw = pin_pw;

    char *end = NULL;
    unsigned long features  = strtoul(FEATURES_1, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features1 = (uint32_t) features;

    features  = strtoul(FEATURES_2, &end, 16);
    if (!end || (features & 0xFFFFFFFF) != features) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_BADFEATURES;
        return NULL;
    } 
    dnssd->features2 = (uint32_t) features;

    dnssd->name_len = name_len;
    dnssd->name = calloc(1, name_len + 1);
    if (!dnssd->name) {
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }
    memcpy(dnssd->name, name, name_len);

    dnssd->hw_addr_len = hw_addr_len;
    dnssd->hw_addr = calloc(1, dnssd->hw_addr_len);
    if (!dnssd->hw_addr) {
        free(dnssd->name);
        free(dnssd);
        if (error) *error = DNSSD_ERROR_OUTOFMEM;
        return NULL;
    }

    memcpy(dnssd->hw_addr, hw_addr, hw_addr_len);

    dnssd->dnssd_private = NULL;
    dnssd->dnssd_private = dnssd_private_init(dnssd, error);
    if (!dnssd->dnssd_private) {
        free(dnssd->hw_addr);
        free(dnssd->name);
        free(dnssd);
        return NULL;
    }

    return dnssd;
}

void
dnssd_destroy(dnssd_t *dnssd)
{
    if (dnssd) {
        if (dnssd->hw_addr) {
            free(dnssd->hw_addr);
        }
        if (dnssd->name) {
            free(dnssd->name);
        }
        if (dnssd->dnssd_private) {
            dnssd_private_destroy(dnssd->dnssd_private);
        }

        free(dnssd);
    }
}

const char *
dnssd_get_name(dnssd_t *dnssd, int *length)
{
    *length = dnssd->name_len;
    return dnssd->name;
}

const char *
dnssd_get_hw_addr(dnssd_t *dnssd, int *length)
{
    *length = dnssd->hw_addr_len;
    return dnssd->hw_addr;
}

uint64_t dnssd_get_airplay_features(dnssd_t *dnssd) {
    uint64_t features = ((uint64_t) dnssd->features2) << 32;
    features += (uint64_t) dnssd->features1;
    return features;
}

void dnssd_set_pk(dnssd_t *dnssd, char * pk_str) {
    dnssd->pk = pk_str;
}

void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val) {
    uint32_t mask = 0;
    uint32_t *features = 0;
    if (bit < 0 || bit > 63) return;
    if (val < 0 || val > 1) return;
    if (bit >= 32) {
        mask = 0x1 << (bit - 32);
        features = &(dnssd->features2);
    } else {
        mask = 0x1 << bit;
        features = &(dnssd->features1);
    }
    if (val) {
        *features = *features | mask;
    } else {
        *features = *features & ~mask;
    }
}
