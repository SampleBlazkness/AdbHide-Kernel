/* adbng - SPDX-License-Identifier: MIT
 *
 * sync: service.  Read direction (STAT / LIST / RECV) plus the write direction
 * (SEND) added in M5a.
 *
 * The sync protocol runs *inside* an ADB channel: the channel carries a stream
 * of (id, length) frames, and each frame may contain DATA / FAIL / a struct.
 * SEND is a root file write capability, so it stays refused until the daemon is
 * given an allow-list of directory prefixes: sync_config_write_roots() takes
 * ADBNG_WRITE_ROOTS ("/a:/b") or falls back to /data/adb/adbng/write_roots.conf.
 * With no roots configured every SEND is answered with FAIL.
 */
#ifndef ADBNG_SYNC_H
#define ADBNG_SYNC_H

#include <stddef.h>
#include <stdint.h>

struct sync_state;

struct sync_state *sync_new(void);
void sync_free(struct sync_state *s);

/* Configure the SEND allow-list. spec == NULL means "read the config file". */
void sync_config_write_roots(struct sync_state *s, const char *spec);

/* Bytes received from the host (already unwrapped from WRTE packets). */
int sync_feed(struct sync_state *s, const unsigned char *data, size_t len);

/* Generate more output: reads files, appends DATA/DONE frames. */
void sync_pump(struct sync_state *s);

size_t sync_pending(const struct sync_state *s);
size_t sync_take(struct sync_state *s, unsigned char *buf, size_t max);

/* QUIT seen and nothing left to send. */
int sync_finished(const struct sync_state *s);

#endif