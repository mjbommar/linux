// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — lifecycle (probe, init, shutdown).
 *
 * Workstream D-03a: real /dev/kvm probe.
 * Workstream D-03b: eager KVM_CREATE_VM in init(); per-mm state
 * lives in mm.c and refcounts the single shared VM per the
 * decisions-log D57 one-VM-per-UML-process model.
 * Workstream D-03c: single giant memslot covering the UML
 * address space registered at init time per the D-03c memslot-
 * policy design note (Policy A). mm_map / mm_unmap then reduce
 * to host-side mmap / munmap (follow-on commit); the memslot
 * itself is static after init.
 *
 *   - probe()    opens /dev/kvm and issues KVM_GET_API_VERSION
 *                to confirm the host kernel speaks the stable
 *                ABI version (12, since 2.6.22). On failure,
 *                returns the errno so the arbiter can fall back
 *                per the A-01 contract. Does not retain host
 *                state.
 *   - init()     reopens /dev/kvm, issues KVM_CREATE_VM, then
 *                registers a single KVM_USER_MEMORY_REGION
 *                slot covering guest-physical [0, task_size)
 *                mapped identity-style to host-VA [0, task_size).
 *                Stashes kvm_fd + vm_fd in the module-static
 *                kvm_um for the rest of the backend.
 *   - shutdown() closes vm_fd then kvm_fd. KVM tears down
 *                memslots on vm_fd close; no explicit free.
 *
 * No USER TU is needed at this layer — os_open_file() and
 * os_ioctl_generic() both run in kernel context on UML and call
 * through to host libc via the os-Linux glue. The D-02-draft
 * USER-side lifecycle_user.c that tried to do this in the USER
 * link context segfaulted during early-probe; the kernel-side
 * approach here avoids that class of failure entirely.
 *
 * Subsequent D-03c lands memslot plumbing (KVM_SET_USER_MEMORY_
 * REGION for mm_map/mm_unmap); D-04 creates vCPUs on top.
 */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/ktime.h>
#include <linux/time.h>

#include <asm/page.h>
#include <os.h>
#include <mem.h>			/* uml_physmem */
#include <as-layout.h>			/* physmem_size */
#include <asm/backend.h>
#include <asm/processor-generic.h>	/* task_size */
#include <shared/smp.h>			/* uml_ncpus */

#include "kvm_backend.h"

/*
 * Single per-UML-process KVM context. Lifetime matches
 * init_backend() → uml_cleanup() / backend shutdown(). The
 * mm_attach/mm_detach refcount that ties UML mm lifetime into
 * this struct lives in mm.c; everything kvm_um exposes here is
 * read-only after init().
 */
static struct kvm_um kvm_ctx = {
	.kvm_fd		= -1,
	.vm_fd		= -1,
	.vcpu0_fd	= -1,
	.run0		= NULL,
	.run_size	= 0,
};

int kvm_probe(void)
{
	int fd, api;

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0) {
		pr_info("um: kvm probe: /dev/kvm open failed (%d); falling back\n",
			fd);
		return fd;
	}

	api = os_ioctl_generic(fd, KVM_GET_API_VERSION, 0);
	if (api < 0) {
		pr_warn("um: kvm probe: KVM_GET_API_VERSION failed (%d)\n",
			api);
		os_close_file(fd);
		return api;
	}
	if (api != KVM_API_VERSION) {
		pr_warn("um: kvm probe: host API version %d, built for %d\n",
			api, KVM_API_VERSION);
		os_close_file(fd);
		return -EOPNOTSUPP;
	}

	/*
	 * Probe is a yes/no test for the arbiter. Don't retain the fd
	 * here — reopen in init() once the arbiter has committed to
	 * this backend. Cheap (two syscalls) and avoids a "probe-held
	 * fd leaks if init() fails later" class of bug.
	 */
	os_close_file(fd);
	pr_info("um: kvm probe: /dev/kvm OK (API %d)\n", api);
	return 0;
}

int kvm_init(const struct um_backend_args *args)
{
	int kfd, vmfd;

	(void)args;

	if (kvm_ctx.kvm_fd >= 0) {
		pr_warn("um: kvm init called twice (kvm_fd %d already open)\n",
			kvm_ctx.kvm_fd);
		return -EBUSY;
	}

	/*
	 * Audit round-6 G4: enforce ncpus=1 for the KVM backend.
	 * Same constraint ptrace announces in os-Linux/start_up.c:605
	 * for its own probe. The KVM backend is single-vCPU by
	 * design (vcpu0 is the only vCPU created below) and
	 * run_userspace serializes around the single vcpu0_fd via
	 * KVM_RUN — multi-CPU UML on this backend would have multiple
	 * guest tasks contending for the same vCPU, producing
	 * undefined ordering of guest state.
	 *
	 * If/when SMP support lands, this check moves to per-vCPU
	 * creation in thread_start_idle and the vcpu0_fd singleton
	 * becomes a per-cpu lookup. For now, fail loud at backend
	 * init time with a clear diagnostic.
	 */
	if (uml_ncpus > 1) {
		pr_err("um: kvm init: SMP not supported (ncpus=%d > 1); falling back\n",
		       uml_ncpus);
		return -EOPNOTSUPP;
	}

	kfd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (kfd < 0) {
		pr_err("um: kvm init: /dev/kvm reopen failed (%d) — probe succeeded?\n",
		       kfd);
		return kfd;
	}

	/*
	 * KVM_CREATE_VM with machine-type 0 = default (x86_64 long-
	 * mode VM on Intel/AMD). Per-process, shared across every UML
	 * guest mm; individual UML address spaces are isolated via
	 * CR3 switching on context_switch (D-04), not via separate
	 * VMs. See decisions-log D57.
	 */
	vmfd = os_ioctl_generic(kfd, KVM_CREATE_VM, 0);
	if (vmfd < 0) {
		pr_err("um: kvm init: KVM_CREATE_VM failed (%d)\n", vmfd);
		os_close_file(kfd);
		return vmfd;
	}
	pr_info("um: kvm init: KVM_CREATE_VM ok vmfd=%d\n", vmfd);

	/*
	 * Memslot registration was originally here (D-03c) but
	 * uml_physmem / physmem_size are set by arch_setup() AFTER
	 * init_backend() returns — see arch/um/kernel/um_arch.c
	 * linux_main() line ordering. At this point both globals are
	 * 0, so we can't compute the right userspace_addr /
	 * memory_size. D-04a defers registration to
	 * kvm_ensure_memslot() below, invoked on first run_userspace
	 * call (by which time memory layout is finalized).
	 */

	/*
	 * D-04a: create the first vCPU now. For ncpus=1 UML (the
	 * default) this is the only vCPU; SMP moves creation to
	 * thread_start_idle per 04-ring-transition.md. The run
	 * structure is mmap'd from the vcpu_fd — KVM_GET_VCPU_MMAP_
	 * SIZE reports the size first. On error, close everything
	 * and fall back.
	 */
	{
		int vcpu_fd, mmap_size;
		void *run;

		vcpu_fd = os_ioctl_generic(vmfd, KVM_CREATE_VCPU, 0);
		if (vcpu_fd < 0) {
			pr_err("um: kvm init: KVM_CREATE_VCPU failed (%d)\n",
			       vcpu_fd);
			os_close_file(vmfd);
			os_close_file(kfd);
			return vcpu_fd;
		}

		mmap_size = os_ioctl_generic(kfd, KVM_GET_VCPU_MMAP_SIZE, 0);
		if (mmap_size <= 0) {
			pr_err("um: kvm init: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
			       mmap_size);
			os_close_file(vcpu_fd);
			os_close_file(vmfd);
			os_close_file(kfd);
			return mmap_size ? mmap_size : -EIO;
		}

		run = os_mmap_rw_shared(vcpu_fd, mmap_size);
		if (!run) {
			pr_err("um: kvm init: mmap of kvm_run (size %d) failed\n",
			       mmap_size);
			os_close_file(vcpu_fd);
			os_close_file(vmfd);
			os_close_file(kfd);
			return -ENOMEM;
		}

		kvm_ctx.vcpu0_fd = vcpu_fd;
		kvm_ctx.run0     = run;
		kvm_ctx.run_size = mmap_size;
	}

	kvm_ctx.kvm_fd = kfd;
	kvm_ctx.vm_fd  = vmfd;
	refcount_set(&kvm_ctx.mm_refcount, 0);

	/*
	 * Perf-lever #2: probe KVM_CAP_SYNC_REGS. When supported,
	 * GP regs + RIP + RFLAGS travel through the mmap'd kvm_run
	 * struct at run->s.regs.regs, eliminating KVM_GET_REGS +
	 * KVM_SET_REGS ioctls on the per-syscall hot path. The cap
	 * returns a bitmap of supported reg classes; on x86 that's
	 * KVM_SYNC_X86_REGS (bit 0) / _SREGS (bit 1) / _EVENTS (bit
	 * 2). A zero return means the feature is unsupported and
	 * we fall back to the ioctl path unconditionally.
	 */
	{
		int caps = os_ioctl_generic(kfd, KVM_CHECK_EXTENSION,
					    KVM_CAP_SYNC_REGS);

		kvm_ctx.sync_regs_caps = caps > 0 ? (u64)caps : 0;
	}

	/*
	 * Vision §"Research mode" + task #255: probe + log vPMU
	 * capability. KVM enables vPMU by default on x86 — the
	 * guest can read MSR_IA32_PERFCTR0 etc. and rdpmc reaches
	 * the host's actual PMU counters, not emulated values.
	 * Logging the cap value at init means downstream perf
	 * tools / research builds can confirm the channel is
	 * live. This is a probe-only step today; tuning (event
	 * filtering, KVM_SET_PMU_EVENT_FILTER) is a future
	 * follow-on if research workloads need narrower windows.
	 */
	{
		int pmu = os_ioctl_generic(kfd, KVM_CHECK_EXTENSION,
					   KVM_CAP_PMU_CAPABILITY);
		int filt = os_ioctl_generic(kfd, KVM_CHECK_EXTENSION,
					    KVM_CAP_PMU_EVENT_FILTER);

		kvm_ctx.pmu_caps = pmu > 0 ? (u32)pmu : 0;
		kvm_ctx.pmu_event_filter_supported = filt > 0;
	}

	pr_info("um: kvm init: kvm=%d vm=%d vcpu0=%d run_size=%zu sync_regs_caps=0x%llx pmu_caps=0x%x pmu_filter=%s (memslot deferred to first KVM_RUN)\n",
		kvm_ctx.kvm_fd, kvm_ctx.vm_fd, kvm_ctx.vcpu0_fd,
		kvm_ctx.run_size,
		(unsigned long long)kvm_ctx.sync_regs_caps,
		kvm_ctx.pmu_caps,
		kvm_ctx.pmu_event_filter_supported ? "yes" : "no");

	/*
	 * D-05 nested-virt detection (memo 08 sub-commit #7, minimal
	 * form). CPUID leaf 1, ECX bit 31 = "hypervisor present"
	 * (Intel SDM vol 2). When set, UML is running inside a
	 * virtualisation layer itself; the KVM-within-KVM nested
	 * path typically costs more per VMEXIT than seccomp's
	 * SIGSYS/futex round-trip on the host it actually runs on.
	 *
	 * Operators who explicitly asked for KVM (force=kvm) get it
	 * anyway — the warning here is advisory, not punitive.
	 * Full measurement-based auto-fallback lives in
	 * 05-nested-virt-fallback.md's follow-on work; this is the
	 * "tell the user" baseline.
	 */
	{
		unsigned int eax, ebx, ecx, edx;

		asm volatile("cpuid"
			     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
			     : "a"(1U), "c"(0U));
		if (ecx & (1U << 31))
			pr_warn("um: kvm init: host reports hypervisor-present (CPUID.1:ECX.bit31=1) — nested KVM typically slower than seccomp; consider backend=seccomp if this workload is perf-sensitive\n");
	}
	/*
	 * Shadow_pgd alloc is deferred to first use (memo 09 step 1
	 * lazy-init pattern, mirroring kvm_ensure_memslot). kvm_init
	 * fires from init_backend() during linux_main(), which is
	 * before mm_init() / buddy allocator bring-up — calling
	 * alloc_page() here crashes with "UML: fatal signal" before
	 * start_kernel even runs. First caller that actually needs
	 * shadow_pgd (kvm_enter_guest under memo 09 step 2, or the
	 * A-05 KUnit force-probe) invokes kvm_shadow_pgd_alloc().
	 */

	/*
	 * Harness invocation relocated to a late_initcall in
	 * harness.c (D-04b.2b.1, unblocked by D-05a's real time
	 * ops). By late_initcall firing, uml_physmem /
	 * physmem_size are populated, which D-04b.2b.2's RIP-into-
	 * UML-text arithmetic needs. kvm_init() always returns
	 * normally now; CONFIG_UM_BACKEND_KVM_HARNESS builds just
	 * delay the harness-and-panic until late_initcall time.
	 */

	return 0;
}

void kvm_shutdown(void)
{
	/*
	 * Order: shadow PT (if allocated) → vCPU resources → VM →
	 * /dev/kvm. Shadow PT teardown first because its pages are
	 * GFP-managed and independent of KVM fds; doing it before
	 * closing fds keeps the dependency graph linear.
	 * mmap of kvm_run survives the vcpu_fd close (the mapping
	 * is refcounted in the kernel), so we munmap first via
	 * os_unmap_memory() before closing the fd.
	 */
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	kvm_shadow_pgd_free();
	kvm_gadget_state_free();
	kvm_gadget_vvar_free();
#endif
	if (kvm_ctx.run0) {
		os_unmap_memory(kvm_ctx.run0, kvm_ctx.run_size);
		kvm_ctx.run0 = NULL;
		kvm_ctx.run_size = 0;
	}
	if (kvm_ctx.vcpu0_fd >= 0) {
		os_close_file(kvm_ctx.vcpu0_fd);
		kvm_ctx.vcpu0_fd = -1;
	}
	if (kvm_ctx.vm_fd >= 0) {
		os_close_file(kvm_ctx.vm_fd);
		kvm_ctx.vm_fd = -1;
	}
	if (kvm_ctx.kvm_fd >= 0) {
		os_close_file(kvm_ctx.kvm_fd);
		kvm_ctx.kvm_fd = -1;
	}
}

/*
 * Lazy memslot registration. D-04a defers this out of kvm_init()
 * because uml_physmem / physmem_size aren't set at that point —
 * linux_main() runs init_backend() first, then arch_setup() which
 * populates those globals. run_userspace() (D-04a) calls this
 * once, before the first KVM_RUN, at which point memory layout
 * is final.
 *
 * Returns 0 on success (or if already registered), -errno on
 * failure. Caller decides whether to proceed without a slot.
 */
int kvm_ensure_memslot(void)
{
	static bool registered;
	struct kvm_userspace_memory_region region;
	int rc;

	if (registered)
		return 0;
	if (kvm_ctx.vm_fd < 0)
		return -EIO;
	if (!uml_physmem || !physmem_size) {
		pr_warn("um: kvm memslot: uml_physmem=%lx physmem_size=%llx — boot ordering bug?\n",
			uml_physmem, physmem_size);
		return -EAGAIN;
	}

	region = (struct kvm_userspace_memory_region){
		.slot			= 0,
		.flags			= 0,
		.guest_phys_addr	= 0,
		.memory_size		= physmem_size,
		.userspace_addr		= uml_physmem,
	};
	rc = os_ioctl_generic(kvm_ctx.vm_fd, KVM_SET_USER_MEMORY_REGION,
			      (unsigned long)&region);
	if (rc < 0) {
		pr_err("um: kvm memslot: KVM_SET_USER_MEMORY_REGION(uaddr=%lx size=%llx) failed (%d)\n",
		       uml_physmem, physmem_size, rc);
		return rc;
	}

	registered = true;
	pr_info("um: kvm memslot: guest_phys=0 host_va=%lx size=%llx\n",
		uml_physmem, physmem_size);
	return 0;
}

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
/*
 * Shadow page table lifecycle (memo 09 step 1).
 *
 * Singleton per-UML-process PGD for the KVM_INTEGRATED build;
 * future per-mm variants live on mm_id once step 2 lands the
 * fault-in + context_switch wiring.
 *
 * Allocated from normal GFP_KERNEL pages — the shadow PT is a
 * plain x86-hardware-walkable 4-level page table. PFN is the
 * host physical page's PFN; under Policy A memslot (gpa =
 * host_va - uml_physmem) that's what the guest CR3 should
 * resolve. kvm_shadow_pgd_gpa() hands out `__pa(pgd)`, ready
 * to load into kvm_regs.cr3.
 *
 * Empty zeroed PGD at allocation — step 2 fills in the fixed
 * bootstrap mapping (GDT + LSTAR + SYSRET page), step 3 fills
 * user-VA entries on fault.
 */
int kvm_shadow_pgd_alloc(void)
{
	struct page *page;

	if (kvm_ctx.shadow_pgd) {
		pr_warn_once("um: kvm shadow_pgd already allocated (va=%p gpa=0x%llx)\n",
			     kvm_ctx.shadow_pgd,
			     (unsigned long long)kvm_ctx.shadow_pgd_gpa);
		return 0;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		pr_err("um: kvm shadow_pgd: alloc_page failed\n");
		return -ENOMEM;
	}

	kvm_ctx.shadow_pgd_page = page;
	kvm_ctx.shadow_pgd      = page_address(page);
	kvm_ctx.shadow_pgd_gpa  = (u64)__pa(kvm_ctx.shadow_pgd);

	pr_info("um: kvm shadow_pgd: va=%p gpa=0x%llx (memo 09 step 1)\n",
		kvm_ctx.shadow_pgd,
		(unsigned long long)kvm_ctx.shadow_pgd_gpa);
	return 0;
}

void kvm_shadow_pgd_free(void)
{
	if (!kvm_ctx.shadow_pgd_page)
		return;
	__free_page(kvm_ctx.shadow_pgd_page);
	kvm_ctx.shadow_pgd_page = NULL;
	kvm_ctx.shadow_pgd      = NULL;
	kvm_ctx.shadow_pgd_gpa  = 0;
}

u64 kvm_shadow_pgd_gpa(void)
{
	return kvm_ctx.shadow_pgd_gpa;
}

/*
 * Memo 11 G3 gadget state channel lifecycle. Same lazy-
 * alloc + __free_page shape as the shadow PT above. The
 * gadget_state_va field is populated by kvm_enter_guest
 * when it maps the page into the shadow PT; alloc_page
 * doesn't know the guest VA yet.
 */
int kvm_gadget_state_alloc(void)
{
	struct page *page;

	if (kvm_ctx.gadget_state_page) {
		pr_info_once("um: kvm gadget_state already allocated (va=%p gpa=0x%llx)\n",
			     kvm_ctx.gadget_state,
			     (unsigned long long)kvm_ctx.gadget_state_gpa);
		return 0;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		pr_err("um: kvm gadget_state: alloc_page failed\n");
		return -ENOMEM;
	}

	kvm_ctx.gadget_state_page = page;
	kvm_ctx.gadget_state      = page_address(page);
	kvm_ctx.gadget_state_gpa  = (u64)__pa(kvm_ctx.gadget_state);
	kvm_ctx.gadget_state_va   = 0;	/* filled in on first map */

	pr_info("um: kvm gadget_state: va=%p gpa=0x%llx (memo 11 G3)\n",
		kvm_ctx.gadget_state,
		(unsigned long long)kvm_ctx.gadget_state_gpa);
	return 0;
}

void kvm_gadget_state_free(void)
{
	if (!kvm_ctx.gadget_state_page)
		return;
	__free_page(kvm_ctx.gadget_state_page);
	kvm_ctx.gadget_state_page = NULL;
	kvm_ctx.gadget_state      = NULL;
	kvm_ctx.gadget_state_gpa  = 0;
	kvm_ctx.gadget_state_va   = 0;
}

u64 kvm_gadget_state_va(void)
{
	return kvm_ctx.gadget_state_va;
}

u64 kvm_gadget_state_gpa(void)
{
	return kvm_ctx.gadget_state_gpa;
}

/*
 * Rewrite the gadget state page from the current task.
 * Called from kvm_enter_guest right before KVM_RUN so
 * the guest's gadget handlers see up-to-date state. No
 * seqlock needed under ncpus=1 (single vCPU = single
 * writer, host is quiescent during KVM_RUN). Memo 11
 * §"Safety discipline" point 6 tracks the SMP v2
 * seqlock story.
 */
void kvm_gadget_state_refresh(void)
{
	struct kvm_gadget_state *s = kvm_ctx.gadget_state;
	const struct cred *c;

	if (!s)
		return;

	c = current_cred();
	s->seq     = 0;			/* SMP v2 reserved */
	s->cpu_id  = 0;			/* ncpus=1 only for v1 */
	s->tgid    = task_tgid_vnr(current);	/* getpid(2) semantics */
	s->tid     = task_pid_vnr(current);	/* gettid(2) semantics */
	s->ppid    = task_ppid_nr(current);
	s->uid     = from_kuid_munged(current_user_ns(), c->uid);
	s->euid    = from_kuid_munged(current_user_ns(), c->euid);
	s->gid     = from_kgid_munged(current_user_ns(), c->gid);
	s->egid    = from_kgid_munged(current_user_ns(), c->egid);
}

/*
 * Memo 11 G5 gadget vvar lifecycle. Parallel to the state-page
 * helpers above. Alloc is lazy (same shape as shadow_pgd /
 * gadget_state). Free is called from kvm_shutdown. Refresh
 * writes fresh CLOCK_MONOTONIC + CLOCK_REALTIME values under a
 * seqlock so the G5c gadget handler reads a consistent pair.
 */
int kvm_gadget_vvar_alloc(void)
{
	struct page *page;

	if (kvm_ctx.gadget_vvar_page) {
		pr_info_once("um: kvm gadget_vvar already allocated (va=%p gpa=0x%llx)\n",
			     kvm_ctx.gadget_vvar,
			     (unsigned long long)kvm_ctx.gadget_vvar_gpa);
		return 0;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		pr_err("um: kvm gadget_vvar: alloc_page failed\n");
		return -ENOMEM;
	}

	kvm_ctx.gadget_vvar_page = page;
	kvm_ctx.gadget_vvar      = page_address(page);
	kvm_ctx.gadget_vvar_gpa  = (u64)__pa(kvm_ctx.gadget_vvar);
	kvm_ctx.gadget_vvar_va   = 0;	/* filled in by kvm_enter_guest map */

	/*
	 * Audit round-6 G1 + G1-range-followon: seed task_size_cap
	 * once. UML's task_size is a global set in arch_setup()
	 * before any gadget runs, so a single write here covers
	 * every subsequent gadget invocation. No refresh needed
	 * because task_size doesn't change after boot.
	 *
	 * Range-aware bound: the gadget's `cmp ptr, %gs:cap; jbe
	 * fallback` is base-only — it checks `ptr` rather than
	 * `ptr + size - 1`. The largest write any gadget handler
	 * issues is 16 bytes (struct __kernel_timespec for
	 * clock_gettime). Subtract that ceiling from task_size so
	 * a base-only compare effectively covers the whole write
	 * range. Pointers in the last 16 bytes of user space route
	 * to the SYSCALL fallback (handle_syscall + the standard
	 * access_ok path), where access_ok's nowrap + size check
	 * either accepts or rejects per POSIX semantics. Net
	 * user-visible behaviour is unchanged; the gadget just
	 * doesn't service writes that span the very last 16 bytes
	 * of user space — a vanishingly rare case in practice.
	 */
	kvm_ctx.gadget_vvar->task_size_cap = task_size - 16;

	pr_info("um: kvm gadget_vvar: va=%p gpa=0x%llx task_size_cap=0x%llx (task_size=0x%lx, range-margin=16)\n",
		kvm_ctx.gadget_vvar,
		(unsigned long long)kvm_ctx.gadget_vvar_gpa,
		(unsigned long long)kvm_ctx.gadget_vvar->task_size_cap,
		task_size);
	return 0;
}

void kvm_gadget_vvar_free(void)
{
	if (!kvm_ctx.gadget_vvar_page)
		return;
	__free_page(kvm_ctx.gadget_vvar_page);
	kvm_ctx.gadget_vvar_page = NULL;
	kvm_ctx.gadget_vvar      = NULL;
	kvm_ctx.gadget_vvar_gpa  = 0;
	kvm_ctx.gadget_vvar_va   = 0;
}

u64 kvm_gadget_vvar_va(void)
{
	return kvm_ctx.gadget_vvar_va;
}

u64 kvm_gadget_vvar_gpa(void)
{
	return kvm_ctx.gadget_vvar_gpa;
}

/*
 * Write fresh monotonic + realtime timestamps into the vvar
 * page. Seqlock write pattern:
 *
 *   seq++ (now odd → "write in progress")
 *   store fields
 *   seq++ (now even → "stable")
 *
 * Guest-side readers (G5c handler asm) sample seq, check
 * even, read fields, re-sample seq, retry on mismatch.
 * Under ncpus=1 the host is quiescent during KVM_RUN so a
 * concurrent reader race is impossible — the seqlock
 * discipline here is future-proofing for the SMP v2 per-
 * vCPU vvar model and costs nothing (2 extra writes per
 * refresh).
 *
 * ktime_get_ns() returns a monotonic nanosecond count;
 * ktime_get_real_ts64() returns a timespec64. Both are
 * cheap UML-side (~1 us each per kvm_enter_guest, lost in
 * the ~3 us re-entry ioctl cost) and give us standard
 * wallclock + monotonic pairs without hooking UML's
 * timer tick machinery. Higher-frequency updates (via a
 * timer hook) are a post-v1 optimization tracked in memo
 * 11 §"Known limitations".
 */
void kvm_gadget_vvar_refresh(void)
{
	struct kvm_gadget_vvar *v = kvm_ctx.gadget_vvar;
	u64 mono_ns;
	struct timespec64 real_ts;

	if (!v)
		return;

	/* Enter the write critical section. */
	v->seq++;	/* odd → writer in progress */
	smp_wmb();

	mono_ns = ktime_get_ns();
	ktime_get_real_ts64(&real_ts);

	v->monotonic_sec  = (s64)(mono_ns / NSEC_PER_SEC);
	v->monotonic_nsec = (s64)(mono_ns % NSEC_PER_SEC);
	v->realtime_sec   = real_ts.tv_sec;
	v->realtime_nsec  = real_ts.tv_nsec;
	/*
	 * Audit round-5 F8: replenish the gadget call budget.
	 * Every gadget clock_gettime decrements this; when it
	 * goes negative the gadget falls back to the
	 * handle_syscall path, which triggers the next
	 * kvm_enter_guest and thus the next refresh — bounding
	 * vvar staleness to at most KVM_VVAR_BUDGET_INITIAL
	 * gadget calls without needing host-side timer
	 * interruption during KVM_RUN (UML's SIGALRM-based
	 * timer is blocked for the duration of the ioctl).
	 */
	v->budget = KVM_VVAR_BUDGET_INITIAL;

	/* Publish: seq flips even → stable. */
	smp_wmb();
	v->seq++;
}

/*
 * Walk-or-allocate a sub-table inside the shadow PT. `parent`
 * is the u64 entry at the current level; `*next_va` returns the
 * kernel VA of the child table. If the parent is already
 * present, follow it; if not, alloc + zero a fresh child page,
 * fill in the parent entry with P|R/W|U/S|A (permissive — the
 * leaf PTE is what really controls access permissions), and
 * return the new child.
 */
static int kvm_shadow_table_step(u64 *parent, u64 **next_va)
{
	struct page *p;
	u64 pa;
	void *va;

	if (*parent & KVM_X86_PTE_P) {
		pa = *parent & ~0xfffULL & 0x000ffffffffff000ULL;
		*next_va = (u64 *)__va(pa);
		return 0;
	}

	p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!p)
		return -ENOMEM;
	va = page_address(p);
	pa = (u64)__pa(va);
	*parent  = pa | KVM_X86_PTE_P | KVM_X86_PTE_RW |
			KVM_X86_PTE_US | KVM_X86_PTE_A;
	*next_va = va;
	return 0;
}

/*
 * UML PTE bit encoding vs x86 hardware. Hard-coded rather than
 * referencing `_PAGE_*` macros so the relationship is visible
 * at the site (D66's finding is the reason this helper exists).
 * If arch/um/include/asm/pgtable.h ever reshuffles bit
 * positions, the translator below must be updated in lock-step.
 */
#define UM_PTE_PRESENT		0x001
#define UM_PTE_NEEDSYNC		0x002
#define UM_PTE_RW		0x020
#define UM_PTE_USER		0x040
#define UM_PTE_ACCESSED		0x080
#define UM_PTE_DIRTY		0x100

u64 kvm_um_pte_to_x86(u64 um_pte)
{
	u64 out = 0;

	if (!(um_pte & UM_PTE_PRESENT))
		return 0;

	/*
	 * Permissive mapping: if a UML PTE is present we install
	 * an x86 PTE with the same semantic permissions. UML's
	 * _PAGE_RW / _PAGE_USER / _PAGE_ACCESSED / _PAGE_DIRTY
	 * each translate to their hardware counterparts.
	 */
	out |= KVM_X86_PTE_P;
	if (um_pte & UM_PTE_RW)
		out |= KVM_X86_PTE_RW;
	if (um_pte & UM_PTE_USER)
		out |= KVM_X86_PTE_US;
	if (um_pte & UM_PTE_ACCESSED)
		out |= KVM_X86_PTE_A;
	if (um_pte & UM_PTE_DIRTY)
		out |= KVM_X86_PTE_D;

	/*
	 * Preserve the PFN (bits 12..51). UML's bit 1
	 * (_PAGE_NEEDSYNC) is software-only and has no x86
	 * equivalent — safe to drop. NX (bit 63) copies through
	 * unchanged if UML ever sets it.
	 */
	out |= um_pte & 0x000ffffffffff000ULL;
	out |= um_pte & (1ULL << 63);
	return out;
}

/*
 * Eager-fill the shadow PT from a UML logical pgd. Iterates
 * only through present entries (the overwhelming majority of
 * pgd/pud/pmd slots are empty for a typical user process), so
 * cost is O(pages-mapped), not O(VA-space).
 */
int kvm_shadow_fill_from_uml_pgd(void *pgd_va)
{
	u64 *pgd = pgd_va;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	int installed = 0;
	int rc;

	if (!pgd)
		return -EINVAL;

	for (pgd_i = 0; pgd_i < 512; pgd_i++) {
		u64 pgde = pgd[pgd_i];
		u64 *pud;

		if (!(pgde & UM_PTE_PRESENT))
			continue;
		pud = (u64 *)__va(pgde & 0x000ffffffffff000ULL);

		for (pud_i = 0; pud_i < 512; pud_i++) {
			u64 pude = pud[pud_i];
			u64 *pmd;

			if (!(pude & UM_PTE_PRESENT))
				continue;
			pmd = (u64 *)__va(pude & 0x000ffffffffff000ULL);

			for (pmd_i = 0; pmd_i < 512; pmd_i++) {
				u64 pmde = pmd[pmd_i];
				u64 *pte;

				if (!(pmde & UM_PTE_PRESENT))
					continue;
				pte = (u64 *)__va(pmde & 0x000ffffffffff000ULL);

				for (pte_i = 0; pte_i < 512; pte_i++) {
					u64 ume = pte[pte_i];
					u64 x86e;
					u64 va;

					if (!(ume & UM_PTE_PRESENT))
						continue;

					x86e = kvm_um_pte_to_x86(ume);
					if (!x86e)
						continue;
					va = ((u64)pgd_i << 39) |
					     ((u64)pud_i << 30) |
					     ((u64)pmd_i << 21) |
					     ((u64)pte_i << 12);

					rc = kvm_shadow_map_page(va,
								 x86e & 0x000ffffffffff000ULL,
								 x86e & ~0x000ffffffffff000ULL);
					if (rc < 0) {
						pr_warn_ratelimited("um: kvm shadow fill: map_page(va=0x%llx) failed (%d)\n",
								    (unsigned long long)va,
								    rc);
						return rc;
					}
					if (installed < 128) {
						u64 gpa = x86e &
							0x000ffffffffff000ULL;
						u64 fl  = x86e &
							~0x000ffffffffff000ULL;

						pr_info_ratelimited("um: kvm shadow fill[%d]: va=0x%llx -> gpa=0x%llx flags=0x%llx\n",
								    installed,
								    (unsigned long long)va,
								    (unsigned long long)gpa,
								    (unsigned long long)fl);
					}
					installed++;
				}
			}
		}
	}

	pr_info_ratelimited("um: kvm shadow fill: installed %d leaf PTEs from pgd=%p\n",
			    installed, pgd_va);
	return installed;
}

int kvm_shadow_map_page(u64 va, u64 phys_gpa, u64 leaf_flags)
{
	u64 *pgd = kvm_ctx.shadow_pgd;
	u64 *pud = NULL, *pmd = NULL, *pte = NULL;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	int rc;

	if (!pgd)
		return -ENODEV;

	pgd_i = (va >> 39) & 0x1ff;
	pud_i = (va >> 30) & 0x1ff;
	pmd_i = (va >> 21) & 0x1ff;
	pte_i = (va >> 12) & 0x1ff;

	rc = kvm_shadow_table_step(&pgd[pgd_i], &pud);
	if (rc)
		return rc;
	rc = kvm_shadow_table_step(&pud[pud_i], &pmd);
	if (rc)
		return rc;
	rc = kvm_shadow_table_step(&pmd[pmd_i], &pte);
	if (rc)
		return rc;

	/*
	 * Audit round-5 F6: flag the shadow PGD dirty on any leaf
	 * install. Overwriting a present PTE (mmap-over-mmap,
	 * mprotect-then-populate) means the old physical mapping
	 * may still be TLB-cached at the vCPU — mark so
	 * kvm_enter_guest flushes CR3 before the next KVM_RUN.
	 * For fresh installs the flag is conservatively dirty
	 * too; KVM_SET_SREGS is cheap relative to a missed-flush
	 * bug.
	 */
	pte[pte_i] = (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) |
		     (leaf_flags | KVM_X86_PTE_P | KVM_X86_PTE_A);
	kvm_ctx.shadow_dirty = true;
	return 0;
}

/*
 * Audit round-5 F6: clear shadow PTEs for [va_start, va_start+len)
 * and mark the shadow PGD dirty. Uses a page-at-a-time walk
 * because UML mm_unmap is typically called with ranges of one to
 * a handful of pages — the cost of iterating each 4 KiB leaf is
 * dominated by the eventual CR3 reload, not by the walk itself.
 *
 * Pure data transform on the shadow-PGD page tree. The TLB flush
 * is deferred to the next kvm_enter_guest: issuing KVM_SET_SREGS
 * from this context would be incorrect (wrong-vCPU-thread), and
 * batching per entry would amplify the ioctl cost on a range
 * that spans many pages. Matches the pattern used by upstream
 * KVM's own mmu_notifier path.
 *
 * Missing higher-level entries (pgd/pud/pmd not present) skip
 * that subrange — nothing to invalidate.
 */
/*
 * Audit round-6 G2: clear all leaf PTEs in the user half of the
 * singleton shadow PGD. PGD entries 0..255 cover canonical user
 * VA (low 128 TB on x86_64); 256..511 cover the canonical kernel
 * half where bootstrap data/code, gadget state, and vvar live.
 *
 * On every cross-mm context switch the user half must be cleared
 * — without it kvm_shadow_fill_from_uml_pgd would happily layer
 * the new mm's mappings on top of the previous mm's stale leaves,
 * leaking pages across processes.
 *
 * Intermediate PUD/PMD pages are kept attached to their PGD
 * entries so the next mm fill can reuse them without re-allocating
 * — a bounded memory footprint per shadow PGD's lifetime, in line
 * with the pre-existing kvm_shadow_pgd_free() shape that also
 * doesn't free sub-tables. (A full sub-table free + alloc on
 * every context switch is a separate optimization.)
 */
void kvm_shadow_pgd_clear_user(void)
{
	u64 *pgd = kvm_ctx.shadow_pgd;
	unsigned int pgd_i, pud_i, pmd_i;
	unsigned int leaf_tables = 0;

	if (!pgd)
		return;

	for (pgd_i = 0; pgd_i < 256; pgd_i++) {
		u64 pgde = pgd[pgd_i];
		u64 *pud;

		if (!(pgde & KVM_X86_PTE_P))
			continue;
		pud = (u64 *)__va(pgde & 0x000ffffffffff000ULL);

		for (pud_i = 0; pud_i < 512; pud_i++) {
			u64 pude = pud[pud_i];
			u64 *pmd;

			if (!(pude & KVM_X86_PTE_P))
				continue;
			pmd = (u64 *)__va(pude & 0x000ffffffffff000ULL);

			for (pmd_i = 0; pmd_i < 512; pmd_i++) {
				u64 pmde = pmd[pmd_i];
				u64 *pte;

				if (!(pmde & KVM_X86_PTE_P))
					continue;
				pte = (u64 *)__va(pmde &
						  0x000ffffffffff000ULL);
				memset(pte, 0, PAGE_SIZE);
				leaf_tables++;
			}
		}
	}

	if (leaf_tables)
		kvm_ctx.shadow_dirty = true;
}
EXPORT_SYMBOL_GPL(kvm_shadow_pgd_clear_user);

int kvm_shadow_invalidate_va_range(u64 va_start, u64 len)
{
	u64 *pgd = kvm_ctx.shadow_pgd;
	u64 va, va_end;
	unsigned int cleared = 0;

	if (!pgd)
		return -ENODEV;
	if (!len)
		return -EINVAL;

	va_end = (va_start + len + 0xfffULL) & ~0xfffULL;
	for (va = va_start & ~0xfffULL; va < va_end; va += PAGE_SIZE) {
		unsigned int pgd_i = (va >> 39) & 0x1ff;
		unsigned int pud_i = (va >> 30) & 0x1ff;
		unsigned int pmd_i = (va >> 21) & 0x1ff;
		unsigned int pte_i = (va >> 12) & 0x1ff;
		u64 pgde = pgd[pgd_i];
		u64 *pud, *pmd, *pte;

		if (!(pgde & KVM_X86_PTE_P))
			continue;
		pud = (u64 *)__va(pgde & 0x000ffffffffff000ULL);
		if (!(pud[pud_i] & KVM_X86_PTE_P))
			continue;
		pmd = (u64 *)__va(pud[pud_i] & 0x000ffffffffff000ULL);
		if (!(pmd[pmd_i] & KVM_X86_PTE_P))
			continue;
		pte = (u64 *)__va(pmd[pmd_i] & 0x000ffffffffff000ULL);

		if (pte[pte_i] & KVM_X86_PTE_P) {
			pte[pte_i] = 0;
			cleared++;
		}
	}

	if (cleared)
		kvm_ctx.shadow_dirty = true;
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_shadow_invalidate_va_range);
#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

int kvm_backend_fd(void)
{
	return kvm_ctx.kvm_fd;
}

int kvm_backend_vm_fd(void)
{
	return kvm_ctx.vm_fd;
}

int kvm_backend_vcpu0_fd(void)
{
	return kvm_ctx.vcpu0_fd;
}

struct kvm_um *kvm_backend_ctx(void)
{
	return &kvm_ctx;
}
