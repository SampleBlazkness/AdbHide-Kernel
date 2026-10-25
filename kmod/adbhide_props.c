// SPDX-License-Identifier: MIT
//
// M3: property-area redirect, all adb-relevant contexts.
//
// Target apps read the property area through `getprop` child processes, which
// mmap /dev/__properties__/<context>. The redirect therefore happens at the
// file layer: a kretprobe on vfs_open swaps file->f_op for target uids, and
// every later read()/mmap() serves a patched copy from kernel memory. The real
// file is untouched; the process's maps still show the same path.
//
// Snapshots are taken and refreshed by a delayed work; a kretprobe handler must
// never do I/O (preemption is disabled there).
//
// Contexts covered (file names are SELinux contexts, not short names):
//   usb_control_prop          sys.usb.config / sys.usb.state
//   system_prop               persist.sys.usb.config, ro.debuggable, ro.secure,
//                             ro.adb.secure, service.adb.root
//   init_service_status_prop  init.svc.adbd
//   usb_config_prop           sys.usb.config (vendor duplicate)

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/kprobes.h>
#include <linux/dcache.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include "adbhide.h"

#define PROP_DIR "__properties__"
#define BLOB_TTL (5 * HZ)

static int redirect;
module_param(redirect, int, 0444);
MODULE_PARM_DESC(redirect, "1 = serve patched property files to target uids");

struct ab_key {
	const char *name;
	const char *value;
};

struct ab_spec {
	const char *file;
	const struct ab_key *keys;
	unsigned int nkeys;
};

static const struct ab_key keys_usb[] = {
	{ "sys.usb.config", "mtp" },
	{ "sys.usb.state", "mtp" },
};

static const struct ab_key keys_sys[] = {
	{ "persist.sys.usb.config", "mtp" },
	{ "ro.debuggable", "0" },
	{ "ro.secure", "1" },
	{ "ro.adb.secure", "1" },
	{ "service.adb.root", "0" },
};

static const struct ab_key keys_init[] = {
	{ "init.svc.adbd", "stopped" },
};

static const struct ab_key keys_usbcfg[] = {
	{ "sys.usb.config", "mtp" },
};

static const struct ab_spec g_specs[] = {
	{ "u:object_r:usb_control_prop:s0", keys_usb, ARRAY_SIZE(keys_usb) },
	{ "u:object_r:system_prop:s0", keys_sys, ARRAY_SIZE(keys_sys) },
	{ "u:object_r:init_service_status_prop:s0", keys_init, ARRAY_SIZE(keys_init) },
	{ "u:object_r:usb_config_prop:s0", keys_usbcfg, ARRAY_SIZE(keys_usbcfg) },
};

#define AB_NCTX ARRAY_SIZE(g_specs)

struct ab_blob {
	struct kref ref;
	size_t len;
	u8 data[];
};

static struct ab_blob *g_blob[AB_NCTX];
static DEFINE_MUTEX(blob_lock);
static int g_redirect_state;

/* ---------- property area parsing (bionic layout) ---------- */

#define PROP_AREA_HDR  128
#define PROP_TRIE_NAME 20
#define PROP_INFO_VAL  4
#define PROP_VALUE_MAX 92

static inline u32 rd32(const void *p)
{
	return *(const u32 *)p;
}

static int cmp_prop_name(const char *one, u32 one_len, const char *two, u32 two_len)
{
	if (one_len < two_len)
		return -1;
	if (one_len > two_len)
		return 1;
	return strncmp(one, two, one_len);
}

/* root is a dummy node: the first token lives in root->children */
static const u8 *find_prop_info(const u8 *data, const char *name)
{
	const char *remaining = name;
	const u8 *cur = data;

	for (;;) {
		const char *sep = strchr(remaining, '.');
		u32 tok_len = sep ? (u32)(sep - remaining) : (u32)strlen(remaining);
		const u8 *node, *hit = NULL;
		u32 off;

		if (!tok_len)
			return NULL;
		off = rd32(cur + 16);
		if (!off)
			return NULL;

		node = data + off;
		while (node) {
			u32 nl = rd32(node);
			int ret = cmp_prop_name(remaining, tok_len,
						(const char *)(node + PROP_TRIE_NAME), nl);

			if (ret == 0) {
				hit = node;
				break;
			}
			off = rd32(node + (ret < 0 ? 8 : 12));
			node = off ? data + off : NULL;
		}
		if (!hit)
			return NULL;

		if (!sep) {
			off = rd32(hit + 4);
			return off ? data + off : NULL;
		}
		cur = hit;
		remaining = sep + 1;
	}
}

static bool patch_value(u8 *area, size_t len, const char *name, const char *value)
{
	u8 *info;

	if (len < PROP_AREA_HDR + 8)
		return false;
	if (rd32(area + 8) != 0x504f5250)
		return false;

	info = (u8 *)find_prop_info(area + PROP_AREA_HDR, name);
	if (!info)
		return false;
	if ((size_t)(info - area) + PROP_INFO_VAL + PROP_VALUE_MAX > len)
		return false;

	memset(info + PROP_INFO_VAL, 0, PROP_VALUE_MAX);
	strscpy((char *)(info + PROP_INFO_VAL), value, PROP_VALUE_MAX);
	return true;
}

/* ---------- blob ---------- */

static void blob_release(struct kref *ref)
{
	struct ab_blob *b = container_of(ref, struct ab_blob, ref);

	kfree(b);
}

static ssize_t read_prop_file(struct file *f, u8 *dst, size_t cap)
{
	struct address_space *mapping = f->f_mapping;
	size_t done = 0;
	pgoff_t idx = 0;

	if (!mapping)
		return -EINVAL;

	while (done < cap) {
		struct page *pg = find_get_page(mapping, idx);
		size_t n = min_t(size_t, PAGE_SIZE, cap - done);

		if (!pg) {
			memset(dst + done, 0, n);
		} else {
			void *k = adbhide_kmap_local(pg);

			memcpy(dst + done, k, n);
			adbhide_kunmap_local(k);
			put_page(pg);
		}
		done += n;
		idx++;
	}
	return done;
}

static struct ab_blob *load_prop_blob(const struct ab_spec *spec)
{
	char path[96];
	struct file *f;
	struct ab_blob *b;
	ssize_t n;
	size_t cap = 128 * 1024;
	unsigned int i, hit = 0;

	snprintf(path, sizeof(path), "/dev/%s/%s", PROP_DIR, spec->file);
	f = filp_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (IS_ERR(f))
		return ERR_CAST(f);

	b = kzalloc(sizeof(*b) + cap, GFP_KERNEL);
	if (!b) {
		filp_close(f, NULL);
		return ERR_PTR(-ENOMEM);
	}
	kref_init(&b->ref);

	n = read_prop_file(f, b->data, cap);
	filp_close(f, NULL);
	if (n <= 0) {
		kfree(b);
		return ERR_PTR(n ? (long)n : -EIO);
	}
	b->len = n;

	for (i = 0; i < spec->nkeys; i++) {
		if (patch_value(b->data, b->len, spec->keys[i].name,
				spec->keys[i].value))
			hit++;
	}
	pr_info("adbhide_kmod: %s: %zu bytes, %u/%u keys patched\n",
		spec->file, b->len, hit, spec->nkeys);
	return b;
}

/* ---------- fake file_operations ---------- */

static struct ab_blob *blob_get(int idx)
{
	struct ab_blob *b = NULL;

	if (idx < 0 || idx >= AB_NCTX)
		return NULL;
	mutex_lock(&blob_lock);
	if (g_blob[idx]) {
		kref_get(&g_blob[idx]->ref);
		b = g_blob[idx];
	}
	mutex_unlock(&blob_lock);
	return b;
}

static ssize_t ab_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct ab_blob *b = blob_get((int)(long)file->private_data - 1);
	ssize_t ret = 0;

	if (!b)
		return -EIO;
	if (*ppos >= 0 && (size_t)*ppos < b->len) {
		size_t left = b->len - (size_t)*ppos;

		if (count > left)
			count = left;
		if (copy_to_user(buf, b->data + *ppos, count))
			ret = -EFAULT;
		else {
			*ppos += count;
			ret = count;
		}
	}
	kref_put(&b->ref, blob_release);
	return ret;
}

static ssize_t ab_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct ab_blob *b = blob_get((int)(long)iocb->ki_filp->private_data - 1);
	ssize_t ret = 0;

	if (!b)
		return -EIO;
	if (iocb->ki_pos >= 0 && (size_t)iocb->ki_pos < b->len) {
		size_t left = b->len - (size_t)iocb->ki_pos;

		ret = copy_to_iter(b->data + iocb->ki_pos,
				   min(left, iov_iter_count(to)), to);
	}
	kref_put(&b->ref, blob_release);
	return ret;
}

static vm_fault_t ab_fault(struct vm_fault *vmf)
{
	struct ab_blob *b = vmf->vma->vm_private_data;
	unsigned long off = vmf->pgoff << PAGE_SHIFT;
	struct page *page;
	void *kaddr;
	size_t n = 0;

	if (!b)
		return VM_FAULT_SIGBUS;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return VM_FAULT_OOM;

	if (off < b->len) {
		n = min_t(size_t, PAGE_SIZE, b->len - off);
		kaddr = adbhide_kmap_local(page);
		memcpy(kaddr, b->data + off, n);
		adbhide_kunmap_local(kaddr);
	}
	vmf->page = page;
	return 0;
}

static void ab_vm_open(struct vm_area_struct *vma)
{
	struct ab_blob *b = vma->vm_private_data;

	if (b)
		kref_get(&b->ref);
}

static void ab_vm_close(struct vm_area_struct *vma)
{
	struct ab_blob *b = vma->vm_private_data;

	if (b) {
		vma->vm_private_data = NULL;
		kref_put(&b->ref, blob_release);
	}
}

static const struct vm_operations_struct ab_vm_ops = {
	.fault = ab_fault,
	.open = ab_vm_open,
	.close = ab_vm_close,
};

static int ab_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ab_blob *b = blob_get((int)(long)file->private_data - 1);

	if (!b)
		return -EIO;
	if (vma->vm_pgoff) {
		kref_put(&b->ref, blob_release);
		return -EINVAL;
	}

	vma->vm_private_data = b;
	vma->vm_ops = &ab_vm_ops;
	adbhide_vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}

static int ab_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static const struct file_operations ab_fops = {
	.llseek = default_llseek,
	.read = ab_read,
	.read_iter = ab_read_iter,
	.mmap = ab_mmap,
	.release = ab_release,
};

/* ---------- vfs_open kretprobe ---------- */

struct ab_open_data {
	bool wanted;
	int idx;
	struct file *file;
};

static int ab_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);
	struct ab_open_data *d = (struct ab_open_data *)ri->data;
	struct dentry *dent;
	unsigned int i;

	d->wanted = false;
	d->idx = -1;
	d->file = (struct file *)regs_get_kernel_argument(regs, 1);

	if (!redirect || !path || !path->dentry)
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;

	dent = path->dentry;
	if (!dent->d_parent)
		return 0;
	if (dent->d_parent->d_name.len != sizeof(PROP_DIR) - 1 ||
	    memcmp(dent->d_parent->d_name.name, PROP_DIR, sizeof(PROP_DIR) - 1))
		return 0;

	for (i = 0; i < AB_NCTX; i++) {
		size_t len = strlen(g_specs[i].file);

		if (dent->d_name.len == len &&
		    memcmp(dent->d_name.name, g_specs[i].file, len) == 0) {
			d->wanted = true;
			d->idx = (int)i;
			break;
		}
	}
	return 0;
}

static int ab_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ab_open_data *d;
	struct file *file;
	long ret = regs_return_value(regs);

	if (!ri)
		return 0;
	d = (struct ab_open_data *)ri->data;
	if (!d->wanted || d->idx < 0 || ret != 0)
		return 0;

	file = d->file;
	if (!file)
		return 0;

	file->f_op = &ab_fops;
	file->private_data = (void *)(long)(d->idx + 1);
	adbhide_counter_bump(CNT_REDIRECT);
	adbhide_event_add("redir", g_specs[d->idx].file);
	return 0;
}

static struct kretprobe ab_krp = {
	.kp.symbol_name = "vfs_open",
	.entry_handler = ab_entry,
	.handler = ab_ret,
	.data_size = sizeof(struct ab_open_data),
	.maxactive = 64,
};

static void blob_work_fn(struct work_struct *w)
{
	unsigned int i;
	int loaded = 0;

	for (i = 0; i < AB_NCTX; i++) {
		struct ab_blob *nb = load_prop_blob(&g_specs[i]);
		struct ab_blob *old;

		if (IS_ERR(nb))
			continue;
		mutex_lock(&blob_lock);
		old = g_blob[i];
		g_blob[i] = nb;
		mutex_unlock(&blob_lock);
		if (old)
			kref_put(&old->ref, blob_release);
		loaded++;
	}
	if (!loaded)
		pr_warn_ratelimited("adbhide_kmod: no property snapshot loaded\n");
	schedule_delayed_work(to_delayed_work(w), BLOB_TTL);
}

static DECLARE_DELAYED_WORK(blob_work, blob_work_fn);

int adbhide_props_init(void)
{
	int ret;

	if (!redirect) {
		pr_info("adbhide_kmod: redirect disabled\n");
		return 0;
	}

	ret = register_kretprobe(&ab_krp);
	if (ret) {
		pr_warn("adbhide_kmod: kretprobe vfs_open failed: %d\n", ret);
		return 0;
	}

	g_redirect_state = 1;
	schedule_delayed_work(&blob_work, 6 * HZ);
	pr_info("adbhide_kmod: redirect armed on %u contexts\n", (unsigned int)AB_NCTX);
	return 0;
}

void adbhide_props_exit(void)
{
	unsigned int i;

	if (g_redirect_state) {
		unregister_kretprobe(&ab_krp);
		g_redirect_state = 0;
	}
	cancel_delayed_work_sync(&blob_work);
	for (i = 0; i < AB_NCTX; i++) {
		struct ab_blob *old;

		mutex_lock(&blob_lock);
		old = g_blob[i];
		g_blob[i] = NULL;
		mutex_unlock(&blob_lock);
		if (old)
			kref_put(&old->ref, blob_release);
	}
}

int adbhide_redirect_active(void)
{
	return g_redirect_state;
}
