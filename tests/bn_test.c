/* adbng - SPDX-License-Identifier: MIT
 * unit test for the bignum helpers; includes rsa.c to reach the statics.
 */
#include "../src/rsa.c"

#include <stdio.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void dump(const char *tag, const bn *x)
{
    printf("%s: ", tag);
    int top = bn_top_bit(x);
    for (int i = top < 0 ? 0 : top; i >= 0; i--) printf("%d", bn_bit(x, i));
    printf("\n");
}

int main(void)
{
    bn a, b, m, r;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&m, 0, sizeof m);

    /* 3 + 4 mod 7 = 0 (with reduction), 3 + 3 mod 7 = 6 */
    a.v[0] = 3; b.v[0] = 4; m.v[0] = 7;
    bn_addmod(&r, &a, &b, &m);
    printf("3+4 mod7 = %u (expect 0)\n", r.v[0]);
    b.v[0] = 3;
    bn_addmod(&r, &a, &b, &m);
    printf("3+3 mod7 = %u (expect 6)\n", r.v[0]);

    /* shift: 6<<1 mod 7 = 5 */
    b.v[0] = 6;
    bn_shl1mod(&r, &b, &m);
    printf("6*2 mod7 = %u (expect 5)\n", r.v[0]);

    /* mulmod: 3*4 mod 7 = 5, 5*5 mod 7 = 4 */
    a.v[0] = 3; b.v[0] = 4;
    bn_mulmod(&r, &a, &b, &m);
    printf("3*4 mod7 = %u (expect 5)\n", r.v[0]);
    a.v[0] = 5; b.v[0] = 5;
    bn_mulmod(&r, &a, &b, &m);
    printf("5*5 mod7 = %u (expect 4)\n", r.v[0]);

    /* modexp: 3^5 mod 7 = 5 */
    a.v[0] = 3; b.v[0] = 5;
    bn_modexp(&r, &a, &b, &m);
    printf("3^5 mod7 = %u (expect 5)\n", r.v[0]);

    /* 2^10 mod 1000 = 24 */
    bn mm;
    memset(&mm, 0, sizeof mm);
    mm.v[0] = 1000;
    bn two, ten;
    memset(&two, 0, sizeof two);
    memset(&ten, 0, sizeof ten);
    two.v[0] = 2;
    ten.v[0] = 10;
    bn_modexp(&r, &two, &ten, &mm);
    printf("2^10 mod1000 = %u (expect 24)\n", r.v[0]);

    dump("2^10 mod1000 raw", &r);

    /* round trip: big-endian bytes -> limbs -> bytes */
    unsigned char in[256], out[256];
    for (int i = 0; i < 256; i++) in[i] = (unsigned char)(i * 7 + 1);
    bn_from_be(&r, in, sizeof in);
    bn_to_be(&r, out, sizeof out);
    printf("be roundtrip: %s\n", memcmp(in, out, 256) == 0 ? "OK" : "MISMATCH");

    /* EM built by the signature code must survive a round trip too */
    unsigned char token[20];
    for (int i = 0; i < 20; i++) token[i] = (unsigned char)i;
    unsigned char em[256];
    em_build_sha1(em, token, sizeof token);
    bn_from_be(&r, em, 256);
    bn_to_be(&r, out, 256);
    printf("em roundtrip: %s\n", memcmp(em, out, 256) == 0 ? "OK" : "MISMATCH");
    printf("em[0..3]=%02x %02x %02x %02x  em[220..222]=%02x %02x %02x\n", em[0], em[1],
           em[2], em[3], em[220], em[221], em[222]);

    /* mid-size check against python: 2^2000 mod n */
    FILE *f = fopen("build/hostkey.txt", "r");
    if (f) {
        char line[2048];
        bn N;
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "n=", 2) != 0) continue;
            char *hex = line + 2;
            size_t hl = strcspn(hex, "\r\n");
            size_t nb = hl / 2;
            unsigned char raw[256];
            memset(raw, 0, sizeof raw);
            for (size_t i = 0; i < nb; i++) {
                int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
                raw[256 - nb + i] = (unsigned char)(hi << 4 | lo);
            }
            bn_from_be(&N, raw, 256);
        }
        fclose(f);
        bn two2, e2000, res;
        memset(&two2, 0, sizeof two2);
        memset(&e2000, 0, sizeof e2000);
        two2.v[0] = 2;
        e2000.v[0] = 2000;
        bn_modexp(&res, &two2, &e2000, &N);
        unsigned char rb[256];
        bn_to_be(&res, rb, 256);
        printf("2^2000 mod n = ");
        for (int i = 0; i < 256; i++) printf("%02x", rb[i]);
        printf("\n");
    }

    /* full RSA round trip on the real key: EM -> s -> EM */
    {
        FILE *kf = fopen("build/hostkey.txt", "r");
        bn N, D, EM, S, T;
        memset(&N, 0, sizeof N);
        memset(&D, 0, sizeof D);
        if (kf) {
            char line[2048];
            while (fgets(line, sizeof line, kf)) {
                uint32_t *dst = NULL;
                if (!strncmp(line, "n=", 2)) dst = N.v;
                else if (!strncmp(line, "d=", 2)) dst = D.v;
                if (!dst) continue;
                char *hex = line + 2;
                size_t hl = strcspn(hex, "\r\n");
                size_t nb = hl / 2;
                unsigned char raw[256];
                memset(raw, 0, sizeof raw);
                for (size_t i = 0; i < nb; i++)
                    raw[256 - nb + i] =
                        (unsigned char)(hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]));
                bn tmp;
                bn_from_be(&tmp, raw, 256);
                memcpy(dst, tmp.v, sizeof tmp.v);
            }
            fclose(kf);

            unsigned char tok[20];
            for (int i = 0; i < 20; i++) tok[i] = (unsigned char)(0x10 + i);
            unsigned char em[256];
            em_build_sha1(em, tok, sizeof tok);
            bn_from_be(&EM, em, 256);
            bn_modexp(&S, &EM, &D, &N);
            bn eee;
            memset(&eee, 0, sizeof eee);
            eee.v[0] = 65537;
            bn_modexp(&T, &S, &eee, &N);
            unsigned char back[256];
            bn_to_be(&T, back, 256);
            printf("rsa roundtrip (EM^d^e == EM): %s\n",
                   memcmp(em, back, 256) == 0 ? "OK" : "MISMATCH");
        }
    }
    return 0;
}