/* adbng - SPDX-License-Identifier: MIT
 *
 * Transport between the daemon and the host.
 *
 * Exactly one backend is active per run.  The daemon stays single-threaded:
 * the transport never blocks on anything the poll loop did not report first,
 * so a socketpair test and a USB gadget behave the same way.
 */
#ifndef ADBNG_LINK_H
#define ADBNG_LINK_H

#include "proto.h"

#include <poll.h>
#include <stddef.h>

#define LINK_MAX_FDS 3

struct link_ops {
    const char *name;

    /* 0 = ready but waiting for the host (ffs: until ENABLE),
     * 1 = link already up, send CNXN straight away,
     * -1 = failed. */
    int (*init)(void);

    /* Append at most LINK_MAX_FDS entries after `base`; ids[ i ] is the id to
     * pass back to on_event().  Returns the number of entries written. */
    int (*pollfds)(int base, struct pollfd *pfds, int *ids, int max);

    /*  1 = one frame in *m and *payload (payload is malloc'd or NULL)
     *  2 = the link just came up: drop all channels and redo CNXN/AUTH
     *  0 = nothing to deliver
     * -1 = link is gone */
    int (*on_event)(int id, struct amessage *m, unsigned char **payload);

    /* Queue one frame.  Returns 0 or -1. */
    int (*send_frame)(const struct amessage *m, const void *payload, size_t len);

    /* fd that wants POLLOUT while the send queue is non-empty, or -1. */
    int (*out_fd)(void);

    /* Push as much queued data as the transport takes.  0 = done/ok,
     * 1 = would block, keep the POLLOUT registration, -1 = gone. */
    int (*flush)(void);

    void (*shutdown)(void);
};

/* socket, socketpair or anything else that behaves like a byte stream */
const struct link_ops *link_fd(int fd);

/* usb_ffs_dir is the directory that holds ep0/ep1/ep2 for the function */
const struct link_ops *link_ffs(const char *usb_ffs_dir);

/* Blocking read of one complete 24-byte header + payload, with the framing
 * checks every transport needs.  Shared so the ffs path and the socketpair
 * tests exercise the same parser. */
int link_read_frame(int fd, struct amessage *m, unsigned char **payload);

#endif