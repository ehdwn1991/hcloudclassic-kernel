/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IPC_H
#define _LINUX_IPC_H

#include <linux/spinlock.h>
#include <linux/uidgid.h>
#include <linux/rhashtable.h>
#include <uapi/linux/ipc.h>
#include <linux/refcount.h>

#define IPCMNI 32768  /* <= MAX_INT limit for ipc arrays (including sysctl changes) */

/* used by in-kernel data structures */
struct kern_ipc_perm {
#ifdef CONFIG_HCC_IPC
	struct mutex    mutex;
#else
	spinlock_t	lock;
#endif	
	bool		deleted;
	int		id;
	key_t		key;
	kuid_t		uid;
	kgid_t		gid;
	kuid_t		cuid;
	kgid_t		cgid;
	umode_t		mode;
	unsigned long	seq;
	void		*security;

	struct rhash_head khtnode;

	struct rcu_head rcu;
	refcount_t refcount;
} ____cacheline_aligned_in_smp __randomize_layout;

#endif /* _LINUX_IPC_H */
