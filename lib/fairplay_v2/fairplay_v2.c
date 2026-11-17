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

/*
 * Clean-room AirPlay FairPlay selector-2 receiver primitives.
 *
 * The transform below is an algebraically normalized representation derived
 * from black-box Apple CoreFP input/output traces. It intentionally does not
 * reproduce Apple's encoded white-box tables.
 */

#include "fairplay_v2.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <stdlib.h>
#include <string.h>

struct fairplay_v2_s {
    uint8_t receiver_sap[128];
    uint8_t sender_sap[128];
    uint8_t mode;
    int has_setup;
    int has_handshake;
};

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t input_masks[4][16] = {
    {0},
    {0},
    {0x08,0x0e,0x31,0x5d,0xcc,0xc1,0x4e,0x78,0x90,0x27,0x54,0xd8,0x16,0xda,0x41,0x38},
    {0}
};

static const uint8_t output_masks[4][16] = {
    {0xd7,0x22,0xa1,0xba,0xaa,0x10,0x8a,0xf5,0xc0,0xd8,0x0b,0x3c,0xff,0xed,0x93,0xe1},
    {0x4b,0x66,0x8f,0x57,0xa9,0x03,0x86,0x9d,0xff,0xdc,0x78,0x12,0xb5,0x70,0x3e,0xc5},
    {0xbc,0xb7,0x4b,0xb1,0xce,0xdd,0xd3,0x5e,0x09,0xc8,0x2a,0x73,0xe6,0x63,0x76,0xee},
    {0x03,0xe9,0x4b,0xd9,0x12,0xaf,0x45,0x8d,0xc7,0x7a,0xf2,0x51,0x72,0x6c,0x16,0xa5}
};

static const uint8_t chain_masks[4][16] = {
    {0x30,0xe1,0xe4,0x4f,0xaa,0x03,0x6e,0x66,0xde,0xd2,0x38,0xde,0xc6,0x05,0x20,0x3a},
    {0x75,0x6e,0xb0,0x0f,0xe6,0xfa,0x0e,0xa3,0xb6,0xf0,0x8b,0x77,0xfc,0x72,0x46,0x27},
    {0x1b,0x5a,0x63,0x54,0xa4,0xf2,0x59,0x31,0x77,0x8e,0xd0,0x05,0xbc,0x0f,0x72,0xe4},
    {0x6a,0x63,0x60,0xe5,0xe0,0xc5,0xed,0xa4,0x76,0x0f,0x48,0xd5,0x5e,0x80,0x92,0x84}
};

static const char *const round_keys_hex[4] = {
    "523263286e2de137049c00832cd65607"
    "0628a0846816a5207280964167be739d"
    "aaa7fe01c2b15b21b031cd60d78fbefd"
    "dd09aa0f1fb8f12eaf893c4e780682b3"
    "ba1ac7b3a5a2369d0a2b0ad3722d8860"
    "72de17f3d77c216edd572bbdaf7aa3dd"
    "88d4d68a5fa8f7e482ffdc592d857f84"
    "5f06895200ae7eb68251a2efafd4dd6b"
    "97c7f62b9769889d15382a72baecf719"
    "42af22dfd5c6aa42c0fe80307a127729"
    "6a7826bfc28ca7b268baa64b2d9d49bf",
    "d5a9ce40a0f13f2229f9fc95e08eec56"
    "8e837ccb618bcbd7015ec427a8d25093"
    "39d0a009585b6bde5905aff9f1d7ff6a"
    "33c6a2a86b9dc9763298668fc34f99e5"
    "bf287b86d4b5b2f0e62dd47f25624d9a"
    "05cbc3b9d17e71493753a5361231e8ac"
    "e2505270332e2339047d860f164c6ea3"
    "8bcf5837b8e17b0ebc9cfd01aad093a2"
    "7b13629bc3f219957f6ee494d5be7736"
    "cee667980d147e0d727a9a99a7c4edaf"
    "afd5919340a4e654640182428969293a",
    "86989d3be7920dc2f5e37fd5f61124af"
    "7494da7735e813a22e6ac2d994cda344"
    "cb9ec155fe76d2f7d01c102e44d1b36a"
    "f1f3c34e0f8511b9df9901979b48b2fd"
    "abc4975aa44186e37bd88774e0903589"
    "db5230bb7f13b65804cb312ce45b04a5"
    "c2a036d2bdb3808ab978b1a65d23b503"
    "a4754d9e19c6cd14a0be7cb2fd9dc9b1"
    "7aa885ca636e48dec3d0346c3e4dfddd"
    "82fc4478e1920ca6224238ca1c0fc517"
    "7eedff55ed156b1c0842aafbfbe63371",
    "48cb53065e61a30ea823779991ed11e3"
    "5c6b5d40f0605667e9369b7a54370eb8"
    "c4c0316034a06707dd96fc7d89a1f2c5"
    "f24997c7c6e9f0c01b7f0cbd92defe78"
    "e7f22b88211bdb483a64d7f5a8ba298d"
    "0357764a224cad0218287af7b092537a"
    "6cbaacad4ef601af56de7b58e64c2822"
    "058e3f234b783e8c1da645d4fbea6df6"
    "02b27d2c49ca43a0546c0674af866b82"
    "5dcd6e5514072df5406b2b81efed4003"
    "3d2d5e53386c7df2add2e1aff7294558"
};

static const uint8_t mac_prefix[16] = {
    'a','s','d','i','u','A','D','8','A','K','A','9','a','>','/','?'
};
static const uint8_t mac_suffix[16] = {
    '!','D','4','H','&','2','*','2','0','3','k','j','g','h','a','9'
};
static const uint8_t wrapping_prefix[16] = {
    '#','l','*','G','Y','&','c','q','n','w','o','*','&','5','s','d'
};
static const uint8_t wrapping_suffix[16] = {
    '5','7','b','v','&','%','n','i','u','l','q','p','d','g','3','z'
};

static uint8_t
hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
    if (value >= 'a' && value <= 'f') {
        return (uint8_t)(value - 'a' + 10);
    }
    return 0xff;
}

static int
decode_round_keys(uint8_t mode, uint8_t keys[11][16])
{
    const char *encoded;
    size_t i;

    if (mode >= 4) {
        return FAIRPLAY_V2_INVALID;
    }
    encoded = round_keys_hex[mode];
    for (i = 0; i < 11 * 16; i++) {
        uint8_t high = hex_nibble(encoded[2 * i]);
        uint8_t low = hex_nibble(encoded[2 * i + 1]);
        if (high == 0xff || low == 0xff) {
            return FAIRPLAY_V2_CRYPTO;
        }
        ((uint8_t *)keys)[i] = (uint8_t)((high << 4) | low);
    }
    return FAIRPLAY_V2_OK;
}

static uint8_t
gf_multiply(uint8_t a, uint8_t b)
{
    uint8_t product = 0;
    while (b != 0) {
        if ((b & 1) != 0) {
            product ^= a;
        }
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return product;
}

static void
shift_rows(uint8_t state[16])
{
    uint8_t previous[16];
    unsigned row, column;
    memcpy(previous, state, sizeof(previous));
    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            state[4 * column + row] = previous[4 * ((column + row) & 3) + row];
        }
    }
}

static void
inverse_shift_rows(uint8_t state[16])
{
    uint8_t previous[16];
    unsigned row, column;
    memcpy(previous, state, sizeof(previous));
    for (row = 0; row < 4; row++) {
        for (column = 0; column < 4; column++) {
            state[4 * column + row] = previous[4 * ((column - row + 4) & 3) + row];
        }
    }
}

static void
mix_columns(uint8_t state[16])
{
    unsigned column;
    for (column = 0; column < 4; column++) {
        unsigned offset = column * 4;
        uint8_t a = state[offset], b = state[offset + 1];
        uint8_t c = state[offset + 2], d = state[offset + 3];
        state[offset] = gf_multiply(a, 2) ^ gf_multiply(b, 3) ^ c ^ d;
        state[offset + 1] = a ^ gf_multiply(b, 2) ^ gf_multiply(c, 3) ^ d;
        state[offset + 2] = a ^ b ^ gf_multiply(c, 2) ^ gf_multiply(d, 3);
        state[offset + 3] = gf_multiply(a, 3) ^ b ^ c ^ gf_multiply(d, 2);
    }
}

static void
inverse_mix_columns(uint8_t state[16])
{
    unsigned column;
    for (column = 0; column < 4; column++) {
        unsigned offset = column * 4;
        uint8_t a = state[offset], b = state[offset + 1];
        uint8_t c = state[offset + 2], d = state[offset + 3];
        state[offset] = gf_multiply(a, 14) ^ gf_multiply(b, 11) ^ gf_multiply(c, 13) ^ gf_multiply(d, 9);
        state[offset + 1] = gf_multiply(a, 9) ^ gf_multiply(b, 14) ^ gf_multiply(c, 11) ^ gf_multiply(d, 13);
        state[offset + 2] = gf_multiply(a, 13) ^ gf_multiply(b, 9) ^ gf_multiply(c, 14) ^ gf_multiply(d, 11);
        state[offset + 3] = gf_multiply(a, 11) ^ gf_multiply(b, 13) ^ gf_multiply(c, 9) ^ gf_multiply(d, 14);
    }
}

static void
xor_key(uint8_t state[16], const uint8_t key[16])
{
    unsigned i;
    for (i = 0; i < 16; i++) {
        state[i] ^= key[i];
    }
}

static void
encrypt_block(uint8_t state[16], const uint8_t keys[11][16])
{
    unsigned round, i;
    xor_key(state, keys[0]);
    for (i = 0; i < 16; i++) {
        state[i] = aes_sbox[state[i]];
    }
    shift_rows(state);
    for (round = 1; round < 10; round++) {
        mix_columns(state);
        xor_key(state, keys[round]);
        for (i = 0; i < 16; i++) {
            state[i] = aes_sbox[state[i]];
        }
        shift_rows(state);
    }
    xor_key(state, keys[10]);
}

static void
decrypt_block(uint8_t state[16], const uint8_t keys[11][16], const uint8_t inverse_sbox[256])
{
    int round;
    unsigned i;
    xor_key(state, keys[10]);
    for (round = 9; round > 0; round--) {
        inverse_shift_rows(state);
        for (i = 0; i < 16; i++) {
            state[i] = inverse_sbox[state[i]];
        }
        xor_key(state, keys[round]);
        inverse_mix_columns(state);
    }
    inverse_shift_rows(state);
    for (i = 0; i < 16; i++) {
        state[i] = inverse_sbox[state[i]];
    }
    xor_key(state, keys[0]);
}

static int
fairplay_v2_encrypt_sap(uint8_t mode, const uint8_t plaintext[128], uint8_t encrypted[128])
{
    uint8_t keys[11][16], previous[16] = {0};
    unsigned block, i;
    int result;

    if (plaintext == NULL || encrypted == NULL || mode >= 4) {
        return FAIRPLAY_V2_INVALID;
    }
    result = decode_round_keys(mode, keys);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }

    for (block = 0; block < 8; block++) {
        uint8_t state[16];
        for (i = 0; i < 16; i++) {
            state[i] = plaintext[block * 16 + i] ^ input_masks[mode][i];
            if (block > 0) {
                state[i] ^= previous[i] ^ chain_masks[mode][i];
            }
        }
        encrypt_block(state, keys);
        memcpy(previous, state, sizeof(previous));
        for (i = 0; i < 16; i++) {
            encrypted[block * 16 + i] = state[i] ^ output_masks[mode][i];
        }
    }
    OPENSSL_cleanse(keys, sizeof(keys));
    return FAIRPLAY_V2_OK;
}

static int
fairplay_v2_decrypt_sap(uint8_t mode, const uint8_t encrypted[128], uint8_t plaintext[128])
{
    uint8_t keys[11][16], inverse_sbox[256], previous[16] = {0};
    unsigned block, i;
    int result;

    if (encrypted == NULL || plaintext == NULL || mode >= 4) {
        return FAIRPLAY_V2_INVALID;
    }
    result = decode_round_keys(mode, keys);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }
    for (i = 0; i < 256; i++) {
        inverse_sbox[aes_sbox[i]] = (uint8_t)i;
    }

    for (block = 0; block < 8; block++) {
        uint8_t state[16], current[16];
        for (i = 0; i < 16; i++) {
            state[i] = encrypted[block * 16 + i] ^ output_masks[mode][i];
        }
        memcpy(current, state, sizeof(current));
        decrypt_block(state, keys, inverse_sbox);
        for (i = 0; i < 16; i++) {
            plaintext[block * 16 + i] = state[i] ^ input_masks[mode][i];
            if (block > 0) {
                plaintext[block * 16 + i] ^= previous[i] ^ chain_masks[mode][i];
            }
        }
        memcpy(previous, current, sizeof(previous));
    }
    OPENSSL_cleanse(keys, sizeof(keys));
    OPENSSL_cleanse(inverse_sbox, sizeof(inverse_sbox));
    return FAIRPLAY_V2_OK;
}

static uint32_t
load_be32(const uint8_t input[4])
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) | ((uint32_t)input[2] << 8) | input[3];
}

static void
store_be32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static int
valid_record(const uint8_t *record, uint8_t type, uint32_t payload_length)
{
    return memcmp(record, "FPLY", 4) == 0 && record[4] == 2 && record[5] == 1 && record[6] == type && record[7] == 0 &&
           load_be32(record + 8) == payload_length;
}

static void
record_header(uint8_t *record, uint8_t type, uint32_t payload_length)
{
    memcpy(record, "FPLY", 4);
    record[4] = 2;
    record[5] = 1;
    record[6] = type;
    record[7] = 0;
    store_be32(record + 8, payload_length);
}

static int
derive_mac_key(const uint8_t sender_sap[128], const uint8_t receiver_sap[128], uint8_t key[20])
{
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    unsigned length = 0;
    int ok;
    if (digest == NULL) {
        return FAIRPLAY_V2_NOMEM;
    }
    ok = EVP_DigestInit_ex(digest, EVP_sha1(), NULL) == 1 &&
         EVP_DigestUpdate(digest, mac_prefix, sizeof(mac_prefix)) == 1 &&
         EVP_DigestUpdate(digest, sender_sap, 128) == 1 && EVP_DigestUpdate(digest, receiver_sap, 128) == 1 &&
         EVP_DigestUpdate(digest, mac_suffix, sizeof(mac_suffix)) == 1 &&
         EVP_DigestFinal_ex(digest, key, &length) == 1 && length == 20;
    EVP_MD_CTX_free(digest);
    return ok ? FAIRPLAY_V2_OK : FAIRPLAY_V2_CRYPTO;
}

static int
derive_wrapping_key(const uint8_t sender_sap[128], const uint8_t receiver_sap[128], uint8_t key[16])
{
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    unsigned length = 0;
    int ok;
    if (digest == NULL) {
        return FAIRPLAY_V2_NOMEM;
    }
    ok = EVP_DigestInit_ex(digest, EVP_md5(), NULL) == 1 &&
         EVP_DigestUpdate(digest, wrapping_prefix, sizeof(wrapping_prefix)) == 1 &&
         EVP_DigestUpdate(digest, sender_sap, 128) == 1 && EVP_DigestUpdate(digest, receiver_sap, 128) == 1 &&
         EVP_DigestUpdate(digest, wrapping_suffix, sizeof(wrapping_suffix)) == 1 &&
         EVP_DigestFinal_ex(digest, key, &length) == 1 && length == 16;
    EVP_MD_CTX_free(digest);
    return ok ? FAIRPLAY_V2_OK : FAIRPLAY_V2_CRYPTO;
}

static int
decrypt_wrapped_block(const uint8_t key[16], const uint8_t encrypted[16], uint8_t plaintext[16])
{
    EVP_CIPHER_CTX *cipher = EVP_CIPHER_CTX_new();
    int written = 0, final_written = 0;
    int ok;
    if (cipher == NULL) {
        return FAIRPLAY_V2_NOMEM;
    }
    ok = EVP_DecryptInit_ex(cipher, EVP_aes_128_ecb(), NULL, key, NULL) == 1 &&
         EVP_CIPHER_CTX_set_padding(cipher, 0) == 1 &&
         EVP_DecryptUpdate(cipher, plaintext, &written, encrypted, 16) == 1 && written == 16 &&
         EVP_DecryptFinal_ex(cipher, plaintext + written, &final_written) == 1 && final_written == 0;
    EVP_CIPHER_CTX_free(cipher);
    return ok ? FAIRPLAY_V2_OK : FAIRPLAY_V2_CRYPTO;
}

static int
confirmation(const uint8_t sender_sap[128], const uint8_t receiver_sap[128], uint8_t output[20])
{
    static const uint8_t input[2] = {2, 2};
    uint8_t key[20];
    unsigned length = 0;
    int result = derive_mac_key(sender_sap, receiver_sap, key);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }
    if (HMAC(EVP_sha1(), key, sizeof(key), input, sizeof(input), output, &length) == NULL || length != 20) {
        OPENSSL_cleanse(key, sizeof(key));
        return FAIRPLAY_V2_CRYPTO;
    }
    OPENSSL_cleanse(key, sizeof(key));
    return FAIRPLAY_V2_OK;
}

fairplay_v2_t *
fairplay_v2_init(void)
{
    fairplay_v2_t *fairplay = calloc(1, sizeof(fairplay_v2_t));
    if (!fairplay) {
        return NULL;
    }
    fairplay->receiver_sap[1] = 1;
    if (RAND_bytes(fairplay->receiver_sap + 2, 126) != 1) {
        fairplay_v2_destroy(fairplay);
        return NULL;
    }
    return fairplay;
}

void
fairplay_v2_destroy(fairplay_v2_t *fairplay)
{
    if (fairplay != NULL) {
        OPENSSL_cleanse(fairplay, sizeof(*fairplay));
        free(fairplay);
    }
}

int
fairplay_v2_setup(fairplay_v2_t *fairplay, const uint8_t request[16], uint8_t response[142])
{
    int result;
    if (fairplay != NULL) {
        fairplay->has_setup = 0;
        fairplay->has_handshake = 0;
        OPENSSL_cleanse(fairplay->sender_sap, sizeof(fairplay->sender_sap));
    }
    if (fairplay == NULL || request == NULL || response == NULL || !valid_record(request, 1, 4) || request[12] != 2 ||
        request[13] != 0 || request[14] >= 4 || request[15] != 0xbb) {
        return FAIRPLAY_V2_INVALID;
    }

    memset(response, 0, 142);
    record_header(response, 2, 130);
    response[12] = 2;
    response[13] = request[14];
    result = fairplay_v2_encrypt_sap(request[14], fairplay->receiver_sap, response + 14);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }
    fairplay->mode = request[14];
    fairplay->has_setup = 1;
    fairplay->has_handshake = 0;
    OPENSSL_cleanse(fairplay->sender_sap, sizeof(fairplay->sender_sap));
    return FAIRPLAY_V2_OK;
}

int
fairplay_v2_handshake(fairplay_v2_t *fairplay, const uint8_t request[164], uint8_t response[32])
{
    uint8_t expected[20];
    int result;
    if (fairplay != NULL) {
        fairplay->has_handshake = 0;
        OPENSSL_cleanse(fairplay->sender_sap, sizeof(fairplay->sender_sap));
    }
    if (fairplay == NULL || request == NULL || response == NULL || !fairplay->has_setup ||
        !valid_record(request, 3, 152) || request[12] != fairplay->mode ||
        memcmp(request + 13, "\x8f\x1a\x9c", 3) != 0) {
        return FAIRPLAY_V2_INVALID;
    }

    result = fairplay_v2_decrypt_sap(fairplay->mode, request + 16, fairplay->sender_sap);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }
    result = confirmation(fairplay->sender_sap, fairplay->receiver_sap, expected);
    if (result != FAIRPLAY_V2_OK) {
        return result;
    }
    if (CRYPTO_memcmp(expected, request + 144, sizeof(expected)) != 0) {
        OPENSSL_cleanse(expected, sizeof(expected));
        OPENSSL_cleanse(fairplay->sender_sap, sizeof(fairplay->sender_sap));
        return FAIRPLAY_V2_INVALID;
    }

    record_header(response, 4, 20);
    memcpy(response + 12, expected, sizeof(expected));
    OPENSSL_cleanse(expected, sizeof(expected));
    fairplay->has_handshake = 1;
    return FAIRPLAY_V2_OK;
}

int
fairplay_v2_decrypt_key(fairplay_v2_t *fairplay, const uint8_t encrypted_key[72], uint8_t raw_key[16])
{
    uint8_t wrapping_key[16] = {0}, mac_key[20] = {0}, recovered[16] = {0};
    uint8_t authenticated[52] = {0}, expected[20] = {0};
    unsigned expected_length = 0;
    unsigned i;
    int result;

    if (raw_key == NULL) {
        return FAIRPLAY_V2_INVALID;
    }
    memset(raw_key, 0, 16);
    if (fairplay == NULL || encrypted_key == NULL || !fairplay->has_handshake) {
        return FAIRPLAY_V2_INVALID;
    }
    if (memcmp(encrypted_key, "FPLY\x01\x02\x01\x00", 8) != 0 || load_be32(encrypted_key + 8) != 60 ||
        load_be32(encrypted_key + 12) != 0 || load_be32(encrypted_key + 32) != 16) {
        return FAIRPLAY_V2_INVALID;
    }

    result = derive_wrapping_key(fairplay->sender_sap, fairplay->receiver_sap, wrapping_key);
    if (result != FAIRPLAY_V2_OK) {
        goto cleanup;
    }
    result = decrypt_wrapped_block(wrapping_key, encrypted_key + 56, recovered);
    if (result != FAIRPLAY_V2_OK) {
        goto cleanup;
    }
    for (i = 0; i < sizeof(recovered); i++) {
        recovered[i] ^= encrypted_key[16 + i];
    }

    result = derive_mac_key(fairplay->sender_sap, fairplay->receiver_sap, mac_key);
    if (result != FAIRPLAY_V2_OK) {
        goto cleanup;
    }
    memcpy(authenticated, encrypted_key, 36);
    memcpy(authenticated + 36, recovered, sizeof(recovered));
    if (HMAC(EVP_sha1(), mac_key, sizeof(mac_key), authenticated, sizeof(authenticated), expected, &expected_length) ==
            NULL ||
        expected_length != sizeof(expected)) {
        result = FAIRPLAY_V2_CRYPTO;
        goto cleanup;
    }
    if (CRYPTO_memcmp(expected, encrypted_key + 36, sizeof(expected)) != 0) {
        result = FAIRPLAY_V2_INVALID;
        goto cleanup;
    }

    memcpy(raw_key, recovered, sizeof(recovered));
    result = FAIRPLAY_V2_OK;

cleanup:
    OPENSSL_cleanse(wrapping_key, sizeof(wrapping_key));
    OPENSSL_cleanse(mac_key, sizeof(mac_key));
    OPENSSL_cleanse(recovered, sizeof(recovered));
    OPENSSL_cleanse(authenticated, sizeof(authenticated));
    OPENSSL_cleanse(expected, sizeof(expected));
    return result;
}
