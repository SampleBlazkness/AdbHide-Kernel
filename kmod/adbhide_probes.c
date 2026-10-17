// SPDX-License-Identifier: MIT
//
// M2: read-only probes.
//
// Two questions this build answers without changing any behaviour:
//
//   vfs_open       which target uids open files under /dev/__properties__,
//                  and which context file they touch
//   seq_read       who reads /proc/net/tcp and /proc/net/tcp6
//
// Both handlers return 0, never touch registers or return values, and bail out
// on the first uid comparison when the caller is not a target.

#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include "adbhide.h"

static struct kprobe kp_vfs_open = { .symbol_name = "vfs_open" };
static struct kprobe kp_seq_read = { .symbol_name = "seq_read" };

bool adbhide_probe_ok[2];
const char *const adbhide_probe_names[2] = { "vfs_open", "seq_read" };

/* one "first tcp read" event per target uid keeps the ring from flooding */
static bool seq_seen[ADBHIDE_MAX_UIDS];

static bool dentry_name_is(const struct dentry *d, const char *s)
{
	size_t n;

	if (!d)
		return false;
	n = strlen(s);
	return d->d_name.len == n && memcmp(d->d_name.name, s, n) == 0;
}

static int h_vfs_open(struct kprobe *p, struct pt_regs *regs)
{
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);
	struct dentry *d;
	u32 uid;

	if (!path || !path->dentry)
		return 0;

	uid = adbhide_uid_now();
	if (!adbhide_uid_is_target(uid))
		return 0;

	d = path->dentry;
	if (!d->d_parent || !dentry_name_is(d->d_parent, "__properties__"))
		return 0;

	{
		/* d_name is not NUL terminated; copy it out first */
		char name[28];
		size_t len = min_t(size_t, d->d_name.len, sizeof(name) - 1);

		memcpy(name, d->d_name.name, len);
		name[len] = '\0';
		adbhide_counter_bump(CNT_PROP_OPEN);
		adbhide_event_add("prop", name);
	}
	return 0;
}

static int h_seq_read(struct kprobe *p, struct pt_regs *regs)
{
	struct file *f = (struct file *)regs_get_kernel_argument(regs, 0);
	u32 uid;
	int idx;
	bool tcp, tcp6;

	if (!f || !f->f_path.dentry)
		return 0;

	uid = adbhide_uid_now();
	if (!adbhide_uid_is_target(uid))
		return 0;

	tcp = dentry_name_is(f->f_path.dentry, "tcp");
	tcp6 = dentry_name_is(f->f_path.dentry, "tcp6");
	if (!tcp && !tcp6)
		return 0;

	adbhide_counter_bump(tcp ? CNT_TCP_READ : CNT_TCP6_READ);

	idx = adbhide_uid_index(uid);
	if (idx >= 0 && !seq_seen[idx]) {
		seq_seen[idx] = true;
		adbhide_event_add(tcp ? "read" : "read6", tcp ? "tcp" : "tcp6");
	}
	return 0;
}

static void try_register(struct kprobe *kp, int idx)
{
	int ret = register_kprobe(kp);

	adbhide_probe_ok[idx] = (ret == 0);
	if (ret)
		pr_warn("adbhide_kmod: kprobe %s failed: %d\n",
			adbhide_probe_names[idx], ret);
	else
		pr_info("adbhide_kmod: kprobe %s armed\n", adbhide_probe_names[idx]);
}

int adbhide_probes_register(void)
{
	kp_vfs_open.pre_handler = h_vfs_open;
	kp_seq_read.pre_handler = h_seq_read;

	try_register(&kp_vfs_open, 0);
	try_register(&kp_seq_read, 1);
	return 0;
}

void adbhide_probes_unregister(void)
{
	if (adbhide_probe_ok[0]) {
		unregister_kprobe(&kp_vfs_open);
		adbhide_probe_ok[0] = false;
	}
	if (adbhide_probe_ok[1]) {
		unregister_kprobe(&kp_seq_read);
		adbhide_probe_ok[1] = false;
	}
}
