/* adbng - SPDX-License-Identifier: MIT
 *
 * USB FunctionFS transport.
 *
 * The kernel gadget side does the USB part; we only have to
 *
 *   - write the adb interface descriptors (class ff/42/01, two bulk endpoints),
 *   - answer the ep0 event stream (bind / enable / disable / setup / ...),
 *   - shovel frames over ep1 (bulk IN) and ep2 (bulk OUT).
 *
 * Descriptors use the v1 header: full speed + high speed only.  Advertising
 * SuperSpeed would need endpoint companions and a BOS descriptor, and buys
 * nothing for adb - high speed is already ~10x what adb needs.
 */
#include "link.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FFS_DESCRIPTORS_MAGIC 1u

#define FFS_EV_BIND    1
#define FFS_EV_UNBIND  2
#define FFS_EV_ENABLE  3
#define FFS_EV_DISABLE 4
#define FFS_EV_SETUP   5
#define FFS_EV_SUSPEND 6
#define FFS_EV_RESUME  7

/* event ids handed to the daemon; the poll ids are opaque to it */
#define ID_EP0 1
#define ID_OUT 2
#define ID_IN  3

struct ffs_ctrl {
    uint8_t req_type;
    uint8_t req;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed));

struct ffs_event {
    union {
        struct ffs_ctrl setup;
        struct { uint32_t data; uint8_t pad[4]; } misc;
    } u;
    uint8_t type;
    uint8_t pad[3];
} __attribute__((packed));

struct usb_intf_desc {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct usb_ep_desc {
    uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} __attribute__((packed));

struct ffs_func_desc {
    struct usb_intf_desc intf;
    struct usb_ep_desc in;
    struct usb_ep_desc out;
} __attribute__((packed));

struct ffs_descs_v1 {
    uint32_t magic;
    uint32_t length;
    uint32_t fs_count;
    uint32_t hs_count;
    struct ffs_func_desc fs;
    struct ffs_func_desc hs;
} __attribute__((packed));

#define ADB_CLASS 0xff
#define ADB_SUBCLASS 0x42
#define ADB_PROTOCOL 0x01

static const struct ffs_descs_v1 g_descs = {
    .magic = FFS_DESCRIPTORS_MAGIC,
    .length = sizeof(struct ffs_descs_v1),
    .fs_count = 3,
    .hs_count = 3,
    .fs = {
        .intf = { 9, 4, 0, 0, 2, ADB_CLASS, ADB_SUBCLASS, ADB_PROTOCOL, 0 },
        .in = { 7, 5, 0x81, 2, 64, 0 },
        .out = { 7, 5, 0x02, 2, 64, 0 },
    },
    .hs = {
        .intf = { 9, 4, 0, 0, 2, ADB_CLASS, ADB_SUBCLASS, ADB_PROTOCOL, 0 },
        .in = { 7, 5, 0x81, 2, 512, 0 },
        .out = { 7, 5, 0x02, 2, 512, 0 },
    },
};

/* FunctionFS wants a second blob on ep0: the strings table.  Without it the
 * instance stays in FFS_READ_STRINGS, the gadget's `desc_ready` flag is never
 * set and composite_bind() fails the whole gadget with -ENODEV.  Our
 * interface descriptor uses iInterface = 0, so no string is actually needed
 * and an empty table is enough - the write itself is the mandatory part. */
#define FFS_STRINGS_MAGIC 2u

struct ffs_strings_v1 {
    uint32_t magic;
    uint32_t length;
    uint32_t str_count;
    uint32_t lang_count;
} __attribute__((packed));

static const struct ffs_strings_v1 g_strings = {
    .magic = FFS_STRINGS_MAGIC,
    .length = sizeof(struct ffs_strings_v1),
    .str_count = 0,
    .lang_count = 0,
};

static char g_dir[192];
static int g_ep0 = -1;
static int g_ep_in = -1;               /* bulk IN, we write */
static int g_ep_out = -1;              /* bulk OUT, we read */
static int g_enabled;
static int g_need_descs;               /* set on UNBIND, cleared after rewrite */
static long long g_retry_ms;

static unsigned char *g_tx;
static size_t g_tx_len, g_tx_off, g_tx_cap;

static int ep_open(const char *name, int flags, const char *what)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    int fd = open(p, flags | O_CLOEXEC);
    if (fd < 0) {
        logmsg("ffs: open %s (%s): %s", name, what, strerror(errno));
        return -1;
    }
    return fd;
}

static void close_eps(void)
{
    if (g_ep_in >= 0) { close(g_ep_in); g_ep_in = -1; }
    if (g_ep_out >= 0) { close(g_ep_out); g_ep_out = -1; }
}

/* ep1/ep2 only exist once the descriptors are in place; before ENABLE the
 * gadget may not have created them yet, which is not an error. */
static int try_open_eps(void)
{
    if (g_ep_in < 0) {
        g_ep_in = ep_open("ep1", O_RDWR | O_NONBLOCK, "bulk in");
        if (g_ep_in < 0) return -1;
    }
    if (g_ep_out < 0) {
        g_ep_out = ep_open("ep2", O_RDWR, "bulk out");
        if (g_ep_out < 0) return -1;
    }
    return 0;
}

static int handle_setup(const struct ffs_ctrl *c)
{
    /* Standard requests never reach us: the kernel answers them from the
     * descriptor set.  A vendor/class request is acknowledged with an empty
     * reply so the control transfer completes instead of timing out. */
    if (c->req_type & 0x80) {
        if (write(g_ep0, "", 0) < 0 && errno != EINTR) {
            logmsg("ffs: setup IN ack failed: %s", strerror(errno));
            return -1;
        }
    } else {
        unsigned char b;
        if (read(g_ep0, &b, 0) < 0 && errno != EINTR) {
            logmsg("ffs: setup OUT ack failed: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int handle_ep0(void)
{
    struct ffs_event ev;
    ssize_t n = read(g_ep0, &ev, sizeof ev);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        logmsg("ffs: ep0 read: %s", strerror(errno));
        return -1;
    }
    if ((size_t)n != sizeof ev) {
        logmsg("ffs: short ep0 event (%zd bytes)", n);
        return 0;
    }

    switch (ev.type) {
    case FFS_EV_BIND:
        logmsg("ffs: bind");
        return 0;
    case FFS_EV_UNBIND:
        logmsg("ffs: unbind");
        close_eps();
        g_enabled = 0;
        g_need_descs = 1;              /* kernel drops the descriptor set */
        return 0;
    case FFS_EV_ENABLE:
        if (try_open_eps() != 0) return -1;
        g_enabled = 1;
        logmsg("ffs: enabled, adb interface up");
        return 2;                      /* daemon restarts CNXN/AUTH */
    case FFS_EV_DISABLE:
        logmsg("ffs: disabled");
        close_eps();
        g_enabled = 0;
        return 0;
    case FFS_EV_SUSPEND:
        logmsg("ffs: suspend");
        return 0;
    case FFS_EV_RESUME:
        logmsg("ffs: resume");
        return 0;
    case FFS_EV_SETUP:
        return handle_setup(&ev.u.setup);
    default:
        logmsg("ffs: unknown event %u", ev.type);
        return 0;
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Hand FunctionFS the two blobs it expects on ep0.  Order matters: the first
 * write moves the instance to FFS_READ_STRINGS, the second one completes it,
 * creates the ep files and sets desc_ready. */
static int write_descs(void)
{
    ssize_t n = write(g_ep0, &g_descs, sizeof g_descs);
    if (n != (ssize_t)sizeof g_descs) {
        logmsg("ffs: descriptor write returned %zd: %s", n, strerror(errno));
        return -1;
    }
    n = write(g_ep0, &g_strings, sizeof g_strings);
    if (n != (ssize_t)sizeof g_strings) {
        logmsg("ffs: strings write returned %zd: %s", n, strerror(errno));
        return -1;
    }
    logmsg("ffs: descriptors + strings written (%zu + %zu bytes, ff/42/01)",
           sizeof g_descs, sizeof g_strings);
    return 0;
}

/* Closing the last ep0 fd resets the instance, which is also the way out of a
 * half-written state (one blob accepted, the other not). */
static int reopen_ep0(void)
{
    if (g_ep0 >= 0) { close(g_ep0); g_ep0 = -1; }
    g_ep0 = ep_open("ep0", O_RDWR, "control");
    return g_ep0 < 0 ? -1 : 0;
}

/* After UNBIND the kernel throws the descriptor set away (ffs_data_reset), so
 * it has to be handed over again before the next bind.  That reset runs from a
 * workqueue, hence the retry (throttled) from the poll loop instead of doing
 * it inline in the event handler. */
static void maybe_rewrite_descs(void)
{
    if (!g_need_descs) return;
    long long t = now_ms();
    if (g_retry_ms && t - g_retry_ms < 200) return;
    g_retry_ms = t;
    if (reopen_ep0() != 0) return;
    if (write_descs() != 0) return;
    g_need_descs = 0;
    try_open_eps();
}

static int ffs_init(void)
{
    g_ep0 = ep_open("ep0", O_RDWR, "control");
    if (g_ep0 < 0) return -1;
    if (write_descs() != 0) return -1;
    try_open_eps();                    /* ep files exist once we are ACTIVE */
    return 0;                          /* wait for ENABLE */
}

static int ffs_pollfds(int base, struct pollfd *pfds, int *ids, int max)
{
    int n = 0;
    (void)base;
    maybe_rewrite_descs();             /* keeps ep0 fed across a re-bind */
    if (g_ep0 >= 0 && n < max) {
        pfds[n].fd = g_ep0;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        ids[n++] = ID_EP0;
    }
    if (g_ep_out >= 0 && n < max) {
        pfds[n].fd = g_ep_out;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        ids[n++] = ID_OUT;
    }
    if (g_ep_in >= 0 && g_tx_len > g_tx_off && n < max) {
        pfds[n].fd = g_ep_in;
        pfds[n].events = POLLOUT;
        pfds[n].revents = 0;
        ids[n++] = ID_IN;
    }
    return n;
}

static int ffs_on_event(int id, struct amessage *m, unsigned char **payload)
{
    switch (id) {
    case ID_EP0:
        return handle_ep0();
    case ID_OUT: {
        int rc = link_read_frame(g_ep_out, m, payload);
        if (rc == 0) return -1;        /* endpoint went away mid-frame */
        return rc < 0 ? -1 : 1;
    }
    case ID_IN:
        return 0;                      /* POLLOUT: the loop flushes below */
    default:
        return 0;
    }
}

static int ffs_reserve(size_t need)
{
    if (need <= g_tx_cap) return 0;
    size_t cap = g_tx_cap ? g_tx_cap : 4096;
    while (cap < need) cap *= 2;
    unsigned char *nb = realloc(g_tx, cap);
    if (!nb) return -1;
    g_tx = nb;
    g_tx_cap = cap;
    return 0;
}

static int ffs_send_frame(const struct amessage *m, const void *payload, size_t len)
{
    /* Compact first: the queue is drained from g_tx_off. */
    if (g_tx_off) {
        memmove(g_tx, g_tx + g_tx_off, g_tx_len - g_tx_off);
        g_tx_len -= g_tx_off;
        g_tx_off = 0;
    }
    if (ffs_reserve(g_tx_len + AMSG_SIZE + len) != 0) return -1;

    memcpy(g_tx + g_tx_len, m, AMSG_SIZE);
    g_tx_len += AMSG_SIZE;
    if (len) {
        memcpy(g_tx + g_tx_len, payload, len);
        g_tx_len += len;
    }
    return 0;
}

static int ffs_out_fd(void)
{
    return (g_ep_in >= 0 && g_tx_len > g_tx_off) ? g_ep_in : -1;
}

static int ffs_flush(void)
{
    while (g_tx_len > g_tx_off) {
        if (g_ep_in < 0) return 1;     /* not enabled yet, keep the bytes */
        ssize_t n = write(g_ep_in, g_tx + g_tx_off, g_tx_len - g_tx_off);
        if (n > 0) {
            g_tx_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 1;
        logmsg("ffs: ep1 write: %s", strerror(errno));
        return -1;
    }
    g_tx_len = g_tx_off = 0;
    return 0;
}

static void ffs_shutdown(void)
{
    close_eps();
    if (g_ep0 >= 0) { close(g_ep0); g_ep0 = -1; }
    free(g_tx);
    g_tx = NULL;
    g_tx_cap = g_tx_len = g_tx_off = 0;
}

static const struct link_ops g_ffs_ops = {
    .name = "ffs",
    .init = ffs_init,
    .pollfds = ffs_pollfds,
    .on_event = ffs_on_event,
    .send_frame = ffs_send_frame,
    .out_fd = ffs_out_fd,
    .flush = ffs_flush,
    .shutdown = ffs_shutdown,
};

const struct link_ops *link_ffs(const char *usb_ffs_dir)
{
    snprintf(g_dir, sizeof g_dir, "%s", usb_ffs_dir);
    return &g_ffs_ops;
}