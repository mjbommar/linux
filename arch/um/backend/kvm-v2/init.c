// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend lifecycle.
 *
 * probe() is the arbiter's cheap "is this backend usable on this host"
 * check. init() commits to the backend, opens /dev/kvm for the VM
 * lifetime, negotiates required caps, and hands the fd to
 * kvm_v2_vm_create(). Per-VM state lives in context.c.
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <os.h>
#include <asm/backend.h>
#include <asm/trace/um_backend.h>

#include "kvm_v2_backend.h"

/*
 * Cap bitmap layout for the kvm_v2_init tracepoint and cap reporting.
 * Bits 0-7 are required caps; missing any of them fails init. Bits 8+
 * are optional.
 */
#define KVM_V2_CAP_SYNC_REGS		BIT_ULL(0)
#define KVM_V2_CAP_SET_GUEST_DEBUG	BIT_ULL(1)
#define KVM_V2_CAP_HYPERV		BIT_ULL(8)

#define KVM_V2_CAPS_REQUIRED \
	(KVM_V2_CAP_SYNC_REGS | KVM_V2_CAP_SET_GUEST_DEBUG)

/*
 * api_version stays here as a lifetime-of-backend fact about the host
 * kernel. Capability bits move to vm.caps inside kvm_v2_vm_create().
 */
static int kvm_v2_api_version;

/*
 * Probe is the arbiter's go/no-go check. Open /dev/kvm, confirm the
 * stable ABI version, then close. The fd is reopened in init() once
 * the arbiter has committed to v2. Cheap (two syscalls) and avoids
 * leaking the fd if the arbiter rejects v2 for an unrelated reason.
 */
int kvm_v2_probe(void)
{
	int fd, api;

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0) {
		pr_debug("um: kvm-v2 probe: /dev/kvm open failed (%d); will fall back\n",
			 fd);
		return fd;
	}

	api = os_ioctl_generic(fd, KVM_GET_API_VERSION, 0);
	os_close_file(fd);
	if (api < 0) {
		pr_warn("um: kvm-v2 probe: KVM_GET_API_VERSION failed (%d)\n",
			api);
		return api;
	}
	if (api != KVM_API_VERSION) {
		pr_warn("um: kvm-v2 probe: host API %d, built for %d\n",
			api, KVM_API_VERSION);
		return -EOPNOTSUPP;
	}

	pr_debug("um: kvm-v2 probe: /dev/kvm OK (API %d)\n", api);
	return 0;
}

/*
 * Per-cap query. KVM_CHECK_EXTENSION returns >=0 (0 = unsupported,
 * >0 = supported, with the value sometimes carrying cap-specific
 * bitmap data, e.g. SYNC_REGS reports which reg classes sync). Treat
 * any positive return as supported.
 */
static int probe_cap(int fd, unsigned long cap, const char *name,
		     bool required, u64 cap_bit, u64 *caps_out)
{
	int rc = os_ioctl_generic(fd, KVM_CHECK_EXTENSION, cap);

	if (rc < 0) {
		pr_err("um: kvm-v2 init: KVM_CHECK_EXTENSION(%s) failed (%d)\n",
		       name, rc);
		return rc;
	}
	if (rc == 0) {
		if (required) {
			pr_err("um: kvm-v2 init: required cap %s not supported by host KVM\n",
			       name);
			return -EOPNOTSUPP;
		}
		pr_debug("um: kvm-v2 init: optional cap %s not supported (continuing)\n",
			 name);
		return 0;
	}
	*caps_out |= cap_bit;
	return 0;
}

static int kvm_v2_check_api_version(int fd)
{
	int rc;

	rc = os_ioctl_generic(fd, KVM_GET_API_VERSION, 0);
	if (rc < 0) {
		pr_err("um: kvm-v2 init: KVM_GET_API_VERSION failed (%d)\n",
		       rc);
		return rc;
	}
	kvm_v2_api_version = rc;
	return 0;
}

static int kvm_v2_probe_caps(int fd, u64 *caps)
{
	int rc;

	*caps = 0;
	rc = probe_cap(fd, KVM_CAP_SYNC_REGS, "KVM_CAP_SYNC_REGS",
		       true, KVM_V2_CAP_SYNC_REGS, caps);
	if (rc)
		return rc;
	rc = probe_cap(fd, KVM_CAP_SET_GUEST_DEBUG, "KVM_CAP_SET_GUEST_DEBUG",
		       true, KVM_V2_CAP_SET_GUEST_DEBUG, caps);
	if (rc)
		return rc;
	rc = probe_cap(fd, KVM_CAP_HYPERV, "KVM_CAP_HYPERV",
		       false, KVM_V2_CAP_HYPERV, caps);
	if (rc)
		return rc;

	if ((*caps & KVM_V2_CAPS_REQUIRED) != KVM_V2_CAPS_REQUIRED) {
		pr_err("um: kvm-v2 init: required cap bitmap mismatch (have %#llx, need %#llx)\n",
		       *caps, (u64)KVM_V2_CAPS_REQUIRED);
		return -EOPNOTSUPP;
	}
	return 0;
}

static int kvm_v2_create_vcpu_pool(void)
{
	int rc;

	/*
	 * Create the initial vCPU pool. Failure tears the VM back down so init
	 * either fully succeeds or leaves no v2 state behind.
	 */
	rc = kvm_v2_vcpu_create(kvm_v2_vm_get());
	if (rc) {
		pr_err("um: kvm-v2 init: kvm_v2_vcpu_create failed (%d)\n", rc);
		kvm_v2_vm_destroy();
		return rc;
	}
	return 0;
}

static int kvm_v2_open_committed_fd(void)
{
	int fd;

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0)
		pr_err("um: kvm-v2 init: /dev/kvm reopen failed (%d); probe succeeded?\n",
		       fd);

	return fd;
}

static int kvm_v2_create_vm_from_fd(int fd, u64 caps)
{
	int rc;

	/*
	 * Hand /dev/kvm to context.c. From here on the fd lives on
	 * struct kvm_v2_vm and is closed by kvm_v2_vm_destroy alongside
	 * the VM fd (see context.c for the close-order rationale).
	 */
	rc = kvm_v2_vm_create(fd, caps);
	if (rc)
		pr_err("um: kvm-v2 init: kvm_v2_vm_create failed (%d)\n", rc);

	return rc;
}

int kvm_v2_init(const struct um_backend_args *args)
{
	int fd, rc;
	u64 caps;

	(void)args;

	if (kvm_v2_vm_get()) {
		pr_warn("um: kvm-v2 init: called twice (vm already created)\n");
		return -EBUSY;
	}

	fd = kvm_v2_open_committed_fd();
	if (fd < 0)
		return fd;

	rc = kvm_v2_check_api_version(fd);
	if (rc)
		goto err_close;

	rc = kvm_v2_probe_caps(fd, &caps);
	if (rc)
		goto err_close;

	trace_um_backend_kvm_v2_init(fd, kvm_v2_api_version, caps);

	pr_debug("um: kvm-v2 probed: kvm_fd=%d api=%d caps=%#llx (creating VM)\n",
		 fd, kvm_v2_api_version, caps);

	rc = kvm_v2_create_vm_from_fd(fd, caps);
	if (rc)
		goto err_close;

	rc = kvm_v2_create_vcpu_pool();
	if (rc)
		return rc;

	return 0;

err_close:
	os_close_file(fd);
	return rc;
}

void kvm_v2_shutdown(void)
{
	kvm_v2_vcpu_destroy();
	kvm_v2_vm_destroy();
}
