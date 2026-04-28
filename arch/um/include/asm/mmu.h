/* SPDX-License-Identifier: GPL-2.0 */
/* 
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __ARCH_UM_MMU_H
#define __ARCH_UM_MMU_H

#include "linux/types.h"
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <mm_id.h>

struct um_worker;

typedef struct mm_context {
	struct mm_id id;
	struct mutex turnstile;

	struct list_head list;

	/* Address range in need of a TLB sync */
	spinlock_t sync_tlb_lock;
	unsigned long sync_tlb_range_from;
	unsigned long sync_tlb_range_to;

	/*
	 * Per-mm host worker process handle (memo 25 R4 / memo 28).
	 * Always present (cost of a NULL pointer is negligible).
	 * Under CONFIG_UM_WORKER_PROCESS=y this points at an opaque
	 * struct um_worker owned by arch/um/os-Linux/spawner.c; under
	 * =n it stays NULL and seccomp's mm_create falls back to
	 * today's stub-child-in-spawner path. The runtime split lands
	 * when memo 28's E.6 commit flips the defconfig toggle.
	 */
	struct um_worker *worker;
} mm_context_t;

#define INIT_MM_CONTEXT(mm)						\
	.context = {							\
		.turnstile = __MUTEX_INITIALIZER(mm.context.turnstile),	\
		.sync_tlb_lock = __SPIN_LOCK_INITIALIZER(mm.context.sync_tlb_lock), \
	}

#endif
