// SPDX-License-Identifier: MIT
//
// Kill the only visible side effect of the userspace remap: a private file
// mapping prints as r--p in /proc/self/maps, while stock property areas are
// shared and print r--s.
//
// The zygisk layer re-maps /dev/__properties__/<ctx> with MAP_PRIVATE. We catch
// that mmap and, once the syscall has returned, set VM_SHARED|VM_MAYSHARE on
// the resulting VMA. The pages themselves stay private (the redirect still
// works); only the flag the map printer reads changes.
//
// mmap_lock cannot be taken from a kretprobe, so the flag change is deferred
// to a workqueue with mmget()/get_file() references held.

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/workqueue.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include "adbhide.h"

#define PROP_DIR "__properties__"

static const char *const vma_contexts[] = {
	"u:object_r:system_prop:s0",
	"u:object_r:usb_control_prop:s0",
	"u:object_r:usb_config_prop:s0",
	"u:object_r:init_service_status_prop:s0",
};

struct vma_work {
	struct work_struct work;
	struct mm_struct *mm;
	struct file *file;
	unsigned long addr;
};

struct vma_probe_data {
	struct vma_work *pending;
};

static bool prop_context_file(struct file *file)
{
	struct dentry *dent;
	unsigned int i;

	if (!file)
		return false;
	dent = file_dentry(file);
	if (!dent || !dent->d_parent)
		return false;
	if (dent->d_parent->d_name.len != sizeof(PROP_DIR) - 1 ||
	    memcmp(dent->d_parent->d_name.name, PROP_DIR, sizeof(PROP_DIR) - 1))
		return false;

	for (i = 0; i < ARRAY_SIZE(vma_contexts); i++) {
		size_t len = strlen(vma_contexts[i]);

		if (dent->d_name.len == len &&
		    memcmp(dent->d_name.name, vma_contexts[i], len) == 0)
			return true;
	}
	return false;
}

static void vma_work_fn(struct work_struct *w)
{
	struct vma_work *vw = container_of(w, struct vma_work, work);
	struct mm_struct *mm = vw->mm;

	if (mmap_write_lock_killable(mm) == 0) {
		struct vm_area_struct *vma = find_vma(mm, vw->addr);

		if (vma && vma->vm_start == vw->addr && vma->vm_file == vw->file) {
			vm_flags_set(vma, VM_SHARED | VM_MAYSHARE);
			adbhide_event_add("vmaflag", vw->file->f_path.dentry->d_name.name);
		}
		mmap_write_unlock(mm);
	}
	fput(vw->file);
	mmput(mm);
	kfree(vw);
}

static int vma_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct vma_probe_data *d = (struct vma_probe_data *)ri->data;
	struct file *file = (struct file *)regs_get_kernel_argument(regs, 0);
	unsigned long flags = (unsigned long)regs_get_kernel_argument(regs, 4);

	d->pending = NULL;
	if (!(flags & MAP_PRIVATE) || (flags & MAP_SHARED))
		return 0;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!prop_context_file(file))
		return 0;

	{
		struct vma_work *vw = kzalloc(sizeof(*vw), GFP_ATOMIC);

		if (!vw)
			return 0;
		INIT_WORK(&vw->work, vma_work_fn);
		vw->file = file;
		vw->addr = (unsigned long)regs_get_kernel_argument(regs, 1);
		d->pending = vw;
	}
	return 0;
}

static int vma_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct vma_probe_data *d = (struct vma_probe_data *)ri->data;
	unsigned long addr = regs_return_value(regs);

	if (!d->pending)
		return 0;
	if (IS_ERR_VALUE(addr)) {
		kfree(d->pending);
		d->pending = NULL;
		return 0;
	}

	d->pending->addr = addr;
	d->pending->mm = current->mm;
	mmget(d->pending->mm);
	get_file(d->pending->file);
	schedule_work(&d->pending->work);
	d->pending = NULL;
	return 0;
}

static struct kretprobe vma_krp = {
	.kp.symbol_name = "vm_mmap_pgoff",
	.entry_handler = vma_entry,
	.handler = vma_ret,
	.data_size = sizeof(struct vma_probe_data),
	.maxactive = 32,
};

static bool vma_active;

int adbhide_vma_init(void)
{
	if (!adbhide_target_count())
		return 0;
	if (register_kretprobe(&vma_krp)) {
		pr_warn("adbhide_kmod: vm_mmap_pgoff probe failed\n");
		return -ENOENT;
	}
	vma_active = true;
	pr_info("adbhide_kmod: vma flag restore armed\n");
	return 0;
}

void adbhide_vma_exit(void)
{
	if (!vma_active)
		return;
	unregister_kretprobe(&vma_krp);
	vma_active = false;
}
