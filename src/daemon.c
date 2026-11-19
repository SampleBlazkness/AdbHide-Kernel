/* adbng - SPDX-License-Identifier: MIT
 *
 * usbgwd: the device side of the ADB protocol.
 *
 * M2: framing, CNXN banner, AUTH, channel multiplexing.
 * M3: single-threaded poll loop, per-channel flow control, `shell:` (v1 and
 *     v2 sub-packets, optional pty) and `exec:`.
 * M4: the transport is pluggable - byte stream (socketpair) or USB FunctionFS.
 * M5a: sync: write direction (SEND) behind a write-root allow-list.
 * M5b: host authorization - an unknown host key parks the link and waits for a
 *      decision from the manager app instead of being dropped immediately.
 *
 * Still missing: forward:, and the controller that owns the gadget lifecycle.
 */
#include "keys.h"
#include "link.h"
#include "proto.h"
#include "shell.h"
#include "sync.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CHANNELS 16
#define IO_CHUNK     4096          /* bytes per WRTE packet, keeps latency low */

/* shell_v2 stream identifiers, one byte at the front of every WRTE payload */
#define SUB_STDIN  0
#define SUB_STDOUT 1
#define SUB_STDERR 2
#define SUB_EXIT   3
#define SUB_CLOSE_STDIN 4
#define SUB_WINDOW_SIZE 5

/* The tty EOF character.  A pty cannot be half-closed, so `kIdCloseStdin`
 * becomes this byte and the line discipline turns it into read() == 0. */
#define PTY_EOF 0x04

#define EV_LINK  0
#define EV_STDOUT 1
#define EV_STDERR 2
#define EV_STDIN  3

#define SYNC_CHUNK (32u * 1024u)

enum { KIND_NONE = 0, KIND_SHELL = 1, KIND_SYNC = 2 };

struct channel {
    int used;
    uint32_t local;
    uint32_t peer;
    char name[256];
    int kind;
    int v2;                        /* host asked for shell_v2 sub-packets */
    int pty;                       /* fd_in is a pty master (== fd_out) */
    int rows, cols;                /* last window size the host reported */
    pid_t pid;
    int fd_in;                     /* us -> child stdin */
    int fd_out;                    /* child stdout -> us */
    int fd_err;                    /* child stderr -> us, -1 when merged */
    unsigned char *inbuf;          /* host stdin bytes not yet written */
    size_t inlen;
    size_t incap;
    int stdin_done;                /* host closed stdin (v2 kIdCloseStdin) */
    int can_send;                  /* flow control credit from the host */
    int eof_out;
    int eof_err;
    int exit_code;
    int exit_sent;
    struct sync_state *sync;       /* KIND_SYNC */
};

static const struct link_ops *g_link;
static struct key_store g_keys;
static unsigned char g_token[TOKEN_SIZE];
static int g_auth_state;
static int g_token_rounds;

/* ---- M5b: host authorization ---------------------------------------------
 * An unknown host key parks the link instead of being dropped: we publish
 * /data/adb/adbng/pending/<fp>.req and keep answering AUTH TOKEN until the
 * manager writes <fp>.decision with "allow" or "deny".  allow appends the key
 * to adb_keys and reloads it, so the host's next signature just works; deny or
 * a timeout drops the link.  A host that disappears mid-question simply leaves
 * no .req behind, which is what makes the dialog go away.
 */
#define ADBNG_DIR        "/data/adb/adbng"
#define PENDING_SECONDS  120

static const char *g_keys_path;
static char g_pend_fp[17];
static char g_pend_b64[1024];
static char g_pend_host[128];
static time_t g_pend_since;
static int g_pend_active;

/* Every path below hangs off one directory so a self-test can point the whole
 * state at a scratch dir instead of the real /data/adb/adbng. */
static char g_state_dir[192] = ADBNG_DIR;
static char g_pending_dir[224] = ADBNG_DIR "/pending";
static char g_devices_file[224] = ADBNG_DIR "/devices.tsv";

static void mkdir_p(const char *dir);

static void state_init(void)
{
    const char *e = getenv("ADBNG_STATE_DIR");
    if (!e || !*e) return;
    snprintf(g_state_dir, sizeof g_state_dir, "%s", e);
    snprintf(g_pending_dir, sizeof g_pending_dir, "%s/pending", g_state_dir);
    snprintf(g_devices_file, sizeof g_devices_file, "%s/devices.tsv", g_state_dir);
    mkdir_p(g_state_dir);
}

static void mkdir_p(const char *dir)
{
    char tmp[256];
    size_t l;

    snprintf(tmp, sizeof tmp, "%s", dir);
    l = strlen(tmp);
    for (size_t i = 1; i < l; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        mkdir(tmp, 0755);
        tmp[i] = '/';
    }
    mkdir(tmp, 0755);
}

static void pend_publish(void)
{
    char p[256];
    FILE *f;

    mkdir_p(g_pending_dir);
    snprintf(p, sizeof p, "%s/%s.req", g_pending_dir, g_pend_fp);
    f = fopen(p, "w");
    if (!f) {
        logmsg("pending: cannot write %s: %s", p, strerror(errno));
        return;
    }
    /* key=value lines, no JSON: the manager parses this by splitting on '=' */
    fprintf(f, "fingerprint=%s\nhost=%s\nsince=%ld\n", g_pend_fp, g_pend_host,
            (long)g_pend_since);
    fclose(f);
    logmsg("pending: published %s (host=%s)", p, g_pend_host);
}

static void pend_clear(void)
{
    char p[256];

    if (!g_pend_active) return;
    snprintf(p, sizeof p, "%s/%s.req", g_pending_dir, g_pend_fp);
    unlink(p);
    snprintf(p, sizeof p, "%s/%s.decision", g_pending_dir, g_pend_fp);
    unlink(p);
    g_pend_active = 0;
}

static void dev_record(const char *fp, const char *host, const char *decision)
{
    char safe[128];
    size_t n = 0;
    FILE *f;

    /* one TSV line per decision: ts <tab> fp <tab> allow|deny <tab> host */
    for (const char *s = host; *s && n < sizeof safe - 1; s++)
        safe[n++] = (*s == ' ' || *s == '\t') ? '_' : *s;
    safe[n] = 0;

    mkdir_p(g_state_dir);
    f = fopen(g_devices_file, "a");
    if (!f) {
        logmsg("devices: cannot append to %s: %s", g_devices_file, strerror(errno));
        return;
    }
    fprintf(f, "%ld\t%s\t%s\t%s\n", (long)time(NULL), fp, decision, safe);
    fclose(f);
}

/* Returns 0 to keep the link, -1 to drop it. */
static int pend_tick(void)
{
    char p[256];
    char line[32];
    FILE *f;

    if (!g_pend_active) return 0;

    snprintf(p, sizeof p, "%s/%s.decision", g_pending_dir, g_pend_fp);
    f = fopen(p, "r");
    if (f) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);

        if (!strncmp(line, "allow", 5)) {
            FILE *k = g_keys_path ? fopen(g_keys_path, "a") : NULL;
            if (!k) {
                logmsg("pending: cannot append to %s", g_keys_path ? g_keys_path : "?");
                dev_record(g_pend_fp, g_pend_host, "deny");
                pend_clear();
                return -1;
            }
            fprintf(k, "%s %s\n", g_pend_b64, g_pend_host);
            fclose(k);
            keys_load(g_keys_path, &g_keys);
            dev_record(g_pend_fp, g_pend_host, "allow");
            logmsg("pending: allowed %s, waiting for a fresh signature", g_pend_fp);
            pend_clear();
            return 0;
        }

        /* anything that is not "allow" is a deny */
        dev_record(g_pend_fp, g_pend_host, "deny");
        logmsg("pending: denied %s", g_pend_fp);
        pend_clear();
        return -1;
    }

    if (time(NULL) - g_pend_since > PENDING_SECONDS) {
        logmsg("pending: no decision for %s after %ds, dropping", g_pend_fp,
               PENDING_SECONDS);
        pend_clear();       /* not recorded: the user never chose */
        return -1;
    }
    return 0;
}
static struct channel g_chan[MAX_CHANNELS];
static uint32_t g_next_id = 1;

static void send_msg(uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, size_t len)
{
    struct amessage m;
    memset(&m, 0, sizeof m);
    m.command = cmd;
    m.arg0 = a0;
    m.arg1 = a1;
    m.data_length = (uint32_t)len;
    m.magic = amsg_magic(cmd);
    logmsg("-> %-4s arg0=%u arg1=%u len=%zu", amsg_name(cmd), a0, a1, len);
    if (g_link->send_frame(&m, data, len) != 0) return;
    g_link->flush();
}

static void send_wrte(struct channel *c, int stream, const unsigned char *buf, size_t len)
{
    if (!c->v2) {
        send_msg(A_WRTE, c->local, c->peer, buf, len);
        return;
    }
    unsigned char *out = malloc(len + 5);
    if (!out) return;
    uint32_t l = (uint32_t)len;
    out[0] = (unsigned char)stream;
    out[1] = (unsigned char)(l & 0xff);
    out[2] = (unsigned char)((l >> 8) & 0xff);
    out[3] = (unsigned char)((l >> 16) & 0xff);
    out[4] = (unsigned char)((l >> 24) & 0xff);
    memcpy(out + 5, buf, len);
    send_msg(A_WRTE, c->local, c->peer, out, len + 5);
    free(out);
}

static const char *prop_or(const char *key, const char *fallback)
{
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

/* Only list features we actually implement, otherwise a real adb client picks
 * the wrong path and fails in confusing ways. */
static void send_cnxn(void)
{
    char banner[640];
    snprintf(banner, sizeof banner,
             "device::ro.product.name=%s;ro.product.model=%s;"
             "ro.product.device=%s;features=%s",
             prop_or("ADBNG_PRODUCT_NAME", "adbng"),
             prop_or("ADBNG_PRODUCT_MODEL", "unknown"),
             prop_or("ADBNG_PRODUCT_DEVICE", "unknown"),
             "shell_v2");
    send_msg(A_CNXN, A_VERSION, A_MAXDATA, banner, strlen(banner) + 1);
}

static void send_token(void)
{
    if (random_bytes(g_token, sizeof g_token) != 0)
        memset(g_token, 0x5a, sizeof g_token);
    g_token_rounds++;
    g_auth_state = 1;
    send_msg(A_AUTH, AUTH_TOKEN, 0, g_token, TOKEN_SIZE);
}

static struct channel *chan_alloc(const char *name, uint32_t peer)
{
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (!g_chan[i].used) {
            struct channel *c = &g_chan[i];
            memset(c, 0, sizeof *c);
            c->used = 1;
            c->local = g_next_id++;
            c->peer = peer;
            c->pid = -1;
            c->fd_in = -1;
            c->fd_out = -1;
            c->fd_err = -1;
            snprintf(c->name, sizeof c->name, "%s", name);
            return c;
        }
    }
    return NULL;
}

static struct channel *chan_by_local(uint32_t local)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (g_chan[i].used && g_chan[i].local == local) return &g_chan[i];
    return NULL;
}

static void chan_release(struct channel *c)
{
    struct shell_proc p = { .pid = c->pid, .fd_in = c->fd_in,
                            .fd_out = c->fd_out, .fd_err = c->fd_err };
    shell_kill(&p);
    free(c->inbuf);
    if (c->sync) sync_free(c->sync);
    memset(c, 0, sizeof *c);
}

/* Refusing a service: the protocol cannot carry a reason on CLSE, so the
 * explanation only goes to the log. */
static void refuse(uint32_t host_id, const char *name)
{
    logmsg("refuse service '%s'", name);
    send_msg(A_CLSE, g_next_id++, host_id, NULL, 0);
}

static int append_input(struct channel *c, const unsigned char *d, size_t n)
{
    if (c->inlen + n > c->incap) {
        size_t cap = c->incap ? c->incap : 1024;
        while (cap < c->inlen + n) cap *= 2;
        unsigned char *nb = realloc(c->inbuf, cap);
        if (!nb) return -1;
        c->inbuf = nb;
        c->incap = cap;
    }
    memcpy(c->inbuf + c->inlen, d, n);
    c->inlen += n;
    return 0;
}

/* Host half-closed stdin.  A pipe can just be closed; a pty cannot, because
 * the same descriptor also carries the output we still have to deliver.  The
 * tty EOF character is the protocol's own answer: in canonical mode read()
 * returns 0 for the reader, exactly like a pipe EOF.  It is appended to the
 * pending input so it stays behind anything the host already sent. */
static void close_stdin(struct channel *c)
{
    if (c->stdin_done) return;
    c->stdin_done = 1;

    if (c->pty) {
        unsigned char eof = PTY_EOF;
        if (c->fd_in < 0 || append_input(c, &eof, 1) != 0) c->inlen = 0;
        return;
    }
    if (c->fd_in >= 0) { close(c->fd_in); c->fd_in = -1; }
    c->inlen = 0;
}

/* ioctl(TIOCSWINSZ) makes the kernel send SIGWINCH to the foreground process
 * group by itself, so nothing else is needed here. */
static void chan_winsize(struct channel *c, int rows, int cols)
{
    if (rows <= 0 || cols <= 0) return;
    c->rows = rows;
    c->cols = cols;
    if (!c->pty) return;
    shell_pty_winsize(c->fd_in >= 0 ? c->fd_in : c->fd_out, rows, cols);
}

/* "shell[,v2][,raw][,pty][,TERM=x]:cmd" — the argument region is everything
 * between the service name and the first colon.  Unknown flags are ignored on
 * purpose: the host adds new ones over time and refusing a channel because of
 * one we do not care about is worse than dropping it. */
static void open_shell(const char *name, uint32_t host_id)
{
    const char *colon = strchr(name, ':');
    size_t arglen = colon ? (size_t)(colon - name) : strlen(name);
    char args[256];
    if (arglen >= sizeof args) arglen = sizeof args - 1;
    memcpy(args, name, arglen);
    args[arglen] = 0;

    int v2 = 0, pty = 0;
    const char *term = NULL;
    char *save = NULL;
    for (char *t = strtok_r(args, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strcmp(t, "v2")) v2 = 1;
        else if (!strcmp(t, "pty")) pty = 1;
        else if (!strncmp(t, "TERM=", 5)) term = t + 5;
    }
    const char *cmd = colon ? colon + 1 : "";

    struct channel *c = chan_alloc(name, host_id);
    if (!c) { refuse(host_id, name); return; }

    struct shell_opts o;
    memset(&o, 0, sizeof o);
    o.cmd = cmd;
    o.term = term;
    /* A tty always merges stderr; asking for a pty and a separate stderr pipe
     * at the same time is contradictory, so the pty wins. */
    o.separate_err = v2 && !pty;
    o.pty = pty;
    o.rows = 0;
    o.cols = 0;

    struct shell_proc p;
    shell_proc_init(&p);
    if (shell_spawn_sh(&o, &p) != 0) {
        logmsg("shell: spawn failed: %s", strerror(errno));
        chan_release(c);
        refuse(host_id, name);
        return;
    }

    c->kind = KIND_SHELL;
    c->v2 = v2;
    c->pty = p.pty;
    c->pid = p.pid;
    c->fd_in = p.fd_in;
    c->fd_out = p.fd_out;
    c->fd_err = p.fd_err;
    c->can_send = 1;

    logmsg("shell: pid=%d v2=%d pty=%d term=%s err=%s cmd='%s'", (int)p.pid, v2,
           p.pty, term ? term : "-", p.fd_err >= 0 ? "separate" : "merged", cmd);
    send_msg(A_OKAY, c->local, c->peer, NULL, 0);
}

/* "exec:" + argv[0] "\0" argv[1] ... — no shell, no globbing, no splitting. */
static void open_exec(const char *name, size_t name_len, uint32_t host_id)
{
    /* Skip the "exec:" prefix: argv starts right after the colon. */
    const char *rest = name + 5;
    size_t rest_len = name_len - 5;
    char *copy = malloc(rest_len + 1);
    if (!copy) { refuse(host_id, name); return; }
    memcpy(copy, rest, rest_len);
    copy[rest_len] = 0;

    char *argv[64];
    int argc = 0;
    char *cur = copy;
    const char *end = copy + rest_len;
    while (cur < end && argc < (int)(sizeof argv / sizeof argv[0]) - 1) {
        argv[argc++] = cur;
        const char *nul = memchr(cur, 0, (size_t)(end - cur));
        if (!nul) break;
        cur = (char *)nul + 1;
    }
    argv[argc] = NULL;

    if (argc == 0) {
        logmsg("exec: empty command line");
        free(copy);
        refuse(host_id, name);
        return;
    }

    struct channel *c = chan_alloc(name, host_id);
    if (!c) { free(copy); refuse(host_id, name); return; }

    struct shell_proc p;
    shell_proc_init(&p);
    if (shell_spawn_argv(argv, 0, &p) != 0) {
        logmsg("exec: spawn failed: %s", strerror(errno));
        free(copy);
        chan_release(c);
        refuse(host_id, name);
        return;
    }
    /* The child owns its own copy after fork; the parent is done with it. */
    free(copy);

    c->kind = KIND_SHELL;
    c->v2 = 0;                     /* exec: has no v2 form on the host side */
    c->pid = p.pid;
    c->fd_in = p.fd_in;
    c->fd_out = p.fd_out;
    c->fd_err = -1;
    c->can_send = 1;
    logmsg("exec: pid=%d", (int)p.pid);
    send_msg(A_OKAY, c->local, c->peer, NULL, 0);
}

static void open_sync(const char *name, uint32_t host_id)
{
    struct channel *c = chan_alloc(name, host_id);
    if (!c) { refuse(host_id, name); return; }
    c->sync = sync_new();
    if (!c->sync) { chan_release(c); refuse(host_id, name); return; }
    /* Push stays disabled unless the operator listed writable prefixes. */
    sync_config_write_roots(c->sync, getenv("ADBNG_WRITE_ROOTS"));
    c->kind = KIND_SYNC;
    c->v2 = 0;
    c->can_send = 1;
    logmsg("sync: channel opened");
    send_msg(A_OKAY, c->local, c->peer, NULL, 0);
}

static void open_service(const char *name, size_t name_len, uint32_t host_id)
{
    if (!strncmp(name, "shell:", 6) || !strncmp(name, "shell,", 6)) {
        open_shell(name, host_id);
        return;
    }
    if (!strncmp(name, "exec:", 5)) {
        open_exec(name, name_len, host_id);
        return;
    }
    if (!strncmp(name, "sync:", 5)) {
        open_sync(name, host_id);
        return;
    }
    refuse(host_id, name);
}

static int on_auth(const struct amessage *m, const unsigned char *data)
{
    if (m->arg0 == AUTH_SIGNATURE) {
        if (m->data_length != RSA_SIG_SIZE) {
            logmsg("auth: bad signature length %u", m->data_length);
            send_token();
            return 0;
        }
        int idx = keys_verify(&g_keys, g_token, sizeof g_token, data);
        if (idx >= 0) {
            char fp[17];
            key_fingerprint(g_keys.mods[idx], fp);
            logmsg("auth: accepted key %s", fp);
            g_auth_state = 2;
            send_cnxn();
            return 0;
        }
        logmsg("auth: signature rejected (%d round(s))", g_token_rounds);
        /* While a key is waiting for approval the host must stay connected:
         * a real adb client keeps retrying its signature until we answer. */
        if (g_pend_active || g_token_rounds < 3) {
            send_token();
            return 0;
        }
        return -1;
    }

    if (m->arg0 == AUTH_RSAPUBLICKEY) {
        char line[1200];
        char host[128] = "unknown";
        unsigned char blob[1024];
        uint32_t mod[RSA_LIMBS];
        char fp[17];
        size_t n = m->data_length < sizeof line - 1 ? m->data_length : sizeof line - 1;

        /* "<base64 key> <user>@<host>" - the trailing tag becomes the label we
         * store in adb_keys, exactly like /data/misc/adb/adb_keys does. */
        memcpy(line, data, n);
        line[n] = 0;

        char *sp = strchr(line, ' ');
        if (sp) {
            char *tag = sp + 1;
            while (*tag == ' ') tag++;
            snprintf(host, sizeof host, "%s", tag);
            *sp = 0;
        }
        if (line[0] == 0 || strlen(line) >= sizeof g_pend_b64) {
            logmsg("auth: offered key is empty or too long");
            return -1;
        }
        int bl = b64_decode(line, blob, sizeof blob);
        if (bl <= 0 || rsa_modulus_from_pubkey_blob(blob, (size_t)bl, mod) != 0) {
            logmsg("auth: offered key is not a usable RSA public key");
            return -1;
        }
        key_fingerprint(mod, fp);

        if (g_pend_active && !strcmp(fp, g_pend_fp)) {
            send_token();           /* same host asking again */
            return 0;
        }

        pend_clear();
        snprintf(g_pend_fp, sizeof g_pend_fp, "%s", fp);
        snprintf(g_pend_b64, sizeof g_pend_b64, "%s", line);
        snprintf(g_pend_host, sizeof g_pend_host, "%s", host);
        g_pend_since = time(NULL);
        g_pend_active = 1;
        logmsg("auth: host %s (%s) is waiting for approval", fp, host);
        pend_publish();
        send_token();
        return 0;
    }

    logmsg("auth: unexpected sub-command %u", m->arg0);
    return -1;
}

static int on_open(const struct amessage *m, const unsigned char *data)
{
    if (!data || m->data_length == 0) {
        logmsg("open: empty service name");
        return -1;
    }
    char name[512];
    size_t n = m->data_length < sizeof name ? m->data_length : sizeof name - 1;
    memcpy(name, data, n);
    name[n] = 0;
    if (g_auth_state != 2) {
        logmsg("open: '%s' before authentication", name);
        return -1;
    }
    open_service(name, n, m->arg0);
    return 0;
}

static int on_write(const struct amessage *m, const unsigned char *data)
{
    struct channel *c = chan_by_local(m->arg1);
    if (!c) {
        logmsg("write: no channel for id %u", m->arg1);
        return 0;
    }
    /* Every WRTE we receive has to be acknowledged, otherwise the host stalls
     * after its first packet. */
    send_msg(A_OKAY, c->local, c->peer, NULL, 0);

    if (m->data_length == 0) return 0;

    if (c->kind == KIND_SYNC) {
        if (c->sync) sync_feed(c->sync, data, m->data_length);
        return 0;
    }

    if (c->v2) {
        /* Host stdin arrives as sub-payloads too; only stdin matters here. */
        size_t off = 0;
        while (off + 5 <= m->data_length) {
            int stream = data[off];
            uint32_t l = (uint32_t)data[off + 1] | ((uint32_t)data[off + 2] << 8) |
                         ((uint32_t)data[off + 3] << 16) | ((uint32_t)data[off + 4] << 24);
            off += 5;
            if (off + l > m->data_length) break;
            switch (stream) {
            case SUB_STDIN:
                if (c->fd_in >= 0 && !c->stdin_done) append_input(c, data + off, l);
                break;
            case SUB_CLOSE_STDIN:
                close_stdin(c);
                break;
            case SUB_WINDOW_SIZE:
                /* struct winsize as four little-endian uint16: rows, cols,
                 * xpixel, ypixel.  Only the first two matter to us. */
                if (l >= 4) {
                    int rows = (int)((uint32_t)data[off] | ((uint32_t)data[off + 1] << 8));
                    int cols = (int)((uint32_t)data[off + 2] | ((uint32_t)data[off + 3] << 8));
                    chan_winsize(c, rows, cols);
                }
                break;
            default:
                break;
            }
            off += l;
        }
        return 0;
    }

    if (c->fd_in >= 0) append_input(c, data, m->data_length);
    return 0;
}

static void on_close(const struct amessage *m)
{
    struct channel *c = chan_by_local(m->arg1);
    if (!c) return;
    logmsg("close: '%s'", c->name);
    send_msg(A_CLSE, c->local, c->peer, NULL, 0);
    chan_release(c);
}

/* Send the exit status (v2 only), then CLSE.  This has to be the last thing
 * that happens on a channel so nothing overtakes the exit sub-packet. */
static void check_finish(struct channel *c)
{
    if (!c->used || !c->can_send) return;
    if (!c->eof_out) return;
    if (c->fd_err >= 0 && !c->eof_err) return;

    if (!c->exit_sent) {
        if (c->pid > 0) {
            int st = 0;
            pid_t r = waitpid(c->pid, &st, WNOHANG);
            if (r != c->pid) return;      /* still running, poll again later */
            c->pid = -1;
            if (WIFEXITED(st)) c->exit_code = WEXITSTATUS(st);
            else if (WIFSIGNALED(st)) c->exit_code = 128 + WTERMSIG(st);
            else c->exit_code = 1;
        }
        if (c->v2) {
            unsigned char sub[4] = {
                (unsigned char)(c->exit_code & 0xff),
                (unsigned char)((c->exit_code >> 8) & 0xff),
                (unsigned char)((c->exit_code >> 16) & 0xff),
                (unsigned char)((c->exit_code >> 24) & 0xff),
            };
            logmsg("shell: exit %d", c->exit_code);
            send_wrte(c, SUB_EXIT, sub, sizeof sub);
            c->exit_sent = 1;
            c->can_send = 0;             /* wait for the ack before CLSE */
            return;
        }
        c->exit_sent = 1;
        logmsg("shell: exit %d (not reported, v1 has no exit code)", c->exit_code);
    }

    send_msg(A_CLSE, c->local, c->peer, NULL, 0);
    chan_release(c);
}

/* Drop the reading side.  With a pty fd_in and fd_out are the same master, so
 * both fields have to go at once - closing only one leaves a dangling fd that
 * a later poll() would register and then fail on. */
static void close_read_side(struct channel *c, int what, int fd)
{
    if (c->pty) {
        c->fd_in = -1;
        c->fd_out = -1;
        c->inlen = 0;
    } else if (what == EV_STDOUT) {
        c->fd_out = -1;
    } else {
        c->fd_err = -1;
    }
    close(fd);
}

static void handle_channel_io(struct channel *c, int what)
{
    unsigned char buf[IO_CHUNK];

    if (what == EV_STDIN) {
        if (c->fd_in < 0 || c->inlen == 0) return;
        ssize_t n = write(c->fd_in, c->inbuf, c->inlen);
        if (n > 0) {
            memmove(c->inbuf, c->inbuf + n, c->inlen - (size_t)n);
            c->inlen -= (size_t)n;
        } else if (n < 0 && (errno == EPIPE || errno == EIO)) {
            /* Nobody is reading any more.  A pty master still has output to
             * give us, so only the pipe case can drop the descriptor. */
            c->inlen = 0;
            if (!c->pty) { close(c->fd_in); c->fd_in = -1; }
        }
        return;
    }

    int fd = (what == EV_STDOUT) ? c->fd_out : c->fd_err;
    int *eof = (what == EV_STDOUT) ? &c->eof_out : &c->eof_err;
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        send_wrte(c, what == EV_STDOUT ? SUB_STDOUT : SUB_STDERR, buf, (size_t)n);
        c->can_send = 0;               /* flow control: wait for OKAY */
        return;
    }
    /* Linux reports EIO on a pty master once the last slave is closed, which
     * is the pty equivalent of reading EOF from a pipe. */
    if (n == 0 || (n < 0 && errno == EIO)) {
        *eof = 1;
        close_read_side(c, what, fd);
        logmsg("chan '%s': %s EOF", c->name, what == EV_STDOUT ? "stdout" : "stderr");
        return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        *eof = 1;
        close_read_side(c, what, fd);
    }
}

/* One protocol message off the wire.  Returns <0 when the link has to go. */
static int handle_message(const struct amessage *m, unsigned char *data)
{
    int rc = 0;
    switch (m->command) {
    case A_CNXN:
        break;
    case A_AUTH:
        rc = on_auth(m, data);
        break;
    case A_OPEN:
        rc = on_open(m, data);
        break;
    case A_WRTE:
        rc = on_write(m, data);
        break;
    case A_CLSE:
        on_close(m);
        break;
    case A_OKAY: {
        struct channel *c = chan_by_local(m->arg1);
        if (c) c->can_send = 1;
        break;
    }
    case A_STLS:
        logmsg("stls not supported over this transport");
        rc = -1;
        break;
    default:
        logmsg("unknown command %08x", m->command);
        break;
    }
    return rc;
}

/* A new enumeration or a fresh host: nothing from the previous session can be
 * reused, and the host expects CNXN from scratch. */
static void link_restart(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (g_chan[i].used) chan_release(&g_chan[i]);
    g_auth_state = 0;
    g_token_rounds = 0;
    send_cnxn();
    send_token();
}

int main(int argc, char **argv)
{
    const char *keys_path = NULL;
    const char *ffs_path = NULL;
    int fd = -1;
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fd") && i + 1 < argc) {
            fd = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ffs") && i + 1 < argc) {
            ffs_path = argv[++i];
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            keys_path = argv[++i];
        } else if (!strcmp(argv[i], "--verbose")) {
            /* logging is always on for now */
        } else {
            fprintf(stderr, "usage: usbgwd (--fd N | --ffs DIR) --keys PATH\n");
            return 2;
        }
    }
    if ((fd < 0) == (ffs_path == NULL) || !keys_path) {
        fprintf(stderr, "usage: usbgwd (--fd N | --ffs DIR) --keys PATH\n");
        return 2;
    }
    state_init();
    g_link = ffs_path ? link_ffs(ffs_path) : link_fd(fd);

    g_keys_path = keys_path;
    int kcount = keys_load(keys_path, &g_keys);
    if (kcount <= 0) {
        /* Not fatal: a fresh install pairs its first host through the
         * pending-authorization flow instead of needing a key upfront. */
        logmsg("no usable host key in %s, waiting for the first pairing", keys_path);
        kcount = 0;
    }
    logmsg("usbgwd start: link=%s keys=%d", g_link->name, kcount);

    int lr = g_link->init();
    if (lr < 0) {
        logmsg("link init failed");
        return 1;
    }
    if (lr == 1) {
        send_cnxn();
        send_token();
    }

    for (;;) {
        struct pollfd pfds[LINK_MAX_FDS + MAX_CHANNELS * 3];
        int ids[LINK_MAX_FDS + MAX_CHANNELS * 3];
        struct { struct channel *c; int what; } map[LINK_MAX_FDS + MAX_CHANNELS * 3];
        int nfds = 0;
        int link_n = 0;

        /* Link entries come first so the transport can also register a write
         * side (ffs needs one for the bulk IN endpoint). */
        int n = g_link->pollfds(0, pfds, ids, LINK_MAX_FDS);
        for (int i = 0; i < n; i++) {
            map[i].c = NULL;
            map[i].what = EV_LINK;
        }
        link_n = n;
        nfds = n;

        for (int i = 0; i < MAX_CHANNELS; i++) {
            struct channel *c = &g_chan[i];
            if (!c->used) continue;
            if (c->fd_in >= 0 && c->inlen > 0) {
                pfds[nfds].fd = c->fd_in;
                pfds[nfds].events = POLLOUT;
                map[nfds].c = c;
                map[nfds].what = EV_STDIN;
                nfds++;
            }
            if (!c->eof_out && c->fd_out >= 0 && c->can_send) {
                pfds[nfds].fd = c->fd_out;
                pfds[nfds].events = POLLIN;
                map[nfds].c = c;
                map[nfds].what = EV_STDOUT;
                nfds++;
            }
            if (!c->eof_err && c->fd_err >= 0 && c->can_send) {
                pfds[nfds].fd = c->fd_err;
                pfds[nfds].events = POLLIN;
                map[nfds].c = c;
                map[nfds].what = EV_STDERR;
                nfds++;
            }
        }

        /* Short timeout when a child may be on its way out: its pipes are
         * already at EOF but waitpid() has not reported it yet. */
        int timeout = -1;
        for (int i = 0; i < MAX_CHANNELS; i++) {
            struct channel *c = &g_chan[i];
            if (c->used && c->eof_out && (c->fd_err < 0 || c->eof_err) && !c->exit_sent) {
                timeout = 50;
                break;
            }
        }

        /* A pending authorization needs a heartbeat: the decision file is
         * polled, not signalled. */
        if (g_pend_active && timeout < 0) timeout = 1000;
        int pr = poll(pfds, (nfds_t)nfds, timeout);
        if (pr < 0) {
            if (errno == EINTR) continue;
            logmsg("poll failed: %s", strerror(errno));
            break;
        }

        /* Transport events first: a frame, a fresh enumeration, or a dead link.
         * One frame per pass keeps the loop fair between link and children. */
        int dead = 0;
        for (int i = 0; i < link_n; i++) {
            if (!pfds[i].revents) continue;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLOUT)) {
                struct amessage m;
                unsigned char *data = NULL;
                int rc = g_link->on_event(ids[i], &m, &data);
                if (rc == 1) {
                    logmsg("<- %-4s arg0=%u arg1=%u len=%u", amsg_name(m.command), m.arg0,
                           m.arg1, m.data_length);
                    if (handle_message(&m, data) < 0) dead = 1;
                    free(data);
                } else if (rc == 2) {
                    link_restart();
                } else if (rc < 0) {
                    logmsg("link gone");
                    dead = 1;
                }
            }
        }

        if (dead) {
            logmsg("link closed");
            break;
        }
        /* A parked host waits for a decision file written by the manager. */
        if (g_pend_active && pend_tick() < 0) {
            logmsg("pending: dropping link");
            break;
        }
        for (int i = link_n; i < nfds; i++) {
            if (!pfds[i].revents) continue;
            if (!map[i].c || !map[i].c->used) continue;
            handle_channel_io(map[i].c, map[i].what);
        }

        for (int i = 0; i < MAX_CHANNELS; i++) {
            struct channel *c = &g_chan[i];
            if (!c->used) continue;
            if (c->kind != KIND_SYNC) {
                check_finish(c);
                continue;
            }

            sync_pump(c->sync);
            if (c->can_send && sync_pending(c->sync) > 0) {
                static unsigned char buf[SYNC_CHUNK];
                size_t n = sync_take(c->sync, buf, sizeof buf);
                send_msg(A_WRTE, c->local, c->peer, buf, n);
                c->can_send = 0;
            }
            if (c->can_send && sync_finished(c->sync)) {
                logmsg("sync: closing channel");
                send_msg(A_CLSE, c->local, c->peer, NULL, 0);
                chan_release(c);
            }
        }

        /* Push whatever the transport still holds; a full USB endpoint simply
         * returns "would block" and the next poll waits for POLLOUT. */
        if (g_link->flush() < 0) {
            logmsg("link write failed");
            break;
        }
    }

    pend_clear();          /* a question that lost its host must not linger */
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (g_chan[i].used) chan_release(&g_chan[i]);
    g_link->shutdown();

    logmsg("usbgwd exit");
    return 0;
}