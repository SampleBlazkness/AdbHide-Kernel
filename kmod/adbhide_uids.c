// SPDX-License-Identifier: MIT
//
// Target uid list and hit counters. The list arrives as a module parameter
// built by post-fs-data.sh from /data/adb/adbhide/uids.txt; the kernel never
// reads files, so SELinux and kernel-side file IO stay out of the picture.

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "adbhide.h"

static u32 targets[ADBHIDE_MAX_UIDS];
static int target_count;
static u32 counters[CNT_MAX];

int adbhide_parse_targets(const char *spec)
{
	char buf[512];
	char *p, *tok;
	int n = 0;

	target_count = 0;
	if (!spec || !*spec)
		return 0;

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
		targets[n++] = v;
	}

	target_count = n;
	return n;
}

int adbhide_target_count(void)
{
	return target_count;
}

u32 adbhide_target_at(int idx)
{
	if (idx < 0 || idx >= target_count)
		return 0;
	return targets[idx];
}

int adbhide_uid_index(u32 uid)
{
	int i;

	for (i = 0; i < target_count; i++) {
		if (targets[i] == uid)
			return i;
	}
	return -1;
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
