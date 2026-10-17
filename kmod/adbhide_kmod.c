// SPDX-License-Identifier: MIT
//
// AdbHide-Kernel — kernel-level ADB hiding.
//
// M1 scope: load, expose /proc/adbhide_kmod, unload. No hooks yet, so this
// build cannot change any behaviour and cannot crash anything but itself.

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/version.h>

#define ADBHIDE_KVER "0.1.0"
#define ADBHIDE_UID_FILE "/data/adb/adbhide/uids.txt"

static int adbhide_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "adbhide_kmod %s\n", ADBHIDE_KVER);
	seq_printf(m, "kernel %s\n", UTS_RELEASE);
	seq_printf(m, "uid_file %s\n", ADBHIDE_UID_FILE);
	seq_puts(m, "mode probe\n");
	return 0;
}

static int adbhide_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, adbhide_proc_show, NULL);
}

static const struct proc_ops adbhide_proc_ops = {
	.proc_open = adbhide_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int __init adbhide_init(void)
{
	if (!proc_create("adbhide_kmod", 0444, NULL, &adbhide_proc_ops))
		pr_warn("adbhide_kmod: /proc entry not created\n");

	pr_info("adbhide_kmod %s loaded\n", ADBHIDE_KVER);
	return 0;
}

static void __exit adbhide_exit(void)
{
	remove_proc_entry("adbhide_kmod", NULL);
	pr_info("adbhide_kmod unloaded\n");
}

module_init(adbhide_init);
module_exit(adbhide_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Blazkness");
MODULE_DESCRIPTION("Kernel-level ADB hiding for selected apps");
MODULE_VERSION(ADBHIDE_KVER);
