// SPDX-License-Identifier: MIT
//
// M4: hide adbd listener rows from /proc/net/tcp and tcp6 for target uids.
//
// tcp4_seq_show() appends one row per socket to the seq_file buffer. A
// kretprobe cannot skip the function, but it can drop what was appended:
// remember seq->count on entry, restore it on return. The row disappears and
// the remaining rows stay contiguous, so the file still parses normally.
//
// Only TCP_LISTEN sockets on the adb port are dropped, and only for readers
// in the target uid list.

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/seq_file.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_sock.h>
#include "adbhide.h"

static unsigned int adb_port = 5555;
module_param(adb_port, uint, 0444);
MODULE_PARM_DESC(adb_port, "TCP port used by adbd (5555 unless overridden)");

struct ns_data {
	struct seq_file *seq;
	size_t saved;
	bool hit;
};

static int ns_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ns_data *d = (struct ns_data *)ri->data;
	void *v = (void *)regs_get_kernel_argument(regs, 1);
	struct sock *sk;

	d->hit = false;
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!v || v == SEQ_START_TOKEN)
		return 0;

	sk = (struct sock *)v;
	if (sk->sk_state != TCP_LISTEN)
		return 0;
	if (ntohs(inet_sk(sk)->inet_sport) != (u16)adb_port)
		return 0;

	d->seq = (struct seq_file *)regs_get_kernel_argument(regs, 0);
	d->saved = d->seq ? d->seq->count : 0;
	d->hit = d->seq != NULL;
	return 0;
}

static int ns_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct ns_data *d = (struct ns_data *)ri->data;

	if (d->hit) {
		d->seq->count = d->saved;
		adbhide_counter_bump(CNT_NET_HIDDEN);
		adbhide_event_add("netseq", "tcp_listen");
	}
	return 0;
}

static struct kretprobe tcp4_krp = {
	.kp.symbol_name = "tcp4_seq_show",
	.entry_handler = ns_entry,
	.handler = ns_ret,
	.data_size = sizeof(struct ns_data),
	.maxactive = 64,
};

static struct kretprobe tcp6_krp = {
	.kp.symbol_name = "tcp6_seq_show",
	.entry_handler = ns_entry,
	.handler = ns_ret,
	.data_size = sizeof(struct ns_data),
	.maxactive = 64,
};

static bool netseq_active;

int adbhide_netseq_init(void)
{
	int r4, r6;

	r4 = register_kretprobe(&tcp4_krp);
	r6 = register_kretprobe(&tcp6_krp);
	if (r4)
		pr_warn("adbhide_kmod: tcp4_seq_show probe failed: %d\n", r4);
	if (r6)
		pr_warn("adbhide_kmod: tcp6_seq_show probe failed: %d\n", r6);

	netseq_active = (r4 == 0 || r6 == 0);
	if (netseq_active)
		pr_info("adbhide_kmod: netseq filtering port %u (tcp4=%s tcp6=%s)\n",
			adb_port, r4 ? "off" : "on", r6 ? "off" : "on");
	return netseq_active ? 0 : -ENOENT;
}

void adbhide_netseq_exit(void)
{
	if (!netseq_active)
		return;
	if (tcp4_krp.kp.addr)
		unregister_kretprobe(&tcp4_krp);
	if (tcp6_krp.kp.addr)
		unregister_kretprobe(&tcp6_krp);
	netseq_active = false;
}

int adbhide_netseq_active(void)
{
	return netseq_active;
}
