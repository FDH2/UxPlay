/**
 *  Copyright (C) 2012  Juho Vähä-Herttua
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

#ifndef DNSSD_H
#define DNSSD_H
#include <stdint.h>

#ifndef DNSSD_API
# define DNSSD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DNSSD_ERROR_NOERROR       0
#define DNSSD_ERROR_HWADDRLEN     1
#define DNSSD_ERROR_OUTOFMEM      2
#define DNSSD_ERROR_LIBNOTFOUND   3
#define DNSSD_ERROR_PROCNOTFOUND  4
#define DNSSD_ERROR_BADFEATURES   5

typedef struct dnssd_s dnssd_t;

DNSSD_API dnssd_t *dnssd_init(const char *name, int name_len, const char *hw_addr, int hw_addr_len, int *error, unsigned char pin_pw);

DNSSD_API int dnssd_register_raop(dnssd_t *dnssd, unsigned short port);
DNSSD_API int dnssd_register_airplay(dnssd_t *dnssd, unsigned short port);

DNSSD_API void dnssd_unregister_raop(dnssd_t *dnssd);
DNSSD_API void dnssd_unregister_airplay(dnssd_t *dnssd);

DNSSD_API const char *dnssd_get_raop_txt(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_airplay_txt(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_name(dnssd_t *dnssd, int *length);
DNSSD_API const char *dnssd_get_hw_addr(dnssd_t *dnssd, int *length);
DNSSD_API void dnssd_set_airplay_features(dnssd_t *dnssd, int bit, int val);
DNSSD_API uint64_t dnssd_get_airplay_features(dnssd_t *dnssd);
DNSSD_API void dnssd_set_pk(dnssd_t *dnssd, char * pk_str);

DNSSD_API void dnssd_destroy(dnssd_t *dnssd);

#ifdef __cplusplus
}
#endif
#endif
