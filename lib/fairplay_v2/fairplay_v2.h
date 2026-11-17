/**
 *  Copyright (C) 2026  Omar Roth
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 */

#ifndef FAIRPLAY_V2_H
#define FAIRPLAY_V2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fairplay_v2_result {
    FAIRPLAY_V2_OK = 0,
    FAIRPLAY_V2_INVALID = -1,
    FAIRPLAY_V2_NOMEM = -2,
    FAIRPLAY_V2_CRYPTO = -3
};

typedef struct fairplay_v2_s fairplay_v2_t;

/* Create a receiver context with a fresh 128-byte SAP. */
fairplay_v2_t *fairplay_v2_init(void);

void fairplay_v2_destroy(fairplay_v2_t *fairplay);

/* Process the fixed-size selector-2 /fp-setup records. */
int fairplay_v2_setup(fairplay_v2_t *fairplay, const uint8_t request[16], uint8_t response[142]);
int fairplay_v2_handshake(fairplay_v2_t *fairplay, const uint8_t request[164], uint8_t response[32]);

/* Decrypt and authenticate a selector-2 media key after the handshake. */
int fairplay_v2_decrypt_key(fairplay_v2_t *fairplay, const uint8_t encrypted_key[72], uint8_t raw_key[16]);

#ifdef __cplusplus
}
#endif

#endif
