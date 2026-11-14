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
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <net/inet_sock.h>
#include "adbhide.h"

static unsigned int adb_port = 5555;
module_param(adb_port, uint, 0644);
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
	/* Listening rows and the sessions that reach them both name the port. */
	if (ntohs(inet_sk(sk)->inet_sport) != (u16)adb_port &&
	    ntohs(inet_sk(sk)->inet_dport) != (u16)adb_port)
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


/* ---------------------------------------------------- netlink diag 过滤 */

/*
 * `ss` does not read /proc/net: it opens NETLINK_SOCK_DIAG and the kernel
 * builds the reply with inet_diag_dump_icsk(). The listener therefore stayed
 * visible even though every seq_file row was hidden. Roll the skb back over
 * the records whose local or peer port is the adbd port.
 */
struct diag_data {
	struct sk_buff *skb;
	unsigned int saved;
	bool hit;
};

static unsigned int diag_sport(const unsigned char *msg)
{
	return ((unsigned int)msg[4] << 8) | msg[5];
}

static unsigned int diag_dport(const unsigned char *msg)
{
	return ((unsigned int)msg[6] << 8) | msg[7];
}

static int diag_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct diag_data *d = (struct diag_data *)ri->data;
	struct sk_buff *skb = (struct sk_buff *)regs_get_kernel_argument(regs, 1);

	d->hit = false;
	d->skb = NULL;
	d->saved = 0;
	{
		char b[24];

		snprintf(b, sizeof(b), "e%u", skb ? skb->len : 0u);
		adbhide_event_add("diag", b);
	}
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!skb || !skb->data)
		return 0;
	d->skb = skb;
	d->saved = skb->len;
	d->hit = true;
	return 0;
}

static int diag_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct diag_data *d = (struct diag_data *)ri->data;
	struct sk_buff *skb = d->skb;
	unsigned int off = d->saved, out = d->saved;
	bool changed = false;

	if (!d->hit || !skb || (long)regs_return_value(regs) < 0)
		return 0;
	{
		char b[24];

		snprintf(b, sizeof(b), "r%u-%u", skb->len, d->saved);
		adbhide_event_add("diag", b);
	}
	if (skb->len <= d->saved || skb->len > 262144)
		return 0;

	while (off + 16 <= skb->len) {
		struct nlmsghdr *nlh = (struct nlmsghdr *)(skb->data + off);
		unsigned int len = nlh->nlmsg_len;
		const unsigned char *msg;

		if (len < 16 || off + len > skb->len)
			break;
		msg = (const unsigned char *)nlh + 16;
		if (len >= 16 + 8 &&
		    (diag_sport(msg) == (unsigned int)adb_port ||
		     diag_dport(msg) == (unsigned int)adb_port)) {
			changed = true;
			off += len;
			continue;
		}
		if (out != off && len)
			memmove(skb->data + out, skb->data + off, len);
		out += len;
		off += len;
	}

	if (changed && out < skb->len) {
		skb->len = out;
		skb_set_tail_pointer(skb, out);
		adbhide_counter_bump(CNT_NET_HIDDEN);
		adbhide_event_add("netseq", "diag");
	}
	return 0;
}

static struct kretprobe diag_krp = {
	.kp.symbol_name = "inet_diag_dump_icsk",
	.entry_handler = diag_entry,
	.handler = diag_ret,
	.data_size = sizeof(struct diag_data),
	.maxactive = 16,
};

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

	r4 = register_kretprobe(&diag_krp);
	if (r4)
		pr_warn("adbhide_kmod: inet_diag probe failed: %d\n", r4);
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
	if (diag_krp.kp.addr)
		unregister_kretprobe(&diag_krp);
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
