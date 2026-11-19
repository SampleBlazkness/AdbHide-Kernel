/* adbng - SPDX-License-Identifier: MIT */
#include "keys.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

int keys_load(const char *path, struct key_store *ks)
{
    ks->count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[4096];
    int rejected = 0;
    while (fgets(line, sizeof line, f)) {
        char *sep = strpbrk(line, " \t\r\n");
        if (sep) *sep = 0;
        if (!line[0]) continue;

        unsigned char blob[1024];
        int n = b64_decode(line, blob, sizeof blob);
        if (n <= 0) { rejected++; continue; }

        uint32_t mod[RSA_LIMBS];
        if (rsa_modulus_from_pubkey_blob(blob, (size_t)n, mod) != 0) { rejected++; continue; }
        if (ks->count >= KEY_STORE_MAX) { rejected++; continue; }
        memcpy(ks->mods[ks->count++], mod, sizeof mod);
    }
    fclose(f);
    if (rejected) logmsg("keys: %d unusable entry(s) skipped", rejected);
    return ks->count > 0 ? ks->count : -1;
}

int keys_verify(const struct key_store *ks, const void *token, size_t len,
                const unsigned char sig[RSA_SIG_SIZE])
{
    for (int i = 0; i < ks->count; i++)
        if (rsa_verify_sha1(ks->mods[i], sig, token, len) == 0) return i;
    return -1;
}