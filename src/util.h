#ifndef ADBNG_UTIL_H
#define ADBNG_UTIL_H

#include <stddef.h>
#include <stdint.h>

int  io_write_full(int fd, const void *buf, size_t len);
int  io_read_full(int fd, void *buf, size_t len);

void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void sha1(const void *data, size_t len, unsigned char out[20]);

int    b64_decode(const char *in, unsigned char *out, size_t outsz);
size_t b64_encode(const unsigned char *in, size_t len, char *out, size_t outsz);

int random_bytes(void *buf, size_t len);

/* human readable fingerprint of an RSA modulus: first 8 bytes of SHA-1, hex */
void key_fingerprint(const uint32_t mod[64], char out[17]);

#endif