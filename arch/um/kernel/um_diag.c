// SPDX-License-Identifier: GPL-2.0
/*
 * UML diagnostic ring buffer.
 *
 * Records mm-affecting syscalls (mmap/mprotect/munmap) into a small
 * circular buffer with nanosecond timestamps. Dumped via printk on
 * panic so we can correlate the failure site with the recent syscall
 * history without paying the printk-per-syscall latency that would
 * itself perturb the bug being investigated.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/printk.h>
#include <linux/spinlock.h>

#define UM_DIAG_RING_SIZE	512
#define UM_DIAG_RING_MASK	(UM_DIAG_RING_SIZE - 1)

struct um_diag_event {
	u64		t;
	int		nr;
	unsigned long	addr;
	unsigned long	len;
	unsigned long	prot;
	long		ret;
};

static struct um_diag_event um_diag_ring[UM_DIAG_RING_SIZE];
static unsigned int um_diag_head;
static DEFINE_SPINLOCK(um_diag_lock);

void um_diag_record(int nr, unsigned long a, unsigned long l,
		    unsigned long p, long r, u64 t)
{
	unsigned long flags;
	unsigned int idx;

	spin_lock_irqsave(&um_diag_lock, flags);
	idx = um_diag_head & UM_DIAG_RING_MASK;
	um_diag_ring[idx].t    = t;
	um_diag_ring[idx].nr   = nr;
	um_diag_ring[idx].addr = a;
	um_diag_ring[idx].len  = l;
	um_diag_ring[idx].prot = p;
	um_diag_ring[idx].ret  = r;
	um_diag_head++;
	spin_unlock_irqrestore(&um_diag_lock, flags);
}
EXPORT_SYMBOL_GPL(um_diag_record);

static int um_diag_panic(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	unsigned long flags;
	unsigned int head_snap, count, i;

	spin_lock_irqsave(&um_diag_lock, flags);
	head_snap = um_diag_head;
	spin_unlock_irqrestore(&um_diag_lock, flags);

	count = head_snap < UM_DIAG_RING_SIZE ? head_snap : UM_DIAG_RING_SIZE;

	pr_info("um: DIAG panic: dumping last %u mm-syscall events (head_snap=%u)\n",
		count, head_snap);
	for (i = 0; i < count; i++) {
		unsigned int idx = (head_snap - count + i) & UM_DIAG_RING_MASK;
		struct um_diag_event *e = &um_diag_ring[idx];

		pr_info("um: DIAG[%u] t=%llu nr=%d addr=0x%lx len=0x%lx prot=0x%lx ret=%ld\n",
			head_snap - count + i, e->t, e->nr, e->addr, e->len,
			e->prot, e->ret);
	}
	return NOTIFY_DONE;
}

static struct notifier_block um_diag_panic_nb = {
	.notifier_call	= um_diag_panic,
	.priority	= INT_MAX,	/* run before everything else */
};

static int __init um_diag_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &um_diag_panic_nb);
	return 0;
}
arch_initcall(um_diag_init);
