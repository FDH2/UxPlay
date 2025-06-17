/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Copyright (C) 2020 Jaslo Ziska
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
 *
 *  modified by fduncanh 2023
 */

/*
 * Helper methods for various crypto operations.
 * Uses OpenSSL behind the scenes.
*/

#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 128bit AES in CTR mode

#define AES_128_BLOCK_SIZE 16

typedef enum aes_direction_e { AES_DECRYPT, AES_ENCRYPT } aes_direction_t;

typedef struct aes_ctx_s aes_ctx_t;

aes_ctx_t *aes_ctr_init(const uint8_t *key, const uint8_t *iv);
void aes_ctr_reset(aes_ctx_t *ctx);
void aes_ctr_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void aes_ctr_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void aes_ctr_start_fresh_block(aes_ctx_t *ctx);
void aes_ctr_destroy(aes_ctx_t *ctx);

aes_ctx_t *aes_cbc_init(const uint8_t *key, const uint8_t *iv, aes_direction_t direction);
void aes_cbc_reset(aes_ctx_t *ctx);
void aes_cbc_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void aes_cbc_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len);
void aes_cbc_destroy(aes_ctx_t *ctx);

// X25519

#define X25519_KEY_SIZE 32

typedef struct x25519_key_s x25519_key_t;

x25519_key_t *x25519_key_generate(void);
x25519_key_t *x25519_key_from_raw(const unsigned char data[X25519_KEY_SIZE]);
void x25519_key_get_raw(unsigned char data[X25519_KEY_SIZE], const x25519_key_t *key);
void x25519_key_destroy(x25519_key_t *key);
int get_random_bytes(unsigned char *buf, int num);
void pk_to_base64(const unsigned char *pk, int pk_len, char *pk_base64, int len);
  
void x25519_derive_secret(unsigned char secret[X25519_KEY_SIZE], const x25519_key_t *ours, const x25519_key_t *theirs);

// GCM AES 128

int gcm_encrypt(const unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext,
                unsigned char *key, unsigned char *iv, unsigned char *tag);
int gcm_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext,
                unsigned char *key, unsigned char *iv, unsigned char *tag);
// ED25519

#define ED25519_KEY_SIZE 32

typedef struct ed25519_key_s ed25519_key_t;

ed25519_key_t *ed25519_key_generate(const char *device_id, const char * keyfile, int * result);
ed25519_key_t *ed25519_key_from_raw(const unsigned char data[ED25519_KEY_SIZE]);
void ed25519_key_get_raw(unsigned char data[ED25519_KEY_SIZE], const ed25519_key_t *key);
/*
 * Note that this function does *not copy* the OpenSSL key but only the wrapper. The internal OpenSSL key is still the
 * same. Only the reference count is increased so destroying both the original and the copy is allowed.
 */
ed25519_key_t *ed25519_key_copy(const ed25519_key_t *key);
void ed25519_key_destroy(ed25519_key_t *key);

void ed25519_sign(unsigned char *signature, size_t signature_len,
                  const unsigned char *data, size_t data_len,
                  const ed25519_key_t *key);
int ed25519_verify(const unsigned char *signature, size_t signature_len,
                   const unsigned char *data, size_t data_len,
                   const ed25519_key_t *key);

// SHA512

typedef struct sha_ctx_s sha_ctx_t;
sha_ctx_t *sha_init();
void sha_update(sha_ctx_t *ctx, const uint8_t *in, int len);
void sha_final(sha_ctx_t *ctx, uint8_t *out, unsigned int *len);
void sha_reset(sha_ctx_t *ctx);
void sha_destroy(sha_ctx_t *ctx);

//MD5
typedef struct md5_ctx_s md5_ctx_t;
md5_ctx_t *md5_init();
void md5_update(md5_ctx_t *ctx, const uint8_t *in, int len);
void md5_final(md5_ctx_t *ctx, uint8_t *out, unsigned int *len);
void md5_reset(md5_ctx_t *ctx);
void md5_destroy(md5_ctx_t *ctx);
char *get_md5(char *string);
  
#ifdef __cplusplus
}
#endif
#endif
