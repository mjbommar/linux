// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — lifecycle (probe, init, shutdown).
 *
 * Workstream D-03a: real /dev/kvm probe.
 * Workstream D-03b: eager KVM_CREATE_VM in init(); per-mm state
 * lives in mm.c and refcounts the single shared VM per the
 * decisions-log D57 one-VM-per-UML-process model.
 *
 *   - probe()    opens /dev/kvm and issues KVM_GET_API_VERSION
 *                to confirm the host kernel speaks the stable
 *                ABI version (12, since 2.6.22). On failure,
 *                returns the errno so the arbiter can fall back
 *                per the A-01 contract. Does not retain host
 *                state.
 *   - init()     reopens /dev/kvm, issues KVM_CREATE_VM, and
 *                stashes both fds in the module-static kvm_um
 *                for the rest of the backend to reach via
 *                kvm_backend_vm_fd() / kvm_backend_fd().
 *   - shutdown() closes vm_fd then kvm_fd.
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

#include "kvm_backend.h"

/*
 * Single per-UML-process KVM context. Lifetime matches
 * init_backend() → uml_cleanup() / backend shutdown(). The
 * mm_attach/mm_detach refcount that ties UML mm lifetime into
 * this struct lives in mm.c; everything kvm_um exposes here is
 * read-only after init().
 */
static struct kvm_um kvm_ctx = {
	.kvm_fd = -1,
	.vm_fd  = -1,
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
		return -ENOTSUPP;
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

	kvm_ctx.kvm_fd = kfd;
	kvm_ctx.vm_fd  = vmfd;
	refcount_set(&kvm_ctx.mm_refcount, 0);

	pr_info("um: kvm init: /dev/kvm fd %d, vm fd %d acquired; memslots pending (D-03c)\n",
		kvm_ctx.kvm_fd, kvm_ctx.vm_fd);
	return 0;
}

void kvm_shutdown(void)
{
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

struct kvm_um *kvm_backend_ctx(void)
{
	return &kvm_ctx;
}
