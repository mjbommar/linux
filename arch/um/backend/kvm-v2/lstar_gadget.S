/* SPDX-License-Identifier: GPL-2.0 */
/*
 * UML backend v2 (KVM) — Phase H gadget LSTAR trampoline body.
 *
 * Two symbol pairs:
 *   kvm_v2_lstar_fallback_start..._end   — 5-byte fallback (out + sysretq).
 *                                          Used by trampoline_install before
 *                                          per-vCPU state pages exist, and as
 *                                          the permanent body when
 *                                          CONFIG_UM_BACKEND_KVM_V2_GADGET=n.
 *   kvm_v2_lstar_gadget_start..._end     — 343-byte stay-in-guest gadget for
 *                                          getpid/gettid/getppid/getuid/
 *                                          geteuid/getgid/getegid/getcpu/
 *                                          time/clock_gettime(MONOTONIC).
 *                                          Installed by
 *                                          kvm_v2_trampoline_upgrade_to_gadget
 *                                          after exception_install confirms
 *                                          per-vCPU state pages are mapped.
 *
 * Both blobs are POSITION-INDEPENDENT — they target the guest VA
 * KVM_V2_LSTAR_GVA = 0xffffe00000000040, NOT wherever the linker puts the
 * source. All control flow within is rel8/rel32 (PC-relative); all per-vCPU
 * state reads are %gs:disp32 against MSR_KERNEL_GS_BASE = the per-vCPU
 * state page GVA. No absolute addresses, no symbol relocations against
 * anything outside this section.
 *
 * The blobs live in .rodata so the host linker treats them as DATA — host
 * never executes these bytes; only the guest does after memcpy. This also
 * keeps objtool from auditing them as host kernel code (which would —
 * correctly — reject swapgs/sysretq/outb-to-magic-port).
 *
 * Pattern follows arch/x86/kernel/ftrace_64.S's ftrace_caller / _end +
 * arch/x86/kernel/ftrace.c's "memcpy(trampoline, ftrace_caller, end - start)"
 * — the canonical kernel approach for "small position-independent x86_64
 * stub copied at runtime into freshly allocated memory". v1 archive used
 * hand-coded byte tables (kvm-v1-archive/thread.c:873-1213), which made
 * upstream review difficult; v2 ships assembly that GAS encodes.
 *
 * v1 reference:
 *   - kvm-v1-archive/thread.c:873-1213 — original 7-NR + clock + getcpu
 *     hand-coded byte table (port reference)
 *   - kvm-v1-archive/thread.c:907-916 — getcpu cmp+jne+jmp_rel32 pre-check
 *     pattern (NR>0xff)
 *   - kvm-v1-archive/thread.c:1063-1103 — clock_gettime body with seqlock
 *     + budget + bounds check
 *   - kvm-v1-archive/thread.c:1110-1115 — time(2) "no seqlock, 1-second
 *     resolution makes torn read off-by-one at worst" rationale
 *   - kvm-v1-archive/thread.c:942-961 — sched_yield post-mortem (DEMOTED
 *     back to host because gadget skipped UML's scheduler; v2 does NOT
 *     gadget sched_yield).
 */

#include <linux/linkage.h>

/*
 * State-page offsets duplicated from syscall_trap.h. They MUST stay in
 * sync; a build-time static_assert on the assembled blob length
 * (kvm_v2_lstar_gadget_end - kvm_v2_lstar_gadget_start == 343) at the
 * C side gives some protection, but the ultimate truth is that this
 * file and syscall_trap.h must agree byte-for-byte. syscall_trap.h
 * includes C-only content (struct forward decls, function prototypes,
 * enum) so we can't just #include it from a .S file; a future cleanup
 * could split the offsets into an asm-safe sub-header.
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
#define KVM_V2_GADGET_OFF_RECORD	0x24	/* #169 Phase 4: u8 record-mode bypass flag */
#define KVM_V2_GADGET_OFF_TASK_SIZE_CAP	0x28
#define KVM_V2_GADGET_OFF_REAL_SEC	0x30
#define KVM_V2_GADGET_OFF_MONO_SEC	0x38
#define KVM_V2_GADGET_OFF_MONO_NSEC	0x40
#define KVM_V2_GADGET_OFF_BUDGET	0x48
#define KVM_V2_GADGET_OFF_SAVE_RDX	0x50
#define KVM_V2_GADGET_OFF_SAVE_R8	0x58
#define KVM_V2_GADGET_OFF_SAVE_R10	0x60

/*
 * Syscall numbers — Linux x86_64 ABI constants. Hardcoded here
 * rather than #include'd from <asm/unistd.h> because that header
 * drags in a lot more than we need.
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
#define NR_GETCPU	309	/* 0x135 — does NOT fit in 8 bits */

#define UM_KVM_TRAP_SYSCALL	0xf4	/* see syscall_trap.h enum um_kvm_iotrap */

#define CLOCK_MONOTONIC		1	/* asm-generic/posix_types.h equivalent */

	.section .rodata.kvm_v2_lstar_gadget, "a"
	.code64

/* ============================================================
 * Fallback body — 5 bytes, used when the gadget is disabled or
 * during the install window before per-vCPU state pages exist.
 *
 *   out %al, $0xf4   ; KVM_EXIT_IO trap (port 0xf4 = SYSCALL)
 *   sysretq          ; (executed after host advances RIP past the OUT)
 *
 * Byte-identical to the v1 archive's #else 5-byte LSTAR
 * (kvm-v1-archive/thread.c:1215-1218).
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
 * Gadget body — 343 bytes (Phase 4-7), 10 NRs handled in-guest:
 *   getpid/gettid/getppid/getuid/geteuid/getgid/getegid/getcpu/
 *   time(2)/clock_gettime(CLOCK_MONOTONIC).
 *
 * Layout:
 *   +0   swapgs                     ; toggle to per-vCPU state page
 *   +3   getcpu pre-check (NR=309)  ; cmp+jne+jmp_rel32 (NR > 0xff)
 *   +15  upper-byte guard           ; test + jne fallback
 *   +22  7 imm8 dispatch entries    ; cmp $imm8,%al + je rel8
 *   +50  clock_gettime pre-check    ; cmp+jne+jmp_rel32 (in dispatch)
 *   +59  time(2) inline             ; cmp+jne + 37B inline body
 *   +100 fallback                   ; swapgs+out+sysretq
 *   +108 7 pid-family handlers × 10B
 *   +178 shared tail                ; swapgs+sysretq
 *   +184 h_getcpu body              ; 56B
 *   +240 h_clock_gettime body       ; 103B
 *   +343 end
 *
 * NB: GAS computes all rel8/rel32 displacements from the labels.
 * Do NOT use absolute symbol references; this code runs at a
 * different VA than where it's linked.
 *
 * sched_yield (NR=24) is DELIBERATELY NOT gadgeted — see v1 post-
 * mortem at kvm-v1-archive/thread.c:942-961.
 * ============================================================ */
	.balign 1
SYM_DATA_START_LOCAL(kvm_v2_lstar_gadget_start)
	.globl kvm_v2_lstar_gadget_start
gadget_entry:
	/* Toggle GS_BASE to the per-vCPU state page. */
	swapgs

	/*
	 * Save user RDX/R8/R10 into per-vCPU state-page slots
	 * (mainstream-readiness items #1 + #4: Linux x86_64 syscall
	 * ABI preserves all GPRs except RAX/RCX/R11; these three
	 * regs are scratched by h_time/h_getcpu/h_clock_gettime and
	 * must be restored before SYSRETQ — and before the OUTB-to-
	 * fallback path since handle_syscall on the host marshals
	 * regs back into kvm_run->s.regs and the user resumes from
	 * those values).
	 *
	 * Restore is matched at every exit (`tail`, `fallback`).
	 * Pid-family handlers don't actually touch these regs, but
	 * we still pay 27 entry bytes to keep dispatch decisions
	 * uniform (no per-NR branch deciding whether to save).
	 */
	movq	%rdx, %gs:KVM_V2_GADGET_OFF_SAVE_RDX
	movq	%r8,  %gs:KVM_V2_GADGET_OFF_SAVE_R8
	movq	%r10, %gs:KVM_V2_GADGET_OFF_SAVE_R10

	/*
	 * Record-mode bypass (#169 Phase 4, memo 27 §3.3 Option A).
	 * If the per-vCPU RECORD flag is non-zero, force the in-guest
	 * fallback path so observe_syscall sees every NR. Placed AFTER
	 * the SAVE_* writes so the fallback's restore-of-SAVE_* lands
	 * the original user RDX/R8/R10 (the saves are unconditional —
	 * cost when record-off = three movs we'd issue anyway).
	 *
	 * Cost when record OFF: one untaken cmpb + jne per gadget entry.
	 * Cost when record ON: ~30× slowdown on the 11 gadget-handled
	 * NRs (gadget ~50 cyc → handle_syscall ~1500 cyc) which is the
	 * intended record-side overhead.
	 */
	cmpb	$0, %gs:KVM_V2_GADGET_OFF_RECORD
	jne	fallback

	/*
	 * getcpu pre-check (NR=309=0x135 doesn't fit imm8, so we test
	 * against EAX with cmp+jne+jmp rel32 BEFORE the upper-byte
	 * guard rejects high-bit NRs).
	 */
	cmpl	$NR_GETCPU, %eax
	jne	1f			/* skip the rel32 if not getcpu */
	jmp	h_getcpu		/* GAS emits jmp rel32 */
1:

	/*
	 * Upper-byte guard: any bit set in [8..31] of EAX means it's
	 * NOT one of our imm8-dispatch NRs. Catches NRs >= 256 that
	 * share a low byte with a gadget-handled NR (e.g. preadv2
	 * NR=295=0x127 aliases low-byte 0x27 of getpid).
	 */
	testl	$0xffffff00, %eax
	jne	fallback

	/* Pid-family imm8 dispatch — GAS encodes je rel8. */
	cmpb	$NR_GETPID, %al;  je h_tgid
	cmpb	$NR_GETTID, %al;  je h_tid
	cmpb	$NR_GETPPID, %al; je h_ppid
	cmpb	$NR_GETUID, %al;  je h_uid
	cmpb	$NR_GETEUID, %al; je h_euid
	cmpb	$NR_GETGID, %al;  je h_gid
	cmpb	$NR_GETEGID, %al; je h_egid

	/*
	 * clock_gettime pre-check — GAS rel32 jmp keeps reach
	 * unbounded (handler lives at +240 from gadget start).
	 */
	cmpb	$NR_CLOCK_GETTIME, %al
	jne	2f
	jmp	h_clock_gettime
2:

	/*
	 * time(2) inline dispatch+body. NR=201 doesn't go to a
	 * separate handler slot because the 37B body fits inline
	 * without forcing the dispatch chain to grow. v1 reference:
	 * kvm-v1-archive/thread.c:1106-1147.
	 *
	 * RAX preservation: load REAL_SEC into %rdx (NOT %rax) so RAX
	 * stays = NR=201 throughout — if the bounds check falls back,
	 * handle_syscall sees the right NR. Move %rdx → %rax just
	 * before sysretq so the gadget's return value = REAL_SEC.
	 */
	cmpb	$NR_TIME, %al
	jne	fallback
	movq	%gs:KVM_V2_GADGET_OFF_REAL_SEC, %rdx
	testq	%rdi, %rdi		/* tloc may be NULL */
	je	L_time_no_store
	/*
	 * T68 (2026-05-18): operand order is `cmpq %reg, %mem` so GAS
	 * emits opcode 0x39 (CMP r/m64, r64) → flags reflect (cap - rdi);
	 * jbe taken when cap <= rdi (i.e. rdi >= cap) → fallback. The
	 * inverted form `cmpq %mem, %reg` (opcode 0x3B, flags = rdi - cap)
	 * shipped pre-T68 and made `jbe` fire on valid user pointers,
	 * turning h_time's user-store path into dead code. v1 byte-table
	 * reference: kvm-v1-archive/thread.c:1063+ used the same encoding.
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
 * Direct port of v1 thread.c:1177-1208.
 *
 *   int getcpu(unsigned int *cpu, unsigned int *node);
 *
 * On entry: RDI=cpu, RSI=node, RAX=NR=309, swapgs done.
 * Use %edx (NOT %ecx — RCX holds user RIP for sysretq) for the
 * CPU_ID load. Use %r10d for the node-zero-write. Defer
 * `xor %eax, %eax` until past both bounds checks so RAX = NR=309
 * survives any fallback path.
 */
h_getcpu:
	testq	%rdi, %rdi
	je	L_cpu_skip
	/* T68: see h_time bounds-check comment — operand order matters. */
	cmpq	%rdi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback		/* GAS emits jbe rel8/rel32 as needed */
	movl	%gs:KVM_V2_GADGET_OFF_CPU_ID, %edx
	movl	%edx, (%rdi)
L_cpu_skip:
	testq	%rsi, %rsi
	je	L_node_skip
	/* T68: see h_time bounds-check comment. */
	cmpq	%rsi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback
	xorl	%r10d, %r10d
	movl	%r10d, (%rsi)
L_node_skip:
	xorl	%eax, %eax		/* return 0 (success) */
	jmp	tail

/* ----------- h_clock_gettime body (~+240) -----------
 * Direct port of v1 thread.c:1063-1103.
 *
 *   int clock_gettime(clockid_t id, struct timespec *ts);
 *
 * On entry: RDI=clockid, RSI=ts, RAX=NR=228, swapgs done.
 *
 * Sequence:
 *   1. Decrement BUDGET. If negative → fallback (lets host
 *      refresh the vvar; tight loops can't freeze guest time).
 *   2. Verify clockid==CLOCK_MONOTONIC (=1).
 *   3. Snapshot SEQ into %edx (NOT %eax). If odd → writer in
 *      progress, fallback.
 *   4. Read MONO_SEC, MONO_NSEC.
 *   5. Re-read SEQ, compare. If changed → racy, fallback.
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
	/* T68: see h_time bounds-check comment. */
	cmpq	%rsi, %gs:KVM_V2_GADGET_OFF_TASK_SIZE_CAP
	jbe	fallback
	movq	%r10, (%rsi)		/* ts->sec */
	movq	%r8,  8(%rsi)		/* ts->nsec */
	xorl	%eax, %eax		/* return 0 */
	jmp	tail

SYM_DATA_END_LABEL(kvm_v2_lstar_gadget_start, SYM_L_GLOBAL,
		   kvm_v2_lstar_gadget_end)
