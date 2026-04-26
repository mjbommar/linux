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
int kvm_ensure_cpuid_done(void)
{
	const u32 max_entries = 256;
	size_t buf_sz;
	struct kvm_cpuid2 *cpuid;
	int rc;

	if (kvm_ctx.cpuid_done)
		return 0;
	if (kvm_ctx.kvm_fd < 0 || kvm_ctx.vcpu0_fd < 0)
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
				 *   bit 5  = AVX2
				 *   bit 16 = AVX512F
				 *   bit 17 = AVX512DQ
				 *   bit 21 = AVX512IFMA
				 *   bit 26 = AVX512PF
				 *   bit 27 = AVX512ER
				 *   bit 28 = AVX512CD
				 *   bit 30 = AVX512BW
				 *   bit 31 = AVX512VL
				 */
				e->ebx &= ~((1U << 5)  | (1U << 16) |
					    (1U << 17) | (1U << 21) |
					    (1U << 26) | (1U << 27) |
					    (1U << 28) | (1U << 30) |
					    (1U << 31));
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

	rc = os_ioctl_generic(kvm_ctx.vcpu0_fd, KVM_SET_CPUID2,
			      (unsigned long)cpuid);
	if (rc < 0) {
		pr_warn_once("um: kvm: KVM_SET_CPUID2 failed (%d); using KVM-default CPUID\n",
			     rc);
		goto out_free;
	}

	kvm_ctx.cpuid_done = true;
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

	shadow = kzalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return NULL;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		kfree(shadow);
		return NULL;
	}

	shadow->pgd_page = page;
	shadow->pgd      = page_address(page);
	shadow->pgd_gpa  = (u64)__pa(shadow->pgd);
	shadow->dirty    = true;
	shadow->synced   = false;
	shadow->synced_pgd_va = 0;
	mutex_init(&shadow->fill_lock);
	return shadow;
}
EXPORT_SYMBOL_GPL(kvm_shadow_mm_alloc);

void kvm_shadow_mm_free(struct kvm_shadow_mm *shadow)
{
	struct kvm_um *ctx = kvm_backend_ctx();

	if (!shadow)
		return;

	/*
	 * #274 / T8: invalidate the SREGS cache if this shadow's
	 * pgd_gpa matches the cached CR3 we last programmed into the
	 * vCPU. The kvm_enter_guest fast-path skips KVM_SET_SREGS
	 * when (cached_cr3_gpa == new_cr3_gpa && !shadow->dirty); if
	 * we free the underlying PGD page without invalidating that
	 * cache, the next entry may reuse the freed pages as the
	 * vCPU's CR3 source — KVM walks the (now-freed) shadow tree
	 * during shadow-page-table maintenance and reads garbage.
	 *
	 * Setting cached_cr3_gpa = 0 forces the next entry to issue
	 * KVM_SET_SREGS with the new mm's pgd_gpa.
	 */
	if (shadow->pgd_gpa != 0 && ctx->cached_cr3_gpa == shadow->pgd_gpa)
		ctx->cached_cr3_gpa = 0;

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
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
	u64 *pgd = pgd_va;
	unsigned int pgd_i, pud_i, pmd_i, pte_i;
	int installed = 0;
	int rc;

	if (!pgd)
		return -EINVAL;
	if (!shadow)
		return -ENODEV;

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
	 */
	shadow->synced = true;
	shadow->synced_pgd_va = (u64)pgd_va;
	return installed;
}

int kvm_shadow_map_page(u64 va, u64 phys_gpa, u64 leaf_flags)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
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
	pte[pte_i] = (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) |
		     (leaf_flags | KVM_X86_PTE_P | KVM_X86_PTE_A);
	shadow->dirty = true;
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

	if (shadow)
		shadow->dirty = true;
}
EXPORT_SYMBOL_GPL(kvm_shadow_pgd_clear_user);

int kvm_shadow_invalidate_va_range(u64 va_start, u64 len)
{
	struct kvm_shadow_mm *shadow = kvm_shadow_mm_current();
	u64 *pgd;
	u64 va, va_end;
	unsigned int cleared = 0;

	if (!shadow || !shadow->pgd)
		return -ENODEV;
	if (!len)
		return -EINVAL;
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
			pte[pte_i] = 0;
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
	 */
	shadow->dirty = true;
	shadow->synced = false;
	return 0;
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

	pr_info("um: kvm audit_pgd[%s]: leaves=%u match=%u DIV=%u synced=%d pgd=%p\n",
		tag, leaves, matches, diverges,
		shadow->synced, uml_pgd_va);
	return (int)diverges;
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
