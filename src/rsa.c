/* adbng - SPDX-License-Identifier: MIT
 *
 * Minimal RSA-2048 (modular exponentiation only, no key generation, no
 * bignum library).  The public exponent is always 65537 for verification.
 */
#include "rsa.h"
#include "util.h"

#include <string.h>

#define L RSA_LIMBS

typedef struct { uint32_t v[L]; } bn;

static const unsigned char DIGEST_INFO_SHA1[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a,
    0x05, 0x00, 0x04, 0x14
};

static int bn_cmp(const bn *a, const bn *b)
{
    for (int i = L - 1; i >= 0; i--)
        if (a->v[i] != b->v[i]) return a->v[i] < b->v[i] ? -1 : 1;
    return 0;
}

static unsigned bn_add(bn *r, const bn *a, const bn *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < L; i++) {
        uint64_t t = (uint64_t)a->v[i] + b->v[i] + carry;
        r->v[i] = (uint32_t)t;
        carry = t >> 32;
    }
    return (unsigned)carry;
}

static void bn_sub(bn *r, const bn *a, const bn *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < L; i++) {
        uint64_t t = (uint64_t)a->v[i] - b->v[i] - borrow;
        r->v[i] = (uint32_t)t;
        borrow = (t >> 32) & 1;
    }
}

static void bn_addmod(bn *r, const bn *a, const bn *b, const bn *m)
{
    unsigned carry = bn_add(r, a, b);
    if (carry || bn_cmp(r, m) >= 0) bn_sub(r, r, m);
}

static void bn_shl1mod(bn *r, const bn *a, const bn *m)
{
    uint32_t hi = 0;
    for (int i = 0; i < L; i++) {
        uint32_t nv = (a->v[i] << 1) | hi;
        hi = a->v[i] >> 31;
        r->v[i] = nv;
    }
    if (hi || bn_cmp(r, m) >= 0) bn_sub(r, r, m);
}

static inline int bn_bit(const bn *a, int i) { return (a->v[i >> 5] >> (i & 31)) & 1; }

/* r = a * b mod m, shift-and-add over the bits of a.
 * Inputs are copied first: callers do things like r = a * a. */
static void bn_mulmod(bn *r, const bn *a, const bn *b, const bn *m)
{
    bn A = *a, B = *b, acc;
    memset(&acc, 0, sizeof acc);
    for (int i = L * 32 - 1; i >= 0; i--) {
        bn_shl1mod(&acc, &acc, m);
        if (bn_bit(&A, i)) bn_addmod(&acc, &acc, &B, m);
    }
    *r = acc;
}

static int bn_top_bit(const bn *a)
{
    for (int i = L - 1; i >= 0; i--)
        if (a->v[i])
            for (int b = 31; b >= 0; b--)
                if ((a->v[i] >> b) & 1u) return i * 32 + b;
    return -1;
}

/* r = base ^ exp mod m, square-and-multiply */
static void bn_modexp(bn *r, const bn *base, const bn *exp, const bn *m)
{
    bn acc, b = *base;
    memset(&acc, 0, sizeof acc);
    acc.v[0] = 1;
    int top = bn_top_bit(exp);
    for (int i = top; i >= 0; i--) {
        bn_mulmod(&acc, &acc, &acc, m);
        if (bn_bit(exp, i)) bn_mulmod(&acc, &acc, &b, m);
    }
    *r = acc;
}

static void bn_from_be(bn *r, const unsigned char *b, size_t len)
{
    memset(r, 0, sizeof *r);
    for (size_t i = 0; i < len; i++) {
        size_t pos = len - 1 - i;              /* byte position from LSB */
        r->v[pos / 4] |= (uint32_t)b[i] << ((pos % 4) * 8);
    }
}

static void bn_to_be(const bn *a, unsigned char *out, size_t len)
{
    memset(out, 0, len);
    for (size_t i = 0; i < L * 4 && i < len; i++) {
        size_t pos = len - 1 - i;
        out[pos] = (unsigned char)(a->v[i / 4] >> ((i % 4) * 8));
    }
}

static void em_build_sha1(unsigned char em[256], const void *msg, size_t msglen)
{
    unsigned char h[20];
    sha1(msg, msglen, h);
    memset(em, 0xff, 256);
    em[0] = 0x00;
    em[1] = 0x01;
    size_t body = 15 + 20;                     /* DigestInfo || hash */
    size_t ps = 256 - 3 - body;                /* 218 */
    em[2 + ps] = 0x00;
    memcpy(em + 2 + ps + 1, DIGEST_INFO_SHA1, 15);
    memcpy(em + 2 + ps + 16, h, 20);
}

int rsa_verify_sha1(const uint32_t mod[RSA_LIMBS], const unsigned char sig[256],
                    const void *msg, size_t msglen)
{
    bn n, s, e, m;
    memcpy(n.v, mod, sizeof n.v);
    bn_from_be(&s, sig, 256);
    if (bn_cmp(&s, &n) >= 0) return -1;

    memset(&e, 0, sizeof e);
    e.v[0] = 65537;
    bn_modexp(&m, &s, &e, &n);

    unsigned char out[256], expect[256];
    bn_to_be(&m, out, 256);
    em_build_sha1(expect, msg, msglen);
    if (memcmp(out, expect, 256) != 0) return -2;
    return 0;
}

void rsa_sign_sha1(const uint32_t mod[RSA_LIMBS], const uint32_t priv[RSA_LIMBS],
                   const void *msg, size_t msglen, unsigned char out[256])
{
    bn n, d, em, s;
    memcpy(n.v, mod, sizeof n.v);
    memcpy(d.v, priv, sizeof d.v);
    unsigned char em_be[256];
    em_build_sha1(em_be, msg, msglen);
    bn_from_be(&em, em_be, 256);
    bn_modexp(&s, &em, &d, &n);
    bn_to_be(&s, out, 256);
}

int rsa_modulus_from_pubkey_blob(const unsigned char *blob, size_t len,
                                 uint32_t mod_out[RSA_LIMBS])
{
    if (len < PUBKEY_BLOB_SIZE) return -1;
    uint32_t words = (uint32_t)blob[0] | (uint32_t)blob[1] << 8 |
                     (uint32_t)blob[2] << 16 | (uint32_t)blob[3] << 24;
    if (words != 64) return -2;                /* only RSA-2048 supported */
    for (int i = 0; i < RSA_LIMBS; i++) {
        const unsigned char *p = blob + 8 + 4 * i;
        mod_out[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                     (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    return 0;
}

size_t rsa_make_pubkey_blob(const uint32_t mod[RSA_LIMBS], unsigned char *out, size_t outsz)
{
    if (outsz < PUBKEY_BLOB_SIZE) return 0;
    memset(out, 0, PUBKEY_BLOB_SIZE);
    bn n, rr, one;
    memcpy(n.v, mod, sizeof n.v);
    memset(&one, 0, sizeof one);
    one.v[0] = 1;
    /* rr = (2^2048)^2 mod n = 2^4096 mod n */
    for (int i = 0; i < 2 * 2048; i++) bn_shl1mod(&one, &one, &n);
    rr = one;

    /* n0inv = -1 / n[0] mod 2^32 (Newton), n[0] is odd for RSA moduli */
    uint32_t a = mod[0], x = a;
    for (int i = 0; i < 5; i++) x *= 2u - a * x;
    uint32_t n0inv = (uint32_t)0u - x;

    unsigned char *p = out;
    uint32_t words = 64;
    memcpy(p, &words, 4);           /* host is little-endian */
    p += 4;
    memcpy(p, &n0inv, 4);
    p += 4;
    for (int i = 0; i < RSA_LIMBS; i++) {
        memcpy(p, &mod[i], 4);
        p += 4;
    }
    for (int i = 0; i < RSA_LIMBS; i++) {
        memcpy(p, &rr.v[i], 4);
        p += 4;
    }
    uint32_t e = 65537;
    memcpy(p, &e, 4);
    return PUBKEY_BLOB_SIZE;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int rsa_limbs_from_hex(const char *hex, uint32_t out[RSA_LIMBS])
{
    if (!hex) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;

    /* trailing whitespace / newline is not part of the number */
    const char *end = hex + strlen(hex);
    while (end > hex && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                         end[-1] == '\t'))
        end--;
    size_t len = (size_t)(end - hex);
    if (len == 0 || len > RSA_LIMBS * 8) return -1;

    /* odd digit count: pad on the left so the byte alignment is stable */
    unsigned char be[256];
    memset(be, 0, sizeof be);
    size_t pos = 256;
    size_t i = len;
    while (i > 0) {
        int lo = hex_nibble(hex[--i]);
        if (lo < 0) return -1;
        int hi = 0;
        if (i > 0) {
            hi = hex_nibble(hex[--i]);
            if (hi < 0) return -1;
        }
        if (pos == 0) return -1;
        be[--pos] = (unsigned char)(hi << 4 | lo);
    }
    bn tmp;
    bn_from_be(&tmp, be, 256);
    memcpy(out, tmp.v, sizeof tmp.v);
    return 0;
}