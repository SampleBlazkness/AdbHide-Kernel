/* SPDX-License-Identifier: MIT */
#ifndef ADBHIDE_H
#define ADBHIDE_H

#include <linux/types.h>
#include <linux/cred.h>
#include <linux/sched.h>

#define ADBHIDE_KVER      "0.2.0"
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

/* adbhide_props.c (M3) */
int adbhide_props_init(void);
void adbhide_props_exit(void);
int adbhide_redirect_active(void);

#endif
