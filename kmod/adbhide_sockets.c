// SPDX-License-Identifier: MIT
//
// M6: close the channels that bypass property spoofing.
//
// Four leaks survived M5:
//
//   /dev/socket/adbd     the socket file exists while init.svc.adbd says
//                        stopped. open() and stat() still reach it and
//                        access() returns EACCES instead of ENOENT.
//   @jdwp-control        abstract socket stays listable and connectable.
//   /proc/net/unix       prints both of the above by name.
//   /proc/modules        prints adbhide_kmod itself.
//
// Sequence output is filtered like netseq: remember seq->count on entry and
// restore it on return when the appended row names adbd, jdwp or our module.
// Path lookups are failed with -ENOENT for target uids only, so the owner of
// the phone keeps working adb.

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/workqueue.h>
#include <linux/socket.h>
#include <linux/un.h>
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/jiffies.h>
#include <linux/net.h>
#include <linux/string.h>
#include <net/sock.h>
#include "adbhide.h"

static unsigned long adbd_ino;
static struct super_block *adbd_sb;
static struct delayed_work adbd_resolve_work;

static bool name_is(const char *s, size_t len, const char *want)
{
	size_t n = strlen(want);

	return len == n && memcmp(s, want, n) == 0;
}

/* ---------------------------------------------------------------- adbd file */

struct open_data {
	bool hit;
};

static int vfs_open_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct open_data *d = (struct open_data *)ri->data;
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);

	d->hit = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!path || !path->dentry || !path->dentry->d_parent)
		return 0;
	if (!name_is(path->dentry->d_name.name, path->dentry->d_name.len, "adbd"))
		return 0;
	if (!name_is(path->dentry->d_parent->d_name.name,
		     path->dentry->d_parent->d_name.len, "socket"))
		return 0;
	d->hit = true;
	return 0;
}

static int vfs_open_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct open_data *d = (struct open_data *)ri->data;

	if (d->hit && (long)regs_return_value(regs) < 0) {
		regs_set_return_value(regs, -ENOENT);
		adbhide_counter_bump(CNT_SOCK_HIDDEN);
		adbhide_event_add("sock", "adbd");
	}
	return 0;
}

static struct kretprobe vfs_open_krp = {
	.kp.symbol_name = "vfs_open",
	.entry_handler = vfs_open_entry,
	.handler = vfs_open_ret,
	.data_size = sizeof(struct open_data),
	.maxactive = 64,
};

struct stat_data {
	bool hit;
};

static int vfs_statx_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct stat_data *d = (struct stat_data *)ri->data;
	struct filename *fn = (struct filename *)regs_get_kernel_argument(regs, 1);

	d->hit = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!fn || !fn->name)
		return 0;
	if (strcmp(fn->name, "/dev/socket/adbd"))
		return 0;
	d->hit = true;
	return 0;
}

static int vfs_statx_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct stat_data *d = (struct stat_data *)ri->data;

	if (d->hit) {
		regs_set_return_value(regs, -ENOENT);
		adbhide_counter_bump(CNT_SOCK_HIDDEN);
	}
	return 0;
}

static struct kretprobe vfs_statx_krp = {
	.kp.symbol_name = "vfs_statx",
	.entry_handler = vfs_statx_entry,
	.handler = vfs_statx_ret,
	.data_size = sizeof(struct stat_data),
	.maxactive = 64,
};

/* access() goes through inode_permission; SELinux answers EACCES and the
 * detector reads that as "file exists". Turn it into ENOENT for the socket
 * inode only. */
struct perm_data {
	bool hit;
};

static int inode_permission_entry(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	struct perm_data *d = (struct perm_data *)ri->data;
	struct inode *inode = (struct inode *)regs_get_kernel_argument(regs, 1);

	d->hit = false;
	if (!adbd_ino || !inode)
		return 0;
	if (!adbhide_target_count() || !adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!S_ISSOCK(inode->i_mode) || inode->i_ino != adbd_ino)
		return 0;
	if (adbd_sb && inode->i_sb != adbd_sb)
		return 0;
	d->hit = true;
	return 0;
}

static int inode_permission_ret(struct kretprobe_instance *ri,
				struct pt_regs *regs)
{
	struct perm_data *d = (struct perm_data *)ri->data;

	if (d->hit && (long)regs_return_value(regs) == -EACCES) {
		regs_set_return_value(regs, -ENOENT);
		adbhide_counter_bump(CNT_SOCK_HIDDEN);
	}
	return 0;
}

static struct kretprobe inode_permission_krp = {
	.kp.symbol_name = "inode_permission",
	.entry_handler = inode_permission_entry,
	.handler = inode_permission_ret,
	.data_size = sizeof(struct perm_data),
	.maxactive = 64,
};

/* ------------------------------------------------------------------ seq过滤 */

struct seq_data {
	struct seq_file *seq;
	size_t saved;
	bool hit;
};

static bool seq_has(struct seq_file *seq, size_t from, const char *needle)
{
	size_t n = strlen(needle);
	size_t i;

	if (!seq || !seq->buf || seq->count <= from)
		return false;
	if (seq->count - from < n)
		return false;
	for (i = from; i + n <= seq->count; i++)
		if (!memcmp(seq->buf + i, needle, n))
			return true;
	return false;
}

static int seq_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct seq_data *d = (struct seq_data *)ri->data;

	d->hit = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	d->seq = (struct seq_file *)regs_get_kernel_argument(regs, 0);
	d->saved = d->seq ? d->seq->count : 0;
	d->hit = d->seq != NULL;
	return 0;
}

static int unix_seq_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct seq_data *d = (struct seq_data *)ri->data;

	if (!d->hit)
		return 0;
	if (seq_has(d->seq, d->saved, "/adbd") ||
	    seq_has(d->seq, d->saved, "jdwp-control")) {
		d->seq->count = d->saved;
		adbhide_counter_bump(CNT_UNIX_HIDDEN);
		adbhide_event_add("unix", "adbd/jdwp");
	}
	return 0;
}

static struct kretprobe unix_seq_krp = {
	.kp.symbol_name = "unix_seq_show",
	.entry_handler = seq_entry,
	.handler = unix_seq_ret,
	.data_size = sizeof(struct seq_data),
	.maxactive = 64,
};

static int mod_seq_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct seq_data *d = (struct seq_data *)ri->data;

	if (!d->hit)
		return 0;
	if (seq_has(d->seq, d->saved, "adbhide")) {
		d->seq->count = d->saved;
		adbhide_counter_bump(CNT_MOD_HIDDEN);
	}
	return 0;
}

static struct kretprobe mod_seq_krp = {
	.kp.symbol_name = "m_show",
	.entry_handler = seq_entry,
	.handler = mod_seq_ret,
	.data_size = sizeof(struct seq_data),
	.maxactive = 64,
};

/* ------------------------------------------------------- @jdwp-control link */

struct conn_data {
	bool hit;
	bool adbd;
};

static bool is_jdwp(struct sockaddr *addr, int addr_len)
{
	struct sockaddr_un *un = (struct sockaddr_un *)addr;

	if (!addr || addr->sa_family != AF_UNIX)
		return false;
	if (addr_len <= offsetof(struct sockaddr_un, sun_path))
		return false;
	if (un->sun_path[0] != '\0')
		return false;
	if (addr_len < offsetof(struct sockaddr_un, sun_path) + 13)
		return false;
	return !memcmp(un->sun_path + 1, "jdwp-control", 12);
}

/* The adbd socket is a filesystem path, not an abstract name. */
static bool is_adbd_sock(struct sockaddr *addr, int addr_len)
{
	struct sockaddr_un *un = (struct sockaddr_un *)addr;

	if (!addr || addr->sa_family != AF_UNIX)
		return false;
	if (addr_len <= offsetof(struct sockaddr_un, sun_path))
		return false;
	if (un->sun_path[0] != '/')
		return false;
	if (addr_len < offsetof(struct sockaddr_un, sun_path) + 16)
		return false;
	return !memcmp(un->sun_path, "/dev/socket/adbd", 16);
}

static int connect_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct conn_data *d = (struct conn_data *)ri->data;
	struct sockaddr *addr = (struct sockaddr *)regs_get_kernel_argument(regs, 1);
	int len = (int)regs_get_kernel_argument(regs, 2);

	d->hit = false;
	d->adbd = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (is_jdwp(addr, len)) {
		d->hit = true;
	} else if (is_adbd_sock(addr, len)) {
		d->hit = true;
		d->adbd = true;
	}
	return 0;
}

static int connect_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct conn_data *d = (struct conn_data *)ri->data;

	long ret = (long)regs_return_value(regs);

	if (d->hit && (ret == 0 || (d->adbd && ret < 0))) {
		regs_set_return_value(regs, d->adbd ? -ENOENT : -ECONNREFUSED);
		adbhide_counter_bump(CNT_UNIX_HIDDEN);
		adbhide_event_add("unix", d->adbd ? "adbd-connect" : "jdwp-connect");
	}
	return 0;
}

static struct kretprobe stream_connect_krp = {
	.kp.symbol_name = "unix_stream_connect",
	.entry_handler = connect_entry,
	.handler = connect_ret,
	.data_size = sizeof(struct conn_data),
	.maxactive = 32,
};

static struct kretprobe dgram_connect_krp = {
	.kp.symbol_name = "unix_dgram_connect",
	.entry_handler = connect_entry,
	.handler = connect_ret,
	.data_size = sizeof(struct conn_data),
	.maxactive = 32,
};

/* ------------------------------------------------------------------ 生命周期 */

static void adbd_resolve(struct work_struct *w)
{
	static int tries;
	struct path p;

	if (!kern_path("/dev/socket/adbd", LOOKUP_FOLLOW, &p)) {
		struct inode *inode = d_backing_inode(p.dentry);

		if (inode) {
			adbd_ino = inode->i_ino;
			adbd_sb = inode->i_sb;
			pr_info("adbhide_kmod: adbd socket inode %lu\n", adbd_ino);
		}
		path_put(&p);
		return;
	}
	if (++tries < 30)
		schedule_delayed_work(&adbd_resolve_work, 2 * HZ);
}

static bool sock_active;

int adbhide_sockets_init(void)
{
	int r;

	sock_active = true;
	INIT_DELAYED_WORK(&adbd_resolve_work, adbd_resolve);
	schedule_delayed_work(&adbd_resolve_work, 0);

	r = register_kretprobe(&vfs_open_krp);
	if (r) {
		pr_warn("adbhide_kmod: vfs_open probe failed: %d\n", r);
		sock_active = false;
	}
	r = register_kretprobe(&vfs_statx_krp);
	if (r)
		pr_warn("adbhide_kmod: vfs_statx probe failed: %d\n", r);
	r = register_kretprobe(&inode_permission_krp);
	if (r)
		pr_warn("adbhide_kmod: inode_permission probe failed: %d\n", r);
	r = register_kretprobe(&unix_seq_krp);
	if (r)
		pr_warn("adbhide_kmod: unix_seq_show probe failed: %d\n", r);
	r = register_kretprobe(&mod_seq_krp);
	if (r)
		pr_warn("adbhide_kmod: m_show probe failed: %d\n", r);
	r = register_kretprobe(&stream_connect_krp);
	if (r)
		pr_warn("adbhide_kmod: unix_stream_connect probe failed: %d\n", r);
	r = register_kretprobe(&dgram_connect_krp);
	if (r)
		pr_warn("adbhide_kmod: unix_dgram_connect probe failed: %d\n", r);

	pr_info("adbhide_kmod: socket hiding %s\n", sock_active ? "on" : "off");
	return sock_active ? 0 : -ENOENT;
}

void adbhide_sockets_exit(void)
{
	if (!sock_active)
		return;
	cancel_delayed_work_sync(&adbd_resolve_work);
	if (vfs_open_krp.kp.addr)
		unregister_kretprobe(&vfs_open_krp);
	if (vfs_statx_krp.kp.addr)
		unregister_kretprobe(&vfs_statx_krp);
	if (inode_permission_krp.kp.addr)
		unregister_kretprobe(&inode_permission_krp);
	if (unix_seq_krp.kp.addr)
		unregister_kretprobe(&unix_seq_krp);
	if (mod_seq_krp.kp.addr)
		unregister_kretprobe(&mod_seq_krp);
	if (stream_connect_krp.kp.addr)
		unregister_kretprobe(&stream_connect_krp);
	if (dgram_connect_krp.kp.addr)
		unregister_kretprobe(&dgram_connect_krp);
	sock_active = false;
}

int adbhide_sockets_active(void)
{
	return sock_active;
}
