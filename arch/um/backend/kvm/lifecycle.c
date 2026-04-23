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
#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>

#include <os.h>
#include <asm/backend.h>
#include <asm/processor-generic.h>	/* task_size */

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
	 * Policy A from 03b-memslot-policy.md: one giant memslot at
	 * init covers the whole UML address space. guest_phys_addr =
	 * 0, userspace_addr = 0, memory_size = task_size. KVM's EPT
	 * populates lazily on first guest access, so this is cheap
	 * even at the ~128 TiB task_size ceiling — we're not
	 * pre-faulting anything, just declaring the range as "valid
	 * guest-physical memory backed by the UML kernel's own VA".
	 *
	 * D-04a: KVM rejects userspace_addr=0 + oversized slot on
	 * real hosts. Treat registration failure as non-fatal for
	 * the scaffold — D-04b revisits the memslot parameters
	 * alongside SREGS/CR3 setup. Without the slot, KVM_RUN will
	 * fault on any guest access, which is exactly the expected
	 * failure mode for D-04a ("first KVM_RUN returns with a
	 * readable error").
	 */
	{
		struct kvm_userspace_memory_region region = {
			.slot			= 0,
			.flags			= 0,
			.guest_phys_addr	= 0,
			.memory_size		= task_size,
			.userspace_addr		= 0,
		};
		int rc = os_ioctl_generic(vmfd, KVM_SET_USER_MEMORY_REGION,
					  (unsigned long)&region);

		if (rc < 0)
			pr_warn("um: kvm init: KVM_SET_USER_MEMORY_REGION failed (%d); deferring to D-04b\n",
				rc);
		else
			pr_info("um: kvm init: memslot [0,%lx) registered\n",
				task_size);
	}

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

	pr_info("um: kvm init: kvm=%d vm=%d vcpu0=%d run_size=%zu memslot [0,%lx)\n",
		kvm_ctx.kvm_fd, kvm_ctx.vm_fd, kvm_ctx.vcpu0_fd,
		kvm_ctx.run_size, task_size);
	return 0;
}

void kvm_shutdown(void)
{
	/*
	 * Order: vCPU resources, then VM, then /dev/kvm. mmap of
	 * kvm_run survives the vcpu_fd close (the mapping is
	 * refcounted in the kernel), so we munmap first via
	 * os_unmap_memory() before closing the fd.
	 */
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
