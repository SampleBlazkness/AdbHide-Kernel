/* adbng - SPDX-License-Identifier: MIT
 *
 * sync: service, read direction only (STAT / LIST / RECV).
 *
 * The sync protocol runs *inside* an ADB channel: the channel carries a stream
 * of (id, length) frames, and each frame may contain DATA / FAIL / a struct.
 * Sending (push) is deliberately not implemented yet: it would be a root file
 * write capability with no authorization layer in front of it.
 */
#ifndef ADBNG_SYNC_H
#define ADBNG_SYNC_H

#include <stddef.h>
#include <stdint.h>

struct sync_state;

struct sync_state *sync_new(void);
void sync_free(struct sync_state *s);

/* Bytes received from the host (already unwrapped from WRTE packets). */
int sync_feed(struct sync_state *s, const unsigned char *data, size_t len);

/* Generate more output: reads files, appends DATA/DONE frames. */
void sync_pump(struct sync_state *s);

size_t sync_pending(const struct sync_state *s);
size_t sync_take(struct sync_state *s, unsigned char *buf, size_t max);

/* QUIT seen and nothing left to send. */
int sync_finished(const struct sync_state *s);

#endif