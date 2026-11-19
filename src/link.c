/* adbng - SPDX-License-Identifier: MIT
 *
 * Shared frame reader plus the plain-stream transport (socket / socketpair).
 */
#include "link.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read one complete frame.  A frame is a 24-byte header followed by exactly
 * data_length bytes; both a socket and a USB bulk endpoint carry that as a
 * plain byte stream, so the same code works for every transport. */
int link_read_frame(int fd, struct amessage *m, unsigned char **payload)
{
    *payload = NULL;
    if (io_read_full(fd, m, AMSG_SIZE) != 0) return 0;

    if (m->magic != amsg_magic(m->command)) {
        logmsg("bad magic for %08x", m->command);
        return -1;
    }
    if (m->data_length > A_MAXDATA) {
        logmsg("oversized payload %u", m->data_length);
        return -1;
    }
    if (m->data_length == 0) return 1;

    unsigned char *buf = malloc(m->data_length + 1);
    if (!buf) return -1;
    if (io_read_full(fd, buf, m->data_length) != 0) {
        free(buf);
        return 0;
    }
    buf[m->data_length] = 0;
    *payload = buf;
    return 1;
}

/* ---------------- plain stream ---------------- */

static int g_fd = -1;

static int fd_init(void) { return g_fd >= 0 ? 1 : -1; }

static int fd_pollfds(int base, struct pollfd *pfds, int *ids, int max)
{
    if (max < 1) return 0;
    pfds[0].fd = g_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    ids[0] = base;
    return 1;
}

static int fd_on_event(int id, struct amessage *m, unsigned char **payload)
{
    (void)id;
    int rc = link_read_frame(g_fd, m, payload);
    if (rc == 0) return -1;            /* peer closed mid-frame: treat as gone */
    if (rc < 0) return -1;
    return 1;
}

static int fd_send_frame(const struct amessage *m, const void *payload, size_t len)
{
    if (io_write_full(g_fd, m, AMSG_SIZE) != 0) return -1;
    if (len && io_write_full(g_fd, payload, len) != 0) return -1;
    return 0;
}

static int fd_out_fd(void) { return -1; }
static int fd_flush(void) { return 0; }

static void fd_shutdown(void)
{
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
}

static const struct link_ops g_fd_ops = {
    .name = "stream",
    .init = fd_init,
    .pollfds = fd_pollfds,
    .on_event = fd_on_event,
    .send_frame = fd_send_frame,
    .out_fd = fd_out_fd,
    .flush = fd_flush,
    .shutdown = fd_shutdown,
};

const struct link_ops *link_fd(int fd)
{
    g_fd = fd;
    return &g_fd_ops;
}