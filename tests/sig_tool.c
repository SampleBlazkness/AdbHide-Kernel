/* adbng - SPDX-License-Identifier: MIT
 * debug helper: sign with the test key, verify the same signature, dump hex.
 * usage: sig_tool <hostkey.txt> <hexmsg>
 */
#include "rsa.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_limbs(const char *hex, uint32_t out[RSA_LIMBS])
{
    size_t len = strlen(hex);
    while (len && (hex[len - 1] == '\n' || hex[len - 1] == '\r')) len--;
    if (len > 512) len = 512;
    memset(out, 0, sizeof(uint32_t) * RSA_LIMBS);
    for (int i = 0; i < RSA_LIMBS; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) {
            long idx = (long)len - 1 - (long)(i * 4 + b);
            if (idx < 0) break;
            int h = hexval(hex[idx]);
            if (h < 0) return -1;
            v |= (uint32_t)h << (8 * b);
        }
        out[i] = v;
    }
    return (int)len;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <hostkey.txt> <hexmsg>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("hostkey"); return 2; }
    char line[2048];
    uint32_t n[RSA_LIMBS], d[RSA_LIMBS];
    int nlen = 0, dlen = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "n=", 2)) nlen = parse_hex_limbs(line + 2, n);
        else if (!strncmp(line, "d=", 2)) dlen = parse_hex_limbs(line + 2, d);
    }
    fclose(f);
    printf("parsed n: %d hex chars, d: %d hex chars\n", nlen, dlen);

    unsigned char msg[64];
    size_t msglen = strlen(argv[2]) / 2;
    for (size_t i = 0; i < msglen; i++)
        msg[i] = (unsigned char)(hexval(argv[2][2 * i]) << 4 | hexval(argv[2][2 * i + 1]));

    unsigned char sig[RSA_SIG_SIZE];
    rsa_sign_sha1(n, d, msg, msglen, sig);
    printf("sig=");
    for (int i = 0; i < RSA_SIG_SIZE; i++) printf("%02x", sig[i]);
    printf("\n");

    int rc = rsa_verify_sha1(n, sig, msg, msglen);
    printf("verify_rc=%d\n", rc);

    /* also dump the modulus as seen by the verifier, to compare with the file */
    printf("n limbs: ");
    for (int i = RSA_LIMBS - 1; i >= RSA_LIMBS - 2; i--) printf("%08x ", n[i]);
    printf("\n");
    return rc == 0 ? 0 : 1;
}