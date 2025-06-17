/**
 *  Copyright (C) 2018  Juho Vähä-Herttua
 *  Copyright (C) 2020  Jaslo Ziska
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

#include "crypto.h"

#ifndef PAIRING_H
#define PAIRING_H

#define PAIRING_SIG_SIZE (2 * X25519_KEY_SIZE)


#define SRP_USERNAME_SIZE 24 /* accomodates up to an 8-octet MAC address */
#define SRP_SESSION_KEY_SIZE 40
#define SRP_VERIFIER_SIZE 256
#define SRP_SALT_SIZE 16
#define SRP_PK_SIZE 256
#define SRP_SHA SRP_SHA1
#define SRP_NG SRP_NG_2048
#define SRP_M2_SIZE 64
#define SRP_PRIVATE_KEY_SIZE 32
#define GCM_AUTHTAG_SIZE 16
#define SHA512_KEY_LENGTH 64

typedef struct pairing_s pairing_t;
typedef struct pairing_session_s pairing_session_t;

pairing_t *pairing_init_generate(const char *device_id, const char *keyfile, int *result);
void pairing_get_public_key(pairing_t *pairing, unsigned char public_key[ED25519_KEY_SIZE]);

pairing_session_t *pairing_session_init(pairing_t *pairing);
void pairing_session_set_setup_status(pairing_session_t *session);
int pairing_session_check_handshake_status(pairing_session_t *session);
int pairing_session_handshake(pairing_session_t *session, const unsigned char ecdh_key[X25519_KEY_SIZE],
                              const unsigned char ed_key[ED25519_KEY_SIZE]);
int pairing_session_get_public_key(pairing_session_t *session, unsigned char ecdh_key[X25519_KEY_SIZE]);
int random_pin();
int pairing_session_get_signature(pairing_session_t *session, unsigned char signature[PAIRING_SIG_SIZE]);
int pairing_session_finish(pairing_session_t *session, const unsigned char signature[PAIRING_SIG_SIZE]);
void pairing_session_destroy(pairing_session_t *session);

void pairing_destroy(pairing_t *pairing);

int pairing_get_ecdh_secret_key(pairing_session_t *session, unsigned char ecdh_secret[X25519_KEY_SIZE]);

int srp_new_user(pairing_session_t *session, pairing_t *pairing, const char *device_id, const char *pin,
		 const char **salt, int *len_salt, const char **pk, int *len_pk);
int srp_validate_proof(pairing_session_t *session, pairing_t *pairing, const unsigned char *A,
		       int len_A, unsigned char *proof, int client_proof_len, int proof_len);
int srp_confirm_pair_setup(pairing_session_t *session, pairing_t *pairing, unsigned char *epk,
                           unsigned char *auth_tag);
void get_pairing_session_client_data(pairing_session_t *session, char **username, char **client_pk);
void ed25519_pk_to_base64(const unsigned char *pk, char **pk64);
int pairing_session_make_nonce(pairing_session_t *session, uint64_t *local_time, const char *client_data, unsigned char *nonce, int len);
bool pairing_digest_verify(const char *method, const char * authorization, const char *password);

#endif
