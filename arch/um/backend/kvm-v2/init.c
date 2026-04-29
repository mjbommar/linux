// SPDX-License-Identifier: GPL-2.0
/*
 * UML backend v2 (KVM) — Phase A.1: probe + capability negotiation.
 *
 * Per memo 26 §A.1 + memo 27 Part D.5. Scope is intentionally narrow:
 * confirm the host can run KVM, learn which extensions are available,
 * and emit the observability tracepoint. No KVM_CREATE_VM, no vCPU,
 * no memslot allocator — those are A.2 / A.3 / B.x respectively.
 *
 * Why probe + init are separate (mirrors the v1 archive's design,
 * memo 21 §D-03a): probe() is the arbiter's "is this backend usable
 * on this host" yes/no. init() is "commit to this backend; set up
 * persistent state." Phase A.1 leaves init() lightweight on purpose —
 * the persistent VM context belongs to A.2 (context.c).
 *
 * Why the cold ops still delegate to seccomp in this commit
 * (kvm_v2_backend.h header comment): the dispatch macro neither
 * NULL-checks nor synthesizes -ENOSYS, and validate_hot_ops() panics
 * on NULL HOT ops. A delegating ops table keeps `force=kvm-v2`
 * boot-stable while individual ops migrate to v2 implementations
 * across A.2 → J.x.
 */

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
 * Cap bitmap layout for the kvm_v2_init tracepoint (and any future
 * introspection). Bits 0-7 are required caps; missing any of them
 * fails init. Bits 8+ are advisory / optional.
 */
#define KVM_V2_CAP_SYNC_REGS		(1ULL << 0)
#define KVM_V2_CAP_SET_GUEST_DEBUG	(1ULL << 1)
#define KVM_V2_CAP_HYPERV		(1ULL << 8)

#define KVM_V2_CAPS_REQUIRED \
	(KVM_V2_CAP_SYNC_REGS | KVM_V2_CAP_SET_GUEST_DEBUG)

static int kvm_v2_kvm_fd = -1;	/* /dev/kvm; closed in shutdown() */
static int kvm_v2_api_version;
static u64 kvm_v2_caps;

/*
 * Probe is the arbiter's go/no-go check. Open /dev/kvm, confirm the
 * stable ABI version, then close — the fd is reopened in init() once
 * the arbiter has committed to v2. Cheap (two syscalls) and avoids
 * leaking the fd if the arbiter rejects v2 for an unrelated reason.
 */
int kvm_v2_probe(void)
{
	int fd, api;

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0) {
		pr_info("um: kvm-v2 probe: /dev/kvm open failed (%d); will fall back\n",
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

	pr_info("um: kvm-v2 probe: /dev/kvm OK (API %d)\n", api);
	return 0;
}

/*
 * Per-cap query. KVM_CHECK_EXTENSION returns >=0 (0 = unsupported,
 * >0 = supported, with the value sometimes carrying cap-specific
 * bitmap data — e.g. SYNC_REGS reports which reg classes sync). For
 * Phase A.1 we collapse to "any positive return = supported."
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
		pr_info("um: kvm-v2 init: optional cap %s not supported (continuing)\n",
			name);
		return 0;
	}
	*caps_out |= cap_bit;
	return 0;
}

int kvm_v2_init(const struct um_backend_args *args)
{
	int fd, rc;
	u64 caps = 0;

	(void)args;

	if (kvm_v2_kvm_fd >= 0) {
		pr_warn("um: kvm-v2 init: called twice (kvm_fd=%d)\n",
			kvm_v2_kvm_fd);
		return -EBUSY;
	}

	fd = os_open_file("/dev/kvm", of_rdwr(OPENFLAGS()), 0);
	if (fd < 0) {
		pr_err("um: kvm-v2 init: /dev/kvm reopen failed (%d) — probe succeeded?\n",
		       fd);
		return fd;
	}

	rc = os_ioctl_generic(fd, KVM_GET_API_VERSION, 0);
	if (rc < 0) {
		pr_err("um: kvm-v2 init: KVM_GET_API_VERSION failed (%d)\n", rc);
		goto err_close;
	}
	kvm_v2_api_version = rc;

	rc = probe_cap(fd, KVM_CAP_SYNC_REGS, "KVM_CAP_SYNC_REGS",
		       true, KVM_V2_CAP_SYNC_REGS, &caps);
	if (rc)
		goto err_close;
	rc = probe_cap(fd, KVM_CAP_SET_GUEST_DEBUG, "KVM_CAP_SET_GUEST_DEBUG",
		       true, KVM_V2_CAP_SET_GUEST_DEBUG, &caps);
	if (rc)
		goto err_close;
	rc = probe_cap(fd, KVM_CAP_HYPERV, "KVM_CAP_HYPERV",
		       false, KVM_V2_CAP_HYPERV, &caps);
	if (rc)
		goto err_close;

	if ((caps & KVM_V2_CAPS_REQUIRED) != KVM_V2_CAPS_REQUIRED) {
		pr_err("um: kvm-v2 init: required cap bitmap mismatch (have %#llx, need %#llx)\n",
		       caps, (u64)KVM_V2_CAPS_REQUIRED);
		rc = -EOPNOTSUPP;
		goto err_close;
	}

	kvm_v2_kvm_fd = fd;
	kvm_v2_caps   = caps;

	trace_um_backend_kvm_v2_init(fd, kvm_v2_api_version, caps);

	pr_info("um: kvm-v2 probed: kvm_fd=%d api=%d caps=%#llx (A.1 — vm/vcpu deferred)\n",
		fd, kvm_v2_api_version, caps);
	return 0;

err_close:
	os_close_file(fd);
	return rc;
}

void kvm_v2_shutdown(void)
{
	if (kvm_v2_kvm_fd >= 0) {
		os_close_file(kvm_v2_kvm_fd);
		kvm_v2_kvm_fd = -1;
	}
}
