// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — lifecycle (probe, init, shutdown).
 *
 * Workstream D-03a. Replaces the pr_info placeholders shipped in
 * D-02's stubs.c with a real /dev/kvm probe:
 *
 *   - probe()    opens /dev/kvm via os_open_file() and issues
 *                KVM_GET_API_VERSION to confirm the host kernel
 *                speaks at least the stable ABI version (12,
 *                introduced in 2.6.22 — every kernel we care
 *                about supports it). Failure to open or a
 *                version mismatch returns an error so the
 *                arbiter can fall back per the A-01 contract.
 *   - init()     stashes the kvm fd for the rest of the backend
 *                (D-03b: mm_attach uses it for KVM_CREATE_VM).
 *   - shutdown() closes the kvm fd.
 *
 * No USER TU is needed at this layer — os_open_file() and
 * os_ioctl_generic() both run in kernel context on UML and call
 * through to host libc via the os-Linux glue. The D-02-draft
 * USER-side lifecycle_user.c that tried to do this in the USER
 * link context segfaulted during early-probe; the kernel-side
 * approach here avoids that class of failure entirely.
 *
 * Subsequent D-03 commits build on this: D-03b wires mm_attach
 * to KVM_CREATE_VM using `kvm_fd`; D-03c adds memslot plumbing
 * via KVM_SET_USER_MEMORY_REGION for mm_map/mm_unmap.
 */
#include <linux/errno.h>
#include <linux/kvm.h>
#include <linux/printk.h>

#include <os.h>
#include <asm/backend.h>

#include "kvm_backend.h"

/*
 * The /dev/kvm fd. Held for the lifetime of the UML process once
 * init() succeeds. -1 until probe() opens it. Single global is
 * fine: KVM gives one /dev/kvm handle per process and the backend
 * is per-process (one UML kernel = one host process).
 */
static int kvm_fd = -1;

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
	int fd;

	(void)args;

	if (kvm_fd >= 0) {
		pr_warn("um: kvm init called twice (fd %d already open)\n",
			kvm_fd);
		return -EBUSY;
	}

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0) {
		pr_err("um: kvm init: /dev/kvm reopen failed (%d) — probe succeeded?\n",
		       fd);
		return fd;
	}

	kvm_fd = fd;
	pr_info("um: kvm init: /dev/kvm fd %d acquired; D-03b memslots pending\n",
		kvm_fd);
	return 0;
}

void kvm_shutdown(void)
{
	if (kvm_fd < 0)
		return;

	os_close_file(kvm_fd);
	kvm_fd = -1;
}

int kvm_backend_fd(void)
{
	return kvm_fd;
}
