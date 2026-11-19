/* adbng - SPDX-License-Identifier: MIT
 *
 * usbgwd: the device side of the ADB protocol.
 *
 * M2 scope: framing, CNXN banner, AUTH (token + signature against a key
 * store),channel multiplexing and one dummy service ("echo:") used by the
 * in-device host tester.  Real services (shell/sync/forward) land in M3.
 */
#include "keys.h"
#include "proto.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CHANNELS 16

struct channel {
    int used;
    uint32_t local;
    uint32_t peer;
    char name[256];
};

static int g_fd = -1;
static struct key_store g_keys;
static unsigned char g_token[TOKEN_SIZE];
static int g_auth_state;      /* 0 = banner sent, 1 = token outstanding, 2 = authed */
static int g_token_rounds;
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
    if (io_write_full(g_fd, &m, AMSG_SIZE) != 0) return;
    if (len) io_write_full(g_fd, data, len);
}

static const char *prop_or(const char *key, const char *fallback)
{
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

static void send_cnxn(void)
{
    char banner[640];
    snprintf(banner, sizeof banner,
             "device::ro.product.name=%s;ro.product.model=%s;"
             "ro.product.device=%s;features=%s",
             prop_or("ADBNG_PRODUCT_NAME", "adbng"),
             prop_or("ADBNG_PRODUCT_MODEL", "unknown"),
             prop_or("ADBNG_PRODUCT_DEVICE", "unknown"),
             "shell_v2,cmd");
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
            g_chan[i].used = 1;
            g_chan[i].local = g_next_id++;
            g_chan[i].peer = peer;
            snprintf(g_chan[i].name, sizeof g_chan[i].name, "%s", name);
            return &g_chan[i];
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

/* Refusing a service: the protocol has no way to carry a reason on CLSE, so we
 * close the channel and put the explanation in the log / README. */
static void refuse(uint32_t host_id, const char *name)
{
    logmsg("refuse service '%s'", name);
    send_msg(A_CLSE, g_next_id++, host_id, NULL, 0);
}

static void open_service(const char *name, uint32_t host_id)
{
    if (strncmp(name, "echo:", 5) == 0) {
        struct channel *c = chan_alloc(name, host_id);
        if (!c) { refuse(host_id, name); return; }
        send_msg(A_OKAY, c->local, c->peer, NULL, 0);
        char reply[256];
        int n = snprintf(reply, sizeof reply, "pong:%s\n", name + 5);
        send_msg(A_WRTE, c->local, c->peer, reply, (size_t)n);
        send_msg(A_CLSE, c->local, c->peer, NULL, 0);
        c->used = 0;
        return;
    }

    static const char *known_but_unimplemented[] = {
        "shell", "exec:", "sync:", "reboot:", "root:", "remount:",
        "jdwp:", "sideload:", "tcpip:", NULL
    };
    for (int i = 0; known_but_unimplemented[i]; i++) {
        size_t l = strlen(known_but_unimplemented[i]);
        if (strncmp(name, known_but_unimplemented[i], l) == 0) {
            refuse(host_id, name);
            return;
        }
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
        if (g_token_rounds >= 3) return -1;
        send_token();
        return 0;
    }

    if (m->arg0 == AUTH_RSAPUBLICKEY) {
        /* A host we have never seen.  Per our security model we never trust it
         * silently: log it and drop the link, the user has to confirm in the
         * app (M6) before the key is written to the store. */
        char line[1200];
        size_t n = m->data_length < sizeof line - 1 ? m->data_length : sizeof line - 1;
        memcpy(line, data, n);
        line[n] = 0;
        char *sp = strchr(line, ' ');
        if (sp) *sp = 0;
        logmsg("auth: unknown host key offered, needs local confirmation");
        logmsg("auth: key = %s", line);
        return -1;
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
    char name[256];
    size_t n = m->data_length < sizeof name - 1 ? m->data_length : sizeof name - 1;
    memcpy(name, data, n);
    name[n] = 0;
    if (g_auth_state != 2) {
        logmsg("open: '%s' before authentication", name);
        return -1;
    }
    open_service(name, m->arg0);
    return 0;
}

static int on_write(const struct amessage *m, const unsigned char *data)
{
    struct channel *c = chan_by_local(m->arg1);
    if (!c) {
        logmsg("write: no channel for id %u", m->arg1);
        return 0;
    }
    logmsg("write: %u byte(s) on '%s'", m->data_length, c->name);
    /* echo channels close right after replying, nothing to do here yet */
    (void)data;
    return 0;
}

static void on_close(const struct amessage *m)
{
    struct channel *c = chan_by_local(m->arg1);
    if (!c) return;
    logmsg("close: '%s'", c->name);
    c->used = 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: usbgwd --fd N --keys PATH [--verbose]\n"
            "  --fd N      connected socket / FunctionFS ep0 file descriptor\n"
            "  --keys PATH trusted host public keys (adb_keys format)\n");
}

int main(int argc, char **argv)
{
    const char *keys_path = NULL;
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--fd") && i + 1 < argc) {
            g_fd = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            keys_path = argv[++i];
        } else if (!strcmp(argv[i], "--verbose")) {
            /* logging is always on for now */
        } else {
            usage();
            return 2;
        }
    }
    if (g_fd < 0 || !keys_path) { usage(); return 2; }

    int kcount = keys_load(keys_path, &g_keys);
    if (kcount <= 0) {
        logmsg("no usable host key in %s", keys_path);
        return 1;
    }
    logmsg("usbgwd start: fd=%d keys=%d", g_fd, kcount);

    send_cnxn();
    send_token();

    for (;;) {
        struct amessage m;
        if (io_read_full(g_fd, &m, AMSG_SIZE) != 0) {
            logmsg("link closed");
            break;
        }
        if (m.magic != amsg_magic(m.command)) {
            logmsg("bad magic for %08x", m.command);
            break;
        }
        if (m.data_length > A_MAXDATA) {
            logmsg("oversized payload %u", m.data_length);
            break;
        }
        unsigned char *data = NULL;
        if (m.data_length) {
            data = malloc(m.data_length);
            if (!data) break;
            if (io_read_full(g_fd, data, m.data_length) != 0) { free(data); break; }
        }
        logmsg("<- %-4s arg0=%u arg1=%u len=%u", amsg_name(m.command), m.arg0, m.arg1,
               m.data_length);

        int rc = 0;
        switch (m.command) {
        case A_CNXN:
            /* host banner; we already sent ours */
            break;
        case A_AUTH:
            rc = on_auth(&m, data);
            break;
        case A_OPEN:
            rc = on_open(&m, data);
            break;
        case A_WRTE:
            rc = on_write(&m, data);
            break;
        case A_CLSE:
            on_close(&m);
            break;
        case A_OKAY:
            break;
        case A_STLS:
            logmsg("stls not supported over this transport");
            rc = -1;
            break;
        default:
            logmsg("unknown command %08x", m.command);
            break;
        }
        free(data);
        if (rc < 0) break;
    }

    logmsg("usbgwd exit");
    return 0;
}