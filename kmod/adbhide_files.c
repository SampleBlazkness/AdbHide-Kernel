// SPDX-License-Identifier: MIT
//
// M5 file layer: /data/misc/adb/adb_keys must look absent to target uids.
//
// The file is 0600 root:shell, so apps already get EACCES from SELinux and
// DAC. Detectors treat "permission denied" as "exists", so the answer is
// rewritten to ENOENT. vfs_getattr() sees the resolved path, which makes the
// check precise; the return value is changed after the call, nothing else.

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include "adbhide.h"

#define ADB_DIR "adb"
#define ADB_KEYS "adb_keys"

struct file_probe_data {
	bool match;
};

static bool adb_keys_path(const struct path *path)
{
	const struct dentry *dent;
	const struct dentry *parent;

	if (!path || !path->dentry)
		return false;
	dent = path->dentry;
	parent = dent->d_parent;
	if (!parent)
		return false;

	if (dent->d_name.len != sizeof(ADB_KEYS) - 1 ||
	    memcmp(dent->d_name.name, ADB_KEYS, sizeof(ADB_KEYS) - 1))
		return false;
	if (parent->d_name.len != sizeof(ADB_DIR) - 1 ||
	    memcmp(parent->d_name.name, ADB_DIR, sizeof(ADB_DIR) - 1))
		return false;
	return true;
}

static int getattr_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct file_probe_data *d = (struct file_probe_data *)ri->data;
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);

	d->match = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!adb_keys_path(path))
		return 0;

	d->match = true;
	return 0;
}

static int getattr_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct file_probe_data *d = (struct file_probe_data *)ri->data;
	long ret = regs_return_value(regs);

	if (!d->match)
		return 0;
	if (ret == 0 || ret == -EACCES || ret == -EPERM) {
		regs_set_return_value(regs, -ENOENT);
		adbhide_counter_bump(CNT_FILE_HIDDEN);
		adbhide_event_add("filehide", ADB_KEYS);
	}
	return 0;
}

static struct kretprobe getattr_krp = {
	.kp.symbol_name = "vfs_getattr",
	.entry_handler = getattr_entry,
	.handler = getattr_ret,
	.data_size = sizeof(struct file_probe_data),
	.maxactive = 64,
};

static bool files_active;

int adbhide_files_init(void)
{
	if (!adbhide_target_count())
		return 0;
	if (register_kretprobe(&getattr_krp)) {
		pr_warn("adbhide_kmod: vfs_getattr probe failed\n");
		return -ENOENT;
	}
	files_active = true;
	pr_info("adbhide_kmod: file layer armed (adb_keys -> ENOENT)\n");
	return 0;
}

void adbhide_files_exit(void)
{
	if (!files_active)
		return;
	unregister_kretprobe(&getattr_krp);
	files_active = false;
}
