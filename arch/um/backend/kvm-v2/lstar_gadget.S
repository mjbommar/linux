/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML KVM backend LSTAR gadget trampoline body.
 *
 * Two symbol pairs:
 *   kvm_v2_lstar_fallback_start..._end   - 5-byte fallback (out + sysretq).
 *                                          Used by trampoline_install before
 *                                          per-vCPU state pages exist, and as
 *                                          the permanent body when
 *                                          CONFIG_UM_BACKEND_KVM_V2_GADGET=n.
 *   kvm_v2_lstar_gadget_start..._end     - stay-in-guest gadget for
 *                                          getpid/gettid/getppid/getuid/
 *                                          geteuid/getgid/getegid/getcpu/
 *                                          time/clock_gettime(MONOTONIC).
 *                                          Installed by
 *                                          kvm_v2_trampoline_upgrade_to_gadget
 *                                          after exception_install confirms
 *                                          per-vCPU state pages are mapped.
 *
 * Both blobs are position-independent. They target the guest VA
 * KVM_V2_LSTAR_GVA = 0xffffe00000000040, NOT wherever the linker puts the
 * source. All control flow within is rel8/rel32 (PC-relative); all per-vCPU
 * state reads are %gs:disp32 against MSR_KERNEL_GS_BASE = the per-vCPU
 * state page GVA. No absolute addresses, no symbol relocations against
 * anything outside this section.
 *
 * The blobs live in .rodata so the host linker treats them as data. Host
 * never executes these bytes; only the guest does after memcpy. This also
 * keeps objtool from treating them as host kernel code, where
 * swapgs/sysretq/outb-to-magic-port would be invalid.
 */

#include <linux/linkage.h>

/*
 * State-page offsets duplicated from syscall_trap.h. The C and assembly
 * definitions must agree byte-for-byte. syscall_trap.h includes C-only
 * content, so keep the small duplicated offset list here.
 */
#define KVM_V2_GADGET_OFF_SEQ		0x00
#define KVM_V2_GADGET_OFF_CPU_ID	0x04
#define KVM_V2_GADGET_OFF_TGID		0x08
#define KVM_V2_GADGET_OFF_TID		0x0c
#define KVM_V2_GADGET_OFF_PPID		0x10
#define KVM_V2_GADGET_OFF_UID		0x14
#define KVM_V2_GADGET_OFF_EUID		0x18
#define KVM_V2_GADGET_OFF_GID		0x1c
#define KVM_V2_GADGET_OFF_EGID		0x20
#define KVM_V2_GADGET_OFF_TASK_SIZE_CAP	0x28
#define KVM_V2_GADGET_OFF_REAL_SEC	0x30
#define KVM_V2_GADGET_OFF_MONO_SEC	0x38
#define KVM_V2_GADGET_OFF_MONO_NSEC	0x40
#define KVM_V2_GADGET_OFF_BUDGET	0x48
#define KVM_V2_GADGET_OFF_SAVE_RDX	0x50
#define KVM_V2_GADGET_OFF_SAVE_R8	0x58
#define KVM_V2_GADGET_OFF_SAVE_R10	0x60
#define KVM_V2_GADGET_OFF_SAVE_RCX	0x68	/* APERF/MPERF: user RCX save */
#define KVM_V2_GADGET_OFF_APERF_CAP	0x70	/* APERF/MPERF: u8 cap-active */

/*
 * Syscall numbers: Linux x86_64 ABI constants. Hardcoded here
 * rather than #include'd from <asm/unistd.h> because that header
 * pulls in unrelated definitions for this data blob.
 */
#define NR_GETPID	39	/* 0x27 */
#define NR_GETTID	186	/* 0xba */
#define NR_GETPPID	110	/* 0x6e */
#define NR_GETUID	102	/* 0x66 */
#define NR_GETEUID	107	/* 0x6b */
#define NR_GETGID	104	/* 0x68 */
#define NR_GETEGID	108	/* 0x6c */
#define NR_TIME		201	/* 0xc9 */
#define NR_CLOCK_GETTIME 228	/* 0xe4 */
#define NR_GETCPU	309	/* 0x135; does NOT fit in 8 bits */
#define NR_UML_APERFMPERF 0xc0de /* UML-private; well above any Linux NR.
				 * Same out-of-imm8 escape-hatch pattern
				 * as getcpu (pre-check before guard). */

#define UM_KVM_TRAP_SYSCALL	0xf4	/* see syscall_trap.h enum um_kvm_iotrap */

#define CLOCK_MONOTONIC		1	/* asm-generic/posix_types.h equivalent */

	.section .rodata.kvm_v2_lstar_gadget, "a"
	.code64

/* ============================================================
 * Fallback body: 5 bytes, used when the gadget is disabled or
 * during the install window before per-vCPU state pages exist.
 *
 *   out %al, $0xf4   ; KVM_EXIT_IO trap (port 0xf4 = SYSCALL)
 *   sysretq          ; (executed after host advances RIP past the OUT)
 *
 * ============================================================ */
	.balign 1
SYM_DATA_START_LOCAL(kvm_v2_lstar_fallback_start)
	.globl kvm_v2_lstar_fallback_start
fallback_only:
	outb	%al, $UM_KVM_TRAP_SYSCALL
	sysretq
SYM_DATA_END_LABEL(kvm_v2_lstar_fallback_start, SYM_L_GLOBAL,
		   kvm_v2_lstar_fallback_end)


/* ============================================================
 * Gadget body: 10 NRs handled in-guest:
 *   getpid/gettid/getppid/getuid/geteuid/getgid/getegid/getcpu/
 *   time(2)/clock_gettime(CLOCK_MONOTONIC).
 *
 * Layout:
 *   - entry saves user scratch registers and toggles GS to the per-vCPU
 *     state page;
 *   - dispatch handles getcpu, APERF/MPERF, pid-family syscalls,
 *     clock_gettime(CLOCK_MONOTONIC), and time(2);
 *   - fallback restores registers, traps to the host path, and returns
 *     through sysretq;
 *   - handled syscalls restore registers through the shared tail.
 *
 * NB: GAS computes all rel8/rel32 displacements from the labels.
 * Do NOT use absolute symbol references; this code runs at a
 * different VA than where it's linked.
 *
 * sched_yield (NR=24) is deliberately not gadgeted because UML's
 * scheduler semantics require the host path.
 * ============================================================ */
	.balign 1
SYM_DATA_START_LOCAL(kvm_v2_lstar_gadget_start)
	.globl kvm_v2_lstar_gadget_start
gadget_entry:
	/* Toggle GS_BASE to the per-vCPU state page. */
	swapgs

	/*
	 * Save user RDX/R8/R10 into per-vCPU state-page slots. Linux
	 * x86_64 syscall ABI preserves all GPRs except RAX/RCX/R11; these three
	 * regs are scratched by h_time/h_getcpu/h_clock_gettime and
	 * must be restored before SYSRETQ and before the OUTB-to-
	 * fallback path since handle_syscall on the host marshals
	 * regs back into kvm_run->s.regs and the user resumes from
	 * those values).
	 *
	 * Restore is matched at every exit (tail, fallback).
	 * Pid-family handlers don't touch these regs, but the fixed
	 * 27-byte entry save keeps dispatch decisions uniform.
	 */
	movq	%rdx, %gs:KVM_V2_GADGET_OFF_SAVE_RDX
	movq	%r8,  %gs:KVM_V2_GADGET_OFF_SAVE_R8
	movq	%r10, %gs:KVM_V2_GADGET_OFF_SAVE_R10

	/*
	 * getcpu pre-check. NR=309=0x135 does not fit imm8, so compare
	 * against EAX before the upper-byte guard rejects high-bit NRs.
	 */
	cmpl	$NR_GETCPU, %eax
	jne	1f			/* skip the rel32 if not getcpu */
	jmp	h_getcpu		/* GAS emits jmp rel32 */
1:

	/*
	 * UML-private APERF/MPERF gadget pre-check (NR=0xc0de; same
	 * out-of-imm8 escape-hatch pattern as getcpu). Handler reads
	 * IA32_APERF (0xE7) and IA32_MPERF (0xE8) at guest CPL=0; the
	 * KVM_CAP_X86_DISABLE_EXITS / APERFMPERF cap enabled in
	 * vm_create lets the rdmsr instructions pass through to
	 * hardware. See syscall_trap.h::KVM_V2_NR_UML_APERFMPERF for
	 * the ABI doc and Documentation/virt/uml/examples/aperf-mperf/
	 * for the consumer.
	 */
	cmpl	$NR_UML_APERFMPERF, %eax
	jne	1f
	jmp	h_aperfmperf
1:

	/*
	 * Upper-byte guard: any bit set in [8..31] of EAX means it's
	 * not one of the imm8-dispatch NRs. Catches NRs >= 256 that
	 * share a low byte with a gadget-handled NR (e.g. preadv2
	 * NR=295=0x127 aliases low-byte 0x27 of getpid).
	 */
	testl	$0xffffff00, %eax
	jne	fallback

	/* Pid-family imm8 dispatch: GAS encodes je rel8. */
	cmpb	$NR_GETPID, %al;  je h_tgid
	cmpb	$NR_GETTID, %al;  je h_tid
	cmpb	$NR_GETPPID, %al; je h_ppid
	cmpb	$NR_GETUID, %al;  je h_uid
	cmpb	$NR_GETEUID, %al; je h_euid
	cmpb	$NR_GETGID, %al;  je h_gid
	cmpb	$NR_GETEGID, %al; je h_egid

	/*
	 * clock_gettime pre-check: GAS rel32 jmp keeps reach
	 * unbounded (handler lives at +240 from gadget start).
	 */
	cmpb	$NR_CLOCK_GETTIME, %al
	jne	2f
	jmp	h_clock_gettime
2:

	/*
	 * time(2) inline dispatch+body. NR=201 doesn't go to a
	 * separate handler slot because the 37B body fits inline
	 * without forcing the dispatch chain to grow.
	 *
	 * RAX preservation: load REAL_SEC into %rdx (NOT %rax) so RAX
	 * stays = NR=201 throughout. If the bounds check falls back,
	 * handle_syscall sees the right NR. Move %rdx to %rax just
	 * before sysretq so the gadget's return value = REAL_SEC.
	 */
	cmpb	$NR_TIME, %al
	jne	fallback
	movq	%gs:KVM_V2_GADGET_OFF_REAL_SEC, %rdx
	testq	%rdi, %rdi		/* tloc may be NULL */
	je	L_time_no_store
	/*
	 * Operand order is cmpq %reg, %mem so flags reflect cap - ptr.
	 * jbe is taken when cap <= ptr, which routes out-of-range user
	 * pointers to the host fallback.
	 */
	cmpq	%rdi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback
	movq	%rdx, (%rdi)		/* *tloc = REAL_SEC */
L_time_no_store:
	movq	%rdx, %rax		/* return value = REAL_SEC */
	jmp	tail

/* ----------- fallback path -----------
 * Restore user RDX/R8/R10 BEFORE swapgs (gs still points at the
 * state page so the gs:disp32 reads land on the saved slots).
 * The OUT instruction's host-side handler reads kvm_run->s.regs;
 * if RDX/R8/R10 are still scratched, handle_syscall sees the wrong
 * arg values and the user-visible registers come back wrong on
 * resume.
 */
fallback:
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_RDX, %rdx
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_R8,  %r8
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_R10, %r10
	swapgs
	outb	%al, $UM_KVM_TRAP_SYSCALL
	sysretq

/* ----------- pid-family handlers (~+108..+177) -----------
 * Each: load 32-bit field from per-vCPU state page into %eax
 * (zero-extends to RAX), jmp to shared tail. RAX = the field
 * value; tail does swapgs+sysretq.
 */
h_tgid:
	movl	%gs:KVM_V2_GADGET_OFF_TGID, %eax
	jmp	tail
h_tid:
	movl	%gs:KVM_V2_GADGET_OFF_TID, %eax
	jmp	tail
h_ppid:
	movl	%gs:KVM_V2_GADGET_OFF_PPID, %eax
	jmp	tail
h_uid:
	movl	%gs:KVM_V2_GADGET_OFF_UID, %eax
	jmp	tail
h_euid:
	movl	%gs:KVM_V2_GADGET_OFF_EUID, %eax
	jmp	tail
h_gid:
	movl	%gs:KVM_V2_GADGET_OFF_GID, %eax
	jmp	tail
h_egid:
	movl	%gs:KVM_V2_GADGET_OFF_EGID, %eax
	/* fall-through to tail */

/* ----------- shared tail -----------
 * Restore user RDX/R8/R10 BEFORE swapgs (matches save at entry).
 * Reached by: pid-family handlers (which don't touch these regs
 * but still pay the restore for uniformity), h_time, h_getcpu,
 * and h_clock_gettime.
 */
tail:
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_RDX, %rdx
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_R8,  %r8
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_R10, %r10
	swapgs
	sysretq

/* ----------- h_getcpu body (~+184) -----------
 *   int getcpu(unsigned int *cpu, unsigned int *node);
 *
 * On entry: RDI=cpu, RSI=node, RAX=NR=309, swapgs done.
 * Use %edx (NOT %ecx; RCX holds user RIP for sysretq) for the
 * CPU_ID load. Use %r10d for the node-zero-write. Defer
 * xor %eax, %eax until past both bounds checks so RAX = NR=309
 * survives any fallback path.
 */
h_getcpu:
	testq	%rdi, %rdi
	je	L_cpu_skip
	/* Operand order matters; see h_time bounds-check comment. */
	cmpq	%rdi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback		/* GAS emits jbe rel8/rel32 as needed */
	movl	%gs:KVM_V2_GADGET_OFF_CPU_ID, %edx
	movl	%edx, (%rdi)
L_cpu_skip:
	testq	%rsi, %rsi
	je	L_node_skip
	/* See h_time bounds-check comment. */
	cmpq	%rsi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback
	xorl	%r10d, %r10d
	movl	%r10d, (%rsi)
L_node_skip:
	xorl	%eax, %eax		/* return 0 (success) */
	jmp	tail

/* ----------- h_clock_gettime body (~+240) -----------
 *   int clock_gettime(clockid_t id, struct timespec *ts);
 *
 * On entry: RDI=clockid, RSI=ts, RAX=NR=228, swapgs done.
 *
 * Sequence:
 *   1. Decrement BUDGET. If negative, fallback so the host can
 *      refresh the vvar; tight loops can't freeze guest time).
 *   2. Verify clockid==CLOCK_MONOTONIC (=1).
 *   3. Snapshot SEQ into %edx (NOT %eax). If odd, writer in
 *      progress, fallback.
 *   4. Read MONO_SEC, MONO_NSEC.
 *   5. Re-read SEQ, compare. If changed, fallback.
 *   6. Bounds-check user timespec.
 *   7. Store both qwords. RAX=0. swapgs+sysretq.
 *
 * RAX preservation: NR=228 stays in RAX throughout. xor %eax,%eax
 * is the LAST step before swapgs+sysretq.
 */
h_clock_gettime:
	subl	$1, %gs:KVM_V2_GADGET_OFF_BUDGET
	js	fallback
	cmpl	$CLOCK_MONOTONIC, %edi
	jne	fallback
	movl	%gs:KVM_V2_GADGET_OFF_SEQ, %edx
	testb	$1, %dl
	jne	fallback
	movq	%gs:KVM_V2_GADGET_OFF_MONO_SEC, %r10
	movq	%gs:KVM_V2_GADGET_OFF_MONO_NSEC, %r8
	cmpl	%edx, %gs:KVM_V2_GADGET_OFF_SEQ
	jne	fallback
	/* See h_time bounds-check comment. */
	cmpq	%rsi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback
	movq	%r10, (%rsi)		/* ts->sec */
	movq	%r8,  8(%rsi)		/* ts->nsec */
	xorl	%eax, %eax		/* return 0 */
	jmp	tail

/* ----------- h_aperfmperf body -----------
 * UML-private NR=0xc0de.
 *
 *   long uml_aperfmperf(struct um_aperfmperf *out);
 *   struct um_aperfmperf { u64 aperf; u64 mperf; };
 *
 * On entry: RDI=out, RAX=NR=0xc0de, swapgs done, SAVE_RDX/R8/R10
 * already populated by the entry preamble.
 *
 * NULL RDI: skip the rdmsr block and return 0. This lets userspace detect
 * that the gadget handled the UML-private syscall; the host fallback
 * returns -ENOSYS.
 *
 * Non-NULL: bounds-check RDI against TASK_SIZE_CAP (same pattern
 * as h_getcpu / h_clock_gettime). On success, save user RCX
 * (= user RIP for SYSRETQ; rdmsr reads ECX as MSR index input
 * and clobbers it), rdmsr 0xE7 to APERF, rdmsr 0xE8 to MPERF, store
 * both as u64 at *RDI, restore RCX, fall through to tail.
 *
 * RDX gets clobbered by rdmsr (writes to EDX) but tail's restore
 * from SAVE_RDX puts the user value back.  R8/R10 unchanged.
 *
 * The cap-enable in vm_create (Kconfig + cmdline gated) decides
 * whether the rdmsrs pass through to hardware or get KVM-emulated
 * to zero. Both behaviors are valid gadget output; callers that need to
 * distinguish them should consult the debugfs status file.
 */
h_aperfmperf:
	/*
	 * Cap-active gate. If the per-vCPU APERF_CAP byte is zero
	 * (vm_create did not enable KVM_X86_DISABLE_EXITS_APERFMPERF
	 * or KVM rejected the cap), KVM would inject #GP on the
		 * rdmsr below; avoid it before the rdmsr happens by falling
		 * back to the host trap path. The host's handle_syscall
		 * returns -ENOSYS for NR=0xc0de.
		 */
	cmpb	$0, %gs:KVM_V2_GADGET_OFF_APERF_CAP
	je	fallback

	testq	%rdi, %rdi
	je	L_aperf_skip
	/* Operand order matters; see h_time bounds-check comment. */
	cmpq	%rdi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback

	/* Save user RCX before rdmsr clobbers ECX. */
	movq	%rcx, %gs:KVM_V2_GADGET_OFF_SAVE_RCX

	/* rdmsr IA32_APERF (0xE7) returns EDX:EAX (high:low 32 bits). */
	movl	$0xe7, %ecx
	rdmsr
	movl	%eax,  0(%rdi)		/* aperf lo32 */
	movl	%edx,  4(%rdi)		/* aperf hi32 */

	/* rdmsr IA32_MPERF (0xE8) returns EDX:EAX. */
	movl	$0xe8, %ecx
	rdmsr
	movl	%eax,  8(%rdi)		/* mperf lo32 */
	movl	%edx, 12(%rdi)		/* mperf hi32 */

	/* Restore user RCX so SYSRETQ returns to the user RIP. */
	movq	%gs:KVM_V2_GADGET_OFF_SAVE_RCX, %rcx

L_aperf_skip:
	xorl	%eax, %eax		/* return 0 (success) */
	jmp	tail

SYM_DATA_END_LABEL(kvm_v2_lstar_gadget_start, SYM_L_GLOBAL,
		   kvm_v2_lstar_gadget_end)
