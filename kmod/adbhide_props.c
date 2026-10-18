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
//   held in kernel memory. The real file is untouched, and the process's maps
//   show the same path and the same sparse file, so nothing looks synthetic.
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
#include <linux/version.h>
#include "adbhide.h"

#define PROP_CTX_FILE "usb_control_prop"
#define PROP_DIR      "__properties__"

static int redirect;
module_param(redirect, int, 0444);
MODULE_PARM_DESC(redirect, "1 = serve patched property files to target uids");

struct ab_blob {
	struct kref ref;
	size_t len;
	u8 data[];
};

static struct ab_blob *g_blob;
static int g_redirect_state;   /* 0 off, 1 active, negative = error */

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

static const u8 *find_prop_info(const u8 *data, const char *name)
{
	const char *remaining = name;
	const u8 *node = data;

	for (;;) {
		const char *sep = strchr(remaining, '.');
		u32 tok_len = sep ? (u32)(sep - remaining) : (u32)strlen(remaining);
		const u8 *cur = node;

		while (cur) {
			u32 nl = rd32(cur);
			const char *nname = (const char *)(cur + PROP_TRIE_NAME);
			int ret = cmp_prop_name(remaining, tok_len, nname, nl);

			if (ret == 0)
				break;
			{
				u32 off = rd32(cur + (ret < 0 ? 8 : 12));
				cur = off ? data + off : NULL;
			}
		}
		if (!cur)
			return NULL;

		if (!sep) {
			u32 poff = rd32(cur + 4);
			return poff ? data + poff : NULL;
		}
		{
			u32 child = rd32(cur + 16);
			if (!child)
				return NULL;
			node = data + child;
		}
		remaining = sep + 1;
	}
}

static void patch_value(u8 *area, size_t len, const char *name, const char *value)
{
	u8 *info;

	if (len < PROP_AREA_HDR + sizeof(u32) * 3)
		return;
	if (rd32(area + 8) != 0x504f5250)     /* PROP_AREA_MAGIC */
		return;
	info = (u8 *)find_prop_info(area + PROP_AREA_HDR, name);
	if (!info)
		return;
	if ((size_t)(info - area) + PROP_INFO_VAL + PROP_VALUE_MAX > len)
		return;

	memset(info + PROP_INFO_VAL, 0, PROP_VALUE_MAX);
	strscpy((char *)(info + PROP_INFO_VAL), value, PROP_VALUE_MAX);
	pr_info("adbhide_kmod: patched %s -> \"%s\"\n", name, value);
}

/* ---------- blob ---------- */

static void blob_release(struct kref *ref)
{
	struct ab_blob *b = container_of(ref, struct ab_blob, ref);

	kfree(b);
}

static struct ab_blob *load_prop_blob(const char *ctx)
{
	char path[96];
	struct file *f;
	struct ab_blob *b;
	loff_t pos = 0;
	ssize_t n;
	size_t cap = 128 * 1024;

	snprintf(path, sizeof(path), "/dev/%s/%s", PROP_DIR, ctx);
	f = filp_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (IS_ERR(f)) {
		pr_warn("adbhide_kmod: cannot open %s: %ld\n", path, PTR_ERR(f));
		return ERR_CAST(f);
	}

	b = kzalloc(sizeof(*b) + cap, GFP_KERNEL);
	if (!b) {
		filp_close(f, NULL);
		return ERR_PTR(-ENOMEM);
	}
	kref_init(&b->ref);
	b->len = cap;

	n = kernel_read(f, b->data, cap, &pos);
	filp_close(f, NULL);
	if (n <= 0) {
		kfree(b);
		return ERR_PTR(n ? (long)n : -EIO);
	}
	b->len = n;

	/* the values Duck reads through getprop */
	patch_value(b->data, b->len, "sys.usb.config", "mtp");
	patch_value(b->data, b->len, "sys.usb.state", "mtp");
	return b;
}

/* ---------- fake file_operations ---------- */

static ssize_t ab_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct ab_blob *b = file->private_data;
	size_t left;

	if (!b)
		return -EIO;
	if (*ppos < 0 || (size_t)*ppos >= b->len)
		return 0;

	left = b->len - (size_t)*ppos;
	if (count > left)
		count = left;
	if (copy_to_user(buf, b->data + *ppos, count))
		return -EFAULT;
	*ppos += count;
	return count;
}

static ssize_t ab_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct ab_blob *b = iocb->ki_filp->private_data;
	size_t left;

	if (!b)
		return -EIO;
	if (iocb->ki_pos < 0 || (size_t)iocb->ki_pos >= b->len)
		return 0;

	left = b->len - (size_t)iocb->ki_pos;
	return copy_to_iter(b->data + iocb->ki_pos, min(left, iov_iter_count(to)), to);
}

static int ab_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ab_blob *b = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long off;

	if (!b)
		return -EIO;
	if (vma->vm_pgoff)
		return -EINVAL;

	for (off = 0; off < size; off += PAGE_SIZE) {
		struct page *pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
		void *kaddr;
		size_t n = 0;

		if (!pg)
			return -ENOMEM;

		if (off < b->len) {
			n = min_t(size_t, PAGE_SIZE, b->len - off);
			kaddr = kmap_local_page(pg);
			memcpy(kaddr, b->data + off, n);
			kunmap_local(kaddr);
		}

		if (vm_insert_page(vma, vma->vm_start + off, pg)) {
			put_page(pg);
			return -ENOMEM;
		}
		put_page(pg);
	}

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	return 0;
}

static int ab_release(struct inode *inode, struct file *file)
{
	struct ab_blob *b = file->private_data;

	if (b) {
		kref_put(&b->ref, blob_release);
		file->private_data = NULL;
	}
	return 0;
}

static const struct file_operations ab_fops = {
	.owner = THIS_MODULE,
	.llseek = default_llseek,
	.read = ab_read,
	.read_iter = ab_read_iter,
	.mmap = ab_mmap,
	.release = ab_release,
};

/* ---------- vfs_open kretprobe ---------- */

struct ab_open_data {
	struct ab_blob *blob;
};

static int ab_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct path *path = (struct path *)regs_get_kernel_argument(regs, 0);
	struct ab_open_data *d = (struct ab_open_data *)ri->data;
	struct dentry *dent;

	d->blob = NULL;
	if (!redirect || !g_blob || !path || !path->dentry)
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

	kref_get(&g_blob->ref);
	d->blob = g_blob;
	adbhide_counter_bump(CNT_REDIRECT);
	return 0;
}

static int ab_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ab_open_data *d = (struct ab_open_data *)ri->data;
	struct file *file;
	long ret = regs_return_value(regs);

	if (!d->blob)
		return 0;

	/* failed open: drop the reference we took, nothing to redirect */
	if (ret != 0) {
		kref_put(&d->blob->ref, blob_release);
		return 0;
	}

	file = (struct file *)regs_get_kernel_argument(regs, 1);
	if (!file) {
		kref_put(&d->blob->ref, blob_release);
		return 0;
	}

	file->f_op = &ab_fops;
	file->private_data = d->blob;
	d->blob = NULL;
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

	g_redirect_state = -1;
	if (!redirect) {
		pr_info("adbhide_kmod: redirect disabled\n");
		return 0;
	}
	if (!adbhide_target_count()) {
		pr_warn("adbhide_kmod: redirect needs target uids\n");
		return 0;
	}

	g_blob = load_prop_blob(PROP_CTX_FILE);
	if (IS_ERR(g_blob)) {
		long err = PTR_ERR(g_blob);

		g_blob = NULL;
		pr_warn("adbhide_kmod: blob load failed: %ld\n", err);
		return 0;
	}

	ret = register_kretprobe(&ab_krp);
	if (ret) {
		pr_warn("adbhide_kmod: kretprobe vfs_open failed: %d\n", ret);
		kref_put(&g_blob->ref, blob_release);
		g_blob = NULL;
		return 0;
	}

	g_redirect_state = 1;
	pr_info("adbhide_kmod: redirect active on %s (%zu bytes)\n",
		PROP_CTX_FILE, g_blob->len);
	return 0;
}

void adbhide_props_exit(void)
{
	if (g_redirect_state == 1) {
		unregister_kretprobe(&ab_krp);
		g_redirect_state = 0;
	}
	if (g_blob) {
		kref_put(&g_blob->ref, blob_release);
		g_blob = NULL;
	}
}

int adbhide_redirect_active(void)
{
	return g_redirect_state == 1;
}
