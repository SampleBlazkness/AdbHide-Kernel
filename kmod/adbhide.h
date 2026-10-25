/* SPDX-License-Identifier: MIT */
#ifndef ADBHIDE_H
#define ADBHIDE_H

#include <linux/types.h>
#include <linux/version.h>
#include <linux/cred.h>
#include <linux/sched.h>

/*
 * Kernel API compatibility for the KMI range this module is built for
 * (5.10 .. 6.12). kmap_local_page()/kunmap_local() arrived in 5.11 and
 * vm_flags_set() in 6.3.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define adbhide_kmap_local(page) kmap_local_page(page)
#define adbhide_kunmap_local(addr) kunmap_local(addr)
#else
#define adbhide_kmap_local(page) kmap(page)
#define adbhide_kunmap_local(addr) kunmap(addr)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define adbhide_vm_flags_set(vma, flags) vm_flags_set(vma, flags)
#else
#define adbhide_vm_flags_set(vma, flags) ((vma)->vm_flags |= (flags))
#endif

#define ADBHIDE_KVER      "1.2.6"
#define ADBHIDE_MAX_UIDS  64
#define ADBHIDE_MAX_EVENTS 64

struct adbhide_event {
	u64 ts_ns;
	u32 uid;
	char tag[12];
	char name[28];
	char comm[16];
};

/* uids.c */
int adbhide_parse_targets(const char *spec);
int adbhide_target_count(void);
u32 adbhide_target_at(int idx);
int adbhide_uid_index(u32 uid);
bool adbhide_uid_is_target(u32 uid);
u32 adbhide_uid_now(void);

enum adbhide_counter {
	CNT_PROP_OPEN = 0,
	CNT_TCP_READ,
	CNT_TCP6_READ,
	CNT_REDIRECT,
	CNT_NET_HIDDEN,
	CNT_SOCK_HIDDEN,
	CNT_UNIX_HIDDEN,
	CNT_MOD_HIDDEN,
	CNT_MAX,
};

void adbhide_counter_bump(enum adbhide_counter c);
u32 adbhide_counter_get(enum adbhide_counter c);

/* adbhide_main.c */
void adbhide_event_add(const char *tag, const char *name);
unsigned int adbhide_event_count(void);

/* adbhide_probes.c */
extern bool adbhide_probe_ok[2];
extern const char *const adbhide_probe_names[2];

int adbhide_probes_register(void);
void adbhide_probes_unregister(void);

/* adbhide_netseq.c (M4) */
int adbhide_netseq_init(void);
void adbhide_netseq_exit(void);
int adbhide_netseq_active(void);

/* adbhide_sockets.c (M6) */
int adbhide_sockets_init(void);
void adbhide_sockets_exit(void);
int adbhide_sockets_active(void);

/* adbhide_vma.c (M4) */
int adbhide_vma_init(void);
void adbhide_vma_exit(void);

/* adbhide_props.c (M3) */
int adbhide_props_init(void);
void adbhide_props_exit(void);
int adbhide_redirect_active(void);

#endif
