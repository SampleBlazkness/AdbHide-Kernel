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
 * `ss` asks the kernel over NETLINK_SOCK_DIAG instead of reading /proc/net,
 * so the seq_file filtering above never sees it. Denying the socket would be
 * an anomaly of its own, so filter the reply: just before netlink_unicast()
 * hands the skb to the caller, drop the records whose local or peer port is
 * the adbd port. Everything else in the reply stays untouched.
 */
static unsigned int diag_sport(const unsigned char *msg)
{
	return ((unsigned int)msg[4] << 8) | msg[5];
}

static unsigned int diag_dport(const unsigned char *msg)
{
	return ((unsigned int)msg[6] << 8) | msg[7];
}

static int diag_entry(struct kprobe *p, struct pt_regs *regs)
{
	struct sock *ssk = (struct sock *)regs_get_kernel_argument(regs, 0);
	struct sk_buff *skb = (struct sk_buff *)regs_get_kernel_argument(regs, 1);
	unsigned int off = 0, out = 0;
	bool changed = false;

	(void)p;
	if (ssk && ssk->sk_protocol == 4) {
		char b[24];

		if (skb && skb->data && skb->len >= 24) {
			struct nlmsghdr *nlh = (struct nlmsghdr *)skb->data;
			const unsigned char *m = (const unsigned char *)nlh + 16;

			snprintf(b, sizeof(b), "u%u-L%u-T%u-S%u", (unsigned int)adbhide_uid_now(),
				 skb->len, nlh->nlmsg_type, diag_sport(m));
		} else {
			snprintf(b, sizeof(b), "u%u-L%u", (unsigned int)adbhide_uid_now(),
				 skb ? skb->len : 0u);
		}
		adbhide_event_add("nl", b);
	}
	if (!adbhide_target_count())
		return 0;
	if (!adbhide_uid_is_target(adbhide_uid_now()))
		return 0;
	if (!ssk || ssk->sk_protocol != 4)
		return 0;
	if (!skb || !skb->data || skb->len < 24 || skb->len > 262144)
		return 0;

	while (off + 16 <= skb->len) {
		struct nlmsghdr *nlh = (struct nlmsghdr *)(skb->data + off);
		unsigned int len = nlh->nlmsg_len;
		const unsigned char *msg;

		if (len < 16 || off + len > skb->len)
			break;
		msg = (const unsigned char *)nlh + 16;
		if (nlh->nlmsg_type == 20 && len >= 24 &&
		    (diag_sport(msg) == (unsigned int)adb_port ||
		     diag_dport(msg) == (unsigned int)adb_port)) {
			changed = true;
			off += len;
			continue;
		}
		if (out != off)
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

static struct kprobe diag_kp = {
	.symbol_name = "netlink_unicast",
	.pre_handler = diag_entry,
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

	r4 = register_kprobe(&diag_kp);
	if (r4)
		pr_warn("adbhide_kmod: netlink_unicast probe failed: %d\n", r4);
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
	if (diag_kp.addr)
		unregister_kprobe(&diag_kp);
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
