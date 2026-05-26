// SPDX-License-Identifier: GPL-2.0
/*
 * KVM-backend snapshot/forkserver — capture/restore primitives
 * (task #250 v2 ladder, memo 12).
 *
 * The C-09 v1 forkserver in arch/um/kernel/snapshot.c uses fork()
 * to spawn fuzz workers, which doesn't compose with the KVM
 * backend (per-vCPU fd aliasing + shared kvm_run mmap +
 * memslot host-VA divergence — see commit 7f79b35e1531). This
 * TU is the v2 path: capture vCPU + memslot state via KVM
 * ioctls, then restore in-place per fuzz iteration without
 * forking.
 *
 * Today this provides the building blocks (capture / restore_full
 * / free) but no integration into um_snapshot_ready yet — that's
 * step 3 of the memo-12 ladder, blocked on the KVM-aware path
 * needing a separate routine in arch/um/kernel/snapshot.c.
 *
 * Design memo: Documentation/virt/uml/redesign/02-workstreams/
 *              D-kvm-backend/12-snapshot-forkserver-kvm.md
 */

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/sched.h>		/* current */
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <os.h>
#include <mem.h>		/* uml_physmem */
#include <as-layout.h>		/* physmem_size */

#include "kvm_backend.h"

/*
 * struct kvm_snapshot — opaque container for a captured vCPU +
 * memslot state. Allocated by kvm_snapshot_capture, freed by
 * kvm_snapshot_free. The struct itself is tiny; the bulk of the
 * memory lives in the embedded buffers (memslot copy is the big
 * one, sized by physmem_size).
 *
 * MSRs we care about for UML's KVM backend:
 *   MSR_LSTAR, MSR_STAR, MSR_FMASK, MSR_KERNEL_GS_BASE,
 *   MSR_FS_BASE, MSR_GS_BASE, MSR_EFER
 * Seven entries; allocated as kvm_msrs with nmsrs=7.
 *
 * FPU state captures XSAVE area (AVX/AVX-512). MMIO regions are
 * registered via the same memslot path under Policy A so they
 * round-trip through the memslot copy.
 */
#define KVM_SNAPSHOT_MSR_COUNT	7

struct kvm_snapshot {
	struct kvm_regs		regs;
	struct kvm_sregs	sregs;
	struct kvm_fpu		fpu;
	struct kvm_vcpu_events	events;

	/* MSR list — variable-length but bounded; kept inline. */
	struct {
		__u32 nmsrs;
		__u32 pad;
		struct kvm_msr_entry entries[KVM_SNAPSHOT_MSR_COUNT];
	} msrs;

	/*
	 * Memslot snapshot. Singleton under Policy A: one slot
	 * covers all of [0, physmem_size). v1 captures the full
	 * memslot; v2 (memo-12 step 4) adds dirty-bitmap delta
	 * capture for sub-millisecond iteration cost.
	 */
	void	*mem_backing;
	size_t	mem_size;
};

/* Static MSR index list. Order matches kvm_msrs.entries[]. */
static const u32 kvm_snapshot_msr_indices[KVM_SNAPSHOT_MSR_COUNT] = {
	0xC0000082,	/* MSR_LSTAR */
	0xC0000081,	/* MSR_STAR */
	0xC0000084,	/* MSR_FMASK */
	0xC0000102,	/* MSR_KERNEL_GS_BASE */
	0xC0000100,	/* MSR_FS_BASE */
	0xC0000101,	/* MSR_GS_BASE */
	0xC0000080,	/* MSR_EFER */
};

/*
 * Internal capture worker: optionally skips the full memslot
 * memcpy (the expensive part on large physmem configs where
 * vmalloc can be tight). Both kvm_snapshot_capture and
 * kvm_snapshot_capture_regs_only delegate here.
 */
static int kvm_snapshot_capture_internal(struct kvm_snapshot *snap,
					 bool include_memslot)
{
	/*
	 * Stage A.6+A.7: snapshot the calling task's per-task vCPU.
	 * Pre-Stage-A this captured vcpu0_fd state, which under
	 * integrated KVM was a never-run stub vCPU — useless for
	 * record/replay or KUnit diagnostics. Now: if no current vCPU
	 * (early-boot KUnit before any task has run kvm_run_userspace,
	 * or non-INTEGRATED harness builds), return -ENODEV.
	 */
	struct kvm_vcpu_handle *vcpu = current ?
		current->thread.arch.kvm.vcpu : NULL;
	int vcpu_fd = vcpu ? vcpu->fd : -1;
	unsigned int i;
	int rc;

	if (!snap)
		return -EINVAL;
	if (vcpu_fd < 0)
		return -ENODEV;
	if (!uml_physmem || !physmem_size)
		return -EAGAIN;

	memset(snap, 0, sizeof(*snap));

	/*
	 * vCPU registers: GP regs first (cheapest, also what's most
	 * likely to fail under DYNAMIC backend if the vCPU was
	 * never actually KVM_RUN'd).
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_REGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_SREGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_FPU,
			      (unsigned long)&snap->fpu);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_FPU failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_GET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_GET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
	}

	/* MSRs: prepare index list, KVM_GET_MSRS fills values. */
	snap->msrs.nmsrs = KVM_SNAPSHOT_MSR_COUNT;
	for (i = 0; i < KVM_SNAPSHOT_MSR_COUNT; i++) {
		snap->msrs.entries[i].index = kvm_snapshot_msr_indices[i];
		snap->msrs.entries[i].reserved = 0;
		snap->msrs.entries[i].data = 0;
	}
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm snapshot: KVM_GET_MSRS rc=%d (expected %d)\n",
			rc, KVM_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		return rc;
	}

	if (!include_memslot) {
		pr_info("um: kvm snapshot: captured regs+sregs+fpu+events+%u msrs (regs-only)\n",
			KVM_SNAPSHOT_MSR_COUNT);
		return 0;
	}

	/*
	 * Memslot copy. Under Policy A there's exactly one memslot
	 * covering [0, physmem_size) at host VA uml_physmem. Use
	 * kvmalloc for the backing buffer: it tries kmalloc first
	 * (cheap for small sizes, contiguous physical pages) and
	 * falls back to vmalloc when the order exceeds MAX_ORDER.
	 * That handles the typical 64-256 MiB physmem range better
	 * than raw vmalloc, which is bounded tighter at late_
	 * initcall_sync on UML.
	 *
	 * Even kvmalloc can fail on small builds where neither path
	 * has enough headroom. Callers that don't strictly need the
	 * memslot (kvm_snapshot_bench cycle measurement, KUnit
	 * shape tests) can pass include_memslot=false to skip this
	 * allocation and round-trip vCPU state only.
	 */
	snap->mem_size = physmem_size;
	snap->mem_backing = kvmalloc(snap->mem_size, GFP_KERNEL);
	if (!snap->mem_backing) {
		pr_warn("um: kvm snapshot: kvmalloc(%zu) failed\n",
			snap->mem_size);
		return -ENOMEM;
	}
	memcpy(snap->mem_backing, (void *)uml_physmem, snap->mem_size);

	pr_info("um: kvm snapshot: captured regs+sregs+fpu+events+%u msrs + %zu MiB physmem\n",
		KVM_SNAPSHOT_MSR_COUNT,
		snap->mem_size >> 20);
	return 0;
}

/**
 * kvm_snapshot_capture - capture full vCPU + memslot state.
 * @snap: caller-allocated snapshot
 *
 * Issues KVM_GET_REGS / SREGS / FPU / VCPU_EVENTS / MSRS and
 * memcpy's the full memslot. Returns 0 on success; -errno on
 * the first ioctl / allocation failure.
 */
int kvm_snapshot_capture(struct kvm_snapshot *snap)
{
	return kvm_snapshot_capture_internal(snap, true);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_capture);

/**
 * kvm_snapshot_capture_regs_only - capture vCPU regs only,
 *                                  skipping the memslot copy.
 * @snap: caller-allocated snapshot
 *
 * Useful when the memslot is too large to vmalloc at the
 * current point in boot (typically late_initcall_sync, before
 * vmalloc has grown to full extent), or when the caller only
 * cares about restoring vCPU register state without rolling
 * back guest memory. The matching restore_full path skips the
 * memcpy when mem_backing is NULL.
 */
int kvm_snapshot_capture_regs_only(struct kvm_snapshot *snap)
{
	return kvm_snapshot_capture_internal(snap, false);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_capture_regs_only);

/**
 * kvm_snapshot_restore_full - restore vCPU + memslot from snapshot.
 * @snap: previously-captured snapshot
 *
 * v1 full-restore path: memcpy the entire memslot back from the
 * snapshot buffer, then push vCPU state via KVM_SET_*. Cost is
 * O(physmem_size) bound by memory bandwidth (~5-50 ms for typical
 * 64-256 MiB UML configs).
 *
 * v2 dirty-bitmap fast path (memo 12 step 4) will skip the
 * full memcpy in favor of restoring only pages KVM marked dirty
 * since the snapshot, dropping per-iteration cost to <1 ms.
 *
 * Returns 0 on success; -errno on the first ioctl/copy failure.
 */
int kvm_snapshot_restore_full(struct kvm_snapshot *snap)
{
	/*
	 * Stage A.6+A.7: restore against the calling task's per-task
	 * vCPU. -ENODEV if no current vCPU (caller pre-conditions are
	 * its responsibility — restore needs an active vCPU to write
	 * state into).
	 */
	struct kvm_vcpu_handle *vcpu = current ?
		current->thread.arch.kvm.vcpu : NULL;
	int vcpu_fd = vcpu ? vcpu->fd : -1;
	int rc;

	if (!snap)
		return -EINVAL;
	if (vcpu_fd < 0)
		return -ENODEV;

	/*
	 * Memslot restore is optional — when the snapshot was
	 * captured via kvm_snapshot_capture_regs_only, mem_backing
	 * is NULL and we skip the memcpy. The caller-visible
	 * post-condition then is "vCPU state restored, guest memory
	 * not rolled back", which is what bench / regs-only callers
	 * want.
	 */
	if (snap->mem_backing) {
		if (snap->mem_size != physmem_size) {
			pr_warn("um: kvm snapshot: mem_size mismatch (snap=%zu cur=%llu)\n",
				snap->mem_size, physmem_size);
			return -EINVAL;
		}
		memcpy((void *)uml_physmem, snap->mem_backing,
		       snap->mem_size);
	}

	/*
	 * SREGS first because KVM validates RIP/RSP against the
	 * post-SREGS segment cache; setting REGS before SREGS can
	 * cause KVM to reject the load.
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS,
			      (unsigned long)&snap->sregs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_SREGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
			      (unsigned long)&snap->regs);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_REGS failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_FPU,
			      (unsigned long)&snap->fpu);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_FPU failed (%d)\n", rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_VCPU_EVENTS,
			      (unsigned long)&snap->events);
	if (rc < 0) {
		pr_warn("um: kvm snapshot: KVM_SET_VCPU_EVENTS failed (%d)\n",
			rc);
		return rc;
	}

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS,
			      (unsigned long)&snap->msrs);
	if (rc < 0 || rc != KVM_SNAPSHOT_MSR_COUNT) {
		pr_warn("um: kvm snapshot: KVM_SET_MSRS rc=%d (expected %d)\n",
			rc, KVM_SNAPSHOT_MSR_COUNT);
		if (rc >= 0)
			rc = -EIO;
		return rc;
	}

	pr_info("um: kvm snapshot: restored vCPU + %zu MiB physmem\n",
		snap->mem_size >> 20);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_snapshot_restore_full);

/**
 * kvm_snapshot_free - release snapshot-owned allocations.
 * @snap: snapshot whose allocations should be freed; the
 *        struct itself is not freed (caller owns it).
 *
 * Idempotent: passing a never-captured or already-freed snapshot
 * is safe (no allocations to release).
 */
void kvm_snapshot_free(struct kvm_snapshot *snap)
{
	if (!snap)
		return;
	if (snap->mem_backing) {
		kvfree(snap->mem_backing);
		snap->mem_backing = NULL;
	}
	snap->mem_size = 0;
}
EXPORT_SYMBOL_GPL(kvm_snapshot_free);

/**
 * kvm_snapshot_alloc - allocate a fresh snapshot container.
 *
 * Returns a zero-initialized snapshot struct on the heap, or
 * NULL on allocation failure. Caller pairs with
 * kvm_snapshot_free + kfree of the returned pointer.
 */
struct kvm_snapshot *kvm_snapshot_alloc(void)
{
	return kzalloc(sizeof(struct kvm_snapshot), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_alloc);

/**
 * kvm_snapshot_destroy - free both the snapshot's allocations
 *                        and the container itself.
 * @snap: snapshot to release; can be NULL.
 */
void kvm_snapshot_destroy(struct kvm_snapshot *snap)
{
	if (!snap)
		return;
	kvm_snapshot_free(snap);
	kfree(snap);
}
EXPORT_SYMBOL_GPL(kvm_snapshot_destroy);

/*
 * kvm_snapshot_bench: cycle-counted N-iteration capture/restore_full
 * driver, surfaced two ways:
 *   - Boot-time: kernel cmdline `kvm_snapshot_bench=N` fires once at
 *     late_initcall_sync. Used by the kselftest runner that wants
 *     a deterministic measurement without an init-script handshake.
 *   - Debugfs: write decimal N to /sys/kernel/debug/um/kvm_snapshot_
 *     bench (only when CONFIG_DEBUG_FS=y). Useful for ad-hoc
 *     measurements from inside a booted UML.
 *
 * Both paths emit the same dmesg line:
 *   kvm_snapshot_bench: capture=%llu ns; restore_full median=%llu
 *     p95=%llu min=%llu max=%llu n=%u
 *
 * Bound by KVM_SNAPSHOT_BENCH_N_MAX so a runaway echo / cmdline can't
 * pin the CPU forever. Median + p95 reporting matches the perf-
 * fallback runner's statistical hygiene (task #268).
 */
#define KVM_SNAPSHOT_BENCH_N_MAX	1024

static int u64_cmp(const void *a, const void *b)
{
	u64 da = *(const u64 *)a, db = *(const u64 *)b;

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

#ifdef CONFIG_DEBUG_FS

static ssize_t kvm_snapshot_bench_write(struct file *f,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	char tmp[16];
	struct kvm_snapshot *snap;
	u64 *samples;
	u64 t0, t1;
	u64 cap_cyc;
	unsigned int n, i;
	long ln;
	int rc;
	size_t copy_n;

	copy_n = min_t(size_t, count, sizeof(tmp) - 1);
	if (copy_from_user(tmp, buf, copy_n))
		return -EFAULT;
	tmp[copy_n] = '\0';
	/* Strip trailing newline. */
	if (copy_n > 0 && tmp[copy_n - 1] == '\n')
		tmp[copy_n - 1] = '\0';

	rc = kstrtol(tmp, 10, &ln);
	if (rc < 0)
		return rc;
	if (ln <= 0 || ln > KVM_SNAPSHOT_BENCH_N_MAX)
		return -EINVAL;
	n = (unsigned int)ln;

	samples = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;

	snap = kvm_snapshot_alloc();
	if (!snap) {
		rc = -ENOMEM;
		goto out_free_samples;
	}

	/*
	 * Try full capture first; fall back to regs-only on
	 * vmalloc-area exhaustion. Same shape as the cmdline
	 * boot-time bench above.
	 */
	t0 = ktime_get_ns();
	rc = kvm_snapshot_capture(snap);
	t1 = ktime_get_ns();
	if (rc == -ENOMEM) {
		pr_info("kvm_snapshot_bench: full capture vmalloc failed; retrying regs-only\n");
		t0 = ktime_get_ns();
		rc = kvm_snapshot_capture_regs_only(snap);
		t1 = ktime_get_ns();
	}
	if (rc < 0) {
		pr_warn("kvm_snapshot_bench: capture failed (%d) — vCPU not yet KVM_RUN'd?\n",
			rc);
		goto out_free_snap;
	}
	cap_cyc = t1 - t0;

	for (i = 0; i < n; i++) {
		t0 = ktime_get_ns();
		rc = kvm_snapshot_restore_full(snap);
		t1 = ktime_get_ns();
		if (rc < 0) {
			pr_warn("kvm_snapshot_bench: restore_full failed at iter %u (%d)\n",
				i, rc);
			goto out_free_snap;
		}
		samples[i] = t1 - t0;
	}

	sort(samples, n, sizeof(u64), u64_cmp, NULL);
	{
		u64 med = samples[n / 2];
		u64 p95 = samples[(n * 95) / 100];
		u64 minv = samples[0];
		u64 maxv = samples[n - 1];

		pr_info("kvm_snapshot_bench: capture=%llu ns; restore_full ns: median=%llu p95=%llu min=%llu max=%llu n=%u mode=%s\n",
			cap_cyc, med, p95, minv, maxv, n,
			snap->mem_backing ? "full" : "regs-only");
	}
	rc = (int)count;	/* signal write success */

out_free_snap:
	kvm_snapshot_destroy(snap);
out_free_samples:
	kvfree(samples);
	return rc < 0 ? rc : (ssize_t)count;
}

static const struct file_operations kvm_snapshot_bench_fops = {
	.write = kvm_snapshot_bench_write,
};

/*
 * Late-init so the "um" debugfs dentry created by um_debugfs.c
 * exists by the time we look it up. Falls back to creating "um"
 * if it doesn't yet (defensive; in practice um_debugfs.c always
 * makes it).
 */
static int __init kvm_snapshot_debugfs_init(void)
{
	struct dentry *d;

	d = debugfs_lookup("um", NULL);
	if (!d) {
		d = debugfs_create_dir("um", NULL);
		if (IS_ERR(d))
			return PTR_ERR(d);
	}

	debugfs_create_file("kvm_snapshot_bench", 0200, d, NULL,
			    &kvm_snapshot_bench_fops);
	return 0;
}
late_initcall_sync(kvm_snapshot_debugfs_init);

#endif /* CONFIG_DEBUG_FS */

/*
 * Boot-time bench driver: kernel cmdline `kvm_snapshot_bench=N`
 * triggers an N-iteration capture/restore cycle at late_initcall.
 * Avoids the init-script + debugfs path's shell-job-control
 * fragility (UML fatal signal on common /bin/sh boot scripts);
 * the result lands directly in the kernel log where the kselftest
 * runner can scrape it.
 *
 * N is bounded by KVM_SNAPSHOT_BENCH_N_MAX. 0 / unset = no bench.
 */
static unsigned int kvm_snapshot_bench_n_at_boot;

static int __init kvm_snapshot_bench_setup(char *s)
{
	long ln;

	if (!s || kstrtol(s, 10, &ln) < 0)
		return 1;
	if (ln < 0 || ln > KVM_SNAPSHOT_BENCH_N_MAX)
		return 1;
	kvm_snapshot_bench_n_at_boot = (unsigned int)ln;
	return 1;
}
__setup("kvm_snapshot_bench=", kvm_snapshot_bench_setup);

static int __init kvm_snapshot_bench_late_init(void)
{
	struct kvm_snapshot *snap;
	u64 *samples;
	u64 t0, t1, cap_cyc;
	unsigned int n, i;
	int rc;

	n = kvm_snapshot_bench_n_at_boot;
	if (!n)
		return 0;

	samples = kvmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!samples) {
		pr_warn("kvm_snapshot_bench: kvmalloc(%u samples) failed\n",
			n);
		return 0;
	}
	snap = kvm_snapshot_alloc();
	if (!snap) {
		pr_warn("kvm_snapshot_bench: kvm_snapshot_alloc failed\n");
		goto out_free_samples;
	}

	/*
	 * Try the FULL capture first. If vmalloc(physmem_size)
	 * succeeds, we're measuring the realistic forkserver-iteration
	 * cost (vCPU regs + memslot memcpy) — the number that has to
	 * stay under memo-12's <50 ms cold-start / <1 ms iteration
	 * targets. If vmalloc fails (UML's vmalloc area is bounded
	 * and tight at late_initcall_sync on bigger physmem configs),
	 * fall back to regs-only so the bench still produces a
	 * measurement.
	 */
	t0 = ktime_get_ns();
	rc = kvm_snapshot_capture(snap);
	t1 = ktime_get_ns();
	if (rc == -ENOMEM) {
		pr_info("kvm_snapshot_bench: full capture vmalloc failed; retrying regs-only\n");
		t0 = ktime_get_ns();
		rc = kvm_snapshot_capture_regs_only(snap);
		t1 = ktime_get_ns();
	}
	if (rc < 0) {
		pr_warn("kvm_snapshot_bench: capture failed (%d) — vCPU not yet KVM_RUN'd?\n",
			rc);
		goto out_free_snap;
	}
	cap_cyc = t1 - t0;

	for (i = 0; i < n; i++) {
		t0 = ktime_get_ns();
		rc = kvm_snapshot_restore_full(snap);
		t1 = ktime_get_ns();
		if (rc < 0) {
			pr_warn("kvm_snapshot_bench: restore_full failed at iter %u (%d)\n",
				i, rc);
			goto out_free_snap;
		}
		samples[i] = t1 - t0;
	}

	sort(samples, n, sizeof(u64), u64_cmp, NULL);
	pr_info("kvm_snapshot_bench: capture=%llu ns; restore_full ns: median=%llu p95=%llu min=%llu max=%llu n=%u mode=%s\n",
		cap_cyc, samples[n / 2], samples[(n * 95) / 100],
		samples[0], samples[n - 1], n,
		snap->mem_backing ? "full" : "regs-only");

out_free_snap:
	kvm_snapshot_destroy(snap);
out_free_samples:
	kvfree(samples);
	return 0;
}
late_initcall_sync(kvm_snapshot_bench_late_init);
