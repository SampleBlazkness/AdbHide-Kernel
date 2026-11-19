#ifndef ADBNG_KEYS_H
#define ADBNG_KEYS_H

#include "rsa.h"

#define KEY_STORE_MAX 32

struct key_store {
    int count;
    uint32_t mods[KEY_STORE_MAX][RSA_LIMBS];
};

/* file format: one base64 RSAPublicKey blob per line, optionally followed by
 * a space and a comment ("host@name"), exactly like /data/misc/adb/adb_keys */
int keys_load(const char *path, struct key_store *ks);

/* returns index of the key that validated the signature, or -1 */
int keys_verify(const struct key_store *ks, const void *token, size_t len,
                const unsigned char sig[RSA_SIG_SIZE]);

#endif