/* adbng - SPDX-License-Identifier: MIT */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int io_write_full(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int io_read_full(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;   /* EOF */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void logmsg(const char *fmt, ...)
{
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int off = snprintf(line, sizeof line, "[%ld.%03ld] ", (long)ts.tv_sec,
                       (long)(ts.tv_nsec / 1000000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + off, sizeof line - (size_t)off, fmt, ap);
    va_end(ap);
    size_t l = strlen(line);
    if (l < sizeof line - 1) {
        line[l] = '\n';
        line[l + 1] = 0;
    }
    io_write_full(2, line, strlen(line));
}

/* ---------------- SHA-1 ---------------- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const unsigned char *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                     k = 0xca62c1d6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const void *data, size_t len, unsigned char out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    const unsigned char *p = data;
    size_t left = len;
    while (left >= 64) {
        sha1_block(h, p);
        p += 64;
        left -= 64;
    }
    unsigned char tail[128];
    memset(tail, 0, sizeof tail);
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t total = (left + 1 + 8 <= 64) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha1_block(h, tail);
    if (total == 128) sha1_block(h, tail + 64);
    for (int i = 0; i < 5; i++) {
        out[4 * i + 0] = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)(h[i]);
    }
}

/* ---------------- base64 ---------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, unsigned char *out, size_t outsz)
{
    size_t o = 0;
    int quad[4], qn = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        if (*p == '=') break;
        int v = b64val(*p);
        if (v < 0) return -1;
        quad[qn++] = v;
        if (qn == 4) {
            if (o + 3 > outsz) return -1;
            out[o++] = (unsigned char)(quad[0] << 2 | quad[1] >> 4);
            out[o++] = (unsigned char)((quad[1] & 0xf) << 4 | quad[2] >> 2);
            out[o++] = (unsigned char)((quad[2] & 0x3) << 6 | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2) {
        if (o + 1 > outsz) return -1;
        out[o++] = (unsigned char)(quad[0] << 2 | quad[1] >> 4);
    } else if (qn == 3) {
        if (o + 2 > outsz) return -1;
        out[o++] = (unsigned char)(quad[0] << 2 | quad[1] >> 4);
        out[o++] = (unsigned char)((quad[1] & 0xf) << 4 | quad[2] >> 2);
    } else if (qn != 0) {
        return -1;
    }
    return (int)o;
}

size_t b64_encode(const unsigned char *in, size_t len, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        size_t n = len - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (n > 1) v |= (uint32_t)in[i + 1] << 8;
        if (n > 2) v |= in[i + 2];
        if (o + 4 >= outsz) return 0;
        out[o++] = B64[(v >> 18) & 0x3f];
        out[o++] = B64[(v >> 12) & 0x3f];
        out[o++] = n > 1 ? B64[(v >> 6) & 0x3f] : '=';
        out[o++] = n > 2 ? B64[v & 0x3f] : '=';
    }
    out[o] = 0;
    return o;
}

int random_bytes(void *buf, size_t len)
{
    unsigned char *p = buf;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = io_read_full(fd, p, len);
    close(fd);
    return rc;
}

void key_fingerprint(const uint32_t mod[64], char out[17])
{
    unsigned char raw[256], h[20];
    for (int i = 0; i < 64; i++) {
        int off = 256 - 4 * (i + 1);
        raw[off + 0] = (unsigned char)(mod[i] >> 24);
        raw[off + 1] = (unsigned char)(mod[i] >> 16);
        raw[off + 2] = (unsigned char)(mod[i] >> 8);
        raw[off + 3] = (unsigned char)(mod[i]);
    }
    sha1(raw, sizeof raw, h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i] = hex[h[i] >> 4];
        out[2 * i + 1] = hex[h[i] & 0xf];
    }
    out[16] = 0;
}