// SPDX-License-Identifier: MIT
//
// AdbHide-Kernel — kernel-level ADB hiding.
//
// M2: read-only probes. Nothing here changes a syscall result yet; the module
// only reports what target apps actually touch, so M3 can hook the right place.

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include "adbhide.h"

char adbhide_targets[512] = "";
module_param_string(target_uids, adbhide_targets, sizeof(adbhide_targets), 0444);
MODULE_PARM_DESC(target_uids, "Comma separated app uids to watch");

static struct adbhide_event events[ADBHIDE_MAX_EVENTS];
static unsigned int ev_head;
static unsigned int ev_count;
static DEFINE_SPINLOCK(ev_lock);

void adbhide_event_add(const char *tag, const char *name)
{
	struct adbhide_event *e;
	unsigned long flags;

	spin_lock_irqsave(&ev_lock, flags);
	e = &events[ev_head];
	e->ts_ns = ktime_get_ns();
	e->uid = adbhide_uid_now();
	strscpy(e->tag, tag, sizeof(e->tag));
	strscpy(e->name, name, sizeof(e->name));
	strscpy(e->comm, current->comm, sizeof(e->comm));
	ev_head = (ev_head + 1) % ADBHIDE_MAX_EVENTS;
	if (ev_count < ADBHIDE_MAX_EVENTS)
		ev_count++;
	spin_unlock_irqrestore(&ev_lock, flags);
}

unsigned int adbhide_event_count(void)
{
	return ev_count;
}

static void seq_print_events(struct seq_file *m)
{
	unsigned int i, start;

	if (!ev_count)
		return;

	start = (ev_count == ADBHIDE_MAX_EVENTS) ? ev_head : 0;
	seq_printf(m, "events %u\n", ev_count);
	for (i = 0; i < ev_count; i++) {
		const struct adbhide_event *e =
			&events[(start + i) % ADBHIDE_MAX_EVENTS];
		seq_printf(m, "  ts=%llu uid=%u comm=%s tag=%s name=%s\n",
			   e->ts_ns, e->uid, e->comm, e->tag, e->name);
	}
}

static int adbhide_proc_show(struct seq_file *m, void *v)
{
	int i, n = adbhide_target_count();

	seq_printf(m, "adbhide_kmod %s\n", ADBHIDE_KVER);
	seq_printf(m, "kernel %s\n", init_uts_ns.name.release);
	seq_puts(m, "mode probe\n");

	seq_printf(m, "targets %d:", n);
	for (i = 0; i < n; i++)
		seq_printf(m, " %u", adbhide_target_at(i));
	seq_putc(m, '\n');

	seq_printf(m, "probes %s=%s %s=%s\n",
		   adbhide_probe_names[0], adbhide_probe_ok[0] ? "ok" : "failed",
		   adbhide_probe_names[1], adbhide_probe_ok[1] ? "ok" : "failed");

	seq_printf(m, "counters prop_open=%u tcp_read=%u tcp6_read=%u redirect=%u\n",
		   adbhide_counter_get(CNT_PROP_OPEN),
		   adbhide_counter_get(CNT_TCP_READ),
		   adbhide_counter_get(CNT_TCP6_READ),
		   adbhide_counter_get(CNT_REDIRECT));

	seq_printf(m, "redirect %s\n", adbhide_redirect_active() ? "active" : "off");

	seq_print_events(m);
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
	int n;

	n = adbhide_parse_targets(adbhide_targets);
	if (n <= 0)
		pr_warn("adbhide_kmod: no target uids given (target_uids=)\n");

	adbhide_probes_register();
	adbhide_props_init();

	if (!proc_create("adbhide_kmod", 0444, NULL, &adbhide_proc_ops))
		pr_warn("adbhide_kmod: /proc entry not created\n");

	pr_info("adbhide_kmod %s loaded (targets=%d)\n", ADBHIDE_KVER, n);
	return 0;
}

static void __exit adbhide_exit(void)
{
	remove_proc_entry("adbhide_kmod", NULL);
	adbhide_props_exit();
	adbhide_probes_unregister();
	pr_info("adbhide_kmod unloaded\n");
}

module_init(adbhide_init);
module_exit(adbhide_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Blazkness");
MODULE_DESCRIPTION("Kernel-level ADB hiding for selected apps (probe stage)");
MODULE_VERSION(ADBHIDE_KVER);
