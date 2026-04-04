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
 *===================================================================
 * modified by fduncanh 2021-2022
 */

#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/pem.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "utils.h"

#define SALT_PK "UxPlay-Persistent-Not-Secure-Public-Key"

struct aes_ctx_s {
    EVP_CIPHER_CTX *cipher_ctx;
    uint8_t key[AES_128_BLOCK_SIZE];
    uint8_t iv[AES_128_BLOCK_SIZE];
    aes_direction_t direction;
    uint8_t block_offset;
};

uint8_t waste[AES_128_BLOCK_SIZE];

// Common AES utilities

void handle_error(const char* location) {
    long error = ERR_get_error();
    const char* error_str = ERR_error_string(error, NULL);
    fprintf(stderr, "Crypto error at %s: %s\n", location, error_str);
    exit(EXIT_FAILURE);
}

aes_ctx_t *aes_init(const uint8_t *key, const uint8_t *iv, const EVP_CIPHER *type, aes_direction_t direction) {
    aes_ctx_t *ctx = malloc(sizeof(aes_ctx_t));
    assert(ctx != NULL);
    ctx->cipher_ctx = EVP_CIPHER_CTX_new();
    assert(ctx->cipher_ctx != NULL);

    ctx->block_offset = 0;
    ctx->direction = direction;

    if (direction == AES_ENCRYPT) {
        if (!EVP_EncryptInit_ex(ctx->cipher_ctx, type, NULL, key, iv)) {
            handle_error(__func__);
        }
    } else {
        if (!EVP_DecryptInit_ex(ctx->cipher_ctx, type, NULL, key, iv)) {
            handle_error(__func__);
        }
    }

    memcpy(ctx->key, key, AES_128_BLOCK_SIZE);
    memcpy(ctx->iv, iv, AES_128_BLOCK_SIZE);
    EVP_CIPHER_CTX_set_padding(ctx->cipher_ctx, 0);
    return ctx;
}

void aes_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int in_len) {
    int out_len_e = 0;
    if (!EVP_EncryptUpdate(ctx->cipher_ctx, out, &out_len_e, in, in_len)) {
        handle_error(__func__);
    }
    int out_len_f = in_len - out_len_e;
    if (!EVP_EncryptFinal_ex(ctx->cipher_ctx, out + out_len_e, &out_len_f)) {
        handle_error(__func__);
    }
    assert(out_len_e + out_len_f <= in_len);
}

void aes_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int in_len) {
    int out_len_d = 0;
    if (!EVP_DecryptUpdate(ctx->cipher_ctx, out, &out_len_d, in, in_len)) {
        handle_error(__func__);
    }
    int out_len_f = in_len - out_len_d;
    if (!EVP_DecryptFinal_ex(ctx->cipher_ctx, out + out_len_d, &out_len_f)) {
        handle_error(__func__);
    }
    assert(out_len_f + out_len_d <= in_len);
}

void aes_destroy(aes_ctx_t *ctx) {
    if (ctx) {
        EVP_CIPHER_CTX_free(ctx->cipher_ctx);
        free(ctx);
    }
}

void aes_reset(aes_ctx_t *ctx, const EVP_CIPHER *type, aes_direction_t direction) {
    uint8_t key[AES_128_BLOCK_SIZE], iv[AES_128_BLOCK_SIZE]; 
    memcpy(key, ctx->key, AES_128_BLOCK_SIZE);
    memcpy(iv, ctx->iv, AES_128_BLOCK_SIZE);

    if (!EVP_CIPHER_CTX_reset(ctx->cipher_ctx)) {
        handle_error(__func__);
    }

    if (direction == AES_ENCRYPT) {
        if (!EVP_EncryptInit_ex(ctx->cipher_ctx, type, NULL, key, iv)) {
            handle_error(__func__);
        }
    } else {
        if (!EVP_DecryptInit_ex(ctx->cipher_ctx, type, NULL, key, iv)) {
            handle_error(__func__);
        }
    }

    memcpy(ctx->key, key, AES_128_BLOCK_SIZE);
    memcpy(ctx->iv, iv, AES_128_BLOCK_SIZE);
    EVP_CIPHER_CTX_set_padding(ctx->cipher_ctx, 0);
}

// AES CTR

aes_ctx_t *aes_ctr_init(const uint8_t *key, const uint8_t *iv) {
    return aes_init(key, iv, EVP_aes_128_ctr(), AES_ENCRYPT);
}

void aes_ctr_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    aes_encrypt(ctx, in, out, len);
    ctx->block_offset = (ctx->block_offset + len) % AES_128_BLOCK_SIZE;
}

void aes_ctr_start_fresh_block(aes_ctx_t *ctx) {
    // Is there a better way to do this?
    if (ctx->block_offset == 0) return;
    aes_ctr_encrypt(ctx, waste, waste, AES_128_BLOCK_SIZE - ctx->block_offset);
}

void aes_ctr_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    aes_encrypt(ctx, in, out, len);
}

void aes_ctr_reset(aes_ctx_t *ctx) {
    aes_reset(ctx, EVP_aes_128_ctr(), AES_ENCRYPT);
}

void aes_ctr_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// AES CBC

aes_ctx_t *aes_cbc_init(const uint8_t *key, const uint8_t *iv, aes_direction_t direction) {
    return aes_init(key, iv, EVP_aes_128_cbc(), direction);
}

void aes_cbc_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_ENCRYPT);
    aes_encrypt(ctx, in, out, len);
}

void aes_cbc_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_DECRYPT);
    aes_decrypt(ctx, in, out, len);
}

void aes_cbc_reset(aes_ctx_t *ctx) {
    aes_reset(ctx, EVP_aes_128_cbc(), ctx->direction);
}

void aes_cbc_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// X25519

struct x25519_key_s {
    EVP_PKEY *pkey;
};

x25519_key_t *x25519_key_generate(void) {

    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);

    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!pctx) {
        handle_error(__func__);
    }
    if (!EVP_PKEY_keygen_init(pctx)) {
        handle_error(__func__);
    }
    if (!EVP_PKEY_keygen(pctx, &key->pkey)) {
        handle_error(__func__);
    }
    EVP_PKEY_CTX_free(pctx);

    return key;
}

x25519_key_t *x25519_key_from_raw(const unsigned char data[X25519_KEY_SIZE]) {

    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);

    key->pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, data, X25519_KEY_SIZE);
    if (!key->pkey) {
        handle_error(__func__);
    }

    return key;
}

void x25519_key_get_raw(unsigned char data[X25519_KEY_SIZE], const x25519_key_t *key) {
    assert(key);
    if (!EVP_PKEY_get_raw_public_key(key->pkey, data, &(size_t) {X25519_KEY_SIZE})) {
        handle_error(__func__);
    }
}

void x25519_key_destroy(x25519_key_t *key) {
    if (key) {
        EVP_PKEY_free(key->pkey);
        free(key);
    }
}

void x25519_derive_secret(unsigned char secret[X25519_KEY_SIZE], const x25519_key_t *ours, const x25519_key_t *theirs) {
    EVP_PKEY_CTX *pctx;

    assert(ours);
    assert(theirs);

    pctx = EVP_PKEY_CTX_new(ours->pkey, NULL);
    if (!pctx) {
        handle_error(__func__);
    }
    if (!EVP_PKEY_derive_init(pctx)) {
        handle_error(__func__);
    }
    if (!EVP_PKEY_derive_set_peer(pctx, theirs->pkey)) {
        handle_error(__func__);
    }
    if (!EVP_PKEY_derive(pctx, secret, &(size_t) {X25519_KEY_SIZE})) {
        handle_error(__func__);
    }
    EVP_PKEY_CTX_free(pctx);
}

// GCM AES 128

int gcm_encrypt(const unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext,
                unsigned char *key, unsigned char *iv, unsigned char *tag)
{
    EVP_CIPHER_CTX *ctx = NULL;

    int len = 0;
    
    int ciphertext_len = 0;

    if(!(ctx = EVP_CIPHER_CTX_new()))
      handle_error(__func__);

    if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL))
        handle_error(__func__);

    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL))
        handle_error(__func__);

    if(1 != EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv))
        handle_error(__func__);

    if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
        handle_error(__func__);
    ciphertext_len = len;

    if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
        handle_error(__func__);
    ciphertext_len += len;

    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag))
        handle_error(__func__);

    EVP_CIPHER_CTX_free(ctx);

    return ciphertext_len;
}

int gcm_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext,
                unsigned char *key, unsigned char *iv, unsigned char *tag)
{
    EVP_CIPHER_CTX *ctx = NULL;

    if(!(ctx = EVP_CIPHER_CTX_new()))
        handle_error(__func__);

    if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, NULL, NULL))
        handle_error(__func__);

    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, NULL))
        handle_error(__func__);

    if(!EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv))
        handle_error(__func__);

    int len = 0;
    if(!EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len))
        handle_error(__func__);
    int plaintext_len = len;

    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag))
        handle_error(__func__);

    int ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

    EVP_CIPHER_CTX_free(ctx);
    
    if(ret > 0) {
        /* Success */
        plaintext_len += len;
        return plaintext_len;
    } else {
        /* Verify failed */
        return -1;
    }
}

// ED25519

struct ed25519_key_s {
    EVP_PKEY *pkey;
};

ed25519_key_t *ed25519_key_generate(const char *device_id, const char *keyfile, int *result) {
    EVP_PKEY_CTX *pctx = NULL;
    BIO *bp = NULL;
    FILE *file = NULL;
    bool new_pk = false;
    bool use_keyfile = strlen(keyfile);

    *result = 0;

    ed25519_key_t *key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);
   
    if (use_keyfile) {
        file = fopen(keyfile, "r");
        if (file) {
            bp = BIO_new_fp(file, BIO_NOCLOSE);
            key->pkey = PEM_read_PrivateKey(file, NULL, NULL, NULL);
            BIO_free(bp);
            fclose(file);
            if (!key->pkey) {
                new_pk = true;
            }
        } else {
            new_pk = true;
        }
    } else {
        /* generate (insecure) persistent keypair using device_id */
        unsigned char hash[SHA512_DIGEST_LENGTH] = {0};
        char salt[] = SALT_PK;
        sha_ctx_t *ctx = sha_init();
        sha_update(ctx, (const unsigned char *) salt, (unsigned int) strlen(salt));
        sha_update(ctx, (const unsigned char *) device_id, (unsigned int) strlen(device_id));
        sha_final(ctx, hash, NULL);
        sha_destroy(ctx);
        key->pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, (const unsigned char *) hash, ED25519_KEY_SIZE);
    }

    if (new_pk) {
        pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
        if (!pctx) {
            handle_error(__func__);
        }
        if (!EVP_PKEY_keygen_init(pctx)) {
            handle_error(__func__);
        }
        if (!EVP_PKEY_keygen(pctx, &key->pkey)) {
            handle_error(__func__);
        }
        EVP_PKEY_CTX_free(pctx);
        if (use_keyfile) {
            file = fopen(keyfile, "w");
            if (file) {
                bp = BIO_new_fp(file, BIO_NOCLOSE);
                PEM_write_bio_PrivateKey(bp, key->pkey, NULL, NULL, 0, NULL, NULL);
                BIO_free(bp);
                fclose(file);
                *result = 1;
            }
        }
    }
    return key;
}

ed25519_key_t *ed25519_key_from_raw(const unsigned char data[ED25519_KEY_SIZE]) {

    ed25519_key_t *key =  (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);

    key->pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, data, ED25519_KEY_SIZE);
    if (!key->pkey) {
        handle_error(__func__);
    }

    return key;
}

void ed25519_key_get_raw(unsigned char data[ED25519_KEY_SIZE], const ed25519_key_t *key) {
    assert(key);
    if (!EVP_PKEY_get_raw_public_key(key->pkey, data, &(size_t) {ED25519_KEY_SIZE})) {
        handle_error(__func__);
    }
}

ed25519_key_t *ed25519_key_copy(const ed25519_key_t *key) {
    assert(key);

    ed25519_key_t *new_key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(new_key);

    new_key->pkey = key->pkey;
    if (!EVP_PKEY_up_ref(key->pkey)) {
        handle_error(__func__);
    }

    return new_key;
}

void ed25519_sign(unsigned char *signature, size_t signature_len,
                  const unsigned char *data, size_t data_len,
                  const ed25519_key_t *key)
{
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    if (!mctx) {
        handle_error(__func__);
    }

    if (!EVP_DigestSignInit(mctx, NULL, NULL, NULL, key->pkey)) {
        handle_error(__func__);
    }
    if (!EVP_DigestSign(mctx, signature, &signature_len, data, data_len)) {
        handle_error(__func__);
    }

    EVP_MD_CTX_free(mctx);
}

int ed25519_verify(const unsigned char *signature, size_t signature_len,
                   const unsigned char *data, size_t data_len,
                   const ed25519_key_t *key)
{
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    if (!mctx) {
        handle_error(__func__);
    }

    if (!EVP_DigestVerifyInit(mctx, NULL, NULL, NULL, key->pkey)) {
        handle_error(__func__);
    }

    int ret = EVP_DigestVerify(mctx, signature, signature_len, data, data_len);
    if (ret < 0) {
        handle_error(__func__);
    }

    EVP_MD_CTX_free(mctx);

    return ret;
}

void ed25519_key_destroy(ed25519_key_t *key) {
    if (key) {
        EVP_PKEY_free(key->pkey);
        free(key);
    }
}



// SHA 512

struct sha_ctx_s {
    EVP_MD_CTX *digest_ctx;
};

sha_ctx_t *sha_init() {
    sha_ctx_t *ctx = (sha_ctx_t *) calloc(1, sizeof(sha_ctx_t));
    assert(ctx != NULL);
    ctx->digest_ctx = EVP_MD_CTX_new();
    assert(ctx->digest_ctx != NULL);

    if (!EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha512(), NULL)) {
        handle_error(__func__);
    }
    return ctx;
}

void sha_update(sha_ctx_t *ctx, const uint8_t *in, int len) {
    if (!EVP_DigestUpdate(ctx->digest_ctx, in, len)) {
        handle_error(__func__);
    }
}

void sha_final(sha_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    if (!EVP_DigestFinal_ex(ctx->digest_ctx, out, len)) {
        handle_error(__func__);
    }
}

void sha_reset(sha_ctx_t *ctx) {
    if (!EVP_MD_CTX_reset(ctx->digest_ctx) ||
        !EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha512(), NULL)) {
        handle_error(__func__);
    }
}

void sha_destroy(sha_ctx_t *ctx) {
    if (ctx) {
        EVP_MD_CTX_free(ctx->digest_ctx);
        free(ctx);
    }
}

//MD5
struct md5_ctx_s {
    EVP_MD_CTX *digest_ctx;
};

md5_ctx_t *md5_init() {
    md5_ctx_t *ctx = (md5_ctx_t *) calloc(1, sizeof(md5_ctx_t));
    assert(ctx != NULL);
    ctx->digest_ctx = EVP_MD_CTX_new();
    assert(ctx->digest_ctx != NULL);

    if (!EVP_DigestInit_ex(ctx->digest_ctx, EVP_md5(), NULL)) {
        handle_error(__func__);
    }
    return ctx;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *in, int len) {
    if (!EVP_DigestUpdate(ctx->digest_ctx, in, len)) {
        handle_error(__func__);
    }
}

void md5_final(md5_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    if (!EVP_DigestFinal_ex(ctx->digest_ctx, out, len)) {
        handle_error(__func__);
    }
}

void md5_reset(md5_ctx_t *ctx) {
    if (!EVP_MD_CTX_reset(ctx->digest_ctx) ||
        !EVP_DigestInit_ex(ctx->digest_ctx, EVP_md5(), NULL)) {

        handle_error(__func__);
    }
}

void md5_destroy(md5_ctx_t *ctx) {
    if (ctx) {
        EVP_MD_CTX_free(ctx->digest_ctx);
        free(ctx);
    }
}

#define MD5_DIGEST_LENGTH 16
char *get_md5(char *string) {
    unsigned char hash[MD5_DIGEST_LENGTH] = {0};  
    md5_ctx_t *ctx = NULL;
    ctx = md5_init();
    md5_update(ctx, (const unsigned char *) string, strlen(string));
    md5_final(ctx, hash, NULL);
    md5_destroy(ctx);
    ctx = NULL;
    char *result_str = utils_hex_to_string(hash, MD5_DIGEST_LENGTH);
    return result_str;   //must free result_str after use
}

int get_random_bytes(unsigned char *buf, int num) {
    return RAND_bytes(buf, num);
}
#include <stdio.h>
void pk_to_base64(const unsigned char *pk, int pk_len, char *pk_base64, int len) {
    memset(pk_base64, 0, len);
    int len64 = (4 * (pk_len /3)) + (pk_len % 3 ? 4 : 0);
    
    assert (len > len64);
    
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bio = BIO_new(BIO_s_mem());
    BUF_MEM *bufferPtr = NULL;


    bio = BIO_push(b64, bio);
  
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, pk, pk_len);
    BIO_flush(bio);

    BIO_get_mem_ptr(bio, &bufferPtr);
    BIO_set_close(bio, BIO_NOCLOSE);
    BIO_free_all(bio);
    memcpy(pk_base64,(*bufferPtr).data, len64);
}
  
