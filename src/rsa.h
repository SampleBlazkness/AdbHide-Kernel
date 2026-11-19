#ifndef ADBNG_RSA_H
#define ADBNG_RSA_H

#include <stddef.h>
#include <stdint.h>

/* RSA-2048, 64 limbs of 32 bits, little-endian limbs (limb 0 = least significant) */
#define RSA_LIMBS 64

#define RSA_SIG_SIZE     256
#define PUBKEY_BLOB_SIZE 524   /* 4 + 4 + 256 + 256 + 4, ADB RSAPublicKey layout */

/* returns 0 on a valid PKCS#1 v1.5 SHA-1 signature, negative otherwise */
int rsa_verify_sha1(const uint32_t mod[RSA_LIMBS], const unsigned char sig[256],
                    const void *msg, size_t msglen);

/* test-host helper, not used by the daemon */
void rsa_sign_sha1(const uint32_t mod[RSA_LIMBS], const uint32_t priv[RSA_LIMBS],
                   const void *msg, size_t msglen, unsigned char out[256]);

int    rsa_modulus_from_pubkey_blob(const unsigned char *blob, size_t len,
                                    uint32_t mod_out[RSA_LIMBS]);
size_t rsa_make_pubkey_blob(const uint32_t mod[RSA_LIMBS], unsigned char *out, size_t outsz);

/* parse a hex string of any length (optionally odd, optionally 0x-prefixed,
 * trailing whitespace ignored) into big-endian limbs.  Testing helper. */
int rsa_limbs_from_hex(const char *hex, uint32_t out[RSA_LIMBS]);

#endif