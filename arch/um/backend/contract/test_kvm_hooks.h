/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private test hooks into the KVM backend's integrated path
 * (memo 08). Only consumed by arch/um/backend/contract/test_ops.c.
 *
 * Prototypes duplicate the public ones in
 * arch/um/backend/kvm/kvm_backend.h rather than including that
 * header cross-subdirectory — the private KVM header is not on
 * the contract-test translation unit's include path and adding
 * one relative include to this one TU is easier to review than
 * a cross-subdir header plumb.
 *
 * If either side drifts, the kvm_wire_ports_test / NULL-probe
 * test assertions fail at runtime.
 */
#ifndef __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H
#define __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

#include <linux/kvm.h>
#include <linux/types.h>
#include <sysdep/ptrace.h>

int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa);
int kvm_exit_guest_probe(struct uml_pt_regs *dst, const struct kvm_regs *src);
int kvm_bootstrap_copy_lstar(u8 *dst, size_t len);
int kvm_bootstrap_force_init(void);

/*
 * Audit round-4 F2: pure-data helper that computes the R11 value
 * fed to the bootstrap SYSRETQ gadget given a saved user RFLAGS.
 * Exposed so the contract KUnit suite can cover the user-flag
 * round-trip without needing /dev/kvm.
 */
u64 kvm_build_sysret_r11_probe(u64 saved_user_rflags);

/* Memo 09 step 1 lifecycle probes. */
u64 kvm_shadow_pgd_gpa(void);
int kvm_shadow_pgd_alloc(void);
void kvm_shadow_pgd_free(void);

/* Memo 11 G3 gadget state channel probes. */
int kvm_gadget_state_alloc(void);
void kvm_gadget_state_free(void);
void kvm_gadget_state_refresh(void);
u64 kvm_gadget_state_va(void);
u64 kvm_gadget_state_gpa(void);

/* Memo 11 G5 gadget vvar clock-page probes. */
int kvm_gadget_vvar_alloc(void);
void kvm_gadget_vvar_free(void);
void kvm_gadget_vvar_refresh(void);
u64 kvm_gadget_vvar_va(void);
u64 kvm_gadget_vvar_gpa(void);

struct _test_kvm_gadget_vvar {
	u32 seq;
	u32 _pad0;
	s64 monotonic_sec;
	s64 monotonic_nsec;
	s64 realtime_sec;
	s64 realtime_nsec;
	u64 _pad1[4];
};
#define _TEST_KVM_VVAR_OFF_SEQ		0x00
#define _TEST_KVM_VVAR_OFF_MONO_SEC	0x08
#define _TEST_KVM_VVAR_OFF_MONO_NSEC	0x10
#define _TEST_KVM_VVAR_OFF_REAL_SEC	0x18
#define _TEST_KVM_VVAR_OFF_REAL_NSEC	0x20

/*
 * Mirror of struct kvm_gadget_state + offsets from
 * arch/um/backend/kvm/kvm_backend.h. The contract TU
 * doesn't include the private header, so drifting the
 * two copies would fail the _TEST_KVM_GADGET_OFF_*
 * equivalence checks below.
 */
struct _test_kvm_gadget_state {
	u32 seq;
	u32 cpu_id;
	u32 tgid;
	u32 tid;
	u32 ppid;
	u32 uid;
	u32 euid;
	u32 gid;
	u32 egid;
	u32 _pad;
};
#define _TEST_KVM_GADGET_OFF_TGID	0x08
#define _TEST_KVM_GADGET_OFF_TID	0x0c
#define _TEST_KVM_GADGET_OFF_UID	0x14

/*
 * Memo 10 syscall classification. Mirror of the enum in
 * arch/um/backend/kvm/kvm_backend.h; duplicated here for the
 * same reason as the kvm_enter_guest_probe prototype above
 * (private header not on the contract TU's include path).
 */
enum kvm_syscall_class {
	_TEST_KVM_SYSCALL_CLASS_PASSTHROUGH = 0,
	_TEST_KVM_SYSCALL_CLASS_VCPU_STATE,
	_TEST_KVM_SYSCALL_CLASS_SIGFRAME,
	_TEST_KVM_SYSCALL_CLASS_TRAP,
	_TEST_KVM_SYSCALL_CLASS_GADGET,		/* memo 11 G7 */
};
enum kvm_syscall_class kvm_classify_syscall(unsigned long nr);

#define _TEST_UM_KVM_SYSCALL_PORT	0xf4
#define _TEST_UM_KVM_SYSRETQ_PORT	0xf5

/*
 * Task #250 v2 / memo 12 snapshot primitives. Same private-header
 * duplication discipline as the rest of this file. Forward declare
 * the opaque struct so the test TU can hold pointers without
 * needing the full layout.
 */
struct kvm_snapshot;
struct kvm_snapshot *kvm_snapshot_alloc(void);
int kvm_snapshot_capture(struct kvm_snapshot *snap);
int kvm_snapshot_restore_full(struct kvm_snapshot *snap);
void kvm_snapshot_free(struct kvm_snapshot *snap);
void kvm_snapshot_destroy(struct kvm_snapshot *snap);

/* Task #253 / memo 13 record/replay primitives. */
struct kvm_record;
struct kvm_record *kvm_record_alloc(void);
int kvm_record_start(struct kvm_record *rec);
void kvm_record_stop(struct kvm_record *rec);
int kvm_record_replay(struct kvm_record *rec);
void kvm_record_destroy(struct kvm_record *rec);

/* Memo 13 step 2 / 3: observation + consumption helpers. */
void kvm_record_observe_syscall(unsigned long syscall_nr,
				long ret_value,
				u64 arg0_data, u64 arg1_data);
void kvm_record_observe_syscall_buf(unsigned long syscall_nr,
				    long ret_value,
				    u64 user_buf_va,
				    const void *payload,
				    size_t payload_len);
int kvm_record_consume_syscall(unsigned long syscall_nr,
			       long *ret_out,
			       u64 *user_buf_va_out,
			       const void **payload_out,
			       size_t *payload_len_out);
bool kvm_record_strict_replay(void);
int kvm_record_set_strict_replay(bool strict);

/*
 * Memo 13 P2 #13 metadata-buffer extension. Mirrors the public
 * kinds + prototypes from kvm_backend.h. Drift between the two is
 * regression-guarded by the kvm_record_meta_iov_roundtrip_test KUnit
 * case in test_ops.c.
 */
#define KVM_REPLAY_META_NONE		0
#define KVM_REPLAY_META_SOCKADDR	1
#define KVM_REPLAY_META_IOV		2
void kvm_record_observe_syscall_buf_meta(unsigned long syscall_nr,
					 long ret_value,
					 u64 user_buf_va,
					 const void *payload,
					 size_t payload_len,
					 const void *metadata,
					 size_t metadata_len,
					 u32 metadata_kind);
int kvm_record_consume_syscall_meta(unsigned long syscall_nr,
				    long *ret_out,
				    u64 *user_buf_va_out,
				    const void **payload_out,
				    size_t *payload_len_out,
				    const void **metadata_out,
				    size_t *metadata_len_out,
				    u32 *metadata_kind_out);

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
#endif /* __ARCH_UM_BACKEND_CONTRACT_TEST_KVM_HOOKS_H */
