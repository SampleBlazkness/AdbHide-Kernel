/* adbng - SPDX-License-Identifier: MIT
 *
 * sync: service, read direction only.
 *
 * Wire format (v1), all fields little endian:
 *   request  : id(4) length(4) payload[length]
 *   response : same, id is DATA / DONE / FAIL / STAT
 * STAT reply is 12 bytes: mode, size, time (all u32).  mode == 0 means the
 * path does not exist.  LIST replies with directory entries
 * [mode size time namelen name] terminated by an entry with namelen == 0.
 * RECV replies with DATA frames and finishes with DONE carrying the mtime.
 */
#include "sync.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ID_STAT 0x54415453u /* "STAT" */
#define ID_LIST 0x5453494cu /* "LIST" */
#define ID_SEND 0x444e4553u /* "SEND" */
#define ID_RECV 0x56434552u /* "RECV" */
#define ID_DATA 0x41544144u /* "DATA" */
#define ID_DONE 0x454e4f44u /* "DONE" */
#define ID_FAIL 0x4c494146u /* "FAIL" */
#define ID_QUIT 0x54495551u /* "QUIT" */

#define MAX_REQ        4096u
#define DATA_CHUNK     65536u
#define OUT_LOW_WATER (128u * 1024u)

struct sync_state {
    unsigned char *in;
    size_t inlen, incap;
    unsigned char *out;
    size_t outlen, outcap;
    int fd;
    int32_t mtime;
    int quit;
};

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int out_reserve(struct sync_state *s, size_t need)
{
    if (s->outlen + need <= s->outcap) return 0;
    size_t cap = s->outcap ? s->outcap : 8192;
    while (cap < s->outlen + need) cap *= 2;
    unsigned char *nb = realloc(s->out, cap);
    if (!nb) return -1;
    s->out = nb;
    s->outcap = cap;
    return 0;
}

static void out_put(struct sync_state *s, const void *d, size_t n)
{
    if (out_reserve(s, n) != 0) return;
    memcpy(s->out + s->outlen, d, n);
    s->outlen += n;
}

static void out_u32(struct sync_state *s, uint32_t v)
{
    unsigned char b[4] = {
        (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
        (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff),
    };
    out_put(s, b, 4);
}

static void out_hdr(struct sync_state *s, uint32_t id, uint32_t len)
{
    out_u32(s, id);
    out_u32(s, len);
}

static void out_fail(struct sync_state *s, const char *msg)
{
    size_t n = strlen(msg);
    out_hdr(s, ID_FAIL, (uint32_t)n);
    out_put(s, msg, n);
    logmsg("sync: FAIL '%s'", msg);
}

static void do_stat(struct sync_state *s, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        logmsg("sync: STAT %s: %s", path, strerror(errno));
        out_hdr(s, ID_STAT, 12);
        out_u32(s, 0);
        out_u32(s, 0);
        out_u32(s, 0);
        return;
    }
    out_hdr(s, ID_STAT, 12);
    out_u32(s, (uint32_t)st.st_mode);
    out_u32(s, (uint32_t)st.st_size);
    out_u32(s, (uint32_t)st.st_mtime);
    logmsg("sync: STAT %s mode=%o size=%lld", path, (unsigned)st.st_mode,
           (long long)st.st_size);
}

static void do_list(struct sync_state *s, const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s: %s", path, strerror(errno));
        out_fail(s, msg);
        return;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        uint32_t mode = 0, size = 0, mtime = 0;
        if (lstat(full, &st) == 0) {
            mode = (uint32_t)st.st_mode;
            size = (uint32_t)st.st_size;
            mtime = (uint32_t)st.st_mtime;
        }
        uint32_t nlen = (uint32_t)strlen(de->d_name);
        out_hdr(s, ID_LIST, 16 + nlen);
        out_u32(s, mode);
        out_u32(s, size);
        out_u32(s, mtime);
        out_u32(s, nlen);
        out_put(s, de->d_name, nlen);
        count++;
    }
    closedir(d);

    /* terminator: an entry whose name length is zero */
    out_hdr(s, ID_LIST, 16);
    out_u32(s, 0);
    out_u32(s, 0);
    out_u32(s, 0);
    out_u32(s, 0);
    logmsg("sync: LIST %s entries=%d", path, count);
}

static void do_recv(struct sync_state *s, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s: %s", path, strerror(errno));
        out_fail(s, msg);
        return;
    }
    struct stat st;
    s->mtime = (stat(path, &st) == 0) ? (int32_t)st.st_mtime : 0;
    s->fd = fd;
    logmsg("sync: RECV %s", path);
}

static void parse_one(struct sync_state *s, uint32_t id, const char *payload, uint32_t len)
{
    switch (id) {
    case ID_STAT:
        do_stat(s, payload);
        break;
    case ID_LIST:
        do_list(s, payload);
        break;
    case ID_RECV:
        do_recv(s, payload);
        break;
    case ID_SEND:
        out_fail(s, "adbng: push (SEND) is not implemented, this is a read-only build");
        break;
    case ID_QUIT:
        s->quit = 1;
        logmsg("sync: QUIT");
        break;
    default:
        logmsg("sync: unknown id %08x len=%u", id, len);
        out_fail(s, "adbng: unknown sync command");
        break;
    }
}

struct sync_state *sync_new(void)
{
    struct sync_state *s = calloc(1, sizeof *s);
    if (s) s->fd = -1;
    return s;
}

void sync_free(struct sync_state *s)
{
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    free(s->in);
    free(s->out);
    free(s);
}

int sync_feed(struct sync_state *s, const unsigned char *data, size_t len)
{
    if (s->inlen + len > s->incap) {
        size_t cap = s->incap ? s->incap : 4096;
        while (cap < s->inlen + len) cap *= 2;
        unsigned char *nb = realloc(s->in, cap);
        if (!nb) return -1;
        s->in = nb;
        s->incap = cap;
    }
    memcpy(s->in + s->inlen, data, len);
    s->inlen += len;

    size_t off = 0;
    while (s->inlen - off >= 8) {
        uint32_t id = le32(s->in + off);
        uint32_t flen = le32(s->in + off + 4);
        if (flen > MAX_REQ) {
            out_fail(s, "adbng: sync request too large");
            s->quit = 1;
            off = s->inlen;
            break;
        }
        if (s->inlen - off < 8 + (size_t)flen) break;

        /* Payloads are NUL terminated on the wire; make sure ours is too. */
        char stack[512];
        char *path = stack;
        if (flen >= sizeof stack) path = malloc(flen + 1);
        else if (flen == 0) {
            off += 8;
            parse_one(s, id, "", 0);
            continue;
        }
        if (!path) return -1;
        memcpy(path, s->in + off + 8, flen);
        path[flen] = 0;
        parse_one(s, id, path, flen);
        if (path != stack) free(path);

        off += 8 + flen;
    }

    if (off > 0) {
        memmove(s->in, s->in + off, s->inlen - off);
        s->inlen -= off;
    }
    return 0;
}

void sync_pump(struct sync_state *s)
{
    static unsigned char buf[DATA_CHUNK];

    while (s->fd >= 0 && s->outlen < OUT_LOW_WATER) {
        ssize_t n = read(s->fd, buf, sizeof buf);
        if (n > 0) {
            out_hdr(s, ID_DATA, (uint32_t)n);
            out_put(s, buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            out_hdr(s, ID_DONE, 4);
            out_u32(s, (uint32_t)s->mtime);
            close(s->fd);
            s->fd = -1;
            logmsg("sync: RECV done");
            break;
        }
        if (errno == EINTR) continue;
        out_fail(s, "adbng: read error");
        close(s->fd);
        s->fd = -1;
        break;
    }
}

size_t sync_pending(const struct sync_state *s) { return s->outlen; }

size_t sync_take(struct sync_state *s, unsigned char *buf, size_t max)
{
    size_t n = s->outlen < max ? s->outlen : max;
    memcpy(buf, s->out, n);
    memmove(s->out, s->out + n, s->outlen - n);
    s->outlen -= n;
    return n;
}

int sync_finished(const struct sync_state *s) { return s->quit && s->outlen == 0; }