/*
 * Secure Remote Password 6a implementation
 * Copyright (c) 2010 Tom Cocagne. All rights reserved.
 * https://github.com/cocagne/csrp
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013 Tom Cocagne
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *===========================================================================
 * updated (2023) by fduncanh to replace deprecated openssl SHA* hash functions
 * modified (2023) by fduncanh for use with Apple's pair-setup-pin protocol
 */
#define APPLE_VARIANT
#ifdef WIN32
# include <Windows.h>
# include <Wincrypt.h>
#else
# include <sys/time.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include "srp.h"

static int	g_initialized = 0;

typedef struct
{
    BIGNUM     * N;
    BIGNUM     * g;
} NGConstant;

struct NGHex
{
    const char * n_hex;
    const char * g_hex;
};

/* All constants here were pulled from Appendix A of RFC 5054 */
static struct NGHex global_Ng_constants[] = {
#if 0 /* begin removed section 1 */
  { /* 1024 */
   "EEAF0AB9ADB38DD69C33F80AFA8FC5E86072618775FF3C0B9EA2314C9C256576D674DF7496"
   "EA81D3383B4813D692C6E0E0D5D8E250B98BE48E495C1D6089DAD15DC7D7B46154D6B6CE8E"
   "F4AD69B15D4982559B297BCF1885C529F566660E57EC68EDBC3C05726CC02FD4CBF4976EAA"
   "9AFD5138FE8376435B9FC61D2FC0EB06E3",
   "2"
 },
 { /* 1536 */
   "9DEF3CAFB939277AB1F12A8617A47BBBDBA51DF499AC4C80BEEEA961"
   "4B19CC4D5F4F5F556E27CBDE51C6A94BE4607A291558903BA0D0F843"
   "80B655BB9A22E8DCDF028A7CEC67F0D08134B1C8B97989149B609E0B"
   "E3BAB63D47548381DBC5B1FC764E3F4B53DD9DA1158BFD3E2B9C8CF5"
   "6EDF019539349627DB2FD53D24B7C48665772E437D6C7F8CE442734A"
   "F7CCB7AE837C264AE3A9BEB87F8A2FE9B8B5292E5A021FFF5E91479E"
   "8CE7A28C2442C6F315180F93499A234DCF76E3FED135F9BB",
   "2"
 },
#endif /* end removed section 1 */
 { /* 2048 */
   "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
   "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
   "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
   "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
   "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
   "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
   "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73",
   "2"
 },
#if 0 /* begin removed section 2 */
 { /* 3072 */
   "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B"
   "139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485"
   "B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1F"
   "E649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23"
   "DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32"
   "905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF69558"
   "17183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521"
   "ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D7"
   "1E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B1817"
   "7B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82"
   "D120A93AD2CAFFFFFFFFFFFFFFFF",
   "5"
 },
 { /* 4096 */
   "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
   "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
   "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
   "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
   "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8"
   "FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
   "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C"
   "180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
   "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D"
   "04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
   "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D226"
   "1AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
   "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFC"
   "E0FD108E4B82D120A92108011A723C12A787E6D788719A10BDBA5B26"
   "99C327186AF4E23C1A946834B6150BDA2583E9CA2AD44CE8DBBBC2DB"
   "04DE8EF92E8EFC141FBECAA6287C59474E6BC05D99B2964FA090C3A2"
   "233BA186515BE7ED1F612970CEE2D7AFB81BDD762170481CD0069127"
   "D5B05AA993B4EA988D8FDDC186FFB7DC90A6C08F4DF435C934063199"
   "FFFFFFFFFFFFFFFF",
   "5"
 },
 { /* 6144 */
   "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
   "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
   "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
   "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
   "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8"
   "FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
   "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C"
   "180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
   "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D"
   "04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
   "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D226"
   "1AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
   "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFC"
   "E0FD108E4B82D120A92108011A723C12A787E6D788719A10BDBA5B26"
   "99C327186AF4E23C1A946834B6150BDA2583E9CA2AD44CE8DBBBC2DB"
   "04DE8EF92E8EFC141FBECAA6287C59474E6BC05D99B2964FA090C3A2"
   "233BA186515BE7ED1F612970CEE2D7AFB81BDD762170481CD0069127"
   "D5B05AA993B4EA988D8FDDC186FFB7DC90A6C08F4DF435C934028492"
   "36C3FAB4D27C7026C1D4DCB2602646DEC9751E763DBA37BDF8FF9406"
   "AD9E530EE5DB382F413001AEB06A53ED9027D831179727B0865A8918"
   "DA3EDBEBCF9B14ED44CE6CBACED4BB1BDB7F1447E6CC254B33205151"
   "2BD7AF426FB8F401378CD2BF5983CA01C64B92ECF032EA15D1721D03"
   "F482D7CE6E74FEF6D55E702F46980C82B5A84031900B1C9E59E7C97F"
   "BEC7E8F323A97A7E36CC88BE0F1D45B7FF585AC54BD407B22B4154AA"
   "CC8F6D7EBF48E1D814CC5ED20F8037E0A79715EEF29BE32806A1D58B"
   "B7C5DA76F550AA3D8A1FBFF0EB19CCB1A313D55CDA56C9EC2EF29632"
   "387FE8D76E3C0468043E8F663F4860EE12BF2D5B0B7474D6E694F91E"
   "6DCC4024FFFFFFFFFFFFFFFF",
   "5"
 },
 { /* 8192 */
   "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
   "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
   "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
   "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
   "49286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8"
   "FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D"
   "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C"
   "180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
   "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D"
   "04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7D"
   "B3970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D226"
   "1AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
   "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFC"
   "E0FD108E4B82D120A92108011A723C12A787E6D788719A10BDBA5B26"
   "99C327186AF4E23C1A946834B6150BDA2583E9CA2AD44CE8DBBBC2DB"
   "04DE8EF92E8EFC141FBECAA6287C59474E6BC05D99B2964FA090C3A2"
   "233BA186515BE7ED1F612970CEE2D7AFB81BDD762170481CD0069127"
   "D5B05AA993B4EA988D8FDDC186FFB7DC90A6C08F4DF435C934028492"
   "36C3FAB4D27C7026C1D4DCB2602646DEC9751E763DBA37BDF8FF9406"
   "AD9E530EE5DB382F413001AEB06A53ED9027D831179727B0865A8918"
   "DA3EDBEBCF9B14ED44CE6CBACED4BB1BDB7F1447E6CC254B33205151"
   "2BD7AF426FB8F401378CD2BF5983CA01C64B92ECF032EA15D1721D03"
   "F482D7CE6E74FEF6D55E702F46980C82B5A84031900B1C9E59E7C97F"
   "BEC7E8F323A97A7E36CC88BE0F1D45B7FF585AC54BD407B22B4154AA"
   "CC8F6D7EBF48E1D814CC5ED20F8037E0A79715EEF29BE32806A1D58B"
   "B7C5DA76F550AA3D8A1FBFF0EB19CCB1A313D55CDA56C9EC2EF29632"
   "387FE8D76E3C0468043E8F663F4860EE12BF2D5B0B7474D6E694F91E"
   "6DBE115974A3926F12FEE5E438777CB6A932DF8CD8BEC4D073B931BA"
   "3BC832B68D9DD300741FA7BF8AFC47ED2576F6936BA424663AAB639C"
   "5AE4F5683423B4742BF1C978238F16CBE39D652DE3FDB8BEFC848AD9"
   "22222E04A4037C0713EB57A81A23F0C73473FC646CEA306B4BCBC886"
   "2F8385DDFA9D4B7FA2C087E879683303ED5BDD3A062B3CF5B3A278A6"
   "6D2A13F83F44F82DDF310EE074AB6A364597E899A0255DC164F31CC5"
   "0846851DF9AB48195DED7EA1B1D510BD7EE74D73FAF36BC31ECFA268"
   "359046F4EB879F924009438B481C6CD7889A002ED5EE382BC9190DA6"
   "FC026E479558E4475677E9AA9E3050E2765694DFC81F56E880B96E71"
   "60C980DD98EDD3DFFFFFFFFFFFFFFFFF",
   "13"
 },
 #endif /* end removed section 2 */
 {0,0} /* null sentinel */
};


static NGConstant * new_ng( SRP_NGType ng_type, const char * n_hex, const char * g_hex )
{
    NGConstant * ng   = (NGConstant *) malloc( sizeof(NGConstant) );
    if (!ng) {
        printf("Memory allocation failure (ng)\n");
        exit(1);
    }
    ng->N             = BN_new();
    ng->g             = BN_new();

    if( !ng || !ng->N || !ng->g )
       return 0;

    if ( ng_type != SRP_NG_CUSTOM )
    {
        int idx = ng_type;
        if ( ng_type > SRP_NG_CUSTOM )
           idx -= 1;
        n_hex = global_Ng_constants[ idx ].n_hex;
        g_hex = global_Ng_constants[ idx ].g_hex;
    }
    BN_hex2bn( &ng->N, n_hex );
    BN_hex2bn( &ng->g, g_hex );

    return ng;
}

static void delete_ng( NGConstant * ng )
{
   if (ng)
   {
      BN_free( ng->N );
      BN_free( ng->g );
      ng->N = 0;
      ng->g = 0;
      free(ng);
   }
}

typedef struct HashCTX_s {
   EVP_MD_CTX *digest_ctx;
} HashCTX_t;


struct SRPVerifier
{
    SRP_HashAlgorithm  hash_alg;
    NGConstant        *ng;
    const char          * username;
    const unsigned char * bytes_B;
    int                   authenticated;
    int                   rfc5054;
    unsigned char M           [SHA512_DIGEST_LENGTH];
    unsigned char H_AMK       [SHA512_DIGEST_LENGTH];
#ifdef APPLE_VARIANT
    unsigned char session_key [2 * SHA512_DIGEST_LENGTH];
#else
    unsigned char session_key [SHA512_DIGEST_LENGTH];
#endif
};

#if 0   /*begin removed section 3*/
struct SRPUser
{
    SRP_HashAlgorithm  hash_alg;
    NGConstant        *ng;

    BIGNUM *a;
    BIGNUM *A;
    BIGNUM *S;

    const unsigned char * bytes_A;
    int                   authenticated;
    int                   rfc5054;

    const char *          username;
    const unsigned char * password;
    int                   password_len;

    unsigned char M           [SHA512_DIGEST_LENGTH];
    unsigned char H_AMK       [SHA512_DIGEST_LENGTH];
#ifdef APPLE_VARIANT
    unsigned char session_key [2 * SHA512_DIGEST_LENGTH];
#else
    unsigned char session_key [SHA512_DIGEST_LENGTH];
#endif
  
};
#endif   /*end removed section 3*/
static void handle_error(const char* location) {
    long error = ERR_get_error();
    const char* error_str = ERR_error_string(error, NULL);
    fprintf(stderr, "SRP error at %s: %s\n", location, error_str);
    exit(EXIT_FAILURE);
}

static void hash_destroy( HashCTX_t *ctx) {
    if (ctx) {
        EVP_MD_CTX_free(ctx->digest_ctx);
        free(ctx);
    }
}

static HashCTX_t *hash_create()
{
  HashCTX_t *ctx = (HashCTX_t *) malloc(sizeof(HashCTX_t));
  assert(ctx != NULL);
  ctx->digest_ctx = EVP_MD_CTX_new();
  assert(ctx->digest_ctx != NULL);
  return ctx;
}
		    
int hash_reset( HashCTX_t *ctx)
{
  return EVP_MD_CTX_reset( ctx->digest_ctx); 
}

static int hash_init( SRP_HashAlgorithm alg, HashCTX_t *ctx)
{
  int ret;
  switch (alg)
    {
      case SRP_SHA1  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha1(), NULL);
        break;
      case SRP_SHA224  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha224(), NULL);
        break;
      case SRP_SHA256  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha256(), NULL);
        break;
      case SRP_SHA384  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha384(), NULL);
        break;
      case SRP_SHA512  :
        ret = EVP_DigestInit_ex(ctx->digest_ctx, EVP_sha512(), NULL);
        break;
      default:
        return -1;
    }
    return ret;    
}

static int hash_update( SRP_HashAlgorithm alg, HashCTX_t *ctx, const void *data, size_t len )
{
    int ret = EVP_DigestUpdate(ctx->digest_ctx, data, len);
    if (!ret) {  
        handle_error(__func__);
    }
    return ret;
}

static int hash_final( SRP_HashAlgorithm alg, HashCTX_t *ctx, unsigned char *md, unsigned int *md_len )
{
    int ret = EVP_DigestFinal_ex(ctx->digest_ctx, md, md_len);
    if (!ret) {
        handle_error(__func__);
    }
    return  ret;
}

static unsigned char * hash( SRP_HashAlgorithm alg, const unsigned char *d, size_t n, unsigned char *md )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA1( d, n, md );
      case SRP_SHA224: return SHA224( d, n, md );
      case SRP_SHA256: return SHA256( d, n, md );
      case SRP_SHA384: return SHA384( d, n, md );
      case SRP_SHA512: return SHA512( d, n, md );
      default:
        return 0;
    };
}

static int hash_length( SRP_HashAlgorithm alg )
{
    switch (alg)
    {
      case SRP_SHA1  : return SHA_DIGEST_LENGTH;
      case SRP_SHA224: return SHA224_DIGEST_LENGTH;
      case SRP_SHA256: return SHA256_DIGEST_LENGTH;
      case SRP_SHA384: return SHA384_DIGEST_LENGTH;
      case SRP_SHA512: return SHA512_DIGEST_LENGTH;
      default:
        return -1;
    };
}

static BIGNUM * H_nn_orig( SRP_HashAlgorithm alg, const BIGNUM * n1, const BIGNUM * n2 )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n1 = BN_num_bytes(n1);
    int             len_n2 = BN_num_bytes(n2);
    int             nbytes = len_n1 + len_n2;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n1, bin);
    BN_bn2bin(n2, bin + len_n1);
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * H_nn_rfc5054( SRP_HashAlgorithm alg, const BIGNUM * N, const BIGNUM * n1, const BIGNUM * n2 )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_N  = BN_num_bytes(N);
    int             len_n1 = BN_num_bytes(n1);
    int             len_n2 = BN_num_bytes(n2);
    int             nbytes = len_N * 2;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;

    if (len_n1 > len_N || len_n2 > len_N) {
        free (bin);
        return 0;
    }
    
    memset(bin, 0, nbytes);
    BN_bn2bin(n1, bin + (len_N - len_n1));
    BN_bn2bin(n2, bin + (len_N + len_N - len_n2));
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * H_ns( SRP_HashAlgorithm alg, const BIGNUM * n, const unsigned char * bytes, int len_bytes )
{
    unsigned char   buff[ SHA512_DIGEST_LENGTH ];
    int             len_n  = BN_num_bytes(n);
    int             nbytes = len_n + len_bytes;
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if (!bin)
       return 0;
    BN_bn2bin(n, bin);
    memcpy( bin + len_n, bytes, len_bytes );
    hash( alg, bin, nbytes, buff );
    free(bin);
    return BN_bin2bn(buff, hash_length(alg), NULL);
}

static BIGNUM * calculate_x( SRP_HashAlgorithm alg, const BIGNUM * salt, const char * username, const unsigned char * password, int password_len )
{
    unsigned char ucp_hash[SHA512_DIGEST_LENGTH];
    unsigned int ucp_hash_len;
    HashCTX_t *ctx = hash_create();

    hash_init( alg, ctx);
    hash_update( alg, ctx, username, strlen(username) );
    hash_update( alg, ctx, ":", 1 );
    hash_update( alg, ctx, password, password_len );
    hash_final( alg, ctx, ucp_hash, &ucp_hash_len );
    hash_destroy ( ctx);

    return H_ns( alg, salt, ucp_hash, hash_length(alg) );
}

static void update_hash_n( SRP_HashAlgorithm alg, HashCTX_t *ctx, const BIGNUM * n )
{
    unsigned long len = BN_num_bytes(n);
    unsigned char * n_bytes = (unsigned char *) malloc( len );
    if (!n_bytes)
       return;
    BN_bn2bin(n, n_bytes);
    hash_update(alg, ctx, n_bytes, len);
    free(n_bytes);
}

static void hash_num( SRP_HashAlgorithm alg, const BIGNUM * n, unsigned char * dest )
{
    int             nbytes = BN_num_bytes(n);
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    if(!bin)
       return;
    BN_bn2bin(n, bin);
    hash( alg, bin, nbytes, dest );
    free(bin);
}

#ifdef APPLE_VARIANT
/* added for compatibility with Apple's modified srp
 * see https://htmlpreview.github.io/?https://github.com/philippe44/RAOP-Player/blob/master/doc/auth_protocol.html
 */
  
static int hash_session_key( SRP_HashAlgorithm alg, const BIGNUM * n, unsigned char * dest )
{
    HashCTX_t *ctx;
    int             nbytes = BN_num_bytes(n);
    unsigned char * bin    = (unsigned char *) malloc( nbytes );
    unsigned char fourbytes[4] = { 0 };  //Apple's modified SRP protocol
    unsigned int len1, len2;
    if(!bin)
       return -1;
    BN_bn2bin(n, bin);
    
    ctx = hash_create();
    hash_init(alg, ctx);
    hash_update( alg, ctx, bin, nbytes);
    hash_update( alg, ctx, fourbytes, sizeof(fourbytes));
    hash_final( alg, ctx, dest, &len1);
    hash_reset( ctx);

    fourbytes[3] = 1;
    hash_init(alg, ctx);
    hash_update( alg, ctx, bin, nbytes);
    hash_update( alg, ctx, fourbytes, sizeof(fourbytes));
    hash_final( alg, ctx, dest + len1, &len2);
    hash_destroy( ctx);

    free(bin);
    return len1 + len2;
}
#endif

static void calculate_M( SRP_HashAlgorithm alg, NGConstant *ng, unsigned char * dest, const char * I, const BIGNUM * s,
                         const BIGNUM * A, const BIGNUM * B, const unsigned char * K )
{
    unsigned char H_N[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_g[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_I[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned char H_xor[ SHA512_DIGEST_LENGTH ] = {0};
    unsigned int dest_len;
    HashCTX_t *ctx;
    int           i = 0;
    int           hash_len = hash_length(alg);

    hash_num( alg, ng->N, H_N );
    hash_num( alg, ng->g, H_g );
    hash(alg, (const unsigned char *)I, strlen(I), H_I);

    for (i=0; i < hash_len; i++ )
        H_xor[i] = H_N[i] ^ H_g[i];

    ctx = hash_create();
    hash_init( alg, ctx);
    hash_update( alg, ctx, H_xor, hash_len );
    hash_update( alg, ctx, H_I,   hash_len );
    update_hash_n( alg, ctx, s );
    update_hash_n( alg, ctx, A );
    update_hash_n( alg, ctx, B );
#ifdef APPLE_VARIANT  /* Apple's SRP session key length  is 2 x hash_len */
    hash_update( alg, ctx, K, 2 * hash_len );
#else
    hash_update( alg, ctx, K, hash_len );
#endif
    hash_final( alg, ctx, dest, &dest_len );
    hash_destroy ( ctx);
    
}

static void calculate_H_AMK( SRP_HashAlgorithm alg, unsigned char *dest, const BIGNUM * A, const unsigned char * M, const unsigned char * K )
{
    HashCTX_t *ctx;
    unsigned int dest_len;

    ctx = hash_create();
    hash_init( alg, ctx);
    update_hash_n( alg, ctx, A );
    hash_update( alg, ctx, M, hash_length(alg) );
#ifdef APPLE_VARIANT
    hash_update( alg, ctx, K, 2 * hash_length(alg) );
#else
    hash_update( alg, ctx, K, hash_length(alg) );
#endif
    hash_final( alg, ctx, dest, &dest_len );
    hash_destroy ( ctx);
}



static void init_random()
{
    if (g_initialized)
        return;

#ifdef WIN32
    HCRYPTPROV wctx;
#else
    FILE   *fp   = 0;
#endif
    unsigned char buff[64];

#ifdef WIN32
        CryptAcquireContext(&wctx, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(wctx, sizeof(buff), (BYTE*) buff);
        CryptReleaseContext(wctx, 0);
        g_initialized = 1;
#else
        fp = fopen("/dev/urandom", "r");
        if (fp)
        {
            size_t count = fread(buff, sizeof(buff), 1, fp);
            fclose(fp);
            if (count == 1) {
                g_initialized = 1;
            }
        }
#endif
    if (g_initialized)
       RAND_seed( buff, sizeof(buff) );
}


/***********************************************************************************************************
 *
 *  Exported Functions
 *
 ***********************************************************************************************************/

void srp_random_seed( const unsigned char * random_data, int data_length )
{
    g_initialized = 1;

    if (random_data)
        RAND_seed( random_data, data_length );
}


void srp_create_salted_verification_key( SRP_HashAlgorithm alg,
                                         SRP_NGType ng_type, const char * username,
                                         const unsigned char * password, int len_password,
                                         const unsigned char ** bytes_s, int * len_s,
                                         const unsigned char ** bytes_v, int * len_v,
                                         const char * n_hex, const char * g_hex )
{
    BIGNUM     * s   = BN_new();
    BIGNUM     * v   = BN_new();
    BIGNUM     * x   = 0;
    BN_CTX     * ctx = BN_CTX_new();
    NGConstant * ng  = new_ng( ng_type, n_hex, g_hex );

    if( !s || !v || !ctx || !ng )
       goto cleanup_and_exit;

    init_random(); /* Only happens once */

#ifdef APPLE_VARIANT  //use a 16 byte salt
    BN_rand(s, 128, -1, 0);
#else
    BN_rand(s, 32, -1, 0);
#endif    
    x = calculate_x( alg, s, username, password, len_password );

    if( !x )
       goto cleanup_and_exit;

    BN_mod_exp(v, ng->g, x, ng->N, ctx);

    *len_s   = BN_num_bytes(s);
    *len_v   = BN_num_bytes(v);

    *bytes_s = (const unsigned char *) malloc( *len_s );
    *bytes_v = (const unsigned char *) malloc( *len_v );

    if (!bytes_s || !bytes_v)
       goto cleanup_and_exit;

    BN_bn2bin(s, (unsigned char *) *bytes_s);
    BN_bn2bin(v, (unsigned char *) *bytes_v);

 cleanup_and_exit:
    delete_ng( ng );
    BN_free(s);
    BN_free(v);
    BN_free(x);
    BN_CTX_free(ctx);
}
#ifdef APPLE_VARIANT

/* Out: bytes_B, len_B, bytes_b, len_b 
 * On failure, bytes_B and bytes_b  will be set to NULL 
 * len_B  and len_will be set to 0
 */
void srp_create_server_ephemeral_key( SRP_HashAlgorithm alg, SRP_NGType ng_type,
                                      const unsigned char * bytes_v, int len_v,  
                                      const unsigned char * bytes_b, int len_b,
                                      const unsigned char ** bytes_B, int * len_B,
                                      const char * n_hex, const char * g_hex,
                                      int rfc5054_compat ) {
  BIGNUM             *v    = BN_bin2bn(bytes_v, len_v, NULL);
  BIGNUM             *tmp1 = BN_new();
  BIGNUM             *tmp2 = BN_new();
  BIGNUM             *B    = BN_new();
  BIGNUM             *b    = BN_new();
  BIGNUM             *k    = 0;
  BN_CTX             *ctx  = BN_CTX_new();
  NGConstant         *ng   = new_ng( ng_type, n_hex, g_hex );

  *len_B   = 0;
  *bytes_B = 0;

  if( !v || !B || !b || !tmp1 || !tmp2 || !ctx || !ng )
    goto cleanup_and_exit;

  b = BN_bin2bn(bytes_b, len_b, NULL);
  
  if (rfc5054_compat)
    k = H_nn_rfc5054(alg, ng->N, ng->N, ng->g);
  else
    k = H_nn_orig(alg, ng->N, ng->g);
  
  if(!k)
    goto cleanup_and_exit;

  /* B = kv + g^b */
  if (rfc5054_compat)
    {
      BN_mod_mul(tmp1, k, v, ng->N, ctx);
      BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
      BN_mod_add(B, tmp1, tmp2, ng->N, ctx);
    }
  else
    {
      BN_mul(tmp1, k, v, ctx);
      BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
      BN_add(B, tmp1, tmp2);
    }

  *len_B   = BN_num_bytes(B);
  *bytes_B = (const unsigned char *)malloc( *len_B );
   BN_bn2bin( B, (unsigned char *) *bytes_B );  

 cleanup_and_exit:
   BN_free(v);
   if (k) BN_free(k);
   BN_free(B);
   BN_free(b);
   BN_free(tmp1);
   BN_free(tmp2);
   BN_CTX_free(ctx);
}
#endif

/* Out: bytes_B, len_B.
 *
 * On failure, bytes_B will be set to NULL and len_B will be set to 0
 */
struct SRPVerifier *  srp_verifier_new( SRP_HashAlgorithm alg, SRP_NGType ng_type, const char * username,
                                        const unsigned char * bytes_s, int len_s,
                                        const unsigned char * bytes_v, int len_v,
                                        const unsigned char * bytes_A, int len_A,
#ifdef APPLE_VARIANT
					const unsigned char * bytes_b, int len_b,
#endif
                                        const unsigned char ** bytes_B, int * len_B,
                                        const char * n_hex, const char * g_hex,
                                        int rfc5054_compat )
{
    BIGNUM             *s    = BN_bin2bn(bytes_s, len_s, NULL);
    BIGNUM             *v    = BN_bin2bn(bytes_v, len_v, NULL);
    BIGNUM             *A    = BN_bin2bn(bytes_A, len_A, NULL);
    BIGNUM             *u    = 0;
    BIGNUM             *B    = BN_new();
    BIGNUM             *S    = BN_new();
    BIGNUM             *b    = BN_new();
    BIGNUM             *k    = 0;
    BIGNUM             *tmp1 = BN_new();
    BIGNUM             *tmp2 = BN_new();
    BN_CTX             *ctx  = BN_CTX_new();
    int                 ulen = strlen(username) + 1;
    NGConstant         *ng   = new_ng( ng_type, n_hex, g_hex );
    struct SRPVerifier *ver  = 0;

    *len_B   = 0;
    *bytes_B = 0;

    if( !s || !v || !A || !B || !S || !b || !tmp1 || !tmp2 || !ctx || !ng )
       goto cleanup_and_exit;

    ver = (struct SRPVerifier *) malloc( sizeof(struct SRPVerifier) );

    if (!ver)
       goto cleanup_and_exit;

    init_random(); /* Only happens once */

    ver->username = (char *) malloc( ulen );
    ver->hash_alg = alg;
    ver->ng       = ng;

    if (!ver->username)
    {
       free(ver);
       ver = 0;
       goto cleanup_and_exit;
    }

    memcpy( (char*)ver->username, username, ulen );

    ver->authenticated = 0;
    ver->rfc5054 = rfc5054_compat;

    /* SRP-6a safety check */
    BN_mod(tmp1, A, ng->N, ctx);
    if ( !BN_is_zero(tmp1) )
    {
#ifdef APPLE_VARIANT
       if ( !len_b || !bytes_b) {
#endif
       BN_rand(b, 256, -1, 0);
#ifdef APPLE_VARIANT
       } else {
           b = BN_bin2bn(bytes_b, len_b, NULL);
       }
#endif

       if (rfc5054_compat)
          k = H_nn_rfc5054(alg, ng->N, ng->N, ng->g);
       else
          k = H_nn_orig(alg, ng->N, ng->g);

       if(!k)
       {
          free(ver);
          ver = 0;
          goto cleanup_and_exit;
       }

       /* B = kv + g^b */
       if (rfc5054_compat)
       {
          BN_mod_mul(tmp1, k, v, ng->N, ctx);
          BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
          BN_mod_add(B, tmp1, tmp2, ng->N, ctx);
       }
       else
       {
          BN_mul(tmp1, k, v, ctx);
          BN_mod_exp(tmp2, ng->g, b, ng->N, ctx);
          BN_add(B, tmp1, tmp2);
       }

       if (rfc5054_compat)
          u = H_nn_rfc5054(alg, ng->N, A, B);
       else
          u = H_nn_orig(alg, A, B);

       if(!u)
       {
          free(ver);
          ver = 0;
          goto cleanup_and_exit;
       }

       /* S = (A *(v^u)) ^ b */
       BN_mod_exp(tmp1, v, u, ng->N, ctx);
       BN_mul(tmp2, A, tmp1, ctx);
       BN_mod_exp(S, tmp2, b, ng->N, ctx);

#ifdef APPLE_VARIANT
       hash_session_key(alg, S, ver->session_key);
#else
       hash_num(alg, S, ver->session_key);
#endif
       calculate_M( alg, ng, ver->M, username, s, A, B, ver->session_key );
       calculate_H_AMK( alg, ver->H_AMK, A, ver->M, ver->session_key );

       *len_B   = BN_num_bytes(B);
       *bytes_B = (const unsigned char *)malloc( *len_B );

       if( !((const unsigned char *)*bytes_B) )
       {
          free( (void*) ver->username );
          free( ver );
          ver = 0;
          *len_B = 0;
          goto cleanup_and_exit;
       }

       BN_bn2bin( B, (unsigned char *) *bytes_B );

       ver->bytes_B = *bytes_B;
    } else {
       free(ver);
       ver = 0;
    }

 cleanup_and_exit:
    BN_free(s);
    BN_free(v);
    BN_free(A);
    if (u) BN_free(u);
    if (k) BN_free(k);
    BN_free(B);
    BN_free(S);
    BN_free(b);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_CTX_free(ctx);

    return ver;
}

void srp_verifier_delete( struct SRPVerifier * ver )
{
   if (ver)
   {
      delete_ng( ver->ng );
      free( (char *) ver->username );
      free( (unsigned char *) ver->bytes_B );
      memset(ver, 0, sizeof(*ver));
      free( ver );
   }
}

int srp_verifier_is_authenticated( struct SRPVerifier * ver )
{
    return ver->authenticated;
}

const char * srp_verifier_get_username( struct SRPVerifier * ver )
{
    return ver->username;
}

const unsigned char * srp_verifier_get_session_key( struct SRPVerifier * ver, int * key_length )
{
    if (key_length)
#ifdef APPLE_VARIANT
      *key_length = 2 * hash_length( ver->hash_alg );
#else
      *key_length = hash_length( ver->hash_alg );
#endif
    return ver->session_key;
}

int srp_verifier_get_session_key_length( struct SRPVerifier * ver )
{
#ifdef APPLE_VARIANT
    return 2 * hash_length( ver->hash_alg );
#else
    return hash_length( ver->hash_alg );
#endif
}

/* user_M must be exactly SHA512_DIGEST_LENGTH bytes in size */
void srp_verifier_verify_session( struct SRPVerifier * ver, const unsigned char * user_M, const unsigned char ** bytes_HAMK )
{
    if ( memcmp( ver->M, user_M, hash_length(ver->hash_alg) ) == 0 )
    {
        ver->authenticated = 1;
        *bytes_HAMK = ver->H_AMK;
    }
    else
        *bytes_HAMK = NULL;
}

/*******************************************************************************/
#if 0    /*begin removed section 4 */
struct SRPUser * srp_user_new( SRP_HashAlgorithm alg, SRP_NGType ng_type, const char * username,
                               const unsigned char * bytes_password, int len_password,
                               const char * n_hex, const char * g_hex,
                               int rfc5054_compat )
{
    struct SRPUser  *usr  = (struct SRPUser *) malloc( sizeof(struct SRPUser) );
    int              ulen = strlen(username) + 1;

    if (!usr)
       goto err_exit;

    init_random(); /* Only happens once */
    usr->hash_alg = alg;
    usr->ng       = new_ng( ng_type, n_hex, g_hex );
    usr->a = BN_new();
    usr->A = BN_new();
    usr->S = BN_new();

    if (!usr->ng || !usr->a || !usr->A || !usr->S)
       goto err_exit;

    usr->username     = (const char *) malloc(ulen);
    usr->password     = (const unsigned char *) malloc(len_password);
    usr->password_len = len_password;

    if (!usr->username || !usr->password)
       goto err_exit;

    memcpy((char *)usr->username, username,       ulen);
    memcpy((char *)usr->password, bytes_password, len_password);

    usr->authenticated = 0;
    usr->rfc5054 = rfc5054_compat;

    usr->bytes_A = 0;

    return usr;

 err_exit:
    if (usr)
    {
       BN_free(usr->a);
       BN_free(usr->A);
       BN_free(usr->S);
       if (usr->username)
          free((void*)usr->username);
       if (usr->password)
       {
          memset((void*)usr->password, 0, usr->password_len);
          free((void*)usr->password);
       }
       free(usr);
    }

    return 0;
}

void srp_user_delete( struct SRPUser * usr )
{
   if( usr )
   {
      BN_free( usr->a );
      BN_free( usr->A );
      BN_free( usr->S );
      delete_ng( usr->ng );

      memset((void*)usr->password, 0, usr->password_len);
      free((char *)usr->username);
      free((char *)usr->password);
      if (usr->bytes_A)
         free( (char *)usr->bytes_A );

      memset(usr, 0, sizeof(*usr));
      free( usr );
   }
}

int srp_user_is_authenticated( struct SRPUser * usr)
{
    return usr->authenticated;
}

const char * srp_user_get_username( struct SRPUser * usr )
{
    return usr->username;
}

const unsigned char * srp_user_get_session_key( struct SRPUser * usr, int * key_length )
{
    if (key_length)
#ifdef APPLE_VARIANT
        *key_length = 2 * hash_length( usr->hash_alg );
#else
        *key_length = hash_length( usr->hash_alg );
#endif
    return usr->session_key;
}

int srp_user_get_session_key_length( struct SRPUser * usr )
{
#ifdef APPLE_VARIANT
    return 2 * hash_length( usr->hash_alg );
#else
    return hash_length( usr->hash_alg );
#endif
}

/* Output: username, bytes_A, len_A */
void  srp_user_start_authentication( struct SRPUser * usr, const char ** username, 
                                     const unsigned char ** bytes_A, int * len_A )
{
    BN_CTX  *ctx  = BN_CTX_new();
    BN_rand(usr->a, 256, -1, 0);
    BN_mod_exp(usr->A, usr->ng->g, usr->a, usr->ng->N, ctx);
    BN_CTX_free(ctx);

    *len_A   = BN_num_bytes(usr->A);
    *bytes_A = (const unsigned char *)malloc( *len_A );

    if (!*bytes_A)
    {
       *len_A = 0;
       *bytes_A = 0;
       *username = 0;
       return;
    }

    BN_bn2bin( usr->A, (unsigned char *) *bytes_A );
    usr->bytes_A = *bytes_A;
    *username = usr->username;
}

/* Output: bytes_M. Buffer length is SHA512_DIGEST_LENGTH */
void  srp_user_process_challenge( struct SRPUser * usr,
                                  const unsigned char * bytes_s, int len_s,
                                  const unsigned char * bytes_B, int len_B,
                                  const unsigned char ** bytes_M, int * len_M)
{
    BIGNUM *s    = BN_bin2bn(bytes_s, len_s, NULL);
    BIGNUM *B    = BN_bin2bn(bytes_B, len_B, NULL);
    BIGNUM *u    = 0;
    BIGNUM *x    = 0;
    BIGNUM *k    = 0;
    BIGNUM *v    = BN_new();
    BIGNUM *tmp1 = BN_new();
    BIGNUM *tmp2 = BN_new();
    BIGNUM *tmp3 = BN_new();
    BN_CTX *ctx  = BN_CTX_new();

    *len_M = 0;
    *bytes_M = 0;

    if( !s || !B || !v || !tmp1 || !tmp2 || !tmp3 || !ctx )
       goto cleanup_and_exit;

    if (usr->rfc5054)
       u = H_nn_rfc5054(usr->hash_alg, usr->ng->N, usr->A, B);
    else
       u = H_nn_orig(usr->hash_alg, usr->A, B);

    if (!u)
       goto cleanup_and_exit;

    x = calculate_x( usr->hash_alg, s, usr->username, usr->password, usr->password_len );

    if (!x)
       goto cleanup_and_exit;

    if (usr->rfc5054)
       k = H_nn_rfc5054(usr->hash_alg, usr->ng->N, usr->ng->N, usr->ng->g);
    else
       k = H_nn_orig(usr->hash_alg, usr->ng->N, usr->ng->g);

    if (!k)
       goto cleanup_and_exit;

    /* SRP-6a safety check */
    if ( !BN_is_zero(B) && !BN_is_zero(u) )
    {
        BN_mod_exp(v, usr->ng->g, x, usr->ng->N, ctx);

        /* S = (B - k*(g^x)) ^ (a + ux) */
        BN_mul(tmp1, u, x, ctx);
        BN_add(tmp2, usr->a, tmp1);             /* tmp2 = (a + ux)      */
        BN_mod_exp(tmp1, usr->ng->g, x, usr->ng->N, ctx);
        BN_mul(tmp3, k, tmp1, ctx);             /* tmp3 = k*(g^x)       */
        BN_sub(tmp1, B, tmp3);                  /* tmp1 = (B - K*(g^x)) */
        BN_mod_exp(usr->S, tmp1, tmp2, usr->ng->N, ctx);

#ifdef APPLE_VARIANT    
        hash_session_key(usr->hash_alg, usr->S, usr->session_key);
#else
        hash_num(usr->hash_alg, usr->S, usr->session_key);
#endif
        calculate_M( usr->hash_alg, usr->ng, usr->M, usr->username, s, usr->A, B, usr->session_key );
        calculate_H_AMK( usr->hash_alg, usr->H_AMK, usr->A, usr->M, usr->session_key );
        *bytes_M = usr->M;
        if (len_M)
            *len_M = hash_length( usr->hash_alg );
    }
    else
    {
        *bytes_M = NULL;
        if (len_M)
            *len_M   = 0;
    }

 cleanup_and_exit:
    BN_free(s);
    BN_free(B);
    BN_free(u);
    BN_free(x);
    BN_free(k);
    BN_free(v);
    BN_free(tmp1);
    BN_free(tmp2);
    BN_free(tmp3);
    BN_CTX_free(ctx);
}

void srp_user_verify_session( struct SRPUser * usr, const unsigned char * bytes_HAMK )
{
    if ( memcmp( usr->H_AMK, bytes_HAMK, hash_length(usr->hash_alg) ) == 0 )
        usr->authenticated = 1;
}
#endif  /*end removed section 4 */
