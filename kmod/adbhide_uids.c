// SPDX-License-Identifier: MIT
//
// Target uid list and hit counters.
//
// The list can arrive two ways: as the `target_uids=` module parameter at
// load time, or later through /proc/adbhide_uids (written by sync.sh after it
// resolves package names). Readers run inside kprobes, so the list is guarded
// by a spinlock instead of being replaced in place.

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include "adbhide.h"

static u32 targets[ADBHIDE_MAX_UIDS];
static int target_count;
static u32 counters[CNT_MAX];
static DEFINE_SPINLOCK(targets_lock);

static int adbhide_set_targets(const u32 *list, int n)
{
	unsigned long flags;

	if (n < 0)
		n = 0;
	if (n > ADBHIDE_MAX_UIDS)
		n = ADBHIDE_MAX_UIDS;

	spin_lock_irqsave(&targets_lock, flags);
	memcpy(targets, list, n * sizeof(u32));
	target_count = n;
	spin_unlock_irqrestore(&targets_lock, flags);
	return n;
}

int adbhide_parse_targets(const char *spec)
{
	char buf[512];
	char *p, *tok;
	u32 list[ADBHIDE_MAX_UIDS];
	int n = 0;

	if (!spec || !*spec)
		return adbhide_set_targets(NULL, 0);

	strscpy(buf, spec, sizeof(buf));

	p = buf;
	while ((tok = strsep(&p, ",")) != NULL && n < ADBHIDE_MAX_UIDS) {
		u32 v;

		tok = strim(tok);
		if (!*tok)
			continue;
		if (kstrtou32(tok, 10, &v))
			continue;
		if (v < 10000 || v > 99999)
			continue;
		list[n++] = v;
	}

	return adbhide_set_targets(list, n);
}

int adbhide_target_count(void)
{
	unsigned long flags;
	int n;

	spin_lock_irqsave(&targets_lock, flags);
	n = target_count;
	spin_unlock_irqrestore(&targets_lock, flags);
	return n;
}

u32 adbhide_target_at(int idx)
{
	unsigned long flags;
	u32 v = 0;

	spin_lock_irqsave(&targets_lock, flags);
	if (idx >= 0 && idx < target_count)
		v = targets[idx];
	spin_unlock_irqrestore(&targets_lock, flags);
	return v;
}

int adbhide_uid_index(u32 uid)
{
	unsigned long flags;
	int i, found = -1;

	spin_lock_irqsave(&targets_lock, flags);
	for (i = 0; i < target_count; i++) {
		if (targets[i] == uid) {
			found = i;
			break;
		}
	}
	spin_unlock_irqrestore(&targets_lock, flags);
	return found;
}

bool adbhide_uid_is_target(u32 uid)
{
	return adbhide_uid_index(uid) >= 0;
}

u32 adbhide_uid_now(void)
{
	return __kuid_val(current_uid());
}

void adbhide_counter_bump(enum adbhide_counter c)
{
	if (c < CNT_MAX)
		counters[c]++;
}

u32 adbhide_counter_get(enum adbhide_counter c)
{
	return c < CNT_MAX ? counters[c] : 0;
}
