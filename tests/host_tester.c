/* adbng - SPDX-License-Identifier: MIT
 *
 * Minimal ADB *host* used to test usbgwd without a PC: it drives the protocol
 * over a socketpair, so the whole handshake plus every service runs inside the
 * phone.
 *
 * usage: host_tester <usbgwd-path> <adb_keys> <hostkey.txt> [bad]
 */
#include "proto.h"
#include "rsa.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fd = -1;
static uint32_t g_next_id = 1;
static int g_pass, g_total;
static char g_reason[512];
static char g_why[512];

struct rx {
    uint32_t cmd, a0, a1, len;
    unsigned char *data;
};

struct chan {
    uint32_t my_id;
    uint32_t dev_id;
};

struct sink {
    unsigned char *out;
    size_t outlen, outcap;
    unsigned char *err;
    size_t errlen, errcap;
    int exit_code;
    int have_exit;
    int closed;
    int wretes;
};

static void send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, size_t len)
{
    struct amessage m;
    memset(&m, 0, sizeof m);
    m.command = cmd;
    m.arg0 = a0;
    m.arg1 = a1;
    m.data_length = (uint32_t)len;
    m.magic = amsg_magic(cmd);
    printf("[host] -> %-4s arg0=%u arg1=%u len=%zu\n", amsg_name(cmd), a0, a1, len);
    io_write_full(g_fd, &m, AMSG_SIZE);
    if (len) io_write_full(g_fd, data, len);
}

/* 1 on message, 0 on clean EOF, -1 on error/timeout */
static int recv_msg(struct rx *r, int timeout_ms)
{
    struct pollfd p = { .fd = g_fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0) return -1;

    struct amessage m;
    if (io_read_full(g_fd, &m, AMSG_SIZE) != 0) return 0;
    if (m.magic != amsg_magic(m.command)) {
        printf("[host] bad magic %08x\n", m.command);
        return -1;
    }
    unsigned char *buf = NULL;
    if (m.data_length) {
        buf = malloc(m.data_length + 1);
        if (!buf) return -1;
        if (io_read_full(g_fd, buf, m.data_length) != 0) { free(buf); return 0; }
        buf[m.data_length] = 0;
    }
    r->cmd = m.command;
    r->a0 = m.arg0;
    r->a1 = m.arg1;
    r->len = m.data_length;
    r->data = buf;
    printf("[host] <- %-4s arg0=%u arg1=%u len=%u\n", amsg_name(m.command), m.arg0, m.arg1,
           m.data_length);
    return 1;
}

static int buf_add(unsigned char **buf, size_t *len, size_t *cap, const void *d, size_t n)
{
    if (*len + n > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (c < *len + n) c *= 2;
        unsigned char *nb = realloc(*buf, c);
        if (!nb) return -1;
        *buf = nb;
        *cap = c;
    }
    memcpy(*buf + *len, d, n);
    *len += n;
    return 0;
}

static int load_hostkey(const char *path, uint32_t n[RSA_LIMBS], uint32_t d[RSA_LIMBS])
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1200];
    int have_n = 0, have_d = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "n=", 2)) {
            if (rsa_limbs_from_hex(line + 2, n) != 0) return -1;
            have_n = 1;
        } else if (!strncmp(line, "d=", 2)) {
            if (rsa_limbs_from_hex(line + 2, d) != 0) return -1;
            have_d = 1;
        }
    }
    fclose(f);
    return (have_n && have_d) ? 0 : -1;
}

static int open_service(const char *svc, size_t len, struct chan *ch)
{
    ch->my_id = g_next_id++;
    send_msg(A_OPEN, ch->my_id, 0, svc, len);

    struct rx r;
    if (recv_msg(&r, 20000) != 1) return -1;
    if (r.cmd == A_CLSE) { free(r.data); return -2; }   /* refused */
    if (r.cmd != A_OKAY) { free(r.data); return -1; }
    ch->dev_id = r.a0;
    free(r.data);
    return 0;
}

/* Read until CLSE, acking every WRTE so the device's flow control keeps
 * moving.  In v2 mode the WRTE payload is a stream of sub-packets. */
static int pump(struct chan *ch, struct sink *s, int v2, int timeout_ms)
{
    for (;;) {
        struct rx r;
        int rc = recv_msg(&r, timeout_ms);
        if (rc <= 0) { snprintf(g_why, sizeof g_why, "link lost while waiting (%d)", rc); return -1; }
        if (r.cmd == A_OKAY) { free(r.data); continue; }
        if (r.cmd == A_CLSE) { free(r.data); s->closed = 1; return 0; }
        if (r.cmd != A_WRTE) { free(r.data); continue; }

        send_msg(A_OKAY, ch->my_id, ch->dev_id, NULL, 0);   /* grant next WRTE */
        s->wretes++;

        if (!v2) {
            if (r.len && buf_add(&s->out, &s->outlen, &s->outcap, r.data, r.len) != 0) {
                free(r.data);
                return -1;
            }
            free(r.data);
            continue;
        }

        size_t off = 0;
        while (off + 5 <= r.len) {
            int stream = r.data[off];
            uint32_t l = (uint32_t)r.data[off + 1] | ((uint32_t)r.data[off + 2] << 8) |
                         ((uint32_t)r.data[off + 3] << 16) | ((uint32_t)r.data[off + 4] << 24);
            off += 5;
            if (off + l > r.len) {
                free(r.data);
                snprintf(g_why, sizeof g_why, "truncated sub-packet");
                return -1;
            }
            if (stream == 1) {
                if (buf_add(&s->out, &s->outlen, &s->outcap, r.data + off, l) != 0) { free(r.data); return -1; }
            } else if (stream == 2) {
                if (buf_add(&s->err, &s->errlen, &s->errcap, r.data + off, l) != 0) { free(r.data); return -1; }
            } else if (stream == 3) {
                if (l != 4) {
                    free(r.data);
                    snprintf(g_why, sizeof g_why, "bad exit sub-packet length %u", l);
                    return -1;
                }
                s->exit_code = (int)((uint32_t)r.data[off] | ((uint32_t)r.data[off + 1] << 8) |
                                     ((uint32_t)r.data[off + 2] << 16) |
                                     ((uint32_t)r.data[off + 3] << 24));
                s->have_exit = 1;
            }
            off += l;
        }
        free(r.data);
    }
}

static void sink_free(struct sink *s)
{
    free(s->out);
    free(s->err);
    memset(s, 0, sizeof *s);
}

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            snprintf(g_reason, sizeof g_reason, __VA_ARGS__); \
            goto fail;                                      \
        }                                                   \
    } while (0)

static int case_shell_v1(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell:echo hello-v1";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 0, 20000) == 0, "%s", g_why);

    CHECK(s.outlen > 0, "no stdout");
    CHECK(memmem(s.out, s.outlen, "hello-v1", 8) != NULL, "stdout missing 'hello-v1': %.*s",
          (int)s.outlen, s.out ? (char *)s.out : "");
    CHECK(s.closed, "channel not closed");
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

static int case_shell_v2(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,raw:echo out; echo err1 >&2; exit 7";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 1, 20000) == 0, "%s", g_why);

    CHECK(s.outlen == 4 && !memcmp(s.out, "out\n", 4), "stdout wrong: %.*s", (int)s.outlen,
          s.out ? (char *)s.out : "");
    CHECK(s.errlen == 5 && !memcmp(s.err, "err1\n", 5), "stderr wrong: %.*s", (int)s.errlen,
          s.err ? (char *)s.err : "");
    CHECK(s.have_exit && s.exit_code == 7, "exit code %d (had=%d)", s.exit_code, s.have_exit);
    CHECK(s.closed, "channel not closed");
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

static int case_big_output(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,raw:head -c 300000 /dev/zero | tr '\\0' 'x'";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 1, 60000) == 0, "%s", g_why);

    CHECK(s.outlen == 300000, "stdout length %zu, expected 300000 (stderr: %.*s)", s.outlen,
          (int)s.errlen, s.err ? (char *)s.err : "");
    CHECK(s.wretes > 1, "only %d WRTE packet(s): output was not chunked", s.wretes);
    size_t bad = 0;
    for (size_t i = 0; i < s.outlen; i++)
        if (s.out[i] != 'x') { bad = i; break; }
    CHECK(bad == 0, "byte %zu is 0x%02x, expected 'x'", bad, s.out[bad]);
    CHECK(s.have_exit && s.exit_code == 0, "exit code %d", s.exit_code);
    printf("[host] big output: %zu bytes in %d WRTE packets\n", s.outlen, s.wretes);
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

/* stdin path: head takes exactly 11 bytes then exits, so we do not need a
 * half-close (which the protocol has no way to express). */
static int case_stdin(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,raw:head -c 11";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);

    const char text[] = "hello-stdin\n";
    unsigned char payload[5 + sizeof text - 1];
    uint32_t l = (uint32_t)(sizeof text - 1);
    payload[0] = 0;
    payload[1] = (unsigned char)(l & 0xff);
    payload[2] = (unsigned char)((l >> 8) & 0xff);
    payload[3] = (unsigned char)((l >> 16) & 0xff);
    payload[4] = (unsigned char)((l >> 24) & 0xff);
    memcpy(payload + 5, text, sizeof text - 1);
    send_msg(A_WRTE, ch.my_id, ch.dev_id, payload, sizeof payload);

    CHECK(pump(&ch, &s, 1, 20000) == 0, "%s", g_why);
    CHECK(s.outlen == 11 && !memcmp(s.out, "hello-stdin", 11), "stdout '%.*s' (%zu bytes)",
          (int)s.outlen, s.out ? (char *)s.out : "", s.outlen);
    CHECK(s.have_exit && s.exit_code == 0, "exit code %d", s.exit_code);
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

/* One shell_v2 sub-packet as host -> device stdin.  The device acks every
 * WRTE with OKAY, which pump() throws away. */
static void send_sub(struct chan *ch, int stream, const void *d, size_t n)
{
    unsigned char buf[5 + 64];
    if (n > 64) n = 64;
    uint32_t l = (uint32_t)n;
    buf[0] = (unsigned char)stream;
    buf[1] = (unsigned char)(l & 0xff);
    buf[2] = (unsigned char)((l >> 8) & 0xff);
    buf[3] = (unsigned char)((l >> 16) & 0xff);
    buf[4] = (unsigned char)((l >> 24) & 0xff);
    if (n) memcpy(buf + 5, d, n);
    send_msg(A_WRTE, ch->my_id, ch->dev_id, buf, n + 5);
}

/* pty: the child has to come up on a real terminal.  `tty` prints the device
 * name only when stdin is one; through a pipe it says "not a tty". */
static int case_pty(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,pty,raw:tty";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 1, 20000) == 0, "%s", g_why);

    CHECK(s.outlen && memmem(s.out, s.outlen, "/dev/pts/", 9) != NULL,
          "stdout has no /dev/pts: '%.*s'", (int)s.outlen, s.out ? (char *)s.out : "");
    CHECK(s.have_exit && s.exit_code == 0, "exit code %d", s.exit_code);
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

/* The point of the controlling terminal: 0x03 must become SIGINT.  Through a
 * pipe this byte is just data, so `sleep 60` would run to completion. */
static int case_pty_ctrl_c(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,pty,raw:sleep 60";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);

    usleep(300000);                    /* let the child reach exec */
    const unsigned char ctrl_c = 0x03;
    send_sub(&ch, 0, &ctrl_c, 1);

    CHECK(pump(&ch, &s, 1, 10000) == 0, "%s", g_why);
    CHECK(s.have_exit && s.exit_code == 130, "exit code %d, expected 130", s.exit_code);
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

/* Window size arrives as its own sub-packet; stty has to see it.  An
 * interactive shell is used so the size is set before the command runs. */
static int case_pty_winsize(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,pty,raw:";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);

    const unsigned char ws[8] = { 31, 0, 97, 0, 0, 0, 0, 0 };   /* 31 rows, 97 cols */
    send_sub(&ch, 5, ws, sizeof ws);
    usleep(300000);
    send_sub(&ch, 0, "stty size\n", 10);
    usleep(300000);
    send_sub(&ch, 0, "exit\n", 5);

    CHECK(pump(&ch, &s, 1, 20000) == 0, "%s", g_why);
    CHECK(memmem(s.out, s.outlen, "31 97", 5) != NULL, "stdout has no '31 97': '%.*s'",
          (int)s.outlen, s.out ? (char *)s.out : "");
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

/* exec: takes a NUL-separated argv, no shell, so the space must survive as
 * part of a single argument. */
static int case_exec(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char a0[] = "exec:";
    const char a1[] = "printf";
    const char a2[] = "hello world\n";
    unsigned char svc[sizeof a0 + sizeof a1 + sizeof a2];
    size_t off = 0;
    memcpy(svc + off, a0, sizeof a0 - 1); off += sizeof a0 - 1;   /* keeps the colon, drops NUL */
    memcpy(svc + off, a1, sizeof a1); off += sizeof a1;
    memcpy(svc + off, a2, sizeof a2); off += sizeof a2;

    int rc = open_service((const char *)svc, off, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 0, 20000) == 0, "%s", g_why);
    CHECK(s.outlen == 12 && !memcmp(s.out, "hello world\n", 12), "stdout '%.*s' (%zu bytes)",
          (int)s.outlen, s.out ? (char *)s.out : "", s.outlen);
    CHECK(s.closed, "channel not closed");
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

static int case_not_found(void)
{
    struct chan ch;
    struct sink s;
    memset(&s, 0, sizeof s);

    const char svc[] = "shell,v2,raw:definitely-not-a-command-xyz123";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == 0, "open failed rc=%d", rc);
    CHECK(pump(&ch, &s, 1, 20000) == 0, "%s", g_why);

    CHECK(s.errlen > 0, "stderr empty, expected a 'not found' message");
    CHECK(s.have_exit && s.exit_code == 127, "exit code %d, expected 127", s.exit_code);
    printf("[host] stderr: %.*s", (int)s.errlen, (char *)s.err);
    sink_free(&s);
    return 0;
fail:
    sink_free(&s);
    return -1;
}

static int case_refused(void)
{
    struct chan ch;
    const char svc[] = "reboot:bootloader";
    int rc = open_service(svc, sizeof svc, &ch);
    CHECK(rc == -2, "expected refusal, got rc=%d", rc);
    return 0;
fail:
    return -1;
}

/* ---------------- sync: helpers ---------------- */

static unsigned char g_syn[1 << 20];
static size_t g_synlen;

struct syncres {
    uint32_t mode, size, time;
    char names[64][128];
    int nnames;
    unsigned char *data;
    size_t datalen, datacap;
    int done;
    char msg[256];
};

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void sync_req(struct chan *ch, uint32_t id, const char *path)
{
    size_t plen = strlen(path) + 1;
    unsigned char buf[8 + 512];
    if (plen > sizeof buf - 8) plen = sizeof buf - 8;
    put32(buf, id);
    put32(buf + 4, (uint32_t)plen);
    memcpy(buf + 8, path, plen);
    send_msg(A_WRTE, ch->my_id, ch->dev_id, buf, 8 + plen);
}

/* want: 0 = a STAT reply, 1 = a whole directory listing, 2 = DATA...DONE */
static int sync_wait(struct chan *ch, int want, struct syncres *r, int timeout_ms)
{
    g_synlen = 0;
    memset(r, 0, sizeof *r);
    size_t off = 0;

    for (;;) {
        while (g_synlen - off >= 8) {
            uint32_t id = get32(g_syn + off);
            uint32_t fl = get32(g_syn + off + 4);
            if (g_synlen - off < 8 + (size_t)fl) break;
            const unsigned char *p = g_syn + off + 8;

            if (id == 0x4c494146u) {              /* FAIL */
                snprintf(r->msg, sizeof r->msg, "%.*s", (int)fl, (const char *)p);
                return -1;
            }
            if (id == 0x41544144u) {              /* DATA */
                if (buf_add(&r->data, &r->datalen, &r->datacap, p, fl) != 0) return -1;
            } else if (id == 0x454e4f44u) {       /* DONE */
                r->done = 1;
                if (want == 2) return 0;
            } else if (id == 0x54415453u && fl == 12) {  /* STAT */
                r->mode = get32(p);
                r->size = get32(p + 4);
                r->time = get32(p + 8);
                if (want == 0) return 0;
            } else if (id == 0x5453494cu && fl >= 16) {   /* LIST */
                uint32_t nl = get32(p + 12);
                if (nl == 0) {
                    if (want == 1) return 0;
                } else if (r->nnames < 64 && nl < 128) {
                    memcpy(r->names[r->nnames], p + 16, nl);
                    r->names[r->nnames][nl] = 0;
                    r->nnames++;
                }
            }
            off += 8 + fl;
        }

        struct rx m;
        if (recv_msg(&m, timeout_ms) <= 0) {
            snprintf(g_why, sizeof g_why, "sync: link lost");
            return -1;
        }
        if (m.cmd == A_WRTE) {
            send_msg(A_OKAY, ch->my_id, ch->dev_id, NULL, 0);
            if (m.len) {
                if (g_synlen + m.len > sizeof g_syn) {
                    free(m.data);
                    snprintf(g_why, sizeof g_why, "sync: receive buffer overflow");
                    return -1;
                }
                memcpy(g_syn + g_synlen, m.data, m.len);
                g_synlen += m.len;
            }
        }
        free(m.data);
    }
}

static int expect_clse(struct chan *ch, int timeout_ms)
{
    for (;;) {
        struct rx m;
        if (recv_msg(&m, timeout_ms) <= 0) return -1;
        if (m.cmd == A_CLSE) { free(m.data); return 0; }
        if (m.cmd == A_WRTE) send_msg(A_OKAY, ch->my_id, ch->dev_id, NULL, 0);
        free(m.data);
    }
}

static int case_sync_stat(const char *path, size_t expect_size)
{
    struct chan ch;
    struct syncres r;
    memset(&r, 0, sizeof r);

    int rc = open_service("sync:", 6, &ch);
    CHECK(rc == 0, "open sync failed rc=%d", rc);

    sync_req(&ch, 0x54415453u, path);          /* STAT */
    CHECK(sync_wait(&ch, 0, &r, 20000) == 0, "STAT failed: %s", r.msg);
    CHECK(r.mode != 0, "STAT mode is 0 (path not found)");
    CHECK((r.mode & 0170000) == 0100000, "mode %o is not a regular file", r.mode);
    CHECK(r.size == expect_size, "size %u, expected %zu", r.size, expect_size);
    printf("[host] stat: mode=%o size=%u mtime=%u\n", r.mode, r.size, r.time);

    sync_req(&ch, 0x54495551u, "");            /* QUIT */
    CHECK(expect_clse(&ch, 20000) == 0, "channel not closed after QUIT");
    free(r.data);
    return 0;
fail:
    free(r.data);
    return -1;
}

static int case_sync_list(const char *dir, const char *n1, const char *n2)
{
    struct chan ch;
    struct syncres r;
    memset(&r, 0, sizeof r);

    int rc = open_service("sync:", 6, &ch);
    CHECK(rc == 0, "open sync failed rc=%d", rc);

    sync_req(&ch, 0x5453494cu, dir);           /* LIST */
    CHECK(sync_wait(&ch, 1, &r, 20000) == 0, "LIST failed: %s", r.msg);

    int found1 = 0, found2 = 0;
    for (int i = 0; i < r.nnames; i++) {
        if (!strcmp(r.names[i], n1)) found1 = 1;
        if (!strcmp(r.names[i], n2)) found2 = 1;
    }
    CHECK(found1 && found2, "listing missing an entry (got %d names)", r.nnames);
    printf("[host] list: %d entries\n", r.nnames);

    sync_req(&ch, 0x54495551u, "");
    CHECK(expect_clse(&ch, 20000) == 0, "channel not closed after QUIT");
    free(r.data);
    return 0;
fail:
    free(r.data);
    return -1;
}

static int case_sync_recv(const char *path, const unsigned char *expect, size_t n)
{
    struct chan ch;
    struct syncres r;
    memset(&r, 0, sizeof r);

    int rc = open_service("sync:", 6, &ch);
    CHECK(rc == 0, "open sync failed rc=%d", rc);

    sync_req(&ch, 0x56434552u, path);          /* RECV */
    CHECK(sync_wait(&ch, 2, &r, 40000) == 0, "RECV failed: %s", r.msg);
    CHECK(r.done, "no DONE frame");
    CHECK(r.datalen == n, "received %zu bytes, expected %zu", r.datalen, n);
    CHECK(memcmp(r.data, expect, n) == 0, "content mismatch");
    printf("[host] recv: %zu bytes, verified\n", r.datalen);

    sync_req(&ch, 0x54495551u, "");
    CHECK(expect_clse(&ch, 20000) == 0, "channel not closed after QUIT");
    free(r.data);
    return 0;
fail:
    free(r.data);
    return -1;
}

static int case_sync_send(const char *path)
{
    struct chan ch;
    struct syncres r;
    memset(&r, 0, sizeof r);

    int rc = open_service("sync:", 6, &ch);
    CHECK(rc == 0, "open sync failed rc=%d", rc);

    sync_req(&ch, 0x444e4553u, path);          /* SEND must be refused */
    CHECK(sync_wait(&ch, 2, &r, 20000) != 0, "SEND was not refused");
    CHECK(strstr(r.msg, "read-only") != NULL, "unexpected FAIL message: %s", r.msg);
    printf("[host] send refused: %s\n", r.msg);

    free(r.data);
    send_msg(A_CLSE, ch.my_id, ch.dev_id, NULL, 0);
    expect_clse(&ch, 20000);
    return 0;
fail:
    free(r.data);
    return -1;
}

static void report(const char *name, int ok)
{
    g_total++;
    if (ok) {
        g_pass++;
        printf("[host] PASS %s\n", name);
    } else {
        printf("[host] FAIL %s: %s\n", name, g_reason[0] ? g_reason : g_why);
    }
}

static int handshake(uint32_t n[RSA_LIMBS], uint32_t d[RSA_LIMBS], int bad_signature)
{
    struct rx r;
    if (recv_msg(&r, 10000) != 1 || r.cmd != A_CNXN) return -1;
    printf("[host] banner: %s\n", r.data ? (char *)r.data : "");
    if (!r.data || !memmem(r.data, r.len, "features=shell_v2", 17)) {
        printf("[host] banner does not advertise shell_v2\n");
    }
    free(r.data);

    const char banner[] = "host::features=shell_v2";
    send_msg(A_CNXN, A_VERSION, A_MAXDATA, banner, sizeof banner);

    if (recv_msg(&r, 10000) != 1 || r.cmd != A_AUTH || r.a0 != AUTH_TOKEN) return -1;
    unsigned char token[TOKEN_SIZE];
    if (r.len < TOKEN_SIZE) { free(r.data); return -1; }
    memcpy(token, r.data, TOKEN_SIZE);
    free(r.data);
    printf("[host] token: ");
    for (int i = 0; i < TOKEN_SIZE; i++) printf("%02x", token[i]);
    printf("\n");

    unsigned char sig[RSA_SIG_SIZE];
    rsa_sign_sha1(n, d, token, TOKEN_SIZE, sig);
    if (bad_signature) sig[100] ^= 0xff;
    send_msg(A_AUTH, AUTH_SIGNATURE, 0, sig, sizeof sig);

    if (recv_msg(&r, 20000) != 1) return -1;
    if (bad_signature) {
        int rejected = (r.cmd == A_AUTH && r.a0 == AUTH_TOKEN);
        free(r.data);
        return rejected ? 0 : -1;
    }
    if (r.cmd != A_CNXN) { free(r.data); return -1; }
    printf("[host] authenticated, banner again: %s\n", r.data ? (char *)r.data : "");
    free(r.data);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <usbgwd> <adb_keys> <hostkey.txt> [bad]\n", argv[0]);
        return 2;
    }
    int bad = (argc > 4 && !strcmp(argv[4], "bad"));

    uint32_t n[RSA_LIMBS], d[RSA_LIMBS];
    if (load_hostkey(argv[3], n, d) != 0) {
        fprintf(stderr, "cannot read host key %s\n", argv[3]);
        return 2;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 2;
    }

    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        if (dup2(sv[1], 3) < 0) _exit(3);
        close(sv[1]);
        setenv("ADBNG_PRODUCT_NAME", "adbng", 1);
        setenv("ADBNG_PRODUCT_MODEL", "adbng-self-test", 1);
        setenv("ADBNG_PRODUCT_DEVICE", "adbng-self-test", 1);
        execl(argv[1], argv[1], "--fd", "3", "--keys", argv[2], (char *)NULL);
        _exit(4);
    }
    close(sv[1]);
    g_fd = sv[0];
    alarm(120);

    struct rx r;
    int hs = handshake(n, d, bad);
    if (bad) {
        report("bad signature rejected", hs == 0);
        printf("[host] RESULT: %d/%d passed\n", g_pass, g_total);
        goto done;
    }
    report("handshake + auth", hs == 1);
    if (hs == 1) {
        report("shell: v1", case_shell_v1() == 0);
        report("shell: v2 stdout/stderr/exit", case_shell_v2() == 0);
        report("shell: v2 300 KB chunked output", case_big_output() == 0);
        report("shell: v2 stdin", case_stdin() == 0);
        report("shell: pty is a tty", case_pty() == 0);
        report("shell: pty ^C -> SIGINT", case_pty_ctrl_c() == 0);
        report("shell: pty window size", case_pty_winsize() == 0);
        report("exec: NUL-separated argv", case_exec() == 0);
        report("shell: command not found -> 127", case_not_found() == 0);
        report("refused service -> CLSE", case_refused() == 0);

        /* ---- sync: (read direction) ---- */
        const char *td = getenv("ADBNG_TEST_DIR");
        if (!td || !*td) td = "/tmp";
        size_t fsize = 200000;
        unsigned char *expect = malloc(fsize);
        if (!expect) { printf("[host] out of memory\n"); goto done; }
        for (size_t i = 0; i < fsize; i++) expect[i] = (unsigned char)((i * 7 + 3) & 0xff);

        char fpath[512], dpath[512], p1[600], p2[600];
        snprintf(fpath, sizeof fpath, "%s/adbng-sync-%d.bin", td, (int)getpid());
        snprintf(dpath, sizeof dpath, "%s/adbng-sync-%d.dir", td, (int)getpid());
        snprintf(p1, sizeof p1, "%s/alpha.txt", dpath);
        snprintf(p2, sizeof p2, "%s/beta.txt", dpath);

        FILE *f = fopen(fpath, "wb");
        if (!f) { printf("[host] cannot create %s\n", fpath); free(expect); goto done; }
        fwrite(expect, 1, fsize, f);
        fclose(f);
        mkdir(dpath, 0700);
        f = fopen(p1, "wb");
        if (f) { fputs("alpha", f); fclose(f); }
        f = fopen(p2, "wb");
        if (f) { fputs("beta", f); fclose(f); }

        report("sync: STAT", case_sync_stat(fpath, fsize) == 0);
        report("sync: LIST", case_sync_list(dpath, "alpha.txt", "beta.txt") == 0);
        report("sync: RECV 200 KB", case_sync_recv(fpath, expect, fsize) == 0);
        report("sync: SEND refused (read-only)", case_sync_send(fpath) == 0);

        unlink(p1);
        unlink(p2);
        rmdir(dpath);
        unlink(fpath);
        free(expect);
    }

    printf("[host] RESULT: %d/%d passed\n", g_pass, g_total);

done:
    close(g_fd);
    int st = 0;
    waitpid(child, &st, 0);
    printf("[host] child exit status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    (void)r;
    return (g_pass == g_total) ? 0 : 1;
}