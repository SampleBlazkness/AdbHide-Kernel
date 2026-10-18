// SPDX-License-Identifier: MIT
//
// M3: property-area redirect.
//
// Target apps read the property area through `getprop` child processes, which
// mmap /dev/__properties__/<context>. Changing the in-process view is useless
// there, so the redirect happens at the file layer:
//
//   kretprobe vfs_open -> if a target uid just opened a property file, swap
//   file->f_op for ours; every later read()/mmap() then serves a patched copy
//   held in kernel memory. The real file is untouched and the process's maps
//   still show the same path, so nothing looks synthetic.
//
// The copy is taken lazily, on first use, and refreshed every few seconds: at
// post-fs-data time the property service has not yet written values such as
// sys.usb.config, so a boot-time snapshot would be missing them.
//
// Only usb_control_prop (sys.usb.config / sys.usb.state) is patched for now.

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/mm.h>
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

#define PROP_CTX_FILE "u:object_r:usb_control_prop:s0"
#define PROP_DIR      "__properties__"
#define BLOB_TTL      (2 * HZ)

static int redirect;
module_param(redirect, int, 0444);
MODULE_PARM_DESC(redirect, "1 = serve patched property files to target uids");

struct ab_blob {
	struct kref ref;
	size_t len;
	u8 data[];
};

static struct ab_blob *g_blob;
static DEFINE_MUTEX(blob_lock);
static int g_redirect_state;   /* 1 = hook armed */

/* ---------- property area parsing (bionic layout) ---------- */

#define PROP_AREA_HDR   128
#define PROP_TRIE_NAME  20
#define PROP_INFO_VAL   4
#define PROP_VALUE_MAX  92

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

/*
 * The root node is a dummy: the first name token lives in root->children, and
 * every further token in the previous node's children.
 */
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
		off = rd32(cur + 16);           /* children */
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
			off = rd32(hit + 4);    /* prop */
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
	if (rd32(area + 8) != 0x504f5250)     /* PROP_AREA_MAGIC */
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

/*
 * kernel_read() is not usable on property files (the vendor KernelSU hook
 * rejects it), so pull the pages straight out of the page cache instead.
 */
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
			/* not populated: the area beyond bytes_used is unused */
			memset(dst + done, 0, n);
		} else {
			void *k = kmap_local_page(pg);

			memcpy(dst + done, k, n);
			kunmap_local(k);
			put_page(pg);
		}
		done += n;
		idx++;
	}
	return done;
}

static struct ab_blob *load_prop_blob(const char *ctx)
{
	char path[96];
	struct file *f;
	struct ab_blob *b;
	ssize_t n;
	size_t cap = 128 * 1024;
	bool p1, p2;

	snprintf(path, sizeof(path), "/dev/%s/%s", PROP_DIR, ctx);
	f = filp_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (IS_ERR(f)) {
		pr_warn_ratelimited("adbhide_kmod: cannot open %s: %ld\n",
				    path, PTR_ERR(f));
		return ERR_CAST(f);
	}

	b = kzalloc(sizeof(*b) + cap, GFP_KERNEL);
	if (!b) {
		filp_close(f, NULL);
		return ERR_PTR(-ENOMEM);
	}
	kref_init(&b->ref);
	b->len = cap;

	n = read_prop_file(f, b->data, cap);
	filp_close(f, NULL);
	if (n <= 0) {
		pr_warn_ratelimited("adbhide_kmod: snapshot read returned %zd\n", n);
		kfree(b);
		return ERR_PTR(n ? (long)n : -EIO);
	}
	b->len = n;

	p1 = patch_value(b->data, b->len, "sys.usb.config", "mtp");
	p2 = patch_value(b->data, b->len, "sys.usb.state", "mtp");
	if (!p1)
		pr_info_ratelimited("adbhide_kmod: sys.usb.config not in snapshot\n");
	if (p1 || p2)
		pr_info_ratelimited("adbhide_kmod: snapshot %zu bytes config=%d state=%d\n",
				    b->len, p1, p2);
	return b;
}

static void blob_work_fn(struct work_struct *w)
{
	struct ab_blob *nb = load_prop_blob(PROP_CTX_FILE);

	if (!IS_ERR(nb)) {
		struct ab_blob *old;

		mutex_lock(&blob_lock);
		old = g_blob;
		g_blob = nb;
		mutex_unlock(&blob_lock);
		if (old)
			kref_put(&old->ref, blob_release);
	}
	schedule_delayed_work(to_delayed_work(w), BLOB_TTL);
}

static DECLARE_DELAYED_WORK(blob_work, blob_work_fn);

/* returns a referenced snapshot, refreshed by blob_work */
static struct ab_blob *blob_get(void)
{
	struct ab_blob *b = NULL;

	mutex_lock(&blob_lock);
	if (g_blob) {
		kref_get(&g_blob->ref);
		b = g_blob;
	}
	mutex_unlock(&blob_lock);
	return b;
}

/* ---------- fake file_operations ---------- */

static ssize_t ab_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct ab_blob *b = blob_get();
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
	struct ab_blob *b = blob_get();
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
		kaddr = kmap_local_page(page);
		memcpy(kaddr, b->data + off, n);
		kunmap_local(kaddr);
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
	struct ab_blob *b = blob_get();

	if (!b)
		return -EIO;
	if (vma->vm_pgoff) {
		kref_put(&b->ref, blob_release);
		return -EINVAL;
	}

	vma->vm_private_data = b;   /* reference held by the vma */
	vma->vm_ops = &ab_vm_ops;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
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
	struct file *file;
};

static int ab_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);
	struct ab_open_data *d = (struct ab_open_data *)ri->data;
	struct dentry *dent;

	d->wanted = false;
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
	if (dent->d_name.len != sizeof(PROP_CTX_FILE) - 1 ||
	    memcmp(dent->d_name.name, PROP_CTX_FILE, sizeof(PROP_CTX_FILE) - 1))
		return 0;

	d->wanted = true;
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
	if (!d->wanted || ret != 0)
		return 0;

	file = d->file;
	if (!file)
		return 0;

	file->f_op = &ab_fops;
	file->private_data = NULL;
	adbhide_counter_bump(CNT_REDIRECT);
	adbhide_event_add("redir", PROP_CTX_FILE);
	return 0;
}

static struct kretprobe ab_krp = {
	.kp.symbol_name = "vfs_open",
	.entry_handler = ab_entry,
	.handler = ab_ret,
	.data_size = sizeof(struct ab_open_data),
	.maxactive = 64,
};

int adbhide_props_init(void)
{
	int ret;

	if (!redirect) {
		pr_info("adbhide_kmod: redirect disabled\n");
		return 0;
	}
	if (!adbhide_target_count()) {
		pr_warn("adbhide_kmod: redirect needs target uids\n");
		return 0;
	}

	ret = register_kretprobe(&ab_krp);
	if (ret) {
		pr_warn("adbhide_kmod: kretprobe vfs_open failed: %d\n", ret);
		return 0;
	}

	g_redirect_state = 1;
	pr_info("adbhide_kmod: redirect armed on %s\n", PROP_CTX_FILE);
	return 0;
}

void adbhide_props_exit(void)
{
	if (g_redirect_state) {
		unregister_kretprobe(&ab_krp);
		g_redirect_state = 0;
	}
	cancel_delayed_work_sync(&blob_work);
	if (g_blob) {
		struct ab_blob *old;

		mutex_lock(&blob_lock);
		old = g_blob;
		g_blob = NULL;
		mutex_unlock(&blob_lock);
		if (old)
			kref_put(&old->ref, blob_release);
	}
}

int adbhide_redirect_active(void)
{
	return g_redirect_state;
}
