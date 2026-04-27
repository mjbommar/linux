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
#include <linux/cleanup.h>		/* guard(mutex) for T11 fill_lock */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/mutex.h>
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
	 * Stage A enabled SMP: per-task vCPU (kvm_vcpu_for_current
	 * lazy-allocates one vcpu_fd per UML task) means each guest
	 * CPU can run on its own host pthread with its own vcpu_fd.
	 * The pre-Stage-A panic on uml_ncpus > 1 was defending the
	 * singleton vcpu0_fd model; that model is gone.
	 *
	 * Open follow-on (SMP.2/SMP.3): wire per-CPU host-thread
	 * pinning + a SIGRTMIN+5 sender for cross-CPU vCPU eviction.
	 * Until those land, ncpus>1 will work but UML's cooperative
	 * scheduler still serializes per-host-thread.
	 */
	if (uml_ncpus > 1)
		pr_info("um: kvm init: SMP enabled (ncpus=%d); per-task vCPU model from Stage A supports it\n",
			uml_ncpus);

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
	 * Stage A.7: vcpu0 is now created ONLY for the !INTEGRATED
	 * harness/test path (arch/um/backend/kvm/harness.c). Under
	 * INTEGRATED, every UML task allocates its own per-task vCPU
	 * via kvm_vcpu_handle_alloc on first kvm_run_userspace; the
	 * vcpu0 singleton is dead weight and was the source of the
	 * "1 vCPU shared across tasks violates KVM's contract" bug
	 * class that drove the 03-architecture-review-2026-04-27
	 * redesign.
	 *
	 * Snapshot/record paths used to fall back to vcpu0_fd — Stage
	 * A.6 migrated them to current->thread.arch.kvm.vcpu, returning
	 * -ENODEV when no current vCPU exists (acceptable on early-boot
	 * KUnit). KVM_GET_VCPU_MMAP_SIZE is still needed at init time
	 * for kvm_vcpu_handle_alloc to know the mmap size.
	 */
	{
		int mmap_size = os_ioctl_generic(kfd, KVM_GET_VCPU_MMAP_SIZE, 0);

		if (mmap_size <= 0) {
			pr_err("um: kvm init: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
			       mmap_size);
			os_close_file(vmfd);
			os_close_file(kfd);
			return mmap_size ? mmap_size : -EIO;
		}
		kvm_ctx.run_size = mmap_size;
	}

#ifndef CONFIG_UM_BACKEND_KVM_INTEGRATED
	{
		int vcpu_fd;
		void *run;

		vcpu_fd = os_ioctl_generic(vmfd, KVM_CREATE_VCPU, 0);
		if (vcpu_fd < 0) {
			pr_err("um: kvm init: KVM_CREATE_VCPU failed (%d)\n",
			       vcpu_fd);
			os_close_file(vmfd);
			os_close_file(kfd);
			return vcpu_fd;
		}

		run = os_mmap_rw_shared(vcpu_fd, kvm_ctx.run_size);
		if (!run) {
			pr_err("um: kvm init: mmap of kvm_run (size %zu) failed\n",
			       kvm_ctx.run_size);
			os_close_file(vcpu_fd);
			os_close_file(vmfd);
			os_close_file(kfd);
			return -ENOMEM;
		}

		kvm_ctx.vcpu0_fd = vcpu_fd;
		kvm_ctx.run0     = run;
	}
#endif

	kvm_ctx.kvm_fd = kfd;
	kvm_ctx.vm_fd  = vmfd;

	/*
	 * Task #273: CPUID passthrough is deferred to first KVM_RUN
	 * via kvm_ensure_cpuid_done(). kvm_init() runs from init_
	 * backend() during linux_main(), BEFORE mm_init() brings up
	 * the buddy allocator (see the alloc_page() comment further
	 * down for the same constraint). kzalloc with GFP_KERNEL
	 * returns NULL here. The first caller that actually needs the
	 * CPUID installed is kvm_enter_guest, which only runs after
	 * full kernel bring-up; deferring keeps the code path simple
	 * and matches the existing kvm_shadow_pgd lazy-init pattern.
	 */

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
	 * Stage A: a no-op host handler for KVM_UM_KICK_SIGNAL is registered
	 * lazily at first kvm_vcpu_handle_alloc (deferred from here because
	 * sigaction during init_backend fights UML's signal-setup ordering).
	 * The kick signal is unused today — KVM_SET_SIGNAL_MASK is NOT
	 * installed (see SIGNAL HANDLING NOTE in thread.c) so SIGALRM
	 * preempts KVM_RUN naturally — but the handler is in place for
	 * future SMP UML where one host CPU will explicitly evict another
	 * host CPU's vCPU via pthread_kill(KVM_UM_KICK_SIGNAL).
	 */

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

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
/*
 * Lazy one-shot host-CPUID passthrough (task #273). Idempotent: the
 * first kvm_enter_guest call calls this; subsequent calls fast-path
 * out via the cpuid_done flag. Deferred out of kvm_init() because
 * the buddy allocator isn't up yet at init_backend() time (same
 * reason the shadow_pgd alloc is lazy — see lifecycle.c:359 comment).
 *
 * Two-step KVM dance:
 *   1. KVM_GET_SUPPORTED_CPUID (VM-fd ioctl) — KVM fills the
 *      kvm_cpuid2 buffer with everything it can virtualize on this
 *      host. nent on input is buffer capacity; on output it's the
 *      count actually filled.
 *   2. KVM_SET_CPUID2 (vCPU-fd ioctl) — install on vCPU. Must
 *      happen before first KVM_RUN, which kvm_enter_guest also
 *      gates.
 *
 * Buffer size: KVM_MAX_CPUID_ENTRIES (kvm_host.h) is 256 in
 * upstream; we use the same conservative cap.
 *
 * Failure here is non-fatal — log and continue with KVM's default
 * feature set, matching pre-#273 behaviour. Workloads that don't
 * dynamically link against modern glibc (e.g. perf-getpid /
 * df-preserve / kvm-bounds, all freestanding ELF) keep working
 * regardless. Workloads that DO need x86-64-v3 features (any
 * dynamically-linked binary on a recent Ubuntu / Fedora host) get
 * the host's feature set on success.
 */
int kvm_ensure_cpuid_done(struct kvm_vcpu_handle *vcpu)
{
	const u32 max_entries = 256;
	size_t buf_sz;
	struct kvm_cpuid2 *cpuid;
	int rc;

	if (!vcpu || vcpu->fd < 0)
		return -ENODEV;
	if (vcpu->cpuid_done)
		return 0;
	if (kvm_ctx.kvm_fd < 0)
		return -ENODEV;

	buf_sz = sizeof(struct kvm_cpuid2) +
		 max_entries * sizeof(struct kvm_cpuid_entry2);
	cpuid = kzalloc(buf_sz, GFP_KERNEL);
	if (!cpuid) {
		pr_warn_once("um: kvm: cpuid kzalloc failed; vCPU CPUID stays at KVM default (host-libc may refuse to load if compiled for x86-64-v3+)\n");
		return -ENOMEM;
	}

	cpuid->nent = max_entries;
	rc = os_ioctl_generic(kvm_ctx.kvm_fd, KVM_GET_SUPPORTED_CPUID,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_warn_once("um: kvm: KVM_GET_SUPPORTED_CPUID failed (%d); using KVM-default CPUID\n",
			     rc);
		goto out_free;
	}

	/*
	 * Review-01 P1 #3: mask host RDRAND / RDSEED CPUID bits so
	 * record/replay determinism doesn't have a hardware-RNG hole.
	 * Guest software falls back to its own RNG (glibc has
	 * software DRBG; getrandom(2) goes through the syscall
	 * record/replay path). KVM_SET_CPUID2 must be called once
	 * before the first KVM_RUN, so we mask up-front whether or
	 * not record/replay is currently active — same choice qemu
	 * makes when its `-cpu ...,-rdrand,-rdseed` option fires.
	 *
	 *   CPUID leaf 1, ECX bit 30 = RDRAND
	 *   CPUID leaf 7 subleaf 0, EBX bit 18 = RDSEED
	 *
	 * Cost vs benefit: software DRBG is ~10× slower than RDRAND
	 * on Sapphire Rapids etc., but that's a microbenchmark
	 * concern; glibc's userspace RNG cache amortizes it. UML's
	 * intended workloads (testing/fuzzing/observability) don't
	 * have a measurable RDRAND throughput requirement.
	 *
	 * Done unconditionally because (a) it's safer-by-default
	 * and (b) KVM_SET_CPUID2 isn't re-issuable post-RUN, so we
	 * can't make it record-mode-only without restructuring the
	 * init order.
	 */
	{
		unsigned int i;

		for (i = 0; i < cpuid->nent; i++) {
			struct kvm_cpuid_entry2 *e = &cpuid->entries[i];

			if (e->function == 1 && e->index == 0) {
				/* RDRAND — record/replay determinism. */
				e->ecx &= ~(1U << 30);
				/*
				 * Task #274: mask XSAVE / OSXSAVE / AVX
				 * family. The KVM backend's sregs setup
				 * (sregs.c) programs CR4 with PAE | OSFXSR
				 * | OSXMMEXCPT — it does NOT set
				 * CR4.OSXSAVE, and we do not program XCR0
				 * via KVM_SET_XCRS. So advertising XSAVE-
				 * dependent features (AVX, AVX2, AVX512,
				 * FMA, VAES, VPCLMULQDQ) sets the guest up
				 * for #UD/#GP whenever userspace
				 * (libcrypto's CPU-feature dispatcher,
				 * Python's hashlib via OpenSSL,
				 * NumPy via BLAS, …) executes one of those
				 * instructions. Symptom: hard-to-trace
				 * SIGSEGV inside libc / libcrypto on first
				 * dlopen. Mask conservatively until proper
				 * XSAVE/XCR0 setup + snapshot/restore land.
				 *
				 * Leaf 1 ECX:
				 *   bit 12 = FMA (depends on AVX)
				 *   bit 26 = XSAVE
				 *   bit 27 = OSXSAVE
				 *   bit 28 = AVX
				 *   bit 29 = F16C (uses YMM, depends on AVX)
				 */
				e->ecx &= ~((1U << 12) | (1U << 26) |
					    (1U << 27) | (1U << 28) |
					    (1U << 29));
			}
			if (e->function == 7 && e->index == 0) {
				/* RDSEED — record/replay determinism. */
				e->ebx &= ~(1U << 18);
				/*
				 * Task #274 cont. — mask AVX2 + every
				 * AVX512 family bit in EBX/ECX/EDX so
				 * libcrypto/Python's CPU-dispatcher
				 * doesn't take a XSAVE-dependent code
				 * path. Bit map mirrors qemu's
				 * `-avx2,-avx512*,-vaes` style cpu
				 * tuning.
				 *
				 * Leaf 7.0 EBX:
				 *   bit 0  = FSGSBASE
				 *   bit 5  = AVX2
				 *   bit 16 = AVX512F
				 *   bit 17 = AVX512DQ
				 *   bit 21 = AVX512IFMA
				 *   bit 26 = AVX512PF
				 *   bit 27 = AVX512ER
				 *   bit 28 = AVX512CD
				 *   bit 30 = AVX512BW
				 *   bit 31 = AVX512VL
				 *
				 * #274 / T20: also mask FSGSBASE. CR4.FSGSBASE
				 * is NOT set by kvm_fill_longmode_segments
				 * (sregs.c only programs PAE | OSFXSR |
				 * OSXMMEXCPT), so RDFSBASE / RDGSBASE /
				 * WRFSBASE / WRGSBASE all #UD when executed
				 * by the guest. glibc 2.31+ uses these for
				 * fast TLS access when the CPUID FSGSBASE
				 * bit is set; advertising the feature without
				 * the OS-side enable bit is a guaranteed
				 * crash on the first TLS access.
				 */
				e->ebx &= ~((1U << 0)  | (1U << 5)  |
					    (1U << 16) | (1U << 17) |
					    (1U << 21) | (1U << 26) |
					    (1U << 27) | (1U << 28) |
					    (1U << 30) | (1U << 31));
				/*
				 * Leaf 7.0 ECX:
				 *   bit 1  = AVX512VBMI
				 *   bit 6  = AVX512VBMI2
				 *   bit 9  = VAES
				 *   bit 10 = VPCLMULQDQ
				 *   bit 11 = AVX512VNNI
				 *   bit 12 = AVX512BITALG
				 *   bit 14 = AVX512VPOPCNTDQ
				 */
				e->ecx &= ~((1U << 1)  | (1U << 6)  |
					    (1U << 9)  | (1U << 10) |
					    (1U << 11) | (1U << 12) |
					    (1U << 14));
				/*
				 * Leaf 7.0 EDX:
				 *   bit 2 = AVX512_4VNNIW
				 *   bit 3 = AVX512_4FMAPS
				 *   bit 8 = AVX512_VP2INTERSECT
				 */
				e->edx &= ~((1U << 2) | (1U << 3) |
					    (1U << 8));
			}
			/*
			 * Leaf 0xD describes XSAVE state components +
			 * sizes. With OSXSAVE off, exposing this leaf
			 * causes glibc's XSAVE-aware setjmp/longjmp +
			 * libcrypto's cpuid_setup to derive sizes for
			 * features we don't actually support. Zero the
			 * leaf entirely — guest sees "no XSAVE state".
			 */
			if (e->function == 0xD)
				e->eax = e->ebx = e->ecx = e->edx = 0;
		}
	}

	rc = os_ioctl_generic(vcpu->fd, KVM_SET_CPUID2,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_warn_once("um: kvm: KVM_SET_CPUID2 failed (%d); using KVM-default CPUID\n",
			     rc);
		goto out_free;
	}

	vcpu->cpuid_done = true;
	pr_info("um: kvm: CPUID passthrough installed (%u entries; RDRAND/RDSEED + XSAVE/AVX/AVX2/AVX512 family + F16C masked — guest CR4.OSXSAVE=0 + XCR0 unset would otherwise trip XSAVE-dependent code paths in libcrypto/glibc)\n",
		cpuid->nent);

out_free:
	kfree(cpuid);
	return rc;
}
#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

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
/*
 * Per-mm shadow PGD allocator (#275). Each UML mm gets its own
 * shadow tree at kvm_mm_attach time. The returned struct lives
 * until kvm_shadow_mm_free is called from kvm_mm_detach. Caller
 * stores the pointer on mm_id->kvm_shadow.
 */
struct kvm_shadow_mm *kvm_shadow_mm_alloc(void)
{
	struct kvm_shadow_mm *shadow;
	struct page *page;
	struct page *iretq_page;

	shadow = kzalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return NULL;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		kfree(shadow);
		return NULL;
	}

	/*
	 * Memo 18 Phase 2: per-mm IRETQ-frame storage. Allocate a
	 * dedicated page; we'll install it in the shadow PGD at a
	 * unique kernel-half VA derived from the shadow pointer in
	 * kvm_shadow_pgd_alloc / first kvm_enter_guest. The page is
	 * zero-init via __GFP_ZERO so a half-built frame can't be
	 * read by the guest before kvm_enter_guest writes the real
	 * frame[0..4].
	 */
	iretq_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!iretq_page) {
		__free_page(page);
		kfree(shadow);
		return NULL;
	}

	shadow->pgd_page = page;
	shadow->pgd      = page_address(page);
	shadow->pgd_gpa  = (u64)__pa(shadow->pgd);
	/*
	 * Stage A.4d: tlb_gen starts at 1. Every fresh vCPU has
	 * last_flushed_tlb_gen=0 from kzalloc, so 1 vs 0 always mismatches
	 * and the first kvm_enter_guest flushes its TLB.
	 */
	atomic64_set(&shadow->tlb_gen, 1);
	atomic_set(&shadow->invalidate_in_progress, 0);
	atomic64_set(&shadow->invalidate_seq, 0);
	shadow->dirty    = true;
	shadow->synced   = false;
	shadow->synced_pgd_va = 0;
	shadow->needs_full_resync = false;
	shadow->direct_sync_install     = 0;
	shadow->direct_sync_clear       = 0;
	shadow->direct_sync_absent      = 0;
	shadow->direct_sync_alloc_fail  = 0;
	shadow->direct_sync_range_clear = 0;
	shadow->iretq_frame_page = iretq_page;
	shadow->iretq_frame_va   = page_address(iretq_page);
	shadow->iretq_frame_gpa  = (u64)__pa(shadow->iretq_frame_va);
	/*
	 * Pick a unique guest VA per shadow_mm in the kernel-half
	 * canonical range (bit 47 sign-extended via 0xffff_8000_*).
	 * Use the shadow_mm pointer's low 24 bits as a unique
	 * offset within a 16 MB carveout at 0xffffc00000000000 —
	 * gives 4096 distinct slots before collision (way more than
	 * UML's typical mm count). Page-aligned naturally.
	 */
	shadow->iretq_frame_va_guest = 0xffffc00000000000ULL |
		(((u64)shadow >> 4) & 0xfffff000ULL);
	mutex_init(&shadow->fill_lock);
	return shadow;
}
EXPORT_SYMBOL_GPL(kvm_shadow_mm_alloc);

void kvm_shadow_mm_free(struct kvm_shadow_mm *shadow)
{
	if (!shadow)
		return;

	/*
	 * Stage A.4c: removed the singleton ctx->cached_cr3_gpa
	 * invalidation here. Per-task vCPU now keeps cached_cr3_gpa
	 * on each task's struct kvm_vcpu_handle. By the time a
	 * shadow_mm is freed (mm teardown), all tasks owning that mm
	 * have exited via exit_thread() which destroys their per-task
	 * vCPU handles — no live cache can point at the shadow being
	 * freed. The pre-A.4c singleton cache invalidation is moot.
	 */

	/*
	 * Free intermediate PUD/PMD/PTE pages by walking the PGD.
	 * Each present non-leaf entry holds a kernel VA via __va(pa).
	 */
	if (shadow->pgd) {
		u64 *pgd = shadow->pgd;
		unsigned int gi;

		for (gi = 0; gi < 512; gi++) {
			u64 *pud;
			unsigned int ui;

			if (!(pgd[gi] & 1ULL))
				continue;
			pud = (u64 *)__va(pgd[gi] & 0x000ffffffffff000ULL);
			for (ui = 0; ui < 512; ui++) {
				u64 *pmd;
				unsigned int mi;

				if (!(pud[ui] & 1ULL))
					continue;
				pmd = (u64 *)__va(pud[ui] & 0x000ffffffffff000ULL);
				for (mi = 0; mi < 512; mi++) {
					struct page *pte_page;

					if (!(pmd[mi] & 1ULL))
						continue;
					pte_page = virt_to_page(__va(pmd[mi] &
								     0x000ffffffffff000ULL));
					__free_page(pte_page);
				}
				__free_page(virt_to_page(pmd));
			}
			__free_page(virt_to_page(pud));
		}
	}
	if (shadow->pgd_page)
		__free_page(shadow->pgd_page);
	if (shadow->iretq_frame_page)
		__free_page(shadow->iretq_frame_page);
	kfree(shadow);
}
EXPORT_SYMBOL_GPL(kvm_shadow_mm_free);

struct kvm_shadow_mm *kvm_shadow_mm_current(void)
{
	struct mm_struct *mm;

	if (!current)
		return NULL;
	mm = current->active_mm;
	if (!mm)
		return NULL;
	return mm->context.id.kvm_shadow;
}
EXPORT_SYMBOL_GPL(kvm_shadow_mm_current);

/*
 * Vestigial singleton API. Pre-#275 each kvm_enter_guest's first
 * call lazy-allocated kvm_ctx.shadow_pgd; post-#275 the per-mm
 * shadow_pgd is allocated at kvm_mm_attach. These stubs no-op so
 * callers (KUnit force-probes) still link.
 */
int kvm_shadow_pgd_alloc(void)
{
	return 0;
}

void kvm_shadow_pgd_free(void)
{
}

u64 kvm_shadow_pgd_gpa(void)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();

	return shadow ? shadow->pgd_gpa : 0;
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
	 * Mirror UML's tlb.c:73-77 fully: if the page hasn't been
	 * accessed, withhold ALL access (return 0 = no shadow entry
	 * installed). Guest reads/writes will fault, recovery sets
	 * young, refill installs a real entry.
	 */
	if (!(um_pte & UM_PTE_ACCESSED))
		return 0;

	/*
	 * Match UML's software-emulated A/D model from
	 * arch/um/kernel/tlb.c:73-77 (update_pte_range), which
	 * derives the host-VA prot as:
	 *   if (!pte_young(*pte))   { r = w = 0; }
	 *   else if (!pte_dirty(*pte)) { w = 0; }
	 * UML has no hardware-managed accessed/dirty bits — the
	 * host process's pgd that mirrors UML's user pgd is
	 * mapped RO for clean pages, so writes via the host VA
	 * fault into UML's trap handler (arch/um/kernel/trap.c)
	 * which sets _PAGE_DIRTY and marks the PTE writable.
	 *
	 * Before this fix, kvm_um_pte_to_x86 set KVM_X86_PTE_RW
	 * solely on UM_PTE_RW, ignoring UM_PTE_DIRTY. The guest
	 * could then write through the shadow without ever
	 * faulting, so UML never observed the write — _PAGE_DIRTY
	 * stayed clear, page reclaim treated the page as clean
	 * and could evict it. On next access the page got
	 * re-read from its source (anonymous ↦ zero, file ↦ disk
	 * contents), losing the user's writes silently. That
	 * exactly matches the import-unittest symptom of
	 * PyObject ob_type fields reading as NULL after they
	 * were initialised to a valid PyTypeObject pointer.
	 *
	 * Cost of the fix: each first-write-to-clean-page now
	 * takes a guest #PF round-trip instead of a fast path.
	 * UML's trap handler responds by marking dirty + RW; the
	 * subsequent fill installs shadow with RW, future writes
	 * are fast. This is the same access pattern the host VA
	 * already pays under update_pte_range — we're now
	 * mirroring the same dirty-emulation semantics in shadow.
	 *
	 * UM_PTE_USER, UM_PTE_ACCESSED, UM_PTE_DIRTY translate
	 * 1-for-1 as before.
	 */
	out |= KVM_X86_PTE_P;
	if ((um_pte & UM_PTE_RW) && (um_pte & UM_PTE_DIRTY))
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
int kvm_shadow_fill_from_uml_pgd(struct kvm_shadow_mm *shadow, void *pgd_va)
{
	u64 *pgd = pgd_va;
	u64 *spgd;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	int installed = 0;
	unsigned int cleared = 0;
	u64 mut_seq_at_start;
	int rc;

	if (!pgd)
		return -EINVAL;
	if (!shadow)
		return -ENODEV;

	/*
	 * Stage B-race fix (2026-04-27): bracket the entire fill
	 * (clear pass + install pass) with kvm_shadow_invalidate_
	 * begin/end. Pairs with kvm_enter_guest's pre-KVM_RUN check
	 * for in_progress > 0 + seq mismatch — closes the window where
	 * the consumer reads a partially-applied fill.
	 */
	kvm_shadow_invalidate_begin(shadow);

	/*
	 * #274 / T11: serialize shadow tree mutation. fill walks the
	 * pgd and inserts leaves; concurrent invalidate (from a
	 * kvm_mm_map / unmap on the same shadow_mm) would race against
	 * the walk and could leave the shadow tree in an inconsistent
	 * intermediate state. UML's normal flow is single-threaded per
	 * mm, but signal-driven preemption can interleave a fault
	 * recovery's fill against an in-progress mm_unmap drain. The
	 * mutex was initialized in kvm_shadow_mm_alloc but never used
	 * until now. guard() auto-unlocks on every return path.
	 */
	guard(mutex)(&shadow->fill_lock);

	/*
	 * Task #90: seqlock-style snapshot of the mutation counter.
	 * fill_lock serializes against OTHER fills, but direct-sync
	 * (kvm_shadow_sync_pte called from set_pte_at via the pgtable
	 * hook) does NOT take fill_lock — it must remain atomic-context
	 * safe. So a leaf write from direct sync can race the fill walk:
	 *
	 *   1. fill clear pass clears slot S
	 *   2. fill install pass reads UML PTE for slot S (value = X)
	 *   3. concurrent set_pte_at writes UML PTE = Y, direct sync
	 *      writes shadow slot S = translated(Y)
	 *   4. fill install pass writes shadow slot S = translated(X)
	 *   5. fill marks shadow synced — but shadow now has stale (X)
	 *      while UML PTE has fresh (Y)
	 *
	 * Detect this by sampling shadow->mut_head_seq before the walk
	 * and comparing after; any direct-sync writer between snap and
	 * end means our fill view is potentially stale. We don't undo
	 * the work — re-installing identical leaves is harmless — but
	 * we leave needs_full_resync=true so the next kvm_enter_guest
	 * re-fills before KVM_RUN, converging shadow to the latest UML
	 * pgd state.
	 *
	 * smp_load_acquire pairs with the smp_wmb in kvm_shadow_record_
	 * mut so any leaf write that bumped the counter is visible to
	 * us before the snap completes.
	 */
	mut_seq_at_start = smp_load_acquire(&shadow->mut_head_seq);

	/*
	 * #274 issue #8: TRANSACTIONAL fill. Walk the user half
	 * (PGD slots 0..255 — the canonical low VA) of the shadow
	 * first and clear every present leaf. Then re-install
	 * leaves from the UML pgd. This guarantees that after fill
	 * returns, the shadow exactly mirrors the UML pgd's user
	 * mappings: no shadow-present-but-pgd-absent stale leaves.
	 *
	 * Kernel half (PGD slots 256..511) is left alone — it
	 * holds the bootstrap / gadget / vvar aliases installed
	 * by kvm_enter_guest before fill. (Per issue #7's audit:
	 * UML's kernel-VA-region pages live at ~0x60000000 which
	 * is in PGD slot 0, not slot 256+; but bootstrap_va lives
	 * in slot 256+ via being placed by alloc_page in the
	 * vmalloc range — verify with kvm_diag_audit_pgd_skip if
	 * collisions are suspected.)
	 *
	 * Without this clear pass, a previous mapping VA X → PFN A
	 * that's been unmap'd in pgd (entry now 0) would persist
	 * as a shadow leaf VA X → PFN A. Guest reads via X return
	 * the OLD physical page's contents — silent stale-data
	 * corruption that grows monotonically over the boot. This
	 * was the most plausible remaining cause of the import-
	 * unittest PyObject->ob_type=NULL pattern: an old mapping
	 * with zeros at offset 0x8 persisting after the page was
	 * supposedly munmapped + remapped to a fresh page (whose
	 * write of valid ob_type went to a different physical page
	 * than the guest's stale-shadow read).
	 *
	 * Cost: one pass over the shadow user half on every fill.
	 * In practice fill rarely runs (cached-skip path catches
	 * most kvm_enter_guest calls), so the cost is acceptable.
	 */
	spgd = shadow->pgd;
	{
		/*
		 * Compute the bootstrap-alias VA range to PRESERVE in the
		 * clear pass. kvm_bootstrap_va spans 4 pages (bootstrap
		 * code+tables, gadget state, vvar, IST stack — all
		 * installed by kvm_enter_guest BEFORE fill). They live
		 * at the alloc_page-returned kernel VA, which on UML
		 * lands in PGD slot 0 (the user half by VA bit-39
		 * extraction). Clearing them would destroy the LSTAR /
		 * IDT / IST mappings the guest needs.
		 */
		u64 alias_lo = kvm_bootstrap_va_get();
		u64 alias_hi = alias_lo + 4 * PAGE_SIZE;

		for (pgd_i = 0; pgd_i < 256; pgd_i++) {
			u64 *spud;

			if (!(spgd[pgd_i] & KVM_X86_PTE_P))
				continue;
			spud = (u64 *)__va(spgd[pgd_i] &
					   0x000ffffffffff000ULL);
			for (pud_i = 0; pud_i < 512; pud_i++) {
				u64 *spmd;

				if (!(spud[pud_i] & KVM_X86_PTE_P))
					continue;
				spmd = (u64 *)__va(spud[pud_i] &
						   0x000ffffffffff000ULL);
				for (pmd_i = 0; pmd_i < 512; pmd_i++) {
					u64 *spte;

					if (!(spmd[pmd_i] & KVM_X86_PTE_P))
						continue;
					spte = (u64 *)__va(spmd[pmd_i] &
							   0x000ffffffffff000ULL);
					for (pte_i = 0; pte_i < 512;
					     pte_i++) {
						u64 va;

						if (!(spte[pte_i] &
						      KVM_X86_PTE_P))
							continue;
						va = ((u64)pgd_i << 39) |
						     ((u64)pud_i << 30) |
						     ((u64)pmd_i << 21) |
						     ((u64)pte_i << 12);
						/* preserve bootstrap aliases */
						if (alias_lo &&
						    va >= alias_lo &&
						    va < alias_hi)
							continue;
						spte[pte_i] = 0;
						cleared++;
					}
				}
			}
		}
	}
	if (cleared) {
		/*
		 * Memo 17 Phase A (Race-I keystone): pair the leaf-write
		 * batch above with the dirty-flag store via smp_wmb +
		 * WRITE_ONCE so the consumer in kvm_enter_guest's
		 * SREGS-skip predicate (which does smp_load_acquire on
		 * shadow->dirty) sees dirty=true after observing the leaf
		 * change. Without this, SIGALRM-driven preemption between
		 * the loop above and the dirty store leaves a window where
		 * a re-entry sees dirty=false → skips KVM_SET_SREGS → no
		 * TLB flush → guest reads via stale TLB.
		 */
		smp_wmb();
		kvm_shadow_mark_dirty(shadow);
		pr_info_ratelimited("um: kvm shadow fill: cleared %u stale user-half leaves before re-fill\n",
				    cleared);
	}

	/*
	 * P0-2 (memo 16 review Agent 3): the install pass below walks
	 * the UML pgd unconditionally and would overwrite the bootstrap
	 * shadow leaves at [bootstrap_va, +4*PAGE_SIZE) if a user pgd
	 * entry happens to collide with the kernel-half alias range.
	 * Mirror the alias-range guard from the clear pass above.
	 * kvm_bootstrap_va lives in PGD slot 0 (alloc_page in lowmem),
	 * same half as user mappings — the clobber is reachable
	 * whenever a user mapping lands in that VA range.
	 *
	 * Task #92: install pass walks PGD slots 0..255 (user half) to
	 * mirror the clear pass scope. Pre-fix, install walked 0..511
	 * and would have re-installed any high-half user-pgd entries —
	 * which clear had NOT cleared, leaving an asymmetric "install
	 * but never clear" window. UML user processes never populate
	 * pgd slots 256..511 (canonical-kernel range), so restricting
	 * is safe and makes the invariant obvious in code: shadow user
	 * half is fully reset on every fill; shadow kernel half is
	 * managed by kvm_enter_guest's bootstrap/iretq/gadget installs
	 * and never touched by fill.
	 */
	{
		u64 alias_lo_install = kvm_bootstrap_va_get();
		u64 alias_hi_install = alias_lo_install + 4 * PAGE_SIZE;

	for (pgd_i = 0; pgd_i < 256; pgd_i++) {
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

					/* P0-2: skip the bootstrap-alias range */
					if (alias_lo_install &&
					    va >= alias_lo_install &&
					    va < alias_hi_install)
						continue;

					rc = kvm_shadow_map_page(shadow, va,
								 x86e & 0x000ffffffffff000ULL,
								 x86e & ~0x000ffffffffff000ULL);
					if (rc < 0) {
						pr_warn_ratelimited("um: kvm shadow fill: map_page(va=0x%llx) failed (%d)\n",
								    (unsigned long long)va,
								    rc);
						kvm_shadow_invalidate_end(shadow);
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
	}	/* P0-2 alias_lo_install scope close */

	pr_info_ratelimited("um: kvm shadow fill: installed %d leaf PTEs from pgd=%p\n",
			    installed, pgd_va);

	/*
	 * Task #242: mark the shadow PT as in-sync with this pgd-VA.
	 * Subsequent kvm_enter_guest calls compare against the
	 * cached mm + VA + synced flag and skip the full re-walk
	 * when nothing has invalidated the mirror. Audit round-7
	 * P2: tracking BOTH mm pointer and pgd-VA closes the
	 * VA-reuse hazard — execve-style mm replacement (where the
	 * old mm's pgd page can be reused for the new mm's pgd) is
	 * caught by the mm-pointer mismatch even if the pgd-VA
	 * happens to coincide.
	 *
	 * `current` here is the task driving the kvm_enter_guest
	 * call — its active_mm is the mm whose pgd we just mirrored.
	 * For non-task callers (KUnit force-probe), current still
	 * points at a task with a valid active_mm under
	 * CONFIG_UM_BACKEND_KVM_INTEGRATED tests; if that ever
	 * changes the cache key just pessimistically misses on the
	 * next entry (filling again is cheap).
	 *
	 * Memo 17 Phase A (Race-I keystone): smp_wmb pairs leaf
	 * writes earlier in this fn with the synced/synced_pgd_va
	 * stores so the consumer (skip-fill predicate) sees a
	 * coherent (synced=true, pgd_va=valid) pair after observing
	 * any leaf change.
	 */
	smp_wmb();
	WRITE_ONCE(shadow->synced_pgd_va, (u64)pgd_va);

	/*
	 * Task #90 seqlock check. Re-read mut_head_seq after the install
	 * pass. If it advanced during our walk, a direct-sync writer
	 * wrote a leaf based on a UML PTE value newer than what fill
	 * observed — our install_pass write may have clobbered it with
	 * a stale value. Mark needs_full_resync so the NEXT entry re-
	 * fills (with a fresh seq snapshot); leave dirty=true to force
	 * the TLB flush. Synced is left FALSE precisely so the next
	 * entry's predicate doesn't skip the repair fill.
	 *
	 * smp_mb on the read side pairs with the smp_wmb in
	 * kvm_shadow_record_mut: any leaf write that completed before
	 * the counter bump must be visible here.
	 */
	smp_mb();
	if (READ_ONCE(shadow->mut_head_seq) != mut_seq_at_start) {
		WRITE_ONCE(shadow->needs_full_resync, true);
		kvm_shadow_mark_dirty(shadow);
		WRITE_ONCE(shadow->synced, false);
		/* Task #94 counters. */
		WRITE_ONCE(shadow->needs_full_resync_set_seqlock_miss,
			   READ_ONCE(shadow->needs_full_resync_set_seqlock_miss) + 1);
		WRITE_ONCE(shadow->synced_clear_seqlock_miss,
			   READ_ONCE(shadow->synced_clear_seqlock_miss) + 1);
		WRITE_ONCE(shadow->dirty_set_fill_install,
			   READ_ONCE(shadow->dirty_set_fill_install) + 1);
		pr_info_ratelimited("um: kvm shadow fill: mut_seq advanced during walk (start=%llu now=%llu) — needs_full_resync=true\n",
				    (unsigned long long)mut_seq_at_start,
				    (unsigned long long)READ_ONCE(shadow->mut_head_seq));
		kvm_shadow_invalidate_end(shadow);
		return installed;
	}
	WRITE_ONCE(shadow->synced, true);
	WRITE_ONCE(shadow->synced_set_fill,
		   READ_ONCE(shadow->synced_set_fill) + 1);
	kvm_shadow_invalidate_end(shadow);
	return installed;
}

int kvm_shadow_map_page(struct kvm_shadow_mm *shadow,
			u64 va, u64 phys_gpa, u64 leaf_flags)
{
	u64 *pgd;
	u64 *pud = NULL, *pmd = NULL, *pte = NULL;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	int rc;

	if (!shadow || !shadow->pgd)
		return -ENODEV;
	pgd = shadow->pgd;

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
	 *
	 * Review-01 P1 #7 attempted an idempotent variant
	 * (only dirty when the PTE actually changes) but it
	 * regressed dyn-loader, suggesting the bootstrap-page
	 * remap path has a subtle interaction with the SREGS-
	 * skip cache. Reverted; the conservative
	 * always-dirty behaviour is the correctness floor and
	 * the perf-fallback ratio at 1.02× post-P1#5 is already
	 * past the parity goal so the optimization isn't load-
	 * bearing. Future work: instrument the failure mode
	 * before re-attempting.
	 */
	/*
	 * #274 issue: don't forcibly OR-in P|A. Pass leaf_flags
	 * unmodified so callers control exactly what bits land. The
	 * fill path's translator (kvm_um_pte_to_x86) already sets P
	 * + A based on the UML PTE; the bootstrap callers explicitly
	 * pass KVM_X86_PTE_P. Forcing P here would silently override
	 * a future translator change that returns leaf_flags=0 to
	 * mean "do not install" — and the redundant A bit drift
	 * defeats the purpose of mirroring UML's software A model.
	 */
	/*
	 * Memo 17 Phase A (Race-I keystone): order leaf write before
	 * dirty-flag store via WRITE_ONCE + smp_wmb so the consumer
	 * in kvm_enter_guest's SREGS-skip predicate (which does
	 * smp_load_acquire on shadow->dirty) cannot observe
	 * dirty=false after the leaf change. This is the same
	 * pattern the keystone fix added to direct-sync; the lazy-
	 * fill path was missed.
	 */
	WRITE_ONCE(pte[pte_i],
		   (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) | leaf_flags);
	smp_wmb();
	kvm_shadow_mark_dirty(shadow);
	/* Task #94 transition counter. */
	WRITE_ONCE(shadow->dirty_set_fill_install,
		   READ_ONCE(shadow->dirty_set_fill_install) + 1);
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
 * Audit round-6 G2 (post-#275 stub).
 *
 * Pre-#275 the kvm shadow PT was a SINGLETON shared across all mms,
 * so a cross-mm context switch had to clear the user half (PGD
 * slots 0..255) to prevent prev's mappings from leaking into next's
 * view through the same singleton tree.
 *
 * Post-#275 each mm has its OWN shadow PGD attached at
 * mm_id->kvm_shadow. A cross-mm switch loads next's shadow PGD as
 * CR3 — there is no shared tree to scrub. This function is now a
 * one-liner stub kept for the existing call sites
 * (kvm_context_switch + KUnit force-probe): it just marks the
 * destination shadow dirty so the next kvm_enter_guest's SREGS
 * reload toggles CR4.PGE and flushes the guest TLB (preventing
 * stale TLB entries from prev's CR3 from being honoured under
 * next's CR3).
 *
 * The actual user-half clear pass still exists, in
 * kvm_shadow_fill_from_uml_pgd (under fill_lock). That clear pass
 * runs against the per-mm shadow tree itself before each fill, to
 * remove leaves that the UML pgd no longer maps.
 */
void kvm_shadow_pgd_clear_user(void)
{
	/*
	 * #275: each UML mm has its own shadow tree, so cross-mm
	 * "switches" no longer need a clear pass — the new mm's
	 * shadow IS a different tree, with no leaves from the old
	 * mm to leak. Function kept for ABI compatibility with the
	 * kvm_context_switch + KUnit force-probe call sites; just
	 * mark the active mm's shadow dirty so the next CR3 reload
	 * triggers a TLB flush.
	 */
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();

	/*
	 * Memo 17 Phase A (Race-I keystone): WRITE_ONCE pairs with
	 * the consumer's smp_load_acquire on shadow->dirty.
	 */
	if (shadow) {
		kvm_shadow_mark_dirty(shadow);
		/* Task #94 transition counter. */
		WRITE_ONCE(shadow->dirty_set_pgd_clear_user,
			   READ_ONCE(shadow->dirty_set_pgd_clear_user) + 1);
	}
}
EXPORT_SYMBOL_GPL(kvm_shadow_pgd_clear_user);

int kvm_shadow_invalidate_va_range(struct kvm_shadow_mm *shadow,
				   u64 va_start, u64 len)
{
	u64 *pgd;
	u64 va, va_end;
	unsigned int cleared = 0;
	int rc = 0;

	if (!shadow || !shadow->pgd)
		return -ENODEV;
	if (!len)
		return -EINVAL;

	/* #274 / T11: serialize against fill (see kvm_shadow_fill_from_uml_pgd). */
	guard(mutex)(&shadow->fill_lock);

	/* Stage B-race fix: bracket the invalidate with begin/end. */
	kvm_shadow_invalidate_begin(shadow);

	pgd = shadow->pgd;

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
			WRITE_ONCE(pte[pte_i], 0);
			cleared++;
		}
	}

	/*
	 * #274 follow-on: ALWAYS mark dirty AND reset synced — not
	 * conditional on cleared > 0. The pgd has just been updated;
	 * shadow no longer mirrors it even if the affected range had
	 * no present shadow leaf to clear (e.g. lazy-fill state where
	 * the new pgd entry hasn't been mirrored into the shadow yet).
	 *
	 * Without unconditional dirty: the next kvm_enter_guest's
	 * SREGS-skip optimization (cached CR3/FS/GS match + !dirty)
	 * skips KVM_SET_SREGS — so the guest TLB is NOT flushed even
	 * though the shadow was just invalidated. Any guest TLB entry
	 * caching the OLD mapping (e.g. RW shadow installed before the
	 * kernel's mprotect-to-RO that triggered this invalidate) keeps
	 * being honoured for the stale duration of the TLB entry,
	 * letting the guest write to a now-RO page silently.
	 *
	 * Cost: one CR3 reload per invalidate that previously would
	 * have skipped. Correctness floor; the perf optimization is the
	 * cached-CR3 skip itself, not the conditional dirty flag.
	 *
	 * Memo 17 Phase A (Race-I keystone): smp_wmb pairs the leaf-
	 * clear loop above with the dirty/synced stores. Without it,
	 * the consumer (smp_load_acquire on dirty in kvm_enter_guest's
	 * SREGS-skip predicate) can read dirty=false after observing
	 * the leaf clears, take the skip, and KVM_RUN with stale TLB.
	 * synced must be cleared first (or together) to prevent the
	 * skip-fill predicate from short-circuiting on the same race.
	 */
	smp_wmb();
	WRITE_ONCE(shadow->synced, false);
	kvm_shadow_mark_dirty(shadow);
	/* Task #94 transition counters. */
	WRITE_ONCE(shadow->synced_clear_invalidate,
		   READ_ONCE(shadow->synced_clear_invalidate) + 1);
	WRITE_ONCE(shadow->dirty_set_invalidate,
		   READ_ONCE(shadow->dirty_set_invalidate) + 1);
	kvm_shadow_invalidate_end(shadow);
	return rc;
}
EXPORT_SYMBOL_GPL(kvm_shadow_invalidate_va_range);

/*
 * #274 phase-1 keystone diagnostic. Walks the UML logical pgd at
 * `va` and the shadow PT at the same `va`, then logs both leaf
 * encodings and whether the shadow agrees with the UML view.
 *
 * Translates the UML leaf PTE through kvm_um_pte_to_x86() so the
 * "expected" and "shadow" values are directly comparable in the
 * same x86 hardware encoding.
 *
 * Equality ignores the A and D status bits — the CPU sets those
 * at runtime as a side effect of access, so they drift legitimately
 * between a freshly-installed shadow entry and a subsequent walk
 * of the UML pgd. PFN, P, RW, US, and NX must match.
 *
 * `tag` is a short identifier for the call site so multiple audit
 * points in one boot are distinguishable.
 *
 * Returns 0 if shadow agrees with UML pgd, 1 if they diverge,
 * negative errno on missing inputs.
 */
int kvm_shadow_audit_va(u64 va, void *uml_pgd_va, const char *tag)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
	u64 *upgd = uml_pgd_va;
	u64 *spgd;
	unsigned int pgd_i = (va >> 39) & 0x1ff;
	unsigned int pud_i = (va >> 30) & 0x1ff;
	unsigned int pmd_i = (va >> 21) & 0x1ff;
	unsigned int pte_i = (va >> 12) & 0x1ff;
	u64 um_pte = 0;
	u64 expected_pte;
	u64 shadow_pte = 0;
	const u64 mask = 0x000ffffffffff000ULL |
			 KVM_X86_PTE_P | KVM_X86_PTE_RW |
			 KVM_X86_PTE_US | (1ULL << 63);
	bool equal;

	if (!tag)
		tag = "?";
	if (!shadow || !shadow->pgd) {
		pr_info("um: kvm audit[%s]: va=0x%llx no shadow\n",
			tag, (unsigned long long)va);
		return -ENODEV;
	}
	if (!upgd) {
		pr_info("um: kvm audit[%s]: va=0x%llx no uml pgd\n",
			tag, (unsigned long long)va);
		return -EINVAL;
	}
	spgd = shadow->pgd;

	/* Walk UML pgd → leaf PTE; UM_PTE_PRESENT at every level. */
	if (upgd[pgd_i] & UM_PTE_PRESENT) {
		u64 *upud = (u64 *)__va(upgd[pgd_i] &
					0x000ffffffffff000ULL);
		if (upud[pud_i] & UM_PTE_PRESENT) {
			u64 *upmd = (u64 *)__va(upud[pud_i] &
						0x000ffffffffff000ULL);
			if (upmd[pmd_i] & UM_PTE_PRESENT) {
				u64 *upte = (u64 *)__va(upmd[pmd_i] &
							0x000ffffffffff000ULL);
				um_pte = upte[pte_i];
			}
		}
	}
	expected_pte = kvm_um_pte_to_x86(um_pte);

	/* Walk shadow PT → leaf PTE; KVM_X86_PTE_P at every level. */
	if (spgd[pgd_i] & KVM_X86_PTE_P) {
		u64 *spud = (u64 *)__va(spgd[pgd_i] &
					0x000ffffffffff000ULL);
		if (spud[pud_i] & KVM_X86_PTE_P) {
			u64 *spmd = (u64 *)__va(spud[pud_i] &
						0x000ffffffffff000ULL);
			if (spmd[pmd_i] & KVM_X86_PTE_P) {
				u64 *spte = (u64 *)__va(spmd[pmd_i] &
							0x000ffffffffff000ULL);
				shadow_pte = spte[pte_i];
			}
		}
	}

	equal = ((expected_pte & mask) == (shadow_pte & mask));

	pr_info("um: kvm audit[%s]: va=0x%llx um_pte=0x%llx expected=0x%llx shadow=0x%llx %s synced=%d\n",
		tag,
		(unsigned long long)va,
		(unsigned long long)um_pte,
		(unsigned long long)expected_pte,
		(unsigned long long)shadow_pte,
		equal ? "EQUAL" : "DIVERGE",
		shadow->synced);
	return equal ? 0 : 1;
}
EXPORT_SYMBOL_GPL(kvm_shadow_audit_va);

/*
 * Task #91: three-way content audit for a suspicious VA.
 *
 * kvm_shadow_audit_va proves PTE EQUALITY (shadow PFN == UML PFN).
 * That is necessary but NOT sufficient: a wrong os_map_memory offset
 * or a host-VA aliasing bug can leave matching PFNs whose underlying
 * BYTES differ across the three access paths the system uses:
 *
 *   path A — UML PTE → page_address(pte_page) — kernel VA derived
 *            from the per-mm pgd. This is the "UML kernel's own
 *            view of the page".
 *
 *   path B — shadow PT GPA → uml_physmem + GPA — kernel VA derived
 *            from the GPA the guest hardware-MMU walks via shadow PT
 *            then EPT. This is "what KVM thinks the guest reads".
 *
 *   path C — user VA direct dereference (the host VA = user VA mapping
 *            installed by os_map_memory). This is "the raw host-PT
 *            view" used by copy_from_user, sigframe setup, etc.
 *
 * If shadow PFN == UML PFN, paths A and B point at the SAME physical
 * frame and must yield identical bytes. If A,B,C all match, the page
 * is coherent across views. If C diverges from A/B, the os_map_memory
 * offset is wrong for this VA — that is the host-VA aliasing class
 * memo 19's playbook flagged as Phase 4 territory.
 *
 * Reads `bytes` bytes (max 64) starting at `va`. pagefault_disable
 * around the user-VA read so the audit doesn't recurse into the
 * fault handler. Returns 0 if all three paths agree, 1 if A/B agree
 * but C diverges (host-VA alias), 2 if A diverges from B (shadow GPA
 * mismatch — should have been caught by kvm_shadow_audit_va), -ENODEV
 * if any path is unreachable.
 *
 * Diagnostic-only; safe to call from any context where the UML pgd
 * and shadow PT are stable.
 */
int kvm_shadow_audit_content_va(u64 va, void *uml_pgd_va, unsigned int bytes,
				const char *tag)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
	u64 *upgd = uml_pgd_va;
	u64 *spgd;
	unsigned int pgd_i = (va >> 39) & 0x1ff;
	unsigned int pud_i = (va >> 30) & 0x1ff;
	unsigned int pmd_i = (va >> 21) & 0x1ff;
	unsigned int pte_i = (va >> 12) & 0x1ff;
	unsigned int page_off = va & 0xfff;
	u64 um_pte = 0, shadow_pte = 0;
	unsigned long um_pfn = 0, sh_pfn = 0;
	u8 buf_a[64] = {0}, buf_b[64] = {0}, buf_c[64] = {0};
	bool ab_equal, ac_equal, bc_equal;
	int got_a = 0, got_b = 0, got_c = 0;

	if (!tag)
		tag = "?";
	if (bytes == 0 || bytes > sizeof(buf_a))
		bytes = 16;
	if (page_off + bytes > PAGE_SIZE)
		bytes = PAGE_SIZE - page_off;
	if (!shadow || !shadow->pgd || !upgd) {
		pr_info("um: kvm audit3[%s]: va=0x%llx unavailable shadow=%p upgd=%p\n",
			tag, (unsigned long long)va, shadow, upgd);
		return -ENODEV;
	}
	spgd = shadow->pgd;

	/* Path A: walk UML pgd → leaf PFN → __va. */
	if (upgd[pgd_i] & UM_PTE_PRESENT) {
		u64 *upud = (u64 *)__va(upgd[pgd_i] & 0x000ffffffffff000ULL);

		if (upud[pud_i] & UM_PTE_PRESENT) {
			u64 *upmd = (u64 *)__va(upud[pud_i] & 0x000ffffffffff000ULL);

			if (upmd[pmd_i] & UM_PTE_PRESENT) {
				u64 *upte = (u64 *)__va(upmd[pmd_i] & 0x000ffffffffff000ULL);

				um_pte = upte[pte_i];
				if (um_pte & UM_PTE_PRESENT) {
					um_pfn = (um_pte >> 12) & 0xffffffffULL;
					memcpy(buf_a,
					       (u8 *)__va((u64)um_pfn << 12) + page_off,
					       bytes);
					got_a = 1;
				}
			}
		}
	}

	/* Path B: walk shadow PT → leaf GPA → __va. */
	if (spgd[pgd_i] & KVM_X86_PTE_P) {
		u64 *spud = (u64 *)__va(spgd[pgd_i] & 0x000ffffffffff000ULL);

		if (spud[pud_i] & KVM_X86_PTE_P) {
			u64 *spmd = (u64 *)__va(spud[pud_i] & 0x000ffffffffff000ULL);

			if (spmd[pmd_i] & KVM_X86_PTE_P) {
				u64 *spte = (u64 *)__va(spmd[pmd_i] & 0x000ffffffffff000ULL);

				shadow_pte = spte[pte_i];
				if (shadow_pte & KVM_X86_PTE_P) {
					sh_pfn = (shadow_pte >> 12) & 0xffffffffULL;
					memcpy(buf_b,
					       (u8 *)__va((u64)sh_pfn << 12) + page_off,
					       bytes);
					got_b = 1;
				}
			}
		}
	}

	/* Path C: user VA direct dereference (host-PT mapping). */
	if (va < TASK_SIZE) {
		pagefault_disable();
		if (!copy_from_kernel_nofault(buf_c, (void *)va, bytes))
			got_c = 1;
		pagefault_enable();
	}

	ab_equal = got_a && got_b && !memcmp(buf_a, buf_b, bytes);
	ac_equal = got_a && got_c && !memcmp(buf_a, buf_c, bytes);
	bc_equal = got_b && got_c && !memcmp(buf_b, buf_c, bytes);

	pr_info("um: kvm audit3[%s]: va=0x%llx bytes=%u got=A%d/B%d/C%d um_pfn=0x%lx sh_pfn=0x%lx A==B:%d A==C:%d B==C:%d\n",
		tag, (unsigned long long)va, bytes, got_a, got_b, got_c,
		um_pfn, sh_pfn, ab_equal, ac_equal, bc_equal);

	if (got_a && got_b && !ab_equal) {
		pr_info("um: kvm audit3[%s]: A_first8=%02x%02x%02x%02x%02x%02x%02x%02x B_first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			tag,
			buf_a[0], buf_a[1], buf_a[2], buf_a[3],
			buf_a[4], buf_a[5], buf_a[6], buf_a[7],
			buf_b[0], buf_b[1], buf_b[2], buf_b[3],
			buf_b[4], buf_b[5], buf_b[6], buf_b[7]);
		return 2;
	}
	if (got_c && (got_a ? !ac_equal : (got_b && !bc_equal))) {
		pr_info("um: kvm audit3[%s]: HOST-VA ALIAS: kernel-view A_first8=%02x%02x%02x%02x%02x%02x%02x%02x user-view C_first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
			tag,
			buf_a[0], buf_a[1], buf_a[2], buf_a[3],
			buf_a[4], buf_a[5], buf_a[6], buf_a[7],
			buf_c[0], buf_c[1], buf_c[2], buf_c[3],
			buf_c[4], buf_c[5], buf_c[6], buf_c[7]);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_shadow_audit_content_va);

/*
 * #274 phase-1 step 2 diagnostic: full-pgd lockstep audit.
 *
 * Walks every present leaf in the UML pgd and looks up the same VA
 * in the shadow PT. Counts (leaves, matches, diverges) and logs the
 * first up-to-`max_log` divergences in detail. Does NOT mutate the
 * shadow tree — purely observational, intended to test the
 * "shadow->synced cache is honest" hypothesis on the cached-skip
 * path of kvm_enter_guest.
 *
 * If the cache is honest, every present UML leaf has a matching
 * shadow leaf and the audit logs "div=0/N". If the cache is lying
 * (pgd mutated without going through kvm_shadow_invalidate_va_range),
 * the divergences identify the missed sync points.
 *
 * Equality semantics match kvm_shadow_audit_va: PFN, P, RW, US, NX
 * must agree; A and D are masked out.
 *
 * Returns the number of divergences (>= 0) on success, negative
 * errno on missing inputs. Cost: O(pages-mapped). Intended for
 * diagnostic builds; production callers should gate with a runtime
 * flag once the bug is found.
 */
int kvm_shadow_audit_pgd(void *uml_pgd_va, const char *tag,
			 unsigned int max_log)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
	u64 *upgd = uml_pgd_va;
	u64 *spgd;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	unsigned int leaves = 0, matches = 0, diverges = 0;
	const u64 mask = 0x000ffffffffff000ULL |
			 KVM_X86_PTE_P | KVM_X86_PTE_RW |
			 KVM_X86_PTE_US | (1ULL << 63);

	if (!tag)
		tag = "?";
	if (!shadow || !shadow->pgd)
		return -ENODEV;
	if (!upgd)
		return -EINVAL;
	spgd = shadow->pgd;

	for (pgd_i = 0; pgd_i < 512; pgd_i++) {
		u64 *upud, *spud_va = NULL;

		if (!(upgd[pgd_i] & UM_PTE_PRESENT))
			continue;
		upud = (u64 *)__va(upgd[pgd_i] & 0x000ffffffffff000ULL);
		if (spgd[pgd_i] & KVM_X86_PTE_P)
			spud_va = (u64 *)__va(spgd[pgd_i] &
					      0x000ffffffffff000ULL);

		for (pud_i = 0; pud_i < 512; pud_i++) {
			u64 *upmd, *spmd_va = NULL;

			if (!(upud[pud_i] & UM_PTE_PRESENT))
				continue;
			upmd = (u64 *)__va(upud[pud_i] &
					   0x000ffffffffff000ULL);
			if (spud_va && (spud_va[pud_i] & KVM_X86_PTE_P))
				spmd_va = (u64 *)__va(spud_va[pud_i] &
						      0x000ffffffffff000ULL);

			for (pmd_i = 0; pmd_i < 512; pmd_i++) {
				u64 *upte, *spte_va = NULL;

				if (!(upmd[pmd_i] & UM_PTE_PRESENT))
					continue;
				upte = (u64 *)__va(upmd[pmd_i] &
						   0x000ffffffffff000ULL);
				if (spmd_va &&
				    (spmd_va[pmd_i] & KVM_X86_PTE_P))
					spte_va = (u64 *)__va(spmd_va[pmd_i] &
							      0x000ffffffffff000ULL);

				for (pte_i = 0; pte_i < 512; pte_i++) {
					u64 ume = upte[pte_i];
					u64 expected, sval;
					u64 va;

					if (!(ume & UM_PTE_PRESENT))
						continue;
					leaves++;
					expected = kvm_um_pte_to_x86(ume);
					sval = spte_va ? spte_va[pte_i] : 0;

					if ((expected & mask) ==
					    (sval & mask)) {
						matches++;
						continue;
					}
					diverges++;
					if (diverges <= max_log) {
						va = ((u64)pgd_i << 39) |
						     ((u64)pud_i << 30) |
						     ((u64)pmd_i << 21) |
						     ((u64)pte_i << 12);
						pr_info("um: kvm audit_pgd[%s]: DIVERGE #%u va=0x%llx um=0x%llx expected=0x%llx shadow=0x%llx\n",
							tag, diverges,
							(unsigned long long)va,
							(unsigned long long)ume,
							(unsigned long long)expected,
							(unsigned long long)sval);
					}
				}
			}
		}
	}

	/*
	 * #274 issue #6: REVERSE-direction scan. Walk the shadow
	 * tree's user half (PGD slots 0..255) and look for present
	 * leaves whose corresponding UML pgd entry is absent. These
	 * are stale shadow leaves — they let the guest read the OLD
	 * physical page after the UML pgd has unmapped the VA. With
	 * the transactional fill (issue #8 fix) in place these
	 * should be zero on every cached-skip; logging non-zero
	 * here would reveal a remaining missed-invalidate path.
	 *
	 * Skip the bootstrap-alias VA range (preserved by fill).
	 */
	{
		u64 *spgd = shadow->pgd;
		u64 *upgd = uml_pgd_va;
		u64 alias_lo = kvm_bootstrap_va_get();
		u64 alias_hi = alias_lo + 4 * PAGE_SIZE;
		unsigned int shadow_extra = 0;
		unsigned int reverse_logged = 0;

		for (pgd_i = 0; pgd_i < 256; pgd_i++) {
			u64 *spud, *upud = NULL;
			bool upgd_present;

			if (!(spgd[pgd_i] & KVM_X86_PTE_P))
				continue;
			spud = (u64 *)__va(spgd[pgd_i] &
					   0x000ffffffffff000ULL);
			upgd_present = !!(upgd[pgd_i] & UM_PTE_PRESENT);
			if (upgd_present)
				upud = (u64 *)__va(upgd[pgd_i] &
						   0x000ffffffffff000ULL);

			for (pud_i = 0; pud_i < 512; pud_i++) {
				u64 *spmd, *upmd = NULL;
				bool upud_present;

				if (!(spud[pud_i] & KVM_X86_PTE_P))
					continue;
				spmd = (u64 *)__va(spud[pud_i] &
						   0x000ffffffffff000ULL);
				upud_present = upud &&
					(upud[pud_i] & UM_PTE_PRESENT);
				if (upud_present)
					upmd = (u64 *)__va(upud[pud_i] &
							   0x000ffffffffff000ULL);

				for (pmd_i = 0; pmd_i < 512; pmd_i++) {
					u64 *spte, *upte = NULL;
					bool upmd_present;

					if (!(spmd[pmd_i] & KVM_X86_PTE_P))
						continue;
					spte = (u64 *)__va(spmd[pmd_i] &
							   0x000ffffffffff000ULL);
					upmd_present = upmd &&
						(upmd[pmd_i] & UM_PTE_PRESENT);
					if (upmd_present)
						upte = (u64 *)__va(upmd[pmd_i] &
								   0x000ffffffffff000ULL);

					for (pte_i = 0; pte_i < 512;
					     pte_i++) {
						u64 va, sval, ume = 0;

						if (!(spte[pte_i] &
						      KVM_X86_PTE_P))
							continue;
						sval = spte[pte_i];
						if (upte)
							ume = upte[pte_i];
						if (ume & UM_PTE_PRESENT)
							continue;
						va = ((u64)pgd_i << 39) |
						     ((u64)pud_i << 30) |
						     ((u64)pmd_i << 21) |
						     ((u64)pte_i << 12);
						if (alias_lo &&
						    va >= alias_lo &&
						    va < alias_hi)
							continue;
						shadow_extra++;
						if (reverse_logged < max_log) {
							reverse_logged++;
							pr_info("um: kvm audit_pgd[%s]: SHADOW-EXTRA #%u va=0x%llx shadow=0x%llx (ume=0x%llx)\n",
								tag,
								shadow_extra,
								(unsigned long long)va,
								(unsigned long long)sval,
								(unsigned long long)ume);
						}
					}
				}
			}
		}

		pr_info("um: kvm audit_pgd[%s]: leaves=%u match=%u DIV=%u SHADOW-EXTRA=%u synced=%d pgd=%p\n",
			tag, leaves, matches, diverges, shadow_extra,
			shadow->synced, uml_pgd_va);
		return (int)(diverges + shadow_extra);
	}
}
EXPORT_SYMBOL_GPL(kvm_shadow_audit_pgd);
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

/*
 * Per-task vCPU handle allocator. Stage A redesign of the KVM backend
 * (Documentation/virt/uml/redesign/03-architecture-review-2026-04-27).
 *
 * Each UML task that reaches kvm_run_userspace allocates one handle
 * via this function on first use. The handle pins a KVM_CREATE_VCPU
 * fd + the corresponding mmap of struct kvm_run for the task's
 * lifetime.
 *
 * KVM contract honoured:
 *   - vcpu fd is owned by exactly one task (no aliasing).
 *   - struct kvm_run mmap is single-writer (no inter-task races on
 *     run->exit_reason / run->s.regs / run->io / run->mmio).
 *   - KVM_SET_SIGNAL_MASK installed by kvm_vcpu_handle_install_sigmask
 *     blocks every host signal except SIGALRM (UML's timer-driven
 *     scheduler tick — required for CPU-bound guest preemption) and
 *     KVM_UM_KICK_SIGNAL (future SMP eviction). Other signals are
 *     deferred to after KVM_RUN returns where unblock_signals()
 *     drains UML's handler queue at a safe point.
 *
 * Returns the new handle on success or an ERR_PTR on failure. Caller
 * (kvm_vcpu_for_current) stashes the pointer on
 * current->thread.arch.kvm.vcpu and never publishes a partially-
 * initialised handle.
 */
struct kvm_vcpu_handle *kvm_vcpu_handle_alloc(void)
{
	/*
	 * vcpu_id allocation: under INTEGRATED, kvm_init() no longer
	 * pre-creates vcpu0 (Stage A.7 deletion), so per-task vCPUs
	 * start at id 0 and increment monotonically. KVM accepts vcpu_id
	 * values up to KVM_MAX_VCPU_IDS (4096 on x86); UML processes
	 * don't realistically approach that.
	 *
	 * Under !INTEGRATED (harness path), vcpu0 is still pre-created
	 * for the harness's own use; INTEGRATED is the production path
	 * and harness builds don't reach kvm_vcpu_handle_alloc anyway,
	 * so the id=0 start is safe.
	 */
	static atomic_t next_vcpu_id = ATOMIC_INIT(0);
	struct kvm_vcpu_handle *h;
	int vcpu_fd, mmap_size, vcpu_id;
	void *run;

	if (kvm_ctx.vm_fd < 0)
		return ERR_PTR(-EIO);

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return ERR_PTR(-ENOMEM);

	vcpu_id = atomic_fetch_inc(&next_vcpu_id);
	vcpu_fd = os_ioctl_generic(kvm_ctx.vm_fd, KVM_CREATE_VCPU,
				   (unsigned long)vcpu_id);
	if (vcpu_fd < 0) {
		pr_err("um: kvm vcpu_alloc: KVM_CREATE_VCPU(id=%d) failed (%d)\n",
		       vcpu_id, vcpu_fd);
		kfree(h);
		return ERR_PTR(vcpu_fd);
	}

	mmap_size = os_ioctl_generic(kvm_ctx.kvm_fd,
				     KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size <= 0) {
		pr_err("um: kvm vcpu_alloc: KVM_GET_VCPU_MMAP_SIZE failed (%d)\n",
		       mmap_size);
		os_close_file(vcpu_fd);
		kfree(h);
		return ERR_PTR(mmap_size ? mmap_size : -EIO);
	}

	run = os_mmap_rw_shared(vcpu_fd, mmap_size);
	if (!run) {
		pr_err("um: kvm vcpu_alloc: mmap of kvm_run (size %d) failed\n",
		       mmap_size);
		os_close_file(vcpu_fd);
		kfree(h);
		return ERR_PTR(-ENOMEM);
	}

	h->fd       = vcpu_fd;
	h->run      = run;
	h->run_size = mmap_size;
	/* All cache flags start false; primed-once paths set them on first use. */

	/*
	 * Install the host no-op signal handler for the kick signal on
	 * first vCPU alloc only. sigaction is process-wide so once is
	 * enough — but we only want to do it lazily, after the host
	 * signal infrastructure is fully up (post-init_backend boot).
	 * A simple atomic guards the install.
	 */
	{
		static atomic_t kick_signal_installed = ATOMIC_INIT(0);

		if (atomic_xchg(&kick_signal_installed, 1) == 0)
			register_kvm_kick_signal(KVM_UM_KICK_SIGNAL);
	}

	pr_info_ratelimited("um: kvm vcpu_alloc: pid=%d tid=%d vcpu_id=%d vcpu_fd=%d run=%p size=%d\n",
			    current->tgid, current->pid, vcpu_id, vcpu_fd, run, mmap_size);
	return h;
}

void kvm_vcpu_handle_destroy(struct kvm_vcpu_handle *h)
{
	if (!h)
		return;
	if (h->run && h->run_size)
		os_unmap_memory(h->run, (int)h->run_size);
	if (h->fd >= 0)
		os_close_file(h->fd);
	kfree(h);
}
