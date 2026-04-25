// SPDX-License-Identifier: GPL-2.0
/*
 * KVM backend — thread + run_userspace ops.
 *
 * Workstream D-04a. Thread lifecycle (thread_create,
 * thread_start_idle, context_switch) on the UML kernel side is
 * the same jmp_buf-based mechanism ptrace and seccomp already
 * use; these wrappers exist so the dispatch macro resolves
 * kvm_<op>() in single-backend KVM_ONLY builds.
 *
 * run_userspace is the first op that actually exercises KVM:
 * ioctl(KVM_RUN) on vcpu0 and dispatch on exit_reason. At D-04a
 * the vCPU has no SREGS / CR3 / LSTAR set up yet (that's D-04b
 * and D-04c), so KVM_RUN is expected to fail-enter or shutdown
 * immediately. Log the exit_reason and panic with a readable
 * message — the panic itself still pre-console-dies today but
 * the log buffer captures enough detail for core-dump analysis.
 *
 * init_thread_regs uses the existing get_safe_registers() helper
 * (same as seccomp_init_thread_regs).
 */
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/jump_label.h>
#include <linux/kvm.h>
#include <linux/mm.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task_stack.h>
#include <linux/spinlock.h>
#include <linux/time-internal.h>	/* time_travel_mode + tt_extra_sched_jiffies */
#include <linux/uaccess.h>	/* copy_from_user */

#include <linux/signal.h>	/* SIGSEGV for sig_info dispatch */

#include <asm/page.h>
#include <asm/processor.h>
#include <asm/prctl.h>		/* ARCH_SET_FS / ARCH_SET_GS */
#include <asm/unistd.h>		/* __NR_arch_prctl */
#include <as-layout.h>
#include <kern_util.h>
#include <mem.h>		/* uml_physmem */
#include <os.h>
#include <registers.h>
#include <sysdep/faultinfo.h>
#include <sysdep/ptrace.h>
#include <sysdep/ptrace_user.h>
#include <asm/backend.h>

#include "kvm_backend.h"

int kvm_thread_create(struct task_struct *p, void *stack,
		      void (*handler)(void))
{
	new_thread(stack, &p->thread.switch_buf, handler);
	return 0;
}

int kvm_thread_start_idle(void *stack, struct thread_struct *t)
{
	return start_idle_thread(stack, &t->switch_buf);
}

void kvm_context_switch(struct task_struct *prev, struct task_struct *next)
{
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
	/*
	 * Audit round-6 G2: when the active mm changes, clear the
	 * user half of the singleton shadow PGD so prev's mappings
	 * don't leak into next's view. kvm_shadow_fill_from_uml_pgd
	 * only INSTALLS present leaves — it never clears absent
	 * ones — so a switch from a process with a populated VA to
	 * one with the same VA unmapped would otherwise let the
	 * second process read prev's data.
	 *
	 * active_mm (not mm) is the right hook: kernel threads
	 * borrow the previous user task's mm via active_mm, and the
	 * shadow PT was filled against that. We only need to clear
	 * when active_mm actually changes.
	 *
	 * The clear marks kvm_ctx.shadow_dirty; next kvm_enter_guest
	 * KVM_SET_SREGS does the CR3 reload that flushes the guest
	 * TLB (per D89: KVM's kvm_set_cr3 always falls through to
	 * kvm_invalidate_pcid).
	 */
	if (prev && next && prev->active_mm != next->active_mm)
		kvm_shadow_pgd_clear_user();
#endif
	switch_threads(&prev->thread.switch_buf, &next->thread.switch_buf);
}

void kvm_init_thread_regs(unsigned long *gp, unsigned long *fp)
{
	get_safe_registers(gp, fp);

#ifdef CONFIG_UM_BACKEND_KVM_ONLY
	/*
	 * Under KVM_ONLY, os_early_checks short-circuits before
	 * init_pid_registers (registers.c:20) runs — there's no
	 * ptraced stub child to PTRACE_GETREGS against. That
	 * leaves the exec_regs baseline zero-filled, so
	 * get_safe_registers above returns all zeros. Two
	 * consequences we have to paper over here:
	 *
	 *   - The A-05 contract KUnit test asserts at least one
	 *     gp[] slot is non-zero (the "init writes *something*"
	 *     invariant).
	 *   - UML's scheduler uses the gp buffer as a thread's
	 *     initial register state; zero-filled gp can pass NULL
	 *     checks but leaves RIP == 0, which isn't useful.
	 *
	 * Seed RIP with a sentinel non-zero value. A real vCPU RIP
	 * is set per-KVM_RUN via KVM_SET_REGS in run_userspace;
	 * this sentinel is only ever observed by the scheduler's
	 * bookkeeping + the contract test, not by the CPU. Use
	 * STUB_START as the sentinel because it's a known-valid
	 * guest VA under UML's existing stub conventions; future
	 * real-run_userspace path will overwrite this before any
	 * KVM_RUN.
	 */
	if (!gp[HOST_IP])
		gp[HOST_IP] = STUB_START;
#endif
}

/*
 * D-05b: backend-neutral host-side IPI — the UML kernel's
 * inter-CPU signal machinery, not a KVM vCPU IPI. Mirrors
 * seccomp_ipi_send + ptrace_ipi_send almost exactly. Under
 * ncpus=1 (default) nobody calls this; the contract still
 * requires a non-NULL slot in the ops table, so we provide
 * the thin wrapper rather than a -EOPNOTSUPP stub.
 */
int kvm_ipi_send(int cpu, int vector)
{
#if IS_ENABLED(CONFIG_SMP)
	return os_send_ipi(cpu, vector);
#else
	(void)cpu;
	(void)vector;
	return 0;
#endif
}

/*
 * Map an exit_reason back to its symbol for pr_info. The list
 * matches arch/x86/kvm/kvm_host.h / Documentation/virt/kvm/api.rst;
 * we only enumerate reasons we expect to see during D-04a..D-04c
 * bring-up. Unknown reasons hit a numeric default.
 */
static const char *kvm_exit_reason_str(u32 r)
{
	switch (r) {
	case KVM_EXIT_UNKNOWN:		return "UNKNOWN";
	case KVM_EXIT_EXCEPTION:	return "EXCEPTION";
	case KVM_EXIT_IO:		return "IO";
	case KVM_EXIT_HYPERCALL:	return "HYPERCALL";
	case KVM_EXIT_DEBUG:		return "DEBUG";
	case KVM_EXIT_HLT:		return "HLT";
	case KVM_EXIT_MMIO:		return "MMIO";
	case KVM_EXIT_SHUTDOWN:		return "SHUTDOWN";
	case KVM_EXIT_FAIL_ENTRY:	return "FAIL_ENTRY";
	case KVM_EXIT_INTR:		return "INTR";
	case KVM_EXIT_INTERNAL_ERROR:	return "INTERNAL_ERROR";
	default:			return "???";
	}
}

#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED
/*
 * -----------------------------------------------------------------
 * D-04/D-05 follow-on: real `run_userspace` integration, sub-commit
 * #1 (memo 08). Production analogues of the harness entry path:
 * state materialization only; KVM_RUN + exit-reason decode land in
 * sub-commits #2-#5. The panic() in kvm_run_userspace below stays
 * in place — this code is reachable only via the explicit gate
 * (CONFIG_UM_BACKEND_KVM_INTEGRATED) + a direct call from a
 * follow-on sub-commit's wiring. D-04a scaffold unchanged.
 * -----------------------------------------------------------------
 */

/*
 * Bootstrap region: a single page inside UML's physmem hosts the
 * production GDT + LSTAR trampoline. Allocated once on first
 * entry and cached; freed only on backend shutdown (deferred to
 * sub-commit #7's D-05 fallback wiring). Guarded by a spinlock
 * so concurrent kvm_enter_guest callers see a consistent GPA.
 *
 * Layout inside the page (see `kvm_enter_guest_init_bootstrap`):
 *   0x000 .. 0x030 — 6-entry GDT (48 bytes, populated by
 *                    kvm_setup_harness_gdt — same ring-0 +
 *                    ring-3 layout the harness uses)
 *   0x040 .. 0x100 — reserved for LSTAR trampoline (sub-commit
 *                    #2 will write the bounce gadget here)
 *
 * Page-aligned; sized at PAGE_SIZE because a 4 KiB alloc is the
 * cheapest long-term-stable thing to ask GFP for and we don't
 * need more. The GPA under the Policy A memslot is
 * `__pa(bootstrap_va)`.
 */
static DEFINE_SPINLOCK(kvm_bootstrap_lock);
static void *kvm_bootstrap_page;	/* kernel VA of the code+tables page */
static void *kvm_bootstrap_page_stack;	/* kernel VA of the IST stack page (F5-followon split) */
static u64   kvm_bootstrap_gpa;		/* __pa() of the code+tables page; 0 if unallocated */
static u64   kvm_bootstrap_stack_gpa;	/* __pa() of the IST stack page */
static u64   kvm_bootstrap_va;		/* kernel VA as a u64 (linear address
					 * the guest CR3 walk resolves to the
					 * bootstrap page's gpa; used for
					 * GDTR / LSTAR / RIP where the CPU
					 * expects a linear, not physical,
					 * address).
					 */

#define KVM_BOOTSTRAP_GDT_OFFSET	0x000	/* 8 entries × 8 B = 64 B */
#define KVM_BOOTSTRAP_LSTAR_OFFSET	0x040	/* 5..~448-byte gadget region */
#define KVM_BOOTSTRAP_TSS_OFFSET	0x200	/* 104-byte TSS (moved from 0x100 in G5b) */
#define KVM_BOOTSTRAP_IDT_OFFSET	0x280	/* 33 × 16 = 528 B */
#define KVM_BOOTSTRAP_PF_HANDLER_OFFSET	0x4a0	/* 11-byte #PF handler (moved from 0x400 in G5b) */
#define KVM_BOOTSTRAP_SYSRET_OFFSET	0x4b0	/* 3-byte SYSRETQ. Pre-task-#272 was
						 * the bootstrap re-entry gadget for
						 * ring-0 → ring-3 transitions; #272
						 * replaced it with IRETQ at offset
						 * 0x4d0 to preserve user RCX/R11
						 * across recoverable #PF. The bytes
						 * stay installed for ABI stability
						 * (downstream tooling that pokes at
						 * the bootstrap page expects them at
						 * this offset) and for the diagnostic
						 * dump (the SHUTDOWN-path logger
						 * prints the bootstrap layout
						 * including this offset). No vCPU
						 * code path executes them today.
						 */
#define KVM_BOOTSTRAP_DF_HANDLER_OFFSET	0x4c0	/* #DF handler (task #269): out %al,$0xfa to
						 * surface a double-fault to the host with
						 * a diagnostic dump. Real kernels always
						 * populate IDT[8]; without this entry a
						 * double-fault during #PF delivery
						 * triple-faults silently and we lose the
						 * cause-of-cascade info.
						 */
#define KVM_BOOTSTRAP_GP_HANDLER_OFFSET	0x4d8	/* 3-byte #GP handler (audit round-7
						 * P1): out %al,$0xf9 ; hlt. Catches
						 * a non-canonical user RIP / RSP
						 * popped by the bootstrap IRETQ +
						 * any #GP raised during user-level
						 * execution (e.g. user task tried
						 * a privileged insn). Routes the
						 * fault to a dedicated host port
						 * 0xf9 so the kvm_run_userspace
						 * dispatcher can deliver SIGSEGV
						 * to the user task instead of
						 * cascading to #DF and panicking.
						 */
#define KVM_BOOTSTRAP_IRETQ_OFFSET	0x4d0	/* 2-byte IRETQ (task #272 recovery
						 * re-entry). SYSRETQ-based bootstrap
						 * clobbers RCX (= user RIP load) and R11
						 * (= user RFLAGS load), which is fine for
						 * fresh-entry / SYSCALL-return paths
						 * (RCX/R11 are already SYSCALL-ABI
						 * caller-saved by then) but DESTROYS user
						 * RCX/R11 across a recoverable #PF.
						 *
						 * IRETQ pops RIP/CS/RFLAGS/RSP/SS from
						 * the current stack and preserves all
						 * GPRs, so it's the correct re-entry for
						 * exception recovery where user RCX/R11
						 * must survive intact. Frame is built on
						 * the IST stack page (RW|NX, mapped at
						 * bootstrap_va + 0x3000) before entry.
						 *
						 * Surfaced by task #272 — ld-linux's RELR
						 * loop uses RCX as the relocation cursor;
						 * SYSRETQ-clobbered RCX terminated the
						 * loop after one iteration, leaving 11/12
						 * relative relocations un-applied.
						 */
/*
 * IST stack top — top of the dedicated stack page, post-F5-followon split.
 * Guest VA layout: bootstrap_va + 0x0000 = code+tables (RO), +0x1000 = state,
 * +0x2000 = vvar, +0x3000 = IST stack (RW NX). Stack TOP is exclusive — first
 * push lands at 0x3ff8.
 */
#define KVM_BOOTSTRAP_STACK_TOP		0x4000
#define KVM_BOOTSTRAP_TSS_SEL		0x30	/* GDT entry 6 (16-byte TSS desc) */
#define KVM_BOOTSTRAP_IDT_ENTRIES	33	/* covers #PF (vector 14) */

/*
 * Bits that MUST be set in the R11 value passed to the bootstrap
 * SYSRETQ gadget so the guest resumes ring-3 correctly. SYSRETQ
 * loads user RFLAGS from R11 after stripping a hardware-fixed
 * mask (AMD64 SDM §6.1.1: RFLAGS ← (R11 & 0x3C7FD7) | 2), so the
 * caller passes the full saved RFLAGS and lets hardware filter.
 *
 *   bit  1 (reserved)  — required 1 per the architectural spec
 *   bit  9 (IF)         — keep interrupts enabled in ring-3; the
 *                          guest cannot disable the host's
 *                          preemption path
 *   bits 12..13 (IOPL)  — hold at 3 so the in-guest ring-3
 *                          protocol ports (0xf4 syscall trap,
 *                          0xf5 halt, 0xfb PF-recovery, 0x80
 *                          debug) remain usable without switching
 *                          CPL
 *
 * All other user-visible bits (CF/PF/AF/ZF/SF/TF/DF/OF, NT, RF,
 * AC, ID) inherit from the saved user RFLAGS so a faulting
 * instruction's retry or a SYSCALL-return continuation sees the
 * correct architectural state. Before audit round-4 F2 this
 * value was hardcoded to 0x3202 which silently dropped the
 * saved user RFLAGS across every recoverable #PF and every
 * SYSCALL round-trip.
 */
#define KVM_RFLAGS_REQ_ON	((1UL << 1) | (1UL << 9) | (3UL << 12))

/*
 * Pure-data helper: compute the R11 value to hand to the
 * bootstrap SYSRETQ gadget given a saved user RFLAGS. Exposed
 * so the contract KUnit suite can cover the round-trip without
 * touching /dev/kvm (audit round-4 F3).
 */
static inline u64 kvm_build_sysret_r11(u64 saved_user_rflags)
{
	return saved_user_rflags | KVM_RFLAGS_REQ_ON;
}

/*
 * LSTAR trampoline: 5 bytes. Byte-identical to
 * `kvm_harness_lstar` in harness.c (D-04c). On SYSCALL entry
 * the CPU jumps here with RCX = post-SYSCALL RIP and R11 =
 * saved RFLAGS. The `out %al, $0xf4` triggers KVM_EXIT_IO on
 * UM_KVM_SYSCALL_PORT (0xf4); the host advances vCPU RIP past
 * the 2-byte `out` to the SYSRETQ, then resumes the vCPU so
 * SYSRETQ runs and delivers control back to the SYSCALL
 * follow-on at RCX. The wire format is deliberately shared
 * with the harness so sub-commit #3's decode can lift the
 * existing harness logic unchanged.
 */
#ifdef CONFIG_UM_BACKEND_KVM_GADGET
/*
 * Memo 11 G4 + G5c: systrap gadget body. Entered via
 * MSR_LSTAR on every SYSCALL from ring-3. Handles the 7
 * pid-family syscalls (G4) plus clock_gettime(
 * CLOCK_MONOTONIC) (G5c) in-guest without a VMEXIT by
 * reading per-task state via swapgs + %gs:<KVM_GADGET_
 * OFF_*> (state page at %gs base) and the vvar page at
 * %gs:<PAGE_SIZE + KVM_VVAR_OFF_*> via disp32. Every
 * other syscall takes the fallback `out %al, $0xf4`
 * VMEXIT path identical to the non-gadget build.
 *
 * Layout (hex offsets from LSTAR_OFFSET = +0x40 in the
 * bootstrap page):
 *
 *   +0 entry:
 *     0f 01 f8          swapgs                         (3 B)
 *
 *   +3 dispatch — 7 cmp $NR, %al; je handler_<NR>:
 *     3c 27 74 <r>      cmp $0x27 (getpid)  je pid_h    (4 B)
 *     3c ba 74 <r>      cmp $0xba (gettid)  je tid_h    (4 B)
 *     3c 6e 74 <r>      cmp $0x6e (getppid) je ppid_h   (4 B)
 *     3c 66 74 <r>      cmp $0x66 (getuid)  je uid_h    (4 B)
 *     3c 6b 74 <r>      cmp $0x6b (geteuid) je euid_h   (4 B)
 *     3c 68 74 <r>      cmp $0x68 (getgid)  je gid_h    (4 B)
 *     3c 6c 74 <r>      cmp $0x6c (getegid) je egid_h   (4 B)
 *     # offset 3 + 28 = 31
 *
 *   +31 fallback (unknown NR — VMEXIT):
 *     0f 01 f8          swapgs  (restore user GS)      (3 B)
 *     e6 f4             out %al, $0xf4                  (2 B)
 *     48 0f 07          sysretq                         (3 B)
 *     # offset 31 + 8 = 39
 *
 *   +39..+102 handlers — 7 × (mov %gs:<OFF>, %eax; jmp tail):
 *     65 8b 04 25 0c 00 00 00  mov %gs:0x08..0x20, %eax (8 B)
 *     eb <r>                   jmp tail                 (2 B)
 *
 *   +109 shared tail:
 *     0f 01 f8          swapgs  (restore user GS)      (3 B)
 *     48 0f 07          sysretq                         (3 B)
 *     # total: 115 B  (fits in +0x40..+0x100 region
 *     # that was reclaimed from the old SYSRET_OFFSET
 *     # which moved to +0x420).
 *
 * The je/jmp displacements below are computed at write
 * time because they depend on the total body layout —
 * the bytes below use placeholders (marked with
 * 0xRR) that the init path patches in. Simpler would be
 * to hardcode them, but keeping the computation
 * explicit makes it easy to add / reorder handlers
 * without a manual recount.
 *
 * Getpid returns tgid (POSIX pid). Gettid returns tid
 * (Linux thread id). See kvm_backend.h for the struct
 * field ↔ offset mapping.
 */
#define KVM_GADGET_FALLBACK_OFF	62	/* offset of fallback `swapgs; out` (G6-f-o) */

static const u8 kvm_bootstrap_lstar_bytes[] = {
	/*
	 * +0 entry: swapgs (user→kernel GS). After this %gs
	 * points at the per-vCPU state page (memo 11 G3 +
	 * MSR_KERNEL_GS_BASE programming in kvm_enter_guest).
	 * Every fallback/handler end restores user GS before
	 * sysretq.
	 */
	0x0f, 0x01, 0xf8,			/* swapgs */

	/*
	 * +3 upper-NR guard prologue (G6-follow-on, audit
	 * round-5 F4 — "syscall dispatch aliases on low byte").
	 * The main dispatch below uses `cmp $imm8, %al` which
	 * only compares the low byte of RAX. Without this
	 * guard, NRs sharing a low byte with a gadget-handled
	 * NR get hijacked — utimensat (280 = 0x118, low=0x18)
	 * aliases sched_yield; preadv (295 = 0x127) aliases
	 * getpid; mount_setattr (442 = 0x1ba) aliases gettid.
	 * Real correctness bug, not theoretical.
	 *
	 * Two-step guard:
	 *   1. Pre-check the one gadget-handled NR that
	 *      exceeds 0xff: getcpu = 0x135. If it matches,
	 *      jmp rel32 to getcpu_body at +282.
	 *   2. Upper-byte test: if any of bits 8..31 of %eax
	 *      are set, the NR can't correspond to any
	 *      cmp-$imm8-handled gadget entry — fallback.
	 *   3. Otherwise low-byte dispatch below is safe.
	 *
	 * Hot-path cost: 2 cmps + 2 jnes before dispatch
	 * (~5-8 cyc on Zen 4 / Alder Lake). Well within G8's
	 * measured 23-34 ns envelope; memo 07 target is 100 ns.
	 */
	/* +3  cmp $0x135, %eax  (NR_getcpu pre-check) */
	0x3d, 0x35, 0x01, 0x00, 0x00,
	/* +8  jne +5 (skip getcpu stub) */
	0x75, 0x05,
	/* +10 jmp rel32 getcpu_body (+330 post-G1); rel32 = 330-15 = 315 = 0x13b */
	0xe9, 0x3b, 0x01, 0x00, 0x00,
	/* +15 test $0xffffff00, %eax  (any high bits?) */
	0xa9, 0x00, 0xff, 0xff, 0xff,
	/* +20 jne +40 → fallback (+62) */
	0x75, 0x28,

	/*
	 * +22 low-NR dispatch. 10 × (cmp imm8 + je rel8) = 40 B.
	 * Order chosen to pack handlers in rel8 reach from
	 * every dispatch je.
	 *
	 *   +22: je getpid_h      (+70)
	 *   +26: je gettid_h      (+84)
	 *   +30: je getppid_h     (+98)
	 *   +34: je getuid_h      (+112)
	 *   +38: je geteuid_h     (+126)
	 *   +42: je getgid_h      (+140)
	 *   +46: je getegid_h     (+154)
	 *   +50: je sched_yield_h (+168)
	 *   +54: je clock_stub    (+176)
	 *   +58: je time_stub     (+181)
	 *   +62: fall through to fallback
	 */
	0x3c, 0x27, 0x74, 44,	/* cmp $0x27 (getpid),  je  */
	0x3c, 0xba, 0x74, 54,	/* cmp $0xba (gettid),  je  */
	0x3c, 0x6e, 0x74, 64,	/* cmp $0x6e (getppid), je  */
	0x3c, 0x66, 0x74, 74,	/* cmp $0x66 (getuid),  je  */
	0x3c, 0x6b, 0x74, 84,	/* cmp $0x6b (geteuid), je  */
	0x3c, 0x68, 0x74, 94,	/* cmp $0x68 (getgid),  je  */
	0x3c, 0x6c, 0x74, 104,	/* cmp $0x6c (getegid), je  */
	/*
	 * Audit round-6 G5: sched_yield demoted from class E to A.
	 * In-gadget short-circuit returned 0 without consulting
	 * UML's scheduler — POSIX says sched_yield is advisory but
	 * the gadget's bypass meant a guest sched_yield loop could
	 * starve other UML tasks for up to ~10 ms (one host timer
	 * tick) before SIGALRM-driven preemption fired. Demoting
	 * back to class A routes through handle_syscall →
	 * sys_sched_yield → schedule(); UML's scheduler gets
	 * immediate attention. Cost: ~13 µs VMEXIT cost per
	 * sched_yield, vs the gadget's ~30 ns. sched_yield is
	 * rarely called in tight loops; the latency hit is the
	 * right tradeoff for correct semantics.
	 *
	 * Implementation is the minimum change: redirect the
	 * dispatch entry's je rel8 to the fallback at +62
	 * (rel8 = 62 - 54 = 8). The handler body bytes at
	 * +168..+175 are now unreachable; kept in place to avoid
	 * disturbing the pid-family / clock / time / getcpu
	 * offsets. A future LSTAR compaction can remove them.
	 */
	0x3c, 0x18, 0x74, 8,	/* cmp $0x18 (sched_yield), je → fallback */
	0x3c, 0xe4, 0x74, 118,	/* cmp $0xe4 (clock_gettime), je → stub */
	0x3c, 0xc9, 0x74, 119,	/* cmp $0xc9 (time), je → stub */

	/* +62  fallback — unknown NR (doubles as target of
	 * the +20 jne and of a falling-through no-match).
	 */
	0x0f, 0x01, 0xf8,			/* swapgs (restore user GS) */
	0xe6, 0xf4,				/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,			/* sysretq */

	/* +70  handler_getpid (inline, 14 B):
	 *   mov %gs:KVM_GADGET_OFF_TGID, %eax; swapgs; sysretq
	 */
	0x65, 0x8b, 0x04, 0x25, 0x08, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +84  handler_gettid */
	0x65, 0x8b, 0x04, 0x25, 0x0c, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +98  handler_getppid */
	0x65, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +112 handler_getuid */
	0x65, 0x8b, 0x04, 0x25, 0x14, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +126 handler_geteuid */
	0x65, 0x8b, 0x04, 0x25, 0x18, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +140 handler_getgid */
	0x65, 0x8b, 0x04, 0x25, 0x1c, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/* +154 handler_getegid */
	0x65, 0x8b, 0x04, 0x25, 0x20, 0x00, 0x00, 0x00,
	0x0f, 0x01, 0xf8,
	0x48, 0x0f, 0x07,

	/*
	 * +168 handler_sched_yield (G6, 8 B):
	 *   xor %eax, %eax; swapgs; sysretq
	 *
	 * Returns 0 without a scheduling hint — sched_yield
	 * is advisory per POSIX; the outer UML scheduler runs
	 * on the next VMEXIT (timer tick, real I/O syscall).
	 */
	0x31, 0xc0,				/* xor %eax, %eax */
	0x0f, 0x01, 0xf8,			/* swapgs */
	0x48, 0x0f, 0x07,			/* sysretq */

	/*
	 * +176 clock stub: jmp rel32 clock_body (+186)
	 *   rel32 = 186 - 181 = 5
	 * +181 time  stub: jmp rel32 time_body  (+289 post-G1)
	 *   rel32 = 289 - 186 = 103 = 0x67
	 *
	 * The stubs sit within rel8 reach of their dispatch
	 * entries; the rel32 jmp then reaches the big bodies
	 * past sched_yield. Time body offset shifted from +274
	 * pre-G1 to +289 post-G1 because the clock body's
	 * G1 bounds check added 15 bytes.
	 */
	0xe9, 0x05, 0x00, 0x00, 0x00,
	0xe9, 0x67, 0x00, 0x00, 0x00,

	/*
	 * +186 handler_clock_gettime (CLOCK_MONOTONIC only, 88 B)
	 *
	 * Audit round-5 F7 part 2: G6's body loaded the
	 * seqlock value into %eax, clobbering NR=228 before
	 * any fallback jne fired. If the host then serviced
	 * fallback via the `out $0xf4` trap, it read RAX=
	 * seq-value as the syscall number — confusion bug.
	 * Fix: load SEQ into %edx so RAX stays NR=228
	 * throughout; a fallback hands the correct NR back
	 * to handle_syscall.
	 *
	 * Audit round-5 F8: prepend a 15-byte call-budget
	 * decrement so the gadget falls back periodically,
	 * letting the host refresh the vvar. Without it, a
	 * tight clock_gettime loop never VMEXITs and guest-
	 * observed time freezes forever. Budget is reset to
	 * KVM_VVAR_BUDGET_INITIAL in kvm_gadget_vvar_refresh.
	 *
	 * All fallback jnes use rel32 — rel8 doesn't reach
	 * fallback at +62 from this offset.
	 *
	 * On entry: RDI = clockid, RSI = struct timespec *ts,
	 *           RAX = 228 (NR), swapgs already done.
	 */
	/* +186 sub $1, %gs:0x1028 (BUDGET); 9 B */
	0x65, 0x83, 0x2c, 0x25, 0x28, 0x10, 0x00, 0x00, 0x01,
	/* +195 js rel32 fallback (+62); rel32 = 62-201 = -139 = 0xffffff75 */
	0x0f, 0x88, 0x75, 0xff, 0xff, 0xff,
	/* +201 cmp $1, %edi */
	0x83, 0xff, 0x01,
	/* +204 jne rel32 fallback (+62); rel32 = 62-210 = -148 = 0xffffff6c */
	0x0f, 0x85, 0x6c, 0xff, 0xff, 0xff,
	/* +210 mov %gs:0x1000, %edx (SEQ); 8 B */
	0x65, 0x8b, 0x14, 0x25, 0x00, 0x10, 0x00, 0x00,
	/* +218 test $1, %dl */
	0xf6, 0xc2, 0x01,
	/* +221 jne rel32 fallback (+62); rel32 = 62-227 = -165 = 0xffffff5b */
	0x0f, 0x85, 0x5b, 0xff, 0xff, 0xff,
	/* +227 mov %gs:0x1008, %r10 (MONO_SEC); 9 B */
	0x65, 0x4c, 0x8b, 0x14, 0x25, 0x08, 0x10, 0x00, 0x00,
	/* +236 mov %gs:0x1010, %r8  (MONO_NSEC); 9 B */
	0x65, 0x4c, 0x8b, 0x04, 0x25, 0x10, 0x10, 0x00, 0x00,
	/* +245 cmp %gs:0x1000, %edx (SEQ re-read); 8 B */
	0x65, 0x3b, 0x14, 0x25, 0x00, 0x10, 0x00, 0x00,
	/* +253 jne rel32 fallback (+62); rel32 = 62-259 = -197 = 0xffffff3b */
	0x0f, 0x85, 0x3b, 0xff, 0xff, 0xff,
	/*
	 * +259 G1 bounds check: cmp %rsi, %gs:0x1030 (TASK_SIZE_CAP)
	 * — flags reflect (cap - rsi); jbe taken when cap <= rsi
	 * (rsi >= cap), routing to fallback so the SYSCALL path
	 * applies access_ok / -EFAULT semantics. 9 B + 6 B.
	 */
	0x65, 0x48, 0x39, 0x34, 0x25, 0x30, 0x10, 0x00, 0x00,
	/* +268 jbe rel32 fallback (+62); rel32 = 62-274 = -212 = 0xffffff2c */
	0x0f, 0x86, 0x2c, 0xff, 0xff, 0xff,
	/* +274 mov %r10, (%rsi) */
	0x4c, 0x89, 0x16,
	/* +277 mov %r8, 8(%rsi) */
	0x4c, 0x89, 0x46, 0x08,
	/* +281 xor %eax, %eax */
	0x31, 0xc0,
	/* +283 swapgs */
	0x0f, 0x01, 0xf8,
	/* +286 sysretq */
	0x48, 0x0f, 0x07,

	/*
	 * +289 handler_time (41 B post-G1 + RAX-preserve fix)
	 *
	 *   time(2): time_t time(time_t *tloc);
	 *
	 * Returns REAL_SEC from the vvar page; optionally
	 * writes it to *tloc. No seqlock retry — time(2) has
	 * 1-second resolution so a torn read is at worst off
	 * by one second (the vDSO uses the same shortcut).
	 *
	 * Audit round-6 G1 (+ F7/2-style RAX preservation):
	 * load REAL_SEC into %rdx (NOT %rax) so RAX stays =
	 * NR=201 throughout the body. If the bounds check
	 * triggers a fallback, handle_syscall sees the right
	 * NR. Move %rdx → %rax just before sysretq so the
	 * gadget's return value is REAL_SEC.
	 *
	 * NULL tloc skips both the bounds check and the store
	 * but still returns REAL_SEC.
	 *
	 * On entry: RDI = tloc (may be NULL), RAX = 201 (NR),
	 *           swapgs already done.
	 */
	/* +289 mov %gs:0x1018, %rdx (REAL_SEC into RDX); 9 B */
	0x65, 0x48, 0x8b, 0x14, 0x25, 0x18, 0x10, 0x00, 0x00,
	/* +298 test %rdi, %rdi */
	0x48, 0x85, 0xff,
	/* +301 je +18 (skip bounds + store, land at mov rdx,rax) */
	0x74, 0x12,
	/* +303 cmp %rdi, %gs:0x1030 (TASK_SIZE_CAP); 9 B */
	0x65, 0x48, 0x39, 0x3c, 0x25, 0x30, 0x10, 0x00, 0x00,
	/* +312 jbe rel32 fallback (+62); rel32 = 62-318 = -256 = 0xffffff00
	 * RAX still = 201 here, so handle_syscall dispatches sys_time.
	 */
	0x0f, 0x86, 0x00, 0xff, 0xff, 0xff,
	/* +318 mov %rdx, (%rdi)  — store REAL_SEC into *tloc */
	0x48, 0x89, 0x17,
	/* +321 mov %rdx, %rax    — return value = REAL_SEC */
	0x48, 0x89, 0xd0,
	/* +324 swapgs */
	0x0f, 0x01, 0xf8,
	/* +327 sysretq */
	0x48, 0x0f, 0x07,

	/*
	 * +330 handler_getcpu (64 B post-G1 + RAX-preserve fix)
	 *
	 *   int getcpu(unsigned int *cpu, unsigned int *node);
	 *
	 * Returns 0. Writes CPU_ID from the per-vCPU state
	 * page to *cpu if non-NULL; writes 0 to *node if
	 * non-NULL (UML has no NUMA — all tasks are node 0).
	 *
	 * Uses %edx (not %ecx) for the CPU_ID load because
	 * RCX still holds the user's return RIP — SYSRETQ
	 * loads RIP from RCX, so clobbering RCX breaks the
	 * sysretq tail.
	 *
	 * Audit round-6 G1: each non-NULL pointer gets a
	 * TASK_SIZE bounds check before the store. NULL
	 * pointers skip the check + store via je.
	 *
	 * Audit round-6 G1 (+ RAX preserve): defer the
	 * `xor %eax, %eax` (return value = 0) until just
	 * before sysretq, and use %r10d for the node-write
	 * zero. This way RAX stays = NR=309 across both
	 * bounds checks; if either fallback fires,
	 * handle_syscall sees the right NR.
	 *
	 * On entry: RDI = cpu ptr, RSI = node ptr, RAX = 309
	 *           (NR), swapgs already done.
	 */
	/* +330 test %rdi, %rdi */
	0x48, 0x85, 0xff,
	/* +333 je +25 (skip cpu bounds check + store) */
	0x74, 0x19,
	/* +335 cmp %rdi, %gs:0x1030 (TASK_SIZE_CAP); 9 B */
	0x65, 0x48, 0x39, 0x3c, 0x25, 0x30, 0x10, 0x00, 0x00,
	/* +344 jbe rel32 fallback (+62); rel32 = 62-350 = -288 = 0xfffffee0 */
	0x0f, 0x86, 0xe0, 0xfe, 0xff, 0xff,
	/* +350 mov %gs:0x04, %edx (CPU_ID); 8 B */
	0x65, 0x8b, 0x14, 0x25, 0x04, 0x00, 0x00, 0x00,
	/* +358 mov %edx, (%rdi) */
	0x89, 0x17,
	/* +360 test %rsi, %rsi */
	0x48, 0x85, 0xf6,
	/* +363 je +21 (skip node bounds check + store, land at xor eax) */
	0x74, 0x15,
	/* +365 cmp %rsi, %gs:0x1030 (TASK_SIZE_CAP); 9 B */
	0x65, 0x48, 0x39, 0x34, 0x25, 0x30, 0x10, 0x00, 0x00,
	/* +374 jbe rel32 fallback (+62); rel32 = 62-380 = -318 = 0xfffffec2 */
	0x0f, 0x86, 0xc2, 0xfe, 0xff, 0xff,
	/* +380 xor %r10d, %r10d (node value = 0; via r10 not rax) */
	0x45, 0x31, 0xd2,
	/* +383 mov %r10d, (%rsi) */
	0x44, 0x89, 0x16,
	/* +386 xor %eax, %eax (return 0) — deferred to here so RAX=NR
	 *      survived all bounds checks above.
	 */
	0x31, 0xc0,
	/* +388 swapgs */
	0x0f, 0x01, 0xf8,
	/* +391 sysretq */
	0x48, 0x0f, 0x07,

	/* total body: 394 bytes (G6-followon + F8 + G1 + RAX preserve;
	 * LSTAR region 448 B).
	 */
};
#else
static const u8 kvm_bootstrap_lstar_bytes[] = {
	0xe6, 0xf4,		/* out %al, $0xf4 */
	0x48, 0x0f, 0x07,	/* sysretq */
};
#endif

/*
 * Ring-3 bootstrap trampoline (memo 08 sub-commit #5a): 3 bytes,
 * just SYSRETQ. Placed at a dedicated offset so host-side
 * kvm_enter_guest can point vCPU RIP here on first entry to
 * transition the guest from CPL=0 (where kvm_setup_production_
 * sregs leaves it after long-mode init) into CPL=3 at the
 * caller's intended user RIP. Setup discipline:
 *
 *   - MSR_STAR[63:48] = 0x18 (bootstrap via #2a's kvm_enter_
 *     guest_program_msrs): SYSRETQ loads CS=(0x18+16)|3=0x2b
 *     (ring-3 code, GDT idx 5) and SS=(0x18+8)|3=0x23 (ring-3
 *     data, GDT idx 4).
 *   - RCX = user's intended RIP (KVM_SET_REGS).
 *   - R11 = 0x3202 (RFLAGS with IF=1, bit-1 reserved-one,
 *     IOPL=3); SYSRETQ loads RFLAGS from R11.
 *   - RIP = bootstrap_va + KVM_BOOTSTRAP_SYSRET_OFFSET.
 *
 * The 3-byte sequence is byte-identical to the LSTAR
 * trampoline's tail, but lives at a distinct offset so a fresh
 * first entry doesn't accidentally trip the OUT-before-SYSRETQ.
 * Session finding (commit 386c04a3219b diagnostic dump): this
 * gadget is what moves the guest from running init in ring-0
 * (the previously-observed CPL=0 triple-fault state) into
 * correct ring-3 execution.
 */
static const u8 kvm_bootstrap_sysret_bytes[] = {
	0x48, 0x0f, 0x07,	/* sysretq */
};

/*
 * Ring-0 → ring-3 IRETQ gadget (task #272). Used on bootstrap
 * re-entry when the host needs ALL user GPRs preserved across the
 * transition — specifically, the recoverable-#PF path. SYSRETQ-
 * based re-entry overwrites RCX (user-RIP carrier) and R11 (user-
 * RFLAGS carrier); IRETQ pops CS:RIP/RFLAGS/SS:RSP from the
 * current stack and leaves every GPR intact.
 *
 * Discipline at the call site:
 *   - kvm_bootstrap_page_stack[0..0x28] holds the iretq frame:
 *       +0x00: user RIP
 *       +0x08: ring-3 CS = 0x33 (selector 6 | RPL=3)
 *       +0x10: user RFLAGS  (bit 1 set is required; IF set; IOPL=3)
 *       +0x18: user RSP
 *       +0x20: ring-3 SS = 0x2b (selector 5 | RPL=3)
 *   - vCPU RIP = bootstrap_va + KVM_BOOTSTRAP_IRETQ_OFFSET
 *   - vCPU RSP = bootstrap_va + 0x3000  (IST stack page base, where
 *     the host wrote the iretq frame).
 *   - vCPU RFLAGS = 0x002 (ring-0 reserved-one bit only).
 *   - User GPRs (RAX..R15) pass through unchanged.
 *
 * The IDT IST1 pointer also lives at +0x4000 (one page above this
 * frame), so a #PF mid-iretq pushes its frame to the IST top
 * without colliding with the bootstrap frame at +0x000.
 */
static const u8 kvm_bootstrap_iretq_bytes[] = {
	0x48, 0xcf,		/* iretq */
};

/*
 * #PF handler (memo 08 sub-commit #5b, audit-followon
 * RAX-preservation): 8 bytes. CPU delivers #PF via IDT[14]
 * with IST=1 → RSP loaded from TSS.IST[1], SS set to null.
 * Error code pushed on stack. Handler:
 *
 *   e6 fb             out %al, $0xfb    ; VMEXIT → host fault-fill
 *   48 83 c4 08       add $8, %rsp      ; pop #PF error code
 *   48 cf             iretq             ; return to ring-3 at faulting RIP
 *
 * The `out` triggers KVM_EXIT_IO on UM_KVM_PF_PORT (0xfb). The
 * host reads CR2 via KVM_GET_SREGS (separately from this VMEXIT
 * — the host doesn't consume %al), invokes UML's fault path to
 * install the backing page, refreshes the shadow PT, then exits
 * the inner KVM_RUN loop via interrupt_end and the outer
 * userspace() loop re-enters at the user RIP extracted from the
 * IST iretq frame.
 *
 * RAX preservation (audit followon): a prior version started
 * with `mov %cr2, %rax` so the host could read the faulting VA
 * out of regs->rax. That read was always redundant — the host
 * pulls CR2 from KVM_GET_SREGS directly — and worse, it
 * clobbered the user's RAX before KVM_GET_REGS captured the
 * vCPU state. On the recoverable #PF path the host stuffs only
 * IP/SP/RFLAGS from the IST frame into `regs`, leaving
 * regs->gp[HOST_AX] whatever KVM_GET_REGS returned — i.e. the
 * (clobbered) cr2 value rather than the user's pre-fault RAX.
 * Dropping the `mov` keeps user RAX live across the fault. AL
 * is whatever the user had (we don't care; the host doesn't
 * consume the OUT byte).
 */
static const u8 kvm_bootstrap_pf_handler_bytes[] = {
	0xe6, 0xfb,			/* out %al, $0xfb  */
	0x48, 0x83, 0xc4, 0x08,		/* add $8, %rsp    */
	0x48, 0xcf,			/* iretq           */
};

#define UM_KVM_PF_PORT	0xfb	/* sub-commit #5b #PF-handler VMEXIT */

/*
 * #DF handler (task #269): a double-fault is unrecoverable —
 * we got here because a #PF (or earlier exception) cascaded
 * before the corresponding handler could complete. Real
 * kernels treat #DF as fatal; ours likewise. We just need to
 * surface the event to the host with the IST frame's saved
 * RIP so the diagnostic can identify which instruction
 * triggered the cascade.
 *
 * #DF is special: the CPU pushes an error_code (always 0) +
 * the iretq frame, but the saved RIP is the RIP AT TIME OF
 * the original fault delivery, not at the faulting handler
 * — i.e. we get the original faulting RIP for free, no
 * separate decode needed.
 *
 * Handler:
 *   e6 fa             out %al, $0xfa    ; VMEXIT with #DF signal
 *   f4                hlt                ; in case host doesn't kill us
 *
 * The host treats KVM_EXIT_IO(0xfa) as fatal — extracts cr2
 * (via KVM_GET_SREGS) + the IST frame's saved RIP/RSP/
 * RFLAGS for the diagnostic, then panics or fatal_sigsegvs
 * the task. The hlt is a defensive backstop; we don't
 * expect to reach it.
 */
static const u8 kvm_bootstrap_df_handler_bytes[] = {
	0xe6, 0xfa,			/* out %al, $0xfa  */
	0xf4,				/* hlt             */
};

#define UM_KVM_DF_PORT	0xfa	/* task #269 #DF-handler VMEXIT */

/*
 * #GP handler (audit round-7 P1 follow-on, task #272 IRETQ
 * hardening). A bootstrap IRETQ from the host-built iretq frame
 * raises #GP if any of:
 *   - frame[0] (user RIP) is non-canonical
 *   - frame[3] (user RSP) is non-canonical
 *   - frame[1] (CS) selector points at a not-present descriptor
 *   - frame[2] (RFLAGS) sets reserved bits in a way the CPU
 *     refuses (e.g. VM bit while leaving long mode)
 *
 * Without an IDT[13] handler the #GP itself faults during delivery
 * (no entry to dispatch to), cascading to #DF — and historically
 * the #DF handler was added in task #269 to surface the cascade.
 * But that conflates "non-canonical user RSP" (a fixable user-
 * task contract violation) with "kernel bug cascade" (always
 * fatal). Splitting them: a #GP gets its own port (0xf9) so the
 * host can dispatch SIGSEGV at the user task while keeping #DF
 * reserved for true kernel-side cascades.
 *
 * Same minimal shape as the #DF handler — the CPU pushes an
 * error_code + iretq frame, the OUT signals the host, and HLT
 * backstops in case the host fails to kill us.
 */
static const u8 kvm_bootstrap_gp_handler_bytes[] = {
	0xe6, 0xf9,			/* out %al, $0xf9  */
	0xf4,				/* hlt             */
};

#define UM_KVM_GP_PORT	0xf9	/* audit-round-7 P1 #GP-handler VMEXIT */

static int kvm_enter_guest_init_bootstrap(void)
{
	void *page;
	void *page_stack;
	u64 gpa;
	u64 stack_gpa;
	unsigned long flags;

	/* Fast path: already allocated. */
	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	if (kvm_bootstrap_page) {
		spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	/*
	 * Allocate outside the lock — GFP_KERNEL can sleep. Second
	 * check under the lock covers the race where two callers
	 * both took the !page branch above; the loser frees its
	 * allocations.
	 *
	 * F5-followon: two pages now — page is the code+tables page
	 * (RO X in guest), page_stack is the dedicated IST stack
	 * (RW NX in guest). Splitting them lets us drop ring-0
	 * write privilege on the LSTAR/IDT/GDT/TSS bytes.
	 */
	page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	page_stack = (void *)get_zeroed_page(GFP_KERNEL);
	if (!page_stack) {
		free_page((unsigned long)page);
		return -ENOMEM;
	}

	gpa = (u64)__pa(page);
	stack_gpa = (u64)__pa(page_stack);

	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	if (kvm_bootstrap_page) {
		spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);
		free_page((unsigned long)page);
		free_page((unsigned long)page_stack);
		return 0;
	}

	/*
	 * Populate the GDT in the page. Same 6-entry layout as the
	 * harness path so SYSRETQ-in-guest lands CS/SS correctly
	 * (memo 08 #1 reuses the harness GDT shape exactly — only
	 * the CR3 + page location differ between harness and
	 * production).
	 */
	kvm_setup_harness_gdt((u64 *)((char *)page +
				       KVM_BOOTSTRAP_GDT_OFFSET));

	/*
	 * Write the LSTAR trampoline bytes at the fixed offset
	 * (memo 08 sub-commit #2). Byte-identical to the harness
	 * wire form; the trampoline is live once MSR_LSTAR is
	 * programmed to point at bootstrap_va +
	 * KVM_BOOTSTRAP_LSTAR_OFFSET.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_LSTAR_OFFSET +
		     sizeof(kvm_bootstrap_lstar_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_LSTAR_OFFSET,
	       kvm_bootstrap_lstar_bytes, sizeof(kvm_bootstrap_lstar_bytes));

	/*
	 * Write the ring-3 bootstrap SYSRETQ gadget (sub-commit
	 * #5a). kvm_enter_guest points first-entry RIP at this
	 * location to transition the guest into CPL=3 at the
	 * user's chosen RIP/RFLAGS.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_SYSRET_OFFSET +
		     sizeof(kvm_bootstrap_sysret_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_SYSRET_OFFSET,
	       kvm_bootstrap_sysret_bytes, sizeof(kvm_bootstrap_sysret_bytes));

	/*
	 * Install the #PF handler bytes (sub-commit #5b). The
	 * handler only runs if IDT + TSS are armed via SREGS in
	 * kvm_enter_guest; until that wires up the bytes sit dormant
	 * in the page and cost nothing.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_PF_HANDLER_OFFSET +
		     sizeof(kvm_bootstrap_pf_handler_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_PF_HANDLER_OFFSET,
	       kvm_bootstrap_pf_handler_bytes,
	       sizeof(kvm_bootstrap_pf_handler_bytes));

	/*
	 * Install the #DF handler bytes (task #269). Same pattern as
	 * the #PF handler — bytes sit dormant in the page until
	 * IDT[8] is armed in kvm_enter_guest's IDT setup below.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_DF_HANDLER_OFFSET +
		     sizeof(kvm_bootstrap_df_handler_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_DF_HANDLER_OFFSET,
	       kvm_bootstrap_df_handler_bytes,
	       sizeof(kvm_bootstrap_df_handler_bytes));

	/*
	 * Install the IRETQ re-entry gadget bytes (task #272). Like
	 * the SYSRETQ gadget but pops the cross-CPL state from a host-
	 * built iretq frame on the IST stack page, preserving every
	 * GPR (RCX/R11 specifically) across the ring-0→ring-3
	 * transition. Used on recoverable-#PF re-entries to keep
	 * user state intact through fault handling.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_IRETQ_OFFSET +
		     sizeof(kvm_bootstrap_iretq_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_IRETQ_OFFSET,
	       kvm_bootstrap_iretq_bytes,
	       sizeof(kvm_bootstrap_iretq_bytes));

	/*
	 * Install the #GP handler bytes (audit round-7 P1). Lives
	 * dormant until kvm_enter_guest's IDT setup arms IDT[13] at
	 * this offset. Catches non-canonical iretq targets + any
	 * privileged-insn #GP from user code; routes to host port
	 * 0xf9 for SIGSEGV dispatch.
	 */
	BUILD_BUG_ON(KVM_BOOTSTRAP_GP_HANDLER_OFFSET +
		     sizeof(kvm_bootstrap_gp_handler_bytes) > PAGE_SIZE);
	memcpy((char *)page + KVM_BOOTSTRAP_GP_HANDLER_OFFSET,
	       kvm_bootstrap_gp_handler_bytes,
	       sizeof(kvm_bootstrap_gp_handler_bytes));

	/*
	 * Extend the GDT to 8 entries: entries 0-5 were populated
	 * by kvm_setup_harness_gdt above (null, ring-0 code, ring-0
	 * data, padding anchor, ring-3 data, ring-3 code).
	 * Entries 6+7 together form the 16-byte TSS descriptor
	 * selected by KVM_BOOTSTRAP_TSS_SEL (0x30) when SREGS loads
	 * TR. AMD64 SDM vol 3 §4.8.3 describes the long-mode system
	 * segment descriptor layout:
	 *
	 *   bits  0..15  limit[0:15]
	 *   bits 16..31  base[0:15]
	 *   bits 32..39  base[16:23]
	 *   bits 40..47  type=9 | S=0 | DPL=0 | P=1  (0x89)
	 *   bits 48..55  limit[16:19] | AVL | G
	 *   bits 56..63  base[24:31]
	 *   desc[1] bits 0..31   base[32:63]
	 *   desc[1] bits 32..63  reserved (0)
	 */
	{
		u64 tss_base = (u64)(unsigned long)page +
				KVM_BOOTSTRAP_TSS_OFFSET;
		u32 tss_limit = 104 - 1;	/* TSS is 104 bytes */
		u64 *gdt = (u64 *)((char *)page + KVM_BOOTSTRAP_GDT_OFFSET);
		u64 low;

		low  = (u64)(tss_limit & 0xffff);
		low |= ((u64)(tss_base & 0xffff)) << 16;
		low |= ((u64)((tss_base >> 16) & 0xff)) << 32;
		low |= ((u64)0x89) << 40;		/* type=9, P=1 */
		low |= ((u64)((tss_limit >> 16) & 0xf)) << 48;
		low |= ((u64)((tss_base >> 24) & 0xff)) << 56;

		gdt[6] = low;
		gdt[7] = (tss_base >> 32) & 0xffffffffULL;
	}

	/*
	 * Zero the TSS + populate IST[1] only. RSP0/RSP1/RSP2 are
	 * not used (all our cross-CPL transitions go through IDT
	 * entries whose IST field points here). IST[1] top-of-
	 * stack is at bootstrap_va + KVM_BOOTSTRAP_STACK_TOP; post
	 * F5-followon split, that's bootstrap_va + 0x4000 (the
	 * exclusive top of the dedicated IST stack page mapped at
	 * +0x3000), so the first push lands at +0x3ff8 inside a
	 * P|RW|NX page that the host code+tables view (P only)
	 * does not overlap.
	 *
	 * TSS layout (AMD64 SDM vol 3 §10.8.2):
	 *   bytes  0..3   reserved
	 *   bytes  4..11  RSP0
	 *   bytes 12..19  RSP1
	 *   bytes 20..27  RSP2
	 *   bytes 28..35  reserved
	 *   bytes 36..43  IST1   ← populated
	 *   bytes 44..51  IST2
	 *   ...
	 *   bytes 96..99  reserved
	 *   bytes 100..103 I/O map base (set past limit → no I/O bitmap)
	 */
	{
		char *tss = (char *)page + KVM_BOOTSTRAP_TSS_OFFSET;
		u64 ist1 = (u64)(unsigned long)page +
				KVM_BOOTSTRAP_STACK_TOP;

		memset(tss, 0, 104);
		*(u64 *)(tss + 36) = ist1;
		*(u16 *)(tss + 102) = 104;	/* IOPB off-of-limit */
	}

	/*
	 * Populate IDT[14] (#PF). Other vectors stay zero — a hit
	 * is a contract-violation that should fail loudly. Long-
	 * mode IDT entry format (Intel SDM vol 3 §6.14.1):
	 *
	 *   bytes  0..1   offset[0:15]
	 *   bytes  2..3   segment selector (0x08 = ring-0 code)
	 *   byte   4      IST (low 3 bits)
	 *   byte   5      type_attr: P|DPL|0|type
	 *                 0x8E = P=1, DPL=0, 0, type=0xE (interrupt
	 *                        gate, 64-bit)
	 *   bytes  6..7   offset[16:31]
	 *   bytes  8..11  offset[32:63]
	 *   bytes 12..15  reserved (0)
	 */
	{
		u64 pf_va = (u64)(unsigned long)page +
				  KVM_BOOTSTRAP_PF_HANDLER_OFFSET;
		u64 df_va = (u64)(unsigned long)page +
				  KVM_BOOTSTRAP_DF_HANDLER_OFFSET;
		u64 gp_va = (u64)(unsigned long)page +
				  KVM_BOOTSTRAP_GP_HANDLER_OFFSET;
		u8 *idt = (u8 *)page + KVM_BOOTSTRAP_IDT_OFFSET;
		u8 *e;

		memset(idt, 0, KVM_BOOTSTRAP_IDT_ENTRIES * 16);

		/* IDT[8] — #DF (double fault), task #269. */
		e = idt + 8 * 16;
		e[0]  = (u8)(df_va & 0xff);
		e[1]  = (u8)((df_va >> 8) & 0xff);
		e[2]  = 0x08;			/* ring-0 code selector */
		e[3]  = 0x00;
		e[4]  = 0x01;			/* IST=1 */
		e[5]  = 0x8e;			/* P|DPL0|int-gate */
		e[6]  = (u8)((df_va >> 16) & 0xff);
		e[7]  = (u8)((df_va >> 24) & 0xff);
		e[8]  = (u8)((df_va >> 32) & 0xff);
		e[9]  = (u8)((df_va >> 40) & 0xff);
		e[10] = (u8)((df_va >> 48) & 0xff);
		e[11] = (u8)((df_va >> 56) & 0xff);

		/*
		 * IDT[13] — #GP (general protection fault), audit
		 * round-7 P1. Catches non-canonical iretq targets +
		 * any privileged-insn #GP from user code so we can
		 * deliver SIGSEGV instead of cascading to #DF (which
		 * is reserved for true kernel bugs and panics).
		 */
		e = idt + 13 * 16;
		e[0]  = (u8)(gp_va & 0xff);
		e[1]  = (u8)((gp_va >> 8) & 0xff);
		e[2]  = 0x08;			/* ring-0 code selector */
		e[3]  = 0x00;
		e[4]  = 0x01;			/* IST=1 */
		e[5]  = 0x8e;			/* P|DPL0|int-gate */
		e[6]  = (u8)((gp_va >> 16) & 0xff);
		e[7]  = (u8)((gp_va >> 24) & 0xff);
		e[8]  = (u8)((gp_va >> 32) & 0xff);
		e[9]  = (u8)((gp_va >> 40) & 0xff);
		e[10] = (u8)((gp_va >> 48) & 0xff);
		e[11] = (u8)((gp_va >> 56) & 0xff);

		/* IDT[14] — #PF (page fault), sub-commit #5b. */
		e = idt + 14 * 16;
		e[0]  = (u8)(pf_va & 0xff);
		e[1]  = (u8)((pf_va >> 8) & 0xff);
		e[2]  = 0x08;			/* ring-0 code selector */
		e[3]  = 0x00;
		e[4]  = 0x01;			/* IST=1 */
		e[5]  = 0x8e;			/* P|DPL0|int-gate */
		e[6]  = (u8)((pf_va >> 16) & 0xff);
		e[7]  = (u8)((pf_va >> 24) & 0xff);
		e[8]  = (u8)((pf_va >> 32) & 0xff);
		e[9]  = (u8)((pf_va >> 40) & 0xff);
		e[10] = (u8)((pf_va >> 48) & 0xff);
		e[11] = (u8)((pf_va >> 56) & 0xff);
		/* bytes 12-15 stay zero from memset. */
	}

	kvm_bootstrap_page       = page;
	kvm_bootstrap_page_stack = page_stack;
	kvm_bootstrap_gpa        = gpa;
	kvm_bootstrap_stack_gpa  = stack_gpa;
	kvm_bootstrap_va         = (u64)(unsigned long)page;
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	pr_info("um: kvm enter_guest: bootstrap page at va=%p gpa=0x%llx lstar=+0x%x sysret=+0x%x (%zu + %zu bytes); ist-stack page at gpa=0x%llx mapped at va+0x3000\n",
		page, (unsigned long long)gpa,
		KVM_BOOTSTRAP_LSTAR_OFFSET, KVM_BOOTSTRAP_SYSRET_OFFSET,
		sizeof(kvm_bootstrap_lstar_bytes),
		sizeof(kvm_bootstrap_sysret_bytes),
		(unsigned long long)stack_gpa);
	return 0;
}

int kvm_bootstrap_force_init(void)
{
	return kvm_enter_guest_init_bootstrap();
}
EXPORT_SYMBOL_GPL(kvm_bootstrap_force_init);

int kvm_bootstrap_copy_lstar(u8 *dst, size_t len)
{
	unsigned long flags;
	void *page;

	if (!dst || len < sizeof(kvm_bootstrap_lstar_bytes))
		return -EINVAL;

	spin_lock_irqsave(&kvm_bootstrap_lock, flags);
	page = kvm_bootstrap_page;
	spin_unlock_irqrestore(&kvm_bootstrap_lock, flags);

	if (!page)
		return -ENODATA;

	memcpy(dst, (char *)page + KVM_BOOTSTRAP_LSTAR_OFFSET,
	       sizeof(kvm_bootstrap_lstar_bytes));
	return sizeof(kvm_bootstrap_lstar_bytes);
}
EXPORT_SYMBOL_GPL(kvm_bootstrap_copy_lstar);

u64 kvm_build_sysret_r11_probe(u64 saved_user_rflags)
{
	return kvm_build_sysret_r11(saved_user_rflags);
}
EXPORT_SYMBOL_GPL(kvm_build_sysret_r11_probe);

#ifdef CONFIG_UM_BACKEND_KVM_GADGET
/*
 * Audit round-5 F7/1: classify a faulting RIP as "inside a user-
 * memory-writing gadget body" and return the NR the gadget was
 * servicing. Used by the #PF recovery path to convert a ring-0
 * gadget-mid-store fault into a proper SYSCALL fallback through
 * handle_syscall, which then returns -EFAULT per POSIX semantics
 * instead of delivering SIGSEGV.
 *
 * Only clock_gettime / time / getcpu issue stores to user
 * memory; the pid-family + sched_yield handlers return via
 * register only and cannot fault in the same way. A fault in
 * any other LSTAR region is a bug (e.g. missing gadget-state
 * page) and the caller should handle that case distinctly.
 *
 * Return: __NR_clock_gettime / __NR_time / __NR_getcpu if
 * fault_rip lies inside one of those handler bodies, otherwise
 * -1.
 *
 * Offset ranges mirror the LSTAR layout in
 * kvm_bootstrap_lstar_bytes[]; any reshuffle there must update
 * the ranges here (the KUnit byte-match test catches layout
 * drift at boot, but doesn't catch range drift — this lookup
 * is a derived invariant).
 */
int kvm_gadget_fault_nr(u64 fault_rip)
{
	u64 base = kvm_bootstrap_va + KVM_BOOTSTRAP_LSTAR_OFFSET;
	u64 off;

	if (fault_rip < base)
		return -1;
	off = fault_rip - base;

	/*
	 * Post-G1 (audit round-6) ranges. Bounds checks plus
	 * RAX-preservation re-encoding pushed each handler
	 * down: clock 186..289, time 289..330, getcpu 330..394.
	 */
	if (off >= 186 && off < 289)
		return __NR_clock_gettime;
	if (off >= 289 && off < 330)
		return __NR_time;
	if (off >= 330 && off < 394)
		return __NR_getcpu;
	return -1;
}
#else
int kvm_gadget_fault_nr(u64 fault_rip)
{
	(void)fault_rip;
	return -1;
}
#endif /* CONFIG_UM_BACKEND_KVM_GADGET */

/*
 * Program MSR_STAR / MSR_LSTAR / MSR_FMASK on vcpu0 so that a
 * guest-side SYSCALL traps into the LSTAR trampoline at
 * `lstar_gpa`. Encoding per AMD64 SDM §6.1.1:
 *
 *   MSR_STAR [47:32] = kernel CS selector (SYSCALL loads this)
 *                    = 0x0008 (ring-0 code; GDT idx 1)
 *   MSR_STAR [63:48] = SYSRET base selector (SYSRETQ loads
 *                      (base+16)|3 as CS, (base+8)|3 as SS)
 *                    = 0x0018 (unused padding slot; forces
 *                      CS=0x28|3=0x2b, SS=0x20|3=0x23 which
 *                      are GDT idx 5 + 4, the ring-3 pair)
 *
 * FMASK = 0 for now; real UML entry needs IF cleared among
 * other bits, but sub-commit #2's trampoline doesn't run any
 * code that races with interrupts — just `out`/`sysretq`.
 * Sub-commit #5 (KVM_EXIT_INTR) revisits.
 */
/*
 * Push MSR_FS_BASE / MSR_GS_BASE into the vCPU. Used by both
 * the initial `kvm_enter_guest` path (seeding from UML's per-
 * task gp[HOST_FS_BASE]) and the post-arch_prctl path in
 * `kvm_decode_syscall` (class B per memo 10). Guest glibc's
 * `_start` issues `arch_prctl(ARCH_SET_FS, tls_addr)` before
 * its first FS-relative load; without this propagation,
 * `fs:0x10` would fault at guest VA 0x10 and spin the
 * bootstrap #PF handler (the cr2=0x10 loop blocking task
 * #192). See Documentation/virt/uml/redesign/02-workstreams/
 * D-kvm-backend/10-syscall-classification.md §"Class B".
 *
 * Idempotent: writing the same FS_BASE twice is a no-op in
 * the vCPU. Called on every arch_prctl regardless of the
 * option (GET_* reads already come from UML's gp[] and
 * pushing them back is harmless).
 */
static int kvm_propagate_fs_gs_base(int vcpu_fd, u64 fs_base, u64 gs_base)
{
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[2];
	} msrs = {
		.info = { .nmsrs = 2 },
		.entries = {
			{
				.index = 0xc0000100,	/* MSR_FS_BASE */
				.data  = fs_base,
			},
			{
				.index = 0xc0000101,	/* MSR_GS_BASE */
				.data  = gs_base,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm: KVM_SET_MSRS(fs=0x%llx gs=0x%llx) failed (%d)\n",
				    (unsigned long long)fs_base,
				    (unsigned long long)gs_base, rc);
		return rc;
	}
	/* Writing 2 MSRs; anything less is a silent reject. */
	if (rc != 2) {
		pr_warn_once("um: kvm: KVM_SET_MSRS(fs/gs) wrote %d/2 MSRs\n",
			     rc);
		return -EIO;
	}
	return 0;
}

/*
 * Memo 11 G3: program MSR_KERNEL_GS_BASE to point at the
 * gadget state page. The gadget's LSTAR handler runs in
 * ring-0, where MSR_GS_BASE holds the USER's %gs (possibly
 * zero, possibly something glibc / arch_prctl(ARCH_SET_GS)
 * set). A `swapgs` at handler entry swaps MSR_GS_BASE
 * with MSR_KERNEL_GS_BASE, making %gs:off resolve to
 * the gadget state page; a second `swapgs` before
 * SYSRETQ restores the user's GS.
 *
 * Separating the gadget state channel from the user's GS
 * keeps userspace arch_prctl(ARCH_SET_GS) semantics
 * intact (memo 11 §"Per-vCPU state channel"); the ~4 cyc
 * cost of two swapgs per gadget call is accounted for in
 * the D71 / memo 11 post-G2 cost model.
 */
static int kvm_enter_guest_program_kernel_gs_base(u64 gadget_state_va)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[1];
	} msrs = {
		.info = { .nmsrs = 1 },
		.entries = {
			{
				.index = 0xc0000102,	/* MSR_KERNEL_GS_BASE */
				.data  = gadget_state_va,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;
	if (!gadget_state_va)
		return 0;	/* unmapped; nothing to program */
	/*
	 * Perf lever #3: gadget_state_va is allocated once per vCPU
	 * and never moves. After the first successful write, skip
	 * the ioctl on every subsequent kvm_enter_guest.
	 */
	if (kvm_backend_ctx()->kernel_gs_base_primed)
		return 0;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_MSRS(kernel_gs_base=0x%llx) failed (%d)\n",
				    (unsigned long long)gadget_state_va, rc);
		return rc;
	}
	if (rc != 1) {
		pr_warn_once("um: kvm enter_guest: KVM_SET_MSRS wrote %d/1 gadget-MSRs\n",
			     rc);
		return -EIO;
	}
	kvm_backend_ctx()->kernel_gs_base_primed = true;
	return 0;
}

static int kvm_enter_guest_program_msrs(u64 lstar_gpa)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct {
		struct kvm_msrs info;
		struct kvm_msr_entry entries[3];
	} msrs = {
		.info = { .nmsrs = 3 },
		.entries = {
			{
				.index = 0xc0000081,	/* MSR_STAR */
				.data  = ((u64)0x0018 << 48) |
					 ((u64)0x0008 << 32),
			},
			{
				.index = 0xc0000082,	/* MSR_LSTAR */
				.data  = lstar_gpa,
			},
			{
				.index = 0xc0000084,	/* MSR_FMASK */
				.data  = 0,
			},
		},
	};
	int rc;

	if (vcpu_fd < 0)
		return -EIO;
	/*
	 * Perf lever #3: STAR/LSTAR/FMASK are compile-time constants
	 * (LSTAR = bootstrap_va + 0x40; STAR = ring-0/ring-3 selector
	 * pair; FMASK = 0). After the first successful write they
	 * never change, so skip the ioctl on every subsequent
	 * kvm_enter_guest.
	 */
	if (kvm_backend_ctx()->msrs_primed)
		return 0;

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_MSRS, (unsigned long)&msrs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_MSRS(lstar=0x%llx) failed (%d)\n",
				    (unsigned long long)lstar_gpa, rc);
		return rc;
	}

	/*
	 * KVM_SET_MSRS returns the number of MSRs actually
	 * written. 3 is the expected value; anything less means
	 * one of STAR/LSTAR/FMASK was rejected and the trampoline
	 * is not armed.
	 */
	if (rc != 3) {
		pr_warn_once("um: kvm enter_guest: KVM_SET_MSRS wrote %d/3 MSRs\n",
			     rc);
		return -EIO;
	}
	kvm_backend_ctx()->msrs_primed = true;
	return 0;
}

/*
 * Marshal `uml_pt_regs` → `struct kvm_regs` for KVM_SET_REGS.
 * uml_pt_regs is a host-shaped pt_regs view; the HOST_* gp[]
 * slots are populated by the existing register helpers
 * (sysdep/ptrace.h). Every GP register that matters for guest
 * execution plus RIP / RSP / RFLAGS gets forwarded; CS / SS /
 * DS etc. are provided via SREGS and not touched here.
 *
 * Pure data-structure transform — unit-testable without
 * /dev/kvm.
 */
static void kvm_uml_regs_to_kvm_regs(struct kvm_regs *dst,
				     const struct uml_pt_regs *src);

/*
 * Reverse marshal: `struct kvm_regs` → `uml_pt_regs`. Called
 * from sub-commit #2b's KVM_RUN loop after KVM_EXIT_IO /
 * KVM_EXIT_MMIO / KVM_EXIT_INTR so UML's common syscall
 * dispatch (and any downstream fault / signal logic) sees
 * the guest's current GP register state.
 *
 * Mirrors the forward marshal exactly. HOST_ORIG_AX is NOT
 * set here — that's an UML-entry-path convention the caller
 * of kvm_decode_syscall arranges once it knows the syscall
 * bucket. Pure data-structure transform; no ioctl.
 */
static void kvm_regs_to_uml_regs(struct uml_pt_regs *dst,
				 const struct kvm_regs *src)
{
	unsigned long *gp = dst->gp;

	gp[HOST_AX]     = src->rax;
	gp[HOST_BX]     = src->rbx;
	gp[HOST_CX]     = src->rcx;
	gp[HOST_DX]     = src->rdx;
	gp[HOST_SI]     = src->rsi;
	gp[HOST_DI]     = src->rdi;
	gp[HOST_BP]     = src->rbp;
	gp[HOST_SP]     = src->rsp;
	gp[HOST_R8]     = src->r8;
	gp[HOST_R9]     = src->r9;
	gp[HOST_R10]    = src->r10;
	gp[HOST_R11]    = src->r11;
	gp[HOST_R12]    = src->r12;
	gp[HOST_R13]    = src->r13;
	gp[HOST_R14]    = src->r14;
	gp[HOST_R15]    = src->r15;
	gp[HOST_IP]     = src->rip;
	gp[HOST_EFLAGS] = src->rflags;
}

static void kvm_uml_regs_to_kvm_regs(struct kvm_regs *dst,
				     const struct uml_pt_regs *src)
{
	const unsigned long *gp = src->gp;

	dst->rax = gp[HOST_AX];
	dst->rbx = gp[HOST_BX];
	dst->rcx = gp[HOST_CX];
	dst->rdx = gp[HOST_DX];
	dst->rsi = gp[HOST_SI];
	dst->rdi = gp[HOST_DI];
	dst->rbp = gp[HOST_BP];
	dst->rsp = gp[HOST_SP];
	dst->r8  = gp[HOST_R8];
	dst->r9  = gp[HOST_R9];
	dst->r10 = gp[HOST_R10];
	dst->r11 = gp[HOST_R11];
	dst->r12 = gp[HOST_R12];
	dst->r13 = gp[HOST_R13];
	dst->r14 = gp[HOST_R14];
	dst->r15 = gp[HOST_R15];
	dst->rip = gp[HOST_IP];
	/*
	 * RFLAGS bit 1 is reserved and must be 1 per AMD64 SDM
	 * §3.1.4. uml_pt_regs preserves whatever the last trap
	 * captured; OR the fixed bit in defensively so a SET_REGS
	 * never trips #GP.
	 */
	dst->rflags = gp[HOST_EFLAGS] | (1UL << 1);
}

/*
 * kvm_enter_guest — materialize the vCPU state from `regs` so a
 * subsequent KVM_RUN enters the UML guest in long-mode ring-0
 * at the RIP the caller chose. Sub-commit #1 of memo 08.
 *
 * Does:
 *   1. Lazy-allocate the bootstrap page (GDT + LSTAR-tramp
 *      slot).
 *   2. Build production SREGS: long-mode segments,
 *      CR0/CR4/EFER, CR3 = __pa(current->active_mm->pgd),
 *      GDT base at the bootstrap page's GPA.
 *   3. KVM_SET_SREGS into vcpu0.
 *   4. Marshal `regs` → kvm_regs, KVM_SET_REGS.
 *
 * Does NOT (yet):
 *   - Program MSR_STAR / MSR_LSTAR / MSR_FMASK. Sub-commit #2
 *     lands the LSTAR trampoline + MSR program; until then,
 *     guest syscalls would fault on LSTAR=0. kvm_enter_guest
 *     is therefore only safe to call against a bootstrap
 *     workload that doesn't issue `syscall` — e.g. a ring-0
 *     HLT to verify the state transition works. The gate in
 *     kvm_run_userspace keeps the panic() path active by
 *     default so we don't accidentally run non-trap-safe
 *     guest code.
 *
 *   - Run the KVM_RUN loop. That's sub-commits #2-#5.
 *
 *   - Pin current->active_mm for the duration. Policy-A
 *     memslot covers all of physmem so a transient mm swap
 *     doesn't invalidate the memory backing, but if a real
 *     run_userspace caller's mm gets freed while KVM_RUN
 *     blocks, CR3 points at freed pages. Sub-commit #2 adds
 *     the `get_task_mm`-style pin.
 *
 * Returns 0 on success, -errno on KVM_SET_SREGS / KVM_SET_REGS
 * failure. Never panics — the harness path does that; the
 * production path surfaces errors to the caller (and from
 * there to the existing fallback logic, D-05 in sub-commit
 * #7).
 */
int kvm_enter_guest(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_sregs sregs;
	struct kvm_regs kregs;
	struct mm_struct *mm;
	u64 cr3_gpa;
	int rc;

	if (vcpu_fd < 0)
		return -EIO;
	if (!regs)
		return -EINVAL;

	rc = kvm_ensure_memslot();
	if (rc < 0)
		return rc;

	/*
	 * Task #273: install host CPUID on the vCPU before its first
	 * KVM_RUN so the guest sees real host x86 features (AVX2 /
	 * x86-64-v3 etc). Idempotent fast-path after the first call;
	 * non-fatal failure (logged once, kvm_enter_guest continues).
	 */
	(void)kvm_ensure_cpuid_done();

	rc = kvm_enter_guest_init_bootstrap();
	if (rc < 0)
		return rc;

	/*
	 * Memo 09 step 2: shadow PT is the guest CR3. Allocate on
	 * first use (kvm_shadow_pgd_alloc is idempotent; lazy per
	 * memo 09 step 1's finding that kvm_init fires before
	 * mm_init) + map the bootstrap page so the guest can fetch
	 * the SYSRETQ gadget + GDT + LSTAR trampoline. Without this
	 * step, D66's triple-fault fires; with it, the guest walks
	 * shadow PT → reaches the bootstrap page → begins executing.
	 *
	 * current->active_mm is still used by kvm_decode_mmio
	 * (memo 09 step 3) to locate the faulting process's
	 * logical pgd for bit-translation; this block no longer
	 * hands its pgd to the CPU as CR3.
	 */
	rc = kvm_shadow_pgd_alloc();
	if (rc < 0)
		return rc;

	/*
	 * Audit round-5 F5 + F5-followon (#230): the bootstrap state
	 * is split into two pages — code + tables (LSTAR, GDT, IDT,
	 * TSS, #PF handler, SYSRETQ) and a dedicated IST stack page.
	 *
	 * Page 1 (code + tables, mapped at bootstrap_va + 0x0000) is
	 * P only — readable + executable by the guest CPU at CPL=0
	 * for instruction fetch (LSTAR, #PF handler, SYSRETQ) and
	 * descriptor-table reads (GDTR / IDTR / TR consult the page
	 * via the segment-cache walk), but not writable. Host-side
	 * init populates GDT / IDT / TSS / handler bytes via the
	 * kernel mapping outside the shadow PT, so dropping ring-0
	 * write privilege on the guest view is harmless to setup
	 * and prevents a guest ring-0 escape from rewriting the
	 * trampoline. US is also off (F5 minimum) so ring-3 can't
	 * read or write the page.
	 *
	 * Page 2 (IST stack, mapped at bootstrap_va + 0x3000) is
	 * P | RW | NX — the CPU pushes the IDT iretq frame on this
	 * page when #PF fires and the #PF handler bytes pop it; NX
	 * blocks any return-to-stack ROP since the handler can't
	 * jump into IST data. STACK_TOP is bootstrap_va + 0x4000
	 * (exclusive); first push lands at +0x3ff8.
	 */
	rc = kvm_shadow_map_page(kvm_bootstrap_va,
				 (u64)__pa(kvm_bootstrap_page),
				 KVM_X86_PTE_P);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(bootstrap) failed (%d)\n",
				    rc);
		return rc;
	}
	rc = kvm_shadow_map_page(kvm_bootstrap_va + 3 * PAGE_SIZE,
				 (u64)__pa(kvm_bootstrap_page_stack),
				 KVM_X86_PTE_P | KVM_X86_PTE_RW |
				 KVM_X86_PTE_NX);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(bootstrap_stack) failed (%d)\n",
				    rc);
		return rc;
	}

	/*
	 * Memo 11 G3 — per-vCPU gadget state channel. Allocate
	 * the page lazily (same shape as shadow_pgd_alloc), map
	 * it one page above the bootstrap page (guest VA =
	 * kvm_bootstrap_va + 0x1000), and refresh it from
	 * `current` right before KVM_RUN so gadget handlers
	 * read up-to-date pid/tgid/uid/gid via %gs:<off>. Maps
	 * read-only from ring-3 (P | US) — only the host writes
	 * via the kernel VA alias.
	 */
	rc = kvm_gadget_state_alloc();
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: gadget_state_alloc failed (%d)\n",
				    rc);
		return rc;
	}
	{
		u64 gstate_va = kvm_bootstrap_va + PAGE_SIZE;

		/*
		 * Audit round-5 F5/F8 alignment: drop US bit. Gadget
		 * handlers access this page at CPL=0 (SYSCALL switches
		 * to ring-0 before fetching LSTAR), so US=0 is
		 * sufficient. Blocking ring-3 direct reads is a
		 * defense-in-depth match for F5's bootstrap-page fix.
		 *
		 * Phase 2 #245: set NX. The state page is data —
		 * pid/uid struct fields read by gadget handlers via
		 * %gs:disp32. No code lives here. NX prevents
		 * return-to-state-page ROP if a future bug ever lets
		 * the gadget body redirect ring-0 control flow into
		 * this region. Mirrors NX on IST stack (D96) and
		 * vvar page below. EFER.NXE is set unconditionally
		 * by kvm_setup_production_sregs (D-XX pre-condition);
		 * without NXE bit 63 is reserved-must-be-zero and
		 * would trigger reserved-bit-violation #PF on read.
		 */
		rc = kvm_shadow_map_page(gstate_va,
					 kvm_gadget_state_gpa(),
					 KVM_X86_PTE_P | KVM_X86_PTE_NX);
		if (rc < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(gadget_state) failed (%d)\n",
					    rc);
			return rc;
		}
		kvm_backend_ctx()->gadget_state_va = gstate_va;
	}
	kvm_gadget_state_refresh();

	/*
	 * Memo 11 G5: allocate + map the vvar clock page
	 * immediately above the state page in guest VA so a
	 * single MSR_KERNEL_GS_BASE (→ state page) covers both
	 * structs via disp32 addressing in the G5c handler asm
	 * (state at %gs:0x00+, vvar at %gs:0x1000+).
	 *
	 * Audit round-5 F8: mapped RW (not just P) because the
	 * clock gadget decrements the budget counter at
	 * %gs:0x1028 via `sub $1, %gs:<disp>` — a ring-0 write
	 * that faults if W=0 (CR0.WP is always set on our
	 * guest, so ring-0 respects write-protect).
	 * US dropped: ring-3 never accesses this page directly
	 * (F5/F8 defense-in-depth).
	 */
	rc = kvm_gadget_vvar_alloc();
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: gadget_vvar_alloc failed (%d)\n",
				    rc);
		return rc;
	}
	{
		u64 vvar_va = kvm_bootstrap_va + 2 * PAGE_SIZE;

		rc = kvm_shadow_map_page(vvar_va,
					 kvm_gadget_vvar_gpa(),
					 KVM_X86_PTE_P | KVM_X86_PTE_RW |
					 KVM_X86_PTE_NX);
		if (rc < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: shadow_map_page(gadget_vvar) failed (%d)\n",
					    rc);
			return rc;
		}
		kvm_backend_ctx()->gadget_vvar_va = vvar_va;
	}
	kvm_gadget_vvar_refresh();

	cr3_gpa = kvm_shadow_pgd_gpa();
	if (!cr3_gpa) {
		pr_warn_once("um: kvm enter_guest: shadow_pgd_gpa is zero after alloc\n");
		return -EIO;
	}

	/*
	 * Memo 09 step 3: eager-fill the shadow PT from the current
	 * process's logical pgd. Every present UML PTE gets a
	 * matching x86-encoded entry via kvm_um_pte_to_x86. Bounded
	 * by actually-mapped pages (typical /bin/true: 20-50 pages;
	 * larger processes scale linearly).
	 *
	 * Called every kvm_enter_guest today — wasteful on repeat
	 * entries but correct. Optimization (skip when UML pgd
	 * hasn't changed since last fill) is a follow-on lift once
	 * D-06 perf measurements quantify the cost.
	 */
	mm = current->active_mm;
	if (mm && mm->pgd) {
		struct kvm_um *fctx = kvm_backend_ctx();
		int filled;

		/*
		 * Task #242 (perf lever #4): if the shadow PT is
		 * already in sync with this mm's pgd — same mm
		 * pointer AND same pgd-VA AND no invalidation event
		 * since the last fill — skip the full re-walk. Every
		 * path that mutates UML's logical pgd flows through
		 * either kvm_shadow_pgd_clear_user (cross-mm switch)
		 * or kvm_shadow_invalidate_va_range (mm_map /
		 * mm_unmap), both of which reset shadow_pgd_synced=
		 * false. Direct refills from the syscall + #PF recovery
		 * paths invoke kvm_shadow_fill_from_uml_pgd which sets
		 * synced=true, keeping the invariant tight without a
		 * generation counter.
		 *
		 * Audit round-7 P2: comparing both the mm pointer AND
		 * the pgd-VA closes the VA-reuse hazard (execve-style
		 * mm replacement where the old mm's freed pgd page
		 * could be reused for the new mm's pgd). The cross-mm
		 * kvm_context_switch hook only fires on prev->active_mm
		 * != next->active_mm; an execve on the same task does
		 * NOT trip it, so an mm-pointer mismatch alone catches
		 * that case.
		 */
		if (fctx->shadow_pgd_synced &&
		    fctx->shadow_pgd_synced_mm == mm &&
		    fctx->shadow_pgd_synced_va == (u64)mm->pgd) {
			pr_info_ratelimited("um: kvm enter_guest: shadow PT already in sync (mm=%p pgd=%p, skip fill)\n",
					    mm, mm->pgd);
			goto fill_done;
		}

		/*
		 * Task #238 — STEP 2: drop kvm_touch_all_user_vmas. The
		 * eager prefault was carrying every user vma's pages
		 * into UML's logical pgd before each kvm_enter_guest so
		 * the shadow fill below could see them, costing
		 * O(mm-size) per syscall (~2870 vma walks for a
		 * dynamically-linked workload).
		 *
		 * That cost was masking a real recovery-path hole: in
		 * #PF recovery we used to invoke `copy_from_user(&probe,
		 * cr2, 1)` as a side-effect to prod UML's fault path
		 * into populating cr2's PTE. STEP 1 (commit 9e71295123a8)
		 * replaced that with a direct handle_page_fault() call
		 * matching arch/um/kernel/trap.c's SIGSEGV path, which
		 * Just Worked for in-vma user faults regardless of
		 * whether touch-all had pre-populated the leaf.
		 *
		 * With STEP 1 in place, dropping the eager touch makes
		 * the fast-path syscall round-trip O(1) again. The
		 * shadow fill still walks current->active_mm->pgd to
		 * mirror its present leaves into the shadow PT — that's
		 * proportional to actually-mapped pages, not vma extent,
		 * and is still cheap (~50 leaves for /bin/true, scaling
		 * with workload mapping density).
		 *
		 * Validation: dyn-loader (init=/bin/echo, hits ld-linux
		 * + libc + multiple shared libs) PASSES under this
		 * STEP-2 path with #272's IRETQ recovery + #273's CPUID
		 * passthrough — all six-or-so lazy CoW recoveries
		 * service correctly through the in-vma path.
		 */
		filled = kvm_shadow_fill_from_uml_pgd(mm->pgd);
		if (filled < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: shadow fill failed (%d)\n",
					    filled);
			return filled;
		}
		pr_info_ratelimited("um: kvm enter_guest: filled %d shadow PTEs (lazy)\n",
				    filled);
fill_done:
		;	/* perf-lever #4 skip-target — falls through */
	}

	/*
	 * Perf lever #3b: skip SREGS reprogramming entirely when
	 * every mutable field matches what we last wrote. Stable
	 * fields (GDT/IDT/TR/CS/SS/CR0/CR4/EFER/APIC) never change
	 * post-init; only CR3 (on mm-switch) and FS_BASE/GS_BASE
	 * (on arch_prctl) drift. shadow_dirty forces a CR3 reload
	 * even when cr3_gpa unchanged (TLB flush discipline from
	 * F6-followon D83). First entry always programs.
	 */
	{
		struct kvm_um *ctx = kvm_backend_ctx();
		u64 cur_fs = regs->gp[HOST_FS_BASE];
		u64 cur_gs = regs->gp[HOST_GS_BASE];

		if (ctx->sregs_primed &&
		    ctx->cached_cr3_gpa == cr3_gpa &&
		    ctx->cached_fs_base == cur_fs &&
		    ctx->cached_gs_base == cur_gs &&
		    !ctx->shadow_dirty)
			goto sregs_done;
	}

	/*
	 * Start from the current SREGS so APIC / TR / LDT bits
	 * KVM expects preserved stay intact (same discipline the
	 * harness follows — see sregs.c preamble).
	 */
	rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_GET_SREGS failed (%d)\n",
				    rc);
		return rc;
	}

	/*
	 * sregs.gdt.base + LSTAR take LINEAR addresses (the CPU
	 * walks the guest CR3 to resolve them), not physical.
	 * Pass the bootstrap page's kernel VA; its walk through
	 * current->active_mm->pgd resolves to bootstrap_gpa under
	 * the Policy A identity memslot. Memo 08 sub-commit #5a
	 * session finding: previously passing a gpa here worked
	 * by accident under the harness's identity-paging setup
	 * (KVM_HARNESS_GDT_OFFSET=0x3000 happens to be a valid
	 * VA in the harness's own 2 MiB mapping); under the
	 * production CR3 (current->active_mm->pgd), that
	 * accidental identity doesn't hold.
	 */
	kvm_setup_production_sregs(&sregs, cr3_gpa, kvm_bootstrap_va);

	/*
	 * Sub-commit #5b: arm the IDT + TSS so guest-side #PF gets
	 * routed to the bootstrap #PF handler instead of triple-
	 * faulting. Both base fields are linear addresses (guest
	 * CR3 resolves them through the shadow PT to the bootstrap
	 * page). TR descriptor lives at GDT[6] (selector 0x30);
	 * `type=11` (0xb) marks it as a busy 64-bit TSS after load
	 * — KVM's KVM_SET_SREGS accepts the available-TSS form and
	 * flips the busy bit on load. The kvm_segment structure
	 * mirrors what the CPU caches after an LTR instruction.
	 */
	sregs.idt.base  = kvm_bootstrap_va + KVM_BOOTSTRAP_IDT_OFFSET;
	sregs.idt.limit = KVM_BOOTSTRAP_IDT_ENTRIES * 16 - 1;
	sregs.tr = (struct kvm_segment){
		.base     = kvm_bootstrap_va + KVM_BOOTSTRAP_TSS_OFFSET,
		.limit    = 104 - 1,
		.selector = KVM_BOOTSTRAP_TSS_SEL,
		.type     = 11,		/* 64-bit busy TSS */
		.present  = 1,
		.dpl      = 0,
		.s        = 0,		/* system segment */
		.g        = 0,
	};

	/*
	 * Seed FS/GS base from the task's stored values (memo 10
	 * class B, sub-commit #5c). UML's sys_arch_prctl stashes
	 * ARCH_SET_FS/GS into gp[HOST_FS_BASE] / gp[HOST_GS_BASE];
	 * the outer userspace() loop may have entered kvm_run_
	 * userspace after a reschedule where the previous
	 * SET_MSRS state was lost. Seed via sregs.fs.base /
	 * gs.base so the first KVM_RUN inherits the right TLS
	 * pointer without a separate KVM_SET_MSRS call. In long
	 * mode KVM keeps the segment cache base and
	 * MSR_{FS,GS}_BASE in sync; writing one writes the other.
	 */
	sregs.fs.base = regs->gp[HOST_FS_BASE];
	sregs.gs.base = regs->gp[HOST_GS_BASE];

	rc = os_ioctl_generic(vcpu_fd, KVM_SET_SREGS, (unsigned long)&sregs);
	if (rc < 0) {
		pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_SREGS(cr3=0x%llx gdt_va=0x%llx) failed (%d)\n",
				    (unsigned long long)cr3_gpa,
				    (unsigned long long)kvm_bootstrap_va, rc);
		return rc;
	}
	{
		struct kvm_um *ctx = kvm_backend_ctx();

		ctx->cached_cr3_gpa = cr3_gpa;
		ctx->cached_fs_base = sregs.fs.base;
		ctx->cached_gs_base = sregs.gs.base;
		ctx->sregs_primed   = true;
		/* shadow_dirty consumed by this CR3 reload. */
		ctx->shadow_dirty   = false;
	}

sregs_done:
	/*
	 * Build vCPU regs from UML regs, then overlay the ring-0 →
	 * ring-3 IRETQ bootstrap shape (task #272).
	 *
	 * Pre-#272 this used a 3-byte SYSRETQ gadget at SYSRET_OFFSET
	 * with kregs.rcx ← user RIP and kregs.r11 ← user RFLAGS, since
	 * SYSRETQ loads RIP from RCX and RFLAGS from R11 on its way to
	 * ring-3. That works for fresh entries and SYSCALL returns
	 * (where RCX/R11 are caller-saved by the SYSCALL ABI anyway)
	 * but DESTROYS user RCX/R11 on a recoverable-#PF re-entry —
	 * symptom: ld-linux's RELR loop uses RCX as the relocation
	 * cursor; SYSRETQ-clobbered RCX terminated the loop after one
	 * iteration, leaving 11/12 relative relocations un-applied
	 * (task #272 wild jump to 0xc680).
	 *
	 * IRETQ pops CS:RIP / RFLAGS / SS:RSP from the current stack
	 * and preserves every GPR. We build the iretq frame on the
	 * IST stack page (host VA = kvm_bootstrap_page_stack, guest VA
	 * = bootstrap_va + 0x3000) at offset 0..0x28, then point vCPU
	 * RSP there and vCPU RIP at the IRETQ_OFFSET gadget. The IST
	 * top (= bootstrap_va + 0x4000, used by IDT[14] when a #PF
	 * fires later) is a full page above offset 0x28, so a mid-
	 * iretq #PF doesn't clobber the bootstrap frame.
	 *
	 * Selectors (must match kvm_setup_harness_gdt's layout):
	 *   ring-3 CS = 0x2b (GDT idx 5, base sel 0x28, RPL=3)
	 *   ring-3 SS = 0x23 (GDT idx 4, base sel 0x20, RPL=3)
	 * Same pair SYSRETQ derives from MSR_STAR[63:48] = 0x18; IRETQ
	 * just requires explicit pushes since it has no STAR-based
	 * shortcut.
	 *
	 * RFLAGS in the iretq frame: pre-OR the always-on bits
	 * (reserved-1, IF, IOPL=3) using the same builder previously
	 * used for SYSRETQ's R11 — same architectural bits, just
	 * delivered through a different transport.
	 */
	kvm_uml_regs_to_kvm_regs(&kregs, regs);
	{
		u64 *frame = (u64 *)kvm_bootstrap_page_stack;

		frame[0] = regs->gp[HOST_IP];
		frame[1] = 0x2bULL;					/* ring-3 CS */
		frame[2] = kvm_build_sysret_r11(regs->gp[HOST_EFLAGS]);	/* RFLAGS */
		frame[3] = regs->gp[HOST_SP];
		frame[4] = 0x23ULL;					/* ring-3 SS */
	}
	kregs.rip    = kvm_bootstrap_va + KVM_BOOTSTRAP_IRETQ_OFFSET;
	kregs.rsp    = kvm_bootstrap_va + 3 * PAGE_SIZE;	/* IST page base */
	kregs.rflags = (1UL << 1);			/* ring-0 RFLAGS */

	/*
	 * Perf-lever #2: when KVM_CAP_SYNC_REGS is supported, hand
	 * the prepared kregs to KVM through the mmap'd kvm_run
	 * struct rather than a KVM_SET_REGS ioctl. Saves one ioctl
	 * per kvm_enter_guest on the fallback syscall path.
	 */
	if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_REGS) {
		struct kvm_run *run = kvm_backend_ctx()->run0;

		run->s.regs.regs = kregs;
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
		run->kvm_valid_regs |= KVM_SYNC_X86_REGS;
	} else {
		rc = os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
				      (unsigned long)&kregs);
		if (rc < 0) {
			pr_warn_ratelimited("um: kvm enter_guest: KVM_SET_REGS(tramp_rip=0x%llx user_rip=0x%llx) failed (%d)\n",
					    (unsigned long long)kregs.rip,
					    (unsigned long long)kregs.rcx, rc);
			return rc;
		}
	}

	/*
	 * Experiment #2: opt-in to KVM_SYNC_X86_SREGS so the post-
	 * VMEXIT CR2 read on the #PF handler path is a memory load
	 * from run->s.regs.sregs.cr2 instead of a KVM_GET_SREGS
	 * ioctl (~50-100ns saved per recoverable fault). KVM updates
	 * the shadow sregs whenever the guest's view changes
	 * (including page faults that store CR2), so the post-RUN
	 * read sees the right value.
	 *
	 * Set valid_regs but NOT dirty_regs — we're not pushing
	 * sregs into the vCPU here (that goes through KVM_SET_SREGS
	 * above when SREGS-skip-cache misses). Just asking KVM to
	 * publish them on the way out.
	 *
	 * Cap-gated: kvm_ctx.sync_regs_caps is the bitmap returned
	 * by KVM_CHECK_EXTENSION(KVM_CAP_SYNC_REGS); KVM_SYNC_X86_
	 * SREGS is bit 1 (KVM_SYNC_X86_REGS is bit 0). When SREGS
	 * isn't in the cap bitmap we fall back to the unconditional
	 * KVM_GET_SREGS ioctl in the #PF handler.
	 */
	if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_SREGS) {
		struct kvm_run *run = kvm_backend_ctx()->run0;

		run->kvm_valid_regs |= KVM_SYNC_X86_SREGS;
	}

	/*
	 * Arm the SYSCALL trap: MSR_LSTAR at the bootstrap page's
	 * LSTAR trampoline (linear address, same rationale as
	 * GDT). MSR_STAR carries the ring-0 / ring-3 selectors;
	 * also used by the ring-3 bootstrap SYSRETQ above.
	 * Idempotent on repeat entry — KVM stores the MSRs on
	 * the vCPU.
	 */
	rc = kvm_enter_guest_program_msrs(kvm_bootstrap_va +
					  KVM_BOOTSTRAP_LSTAR_OFFSET);
	if (rc < 0)
		return rc;

	/*
	 * Memo 11 G3: MSR_KERNEL_GS_BASE -> gadget state VA.
	 * Programmed only when the gadget state page has been
	 * mapped (kvm_gadget_state_va() returns 0 otherwise).
	 * Gadget handlers `swapgs` at entry to swap in this
	 * base; user's MSR_GS_BASE stays whatever sub-commit
	 * #5c set for them.
	 */
	rc = kvm_enter_guest_program_kernel_gs_base(kvm_gadget_state_va());
	if (rc < 0)
		return rc;

	return 0;
}

/*
 * KUnit-visible test hook. Exposed only when the integrated
 * gate is on so a kunit module can exercise the pure data-
 * structure path (no /dev/kvm required). Takes a pre-zeroed
 * kvm_sregs + kvm_regs pair from the caller and a fake
 * uml_pt_regs source so the marshalling logic + sregs
 * production shape are testable without actually calling
 * KVM_SET_*.
 *
 * Kept next to the real implementation so the marshalling
 * helpers have a single definition; sub-commits #2-#5 extend
 * the probe as new fields flow through.
 */
int kvm_enter_guest_probe(struct kvm_sregs *sregs, struct kvm_regs *regs,
			  const struct uml_pt_regs *src,
			  u64 cr3_gpa, u64 gdt_gpa)
{
	if (!sregs || !regs || !src)
		return -EINVAL;
	kvm_setup_production_sregs(sregs, cr3_gpa, gdt_gpa);
	/*
	 * Mirror the real kvm_enter_guest path's FS/GS base seeding
	 * (memo 10 sub-commit #5c) so contract tests catch any drift
	 * between probe and production.
	 */
	sregs->fs.base = src->gp[HOST_FS_BASE];
	sregs->gs.base = src->gp[HOST_GS_BASE];
	kvm_uml_regs_to_kvm_regs(regs, src);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_enter_guest_probe);

/*
 * Reverse-direction probe: exercises the kvm_regs → uml_pt_regs
 * marshal. Sub-commit #2b's KVM_RUN loop feeds KVM_GET_REGS
 * output into this helper; for the contract test today it's a
 * round-trip fidelity check.
 */
int kvm_exit_guest_probe(struct uml_pt_regs *dst, const struct kvm_regs *src)
{
	if (!dst || !src)
		return -EINVAL;
	kvm_regs_to_uml_regs(dst, src);
	return 0;
}
EXPORT_SYMBOL_GPL(kvm_exit_guest_probe);

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */

/*
 * kvm_run_userspace: backend's run_userspace op. Called in a
 * loop by arch/um/os-Linux/skas/process.c::userspace(regs).
 * Contract per Documentation/virt/uml/backend-contract.rst:
 * set up vCPU, run guest until the next trap, fill `regs`
 * with current state (+ regs->is_user=1 + HOST_ORIG_AX for
 * syscalls), dispatch the kernel-side handler, return.
 *
 * Two compile-time flavors:
 *   - CONFIG_UM_BACKEND_KVM_INTEGRATED=y: real KVM_RUN loop
 *     + exit-reason decode (memo 08 sub-commit #2b).
 *   - CONFIG_UM_BACKEND_KVM_INTEGRATED=n: D-04a scaffold
 *     panic, unchanged.
 */
#ifdef CONFIG_UM_BACKEND_KVM_INTEGRATED

/*
 * Called from the syscall-exit branch of kvm_run_userspace.
 * Populates `regs` with the guest's user-mode state read back
 * from KVM_GET_REGS, advances the saved RIP past the 2-byte
 * `out %al, $0xf4` so the post-dispatch KVM_RUN resumes at
 * the trampoline's SYSRETQ, and dispatches the syscall into
 * UML's common sys_call_table path. After dispatch, the
 * caller marshals regs->gp[HOST_AX] (= syscall return) back
 * to vCPU state for the SYSRETQ to deliver to ring-3.
 */
static void kvm_decode_syscall(struct uml_pt_regs *regs,
			       struct kvm_regs *kregs, int vcpu_fd)
{
	/*
	 * `regs->gp[]` already populated by the caller's KVM_GET_REGS
	 * → kvm_regs_to_uml_regs(). RAX holds the guest's original
	 * syscall number (the LSTAR trampoline's `out` didn't
	 * clobber it — the post-out RIP/RFLAGS live in RCX/R11).
	 * Cache the number up front so handle_syscall clobbering
	 * HOST_AX (with the return value) doesn't hide it from the
	 * post-dispatch class-B propagation below.
	 */
	unsigned long syscall_nr = regs->gp[HOST_AX];

	PT_SYSCALL_NR(regs->gp) = syscall_nr;
	regs->is_user = 1;

	/*
	 * SYSCALL saves post-instruction RIP into RCX (and
	 * RFLAGS into R11) before jumping to MSR_LSTAR. For the
	 * break-out-and-re-enter pattern (audit A1), the next
	 * call to kvm_enter_guest's bootstrap IRETQ dance
	 * (post-#272) resumes ring-3 at whatever HOST_IP holds,
	 * so stash the user's continuation RIP there and the
	 * user's saved RFLAGS into HOST_EFLAGS.
	 *
	 * Audit round-4 F2: kvm_regs_to_uml_regs() above set
	 * regs->gp[HOST_EFLAGS] from kregs.rflags, which at
	 * SYSCALL-VMEXIT time is the KERNEL RFLAGS (we're
	 * mid-LSTAR-trampoline, after the CPU masked through
	 * FMASK on entry). The user's RFLAGS was instead saved
	 * into R11 by SYSCALL itself — that's the authoritative
	 * value for resuming the caller. Overwrite HOST_EFLAGS
	 * from regs->gp[HOST_R11] (which kvm_regs_to_uml_regs
	 * populated from kregs.r11) so kvm_enter_guest's
	 * SYSRETQ gadget can restore the correct architectural
	 * state via kvm_build_sysret_r11().
	 *
	 * Historical note: before A1 an inner `for (;;)` loop
	 * stayed in the same KVM_RUN and advanced kregs->rip
	 * past the LSTAR's 2-byte `out` so the trampoline's
	 * sysretq at +0x42 ran in ring-0 and returned to RCX in
	 * ring-3. That worked because the vCPU state carried
	 * through to the next KVM_RUN iteration. Under A1's
	 * per-trap re-entry model, kvm_enter_guest rebuilds all
	 * the vCPU state from `regs`, so HOST_IP must be the
	 * user RIP, not the LSTAR-internal address.
	 */
	regs->gp[HOST_IP]     = regs->gp[HOST_CX];
	regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11];

	/*
	 * Memo 10 class-D short-circuit: syscalls that would be
	 * semantically wrong for a bare-userspace guest (ptrace,
	 * reboot, kexec_load / kexec_file_load, init/finit/
	 * delete_module, bpf) are trapped at the dispatcher
	 * before reaching handle_syscall. Returning -EPERM is the
	 * accurate "we saw the syscall, and we refuse it at this
	 * layer" answer — unlike -ENOSYS (which specifically
	 * means "no such syscall NR in this kernel"), -EPERM is
	 * the established convention for permission-style denials
	 * (see capable() / ns_capable() failure paths throughout
	 * the kernel). Future tightening can promote this to
	 * SIGSYS via signal delivery for audit visibility.
	 */
	if (kvm_classify_syscall(syscall_nr) == KVM_SYSCALL_CLASS_TRAP) {
		pr_info_ratelimited("um: kvm: trapping class-D syscall nr=%lu (memo 10)\n",
				    syscall_nr);
		regs->gp[HOST_AX] = -EPERM;
		goto skip_dispatch;
	}

	/*
	 * Common syscall dispatch path. Writes the return value
	 * into regs->gp[HOST_AX]; the caller marshals that back
	 * to vCPU state.
	 */
	/*
	 * Memo 13 step 3.5: replay-side dispatch. When the active
	 * record is in replay mode, consume the next log entry
	 * instead of issuing handle_syscall — that's how we make
	 * a syscall byte-deterministic across boots. Cursor advances
	 * inside kvm_record_consume_syscall under the lock.
	 *
	 * Returns:
	 *   1  ⇒ entry consumed; caller uses replay_ret as the
	 *        syscall return value, optionally restores the
	 *        user buffer from the side payload.
	 *   0  ⇒ no entry available (record exhausted, or not in
	 *        replay mode); fall through to handle_syscall.
	 *   <0 ⇒ NR-mismatch divergence; reported via warn but
	 *        falls through to live handle_syscall to keep
	 *        forward progress (deviation from log is logged
	 *        but doesn't terminate the run).
	 *
	 * Static-key gate keeps the cost zero when off; the
	 * spinlock-protected kvm_record_consume_syscall is the only
	 * cost when on.
	 */
	if (static_branch_unlikely(&um_kvm_record_enabled)) {
		long replay_ret;
		u64 replay_user_va;
		const void *replay_payload = NULL;
		size_t replay_payload_len = 0;
		int consumed;

		consumed = kvm_record_consume_syscall(syscall_nr,
						      &replay_ret,
						      &replay_user_va,
						      &replay_payload,
						      &replay_payload_len);
		if (consumed > 0) {
			/*
			 * Replay path: skip handle_syscall, serve the
			 * recorded return + restore the user buffer if
			 * the entry carries one.
			 */
			regs->gp[HOST_AX] = (unsigned long)replay_ret;
			if (replay_payload && replay_payload_len) {
				/*
				 * copy_to_user can fault → recursive
				 * record-side hook firing. Suppress by
				 * temporarily dropping replaying state?
				 * No: we hold no record lock here, and
				 * the gated record hook checks
				 * recording, not replaying. The fault
				 * itself goes through #PF recovery
				 * which doesn't touch the record. Safe.
				 */
				if (copy_to_user((void __user *)
						 (unsigned long)replay_user_va,
						 replay_payload,
						 replay_payload_len))
					pr_warn_ratelimited("um: kvm record_replay: copy_to_user(%llu, %zu) failed at cursor restore\n",
							    (unsigned long long)replay_user_va,
							    replay_payload_len);
			}
			goto record_dispatch_done;
		}
		/*
		 * Review-01 P1: divergence / end-of-log policy.
		 *
		 *   consumed < 0  ⇒ -EILSEQ (NR mismatch) or -ENODATA
		 *                   (strict end-of-log). Strict
		 *                   replay refuses the syscall: set
		 *                   regs->gp[HOST_AX] = -EIO and
		 *                   skip handle_syscall — the user
		 *                   task sees a hard error instead
		 *                   of a silent fall-through.
		 *   consumed == 0 ⇒ no entry available AND not
		 *                   strict (loose mode) — fall
		 *                   through to live handle_syscall.
		 *                   Or: not in replay mode at all
		 *                   (recording/idle); ditto.
		 */
		if (consumed < 0) {
			pr_warn_ratelimited("um: kvm record_replay: divergence rc=%d nr=%lu — fail-stop (returning -EIO to guest)\n",
					    consumed, syscall_nr);
			regs->gp[HOST_AX] = (unsigned long)(long)(-EIO);
			goto record_dispatch_done;
		}
		/* consumed == 0 → fall through to live handle_syscall
		 * + observe (recording mode or loose end-of-log).
		 */
	}

	pr_info_ratelimited("um: kvm: dispatching handle_syscall nr=%lu (via LSTAR trampoline)\n",
			    PT_SYSCALL_NR(regs->gp));
	handle_syscall(regs);

	/*
	 * Memo 13 step 2: observe the syscall return for record/replay
	 * if recording is active. static_branch_unlikely turns into a
	 * NOP when the gate is off, so non-recording runtime pays
	 * zero cost. The current payload is the syscall NR + return
	 * value + the first two output u64s pulled from the
	 * regs->gp[HOST_*] convention; full output-buffer capture for
	 * read/write-style syscalls comes in a follow-up.
	 */
	if (static_branch_unlikely(&um_kvm_record_enabled)) {
		/*
		 * Memo 13 step 3: route through the per-NR dispatcher
		 * so getrandom / read-style syscalls capture their
		 * output buffer payload, not just the inline return
		 * value. The dispatcher takes regs directly so each
		 * per-NR case can pluck the syscall args (rdi/rsi/
		 * rdx/r10/r8/r9) it needs without an api change.
		 * Other NRs fall through to the inline-only path
		 * inside kvm_record_observe_dispatch.
		 */
		kvm_record_observe_dispatch(syscall_nr,
					    (long)regs->gp[HOST_AX],
					    regs);
	}

record_dispatch_done:

	/*
	 * Experiment #1 (post-audit-round-7): skip the post-syscall
	 * shadow PT refill for syscall classes that can't mutate the
	 * mm. Only mm-modifying syscalls (mmap/munmap/brk/mprotect/
	 * mremap/etc.) need the refill — and those go through UML's
	 * mm_map / mm_unmap callbacks, which call kvm_shadow_
	 * invalidate_va_range and reset shadow_pgd_synced=false. So
	 * the next kvm_enter_guest's skip-fill check (#242) will
	 * notice the unsynced flag and refill anyway. For non-mm
	 * syscalls (read/write/getpid/clock_gettime/...), the shadow
	 * PT is provably unchanged, and the refill walk is pure
	 * cost.
	 *
	 * Class taxonomy (kvm_classify_syscall, syscall_class.c):
	 *   - PASSTHROUGH: most syscalls, including all the hot
	 *                  ones; can fault paged-out user memory but
	 *                  that's serviced by #PF recovery's own
	 *                  refill, not this post-syscall one.
	 *   - VCPU_STATE:  arch_prctl etc.; modifies vCPU regs but
	 *                  not mm.
	 *   - SIGFRAME:    rt_sigreturn; modifies vCPU regs from
	 *                  sigframe; not mm.
	 *   - TRAP:        ptrace/reboot/etc.; short-circuited
	 *                  earlier with -EPERM; never gets here.
	 *   - GADGET:      gettid/etc.; no fallback after gadget
	 *                  passthrough.
	 *
	 * NONE of these classes mutate UML's mm directly. Mm-
	 * mutating syscalls (mmap/munmap/brk) go through
	 * generic VM helpers that funnel into UML's mm_map/mm_unmap
	 * callbacks, which already invalidate the shadow PT
	 * synchronously. So the unconditional post-syscall fill is
	 * always redundant for the syscall path itself.
	 *
	 * Reverts trivially: re-enable the unconditional fill if
	 * any class is later observed to mutate mm without going
	 * through mm_map/unmap.
	 */

skip_dispatch:
	/*
	 * Class-B post-dispatch propagation (memo 10 sub-commit #5c):
	 * arch_prctl(ARCH_SET_FS/GS) updated gp[HOST_FS_BASE] /
	 * gp[HOST_GS_BASE] inside sys_arch_prctl — propagate those
	 * values into the vCPU's MSR_FS_BASE / MSR_GS_BASE so the
	 * next SYSRETQ-to-ring-3 sees the right TLS pointer. GET
	 * options don't modify gp[] so a SET_MSRS here is harmless
	 * (same values round-tripped). Non-arch_prctl syscalls skip
	 * this path entirely; FS/GS are preserved by KVM across
	 * VMEXITs so there's nothing to do for them.
	 */
	if (syscall_nr == __NR_arch_prctl) {
		int prc = kvm_propagate_fs_gs_base(vcpu_fd,
						   regs->gp[HOST_FS_BASE],
						   regs->gp[HOST_GS_BASE]);
		if (prc < 0) {
			/*
			 * Audit finding A4: UML's sys_arch_prctl
			 * already updated task_struct FS/GS state;
			 * if we can't push that into the vCPU, the
			 * guest's next fs:-relative load will fault
			 * at the wrong address + spin in the #PF
			 * handler. Kill the guest task rather than
			 * continue with a mismatched
			 * task_struct / vCPU view.
			 */
			pr_warn_ratelimited("um: kvm: arch_prctl FS/GS propagate failed (%d); killing guest task\n",
					    prc);
			fatal_sigsegv();
		}
	}

	/*
	 * Post-dispatch UML convention (lifted from seccomp_run_
	 * userspace): clear UPT_SYSCALL_NR so the caller's is_user
	 * sample doesn't re-dispatch, and reset ORIG_AX to -1 on
	 * architectures where its offset differs from syscall-RET.
	 * On x86_64 they're the same offset (HOST_AX); the guard
	 * is a no-op there but kept for symmetry with the other
	 * backends.
	 */
	UPT_SYSCALL_NR(regs) = -1;
	if (PT_SYSCALL_NR_OFFSET != PT_SYSCALL_RET_OFFSET)
		PT_SYSCALL_NR(regs->gp) = -1;

	/*
	 * Push the syscall return (now in regs->gp[HOST_AX])
	 * back into the vCPU state so the next KVM_RUN (via
	 * kvm_enter_guest's bootstrap IRETQ, which picks up
	 * RAX from kregs) delivers the right return value to
	 * ring-3. Failure here leaves handle_syscall's work
	 * stranded — guest resumes with stale RAX, likely
	 * triggering an infinite loop in the glibc error-
	 * check path. Audit A4: fatal rather than silent.
	 */
	kvm_uml_regs_to_kvm_regs(kregs, regs);
	/*
	 * Perf-lever #2: when KVM_SYNC_X86_REGS is live, hand the
	 * post-syscall kregs back to KVM through the mmap'd run
	 * struct rather than a KVM_SET_REGS ioctl.
	 */
	if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_REGS) {
		struct kvm_run *run = kvm_backend_ctx()->run0;

		run->s.regs.regs = *kregs;
		run->kvm_dirty_regs |= KVM_SYNC_X86_REGS;
	} else if (os_ioctl_generic(vcpu_fd, KVM_SET_REGS,
				    (unsigned long)kregs) < 0) {
		pr_warn_ratelimited("um: kvm: post-syscall KVM_SET_REGS failed; killing guest task\n");
		fatal_sigsegv();
	}
}

/*
 * Forward-declared + used by run_userspace's time-travel
 * bookkeeping below. Defined in arch/um/os-Linux/skas/process.c,
 * shared across all run_userspace impls and reset by
 * switch_threads on every context switch. Matches the pattern
 * already landed in seccomp/trap_user.c + ptrace/trap_user.c.
 */
extern unsigned int unscheduled_userspace_iterations;

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_run *run = kvm_backend_ctx()->run0;
	struct kvm_regs kregs;
	int rc;

	if (vcpu_fd < 0 || !run) {
		panic("um: kvm run_userspace: vCPU not initialized (vcpu_fd=%d run=%p)",
		      vcpu_fd, run);
	}

	/*
	 * Time-travel bookkeeping (sub-commit #4 from memo 08).
	 * Under TT_MODE_INFCPU / TT_MODE_EXTERNAL, UML's virtual
	 * clock doesn't advance unless something forces it; a
	 * userspace task spinning in a tight trap loop would freeze
	 * simulated time. Bump tt_extra_sched_jiffies after
	 * CONFIG_UML_MAX_USERSPACE_ITERATIONS unyielded iterations
	 * so the scheduler gets a chance to tick. Matches the
	 * identical guard in seccomp_run_userspace /
	 * ptrace_run_userspace; the counter resets on
	 * switch_threads.
	 */
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL) {
#ifdef CONFIG_UML_MAX_USERSPACE_ITERATIONS
		if (CONFIG_UML_MAX_USERSPACE_ITERATIONS &&
		    unscheduled_userspace_iterations++ >
		    CONFIG_UML_MAX_USERSPACE_ITERATIONS) {
			tt_extra_sched_jiffies += 1;
			unscheduled_userspace_iterations = 0;
		}
#endif
	}

	/*
	 * mm pinning note (sub-commit #4): UML's outer userspace()
	 * loop runs in the context of the task owning
	 * current->active_mm; KVM_RUN executes synchronously in
	 * that same task's context so the mm can't be freed under
	 * us without the task itself being destroyed first. No
	 * explicit mmget/mmput needed here — matches the seccomp
	 * + ptrace backends which also don't pin. If SMP KVM ever
	 * adds cross-task vCPU dispatch, this invariant needs
	 * revisiting.
	 */

	rc = kvm_enter_guest(regs);
	if (rc < 0)
		panic("um: kvm run_userspace: enter_guest failed (%d)", rc);

	/*
	 * Per-trap shape (audit A1 finalized 2026-04-24 round-3):
	 * each kvm_run_userspace call drives ONE KVM_RUN, ONE
	 * dispatch, then interrupt_end() + return. Matches the
	 * contract ptrace / seccomp already honor (trap_user.c
	 * :254 + :157). An earlier revision kept a for (;;) inner
	 * loop that batched SYSCALL + PF "continuation" traps
	 * across multiple KVM_RUN iterations — correct for the
	 * sysretq/iretq completion but silently delayed
	 * interrupt_end() across syscall/fault bursts, up to the
	 * next host timer interrupt (~10ms). The fix for SYSCALL
	 * is kvm_decode_syscall stashing HOST_IP = HOST_CX (the
	 * user's post-SYSCALL RIP); the next kvm_enter_guest's
	 * bootstrap IRETQ dance resumes there. The fix for PF
	 * extracts user RIP + RSP from the IDT-pushed iretq
	 * frame on the IST stack and does the same re-entry
	 * (see the PF case below).
	 *
	 * No scheduler-drift concern: with per-trap exit,
	 * interrupt_end() runs once per trap, scheduling can
	 * happen inside it, and the next kvm_run_userspace
	 * invocation rebuilds the vCPU from scratch — so a
	 * task that scheduled away and comes back sees a fresh
	 * kvm_enter_guest before the next KVM_RUN.
	 *
	 * Cost: ~5 extra ioctls per non-gadget syscall vs the
	 * old inner-loop model (KVM_GET/SET_SREGS + KVM_SET_
	 * REGS + KVM_SET_MSRS in each kvm_enter_guest).
	 * Borne only by non-gadget paths; gadget-handled
	 * syscalls never VMEXIT so they're unaffected.
	 */
	{
		struct kvm_sregs exit_sregs;
		bool sregs_valid = false;

		rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
		if (rc < 0) {
			if (rc == -EINTR)
				goto out_read_regs;
			panic("um: kvm run_userspace: KVM_RUN failed (%d)", rc);
		}

		/*
		 * Perf-lever #2: when KVM_SYNC_X86_REGS is live, KVM
		 * already populated run->s.regs.regs on exit (we set
		 * kvm_valid_regs before the last entry). Skip the
		 * KVM_GET_REGS ioctl.
		 */
		if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_REGS) {
			kregs = run->s.regs.regs;
		} else {
			rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
					      (unsigned long)&kregs);
			if (rc < 0)
				panic("um: kvm run_userspace: KVM_GET_REGS failed (%d)",
				      rc);
		}
		kvm_regs_to_uml_regs(regs, &kregs);

		/*
		 * Audit finding A2 (memo D70): derive is_user from
		 * the observed CPL at VMEXIT instead of hardcoding
		 * it per-case. CPL = cs.selector & 3 (lowest 2
		 * bits of the ring-3 CS selector are the RPL, which
		 * equals CPL for any user-mode selector). seccomp /
		 * ptrace backends only trap from ring-3 so they
		 * always set is_user = 1, but the KVM backend can
		 * also VMEXIT mid-ring-0 (e.g. MMIO fault while
		 * executing the bootstrap #PF handler or an
		 * exception decoder). Misclassifying a ring-0 exit
		 * as is_user = 1 sends the fault through
		 * arch/um/kernel/trap.c's user-signal path at
		 * trap.c:321 instead of the panic path at
		 * trap.c:292 — guest kernel code that faults would
		 * silently signal a phantom user task instead of
		 * panicking the host kernel, making KVM crash
		 * diagnostics untrustworthy.
		 *
		 * Read SREGS best-effort: a failure here isn't
		 * fatal because the per-case defaults below are
		 * correct for their specific exit reasons (SYSCALL
		 * is always CPL=3, the #PF handler entry is always
		 * from CPL=3). The sregs_valid flag tells the MMIO
		 * case (the one that actually needs runtime CPL)
		 * whether to trust the fresh read or fall back.
		 *
		 * Review-01 P1 #5: prefer the synced view via
		 * KVM_SYNC_X86_SREGS when the cap is available — KVM
		 * already populated run->s.regs.sregs on this exit
		 * because we set kvm_valid_regs |= KVM_SYNC_X86_SREGS
		 * before the entry. Saves one ioctl per VMEXIT on
		 * gadget-fallback / syscall / #PF / MMIO paths. Only
		 * fall back to the explicit ioctl when the cap isn't
		 * exposed by this host KVM.
		 */
		if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_SREGS) {
			exit_sregs = run->s.regs.sregs;
			sregs_valid = true;
			regs->is_user = (exit_sregs.cs.selector & 3) != 0;
		} else if (os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
					    (unsigned long)&exit_sregs) >= 0) {
			sregs_valid = true;
			regs->is_user = (exit_sregs.cs.selector & 3) != 0;
		}

		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			if (run->io.port == UM_KVM_SYSCALL_PORT) {
				kvm_decode_syscall(regs, &kregs, vcpu_fd);
				/*
				 * Audit A1 (2026-04-24 round-3):
				 * exit to interrupt_end() per trap, not per
				 * trap-burst. kvm_decode_syscall has already
				 * stashed the user's continuation RIP into
				 * regs->gp[HOST_IP] (= HOST_CX, the post-
				 * SYSCALL user RIP). The next kvm_run_
				 * userspace iteration rebuilds the vCPU from
				 * regs via kvm_enter_guest's bootstrap
				 * SYSRETQ dance, resuming ring-3 at that
				 * RIP. Matches ptrace / seccomp's per-trap
				 * interrupt_end contract.
				 */
				goto out_read_regs;
			}
			if (run->io.port == UM_KVM_SYSRETQ_PORT) {
				/*
				 * Phase III Lift #1b-style ring-3 fallback
				 * emit. Not a normal production flow;
				 * surfaces as a panic so a confused guest
				 * state is caught loudly rather than
				 * silently consumed.
				 */
				panic("um: kvm run_userspace: unexpected ring-3 port 0xf5 exit\n");
			}
			if (run->io.port == UM_KVM_PF_PORT) {
				/*
				 * Sub-commit #5b: guest-side #PF handler
				 * trapped in. Read CR2, touch the page to
				 * force UML fault-in, refresh shadow PT.
				 * If the touch fails (cr2 outside any vma,
				 * NULL-deref, etc.) dispatch through
				 * sig_info[SIGSEGV] — same SIGSEGV path the
				 * MMIO decode uses. That either fixes-up
				 * via on-demand vma expansion OR signals
				 * the guest task so the loop doesn't spin
				 * on a permanently-unresolvable fault.
				 */
				struct kvm_sregs dump_sregs;
				unsigned long cr2;
				bool touched = false;
				u64 fault_rip;
				u64 fault_error_code;
				bool fault_was_write;
				int gadget_nr;
				unsigned long ist_off;
				u8 *ist;

				/*
				 * Experiment #2: prefer the synced view if
				 * KVM published it via KVM_CAP_SYNC_REGS.
				 * Saves a KVM_GET_SREGS ioctl on the hot
				 * fault path (~50-100ns).
				 */
				if (kvm_backend_ctx()->sync_regs_caps &
				    KVM_SYNC_X86_SREGS) {
					cr2 = run->s.regs.sregs.cr2;
				} else {
					if (os_ioctl_generic(vcpu_fd,
							     KVM_GET_SREGS,
							     (unsigned long)&dump_sregs) < 0) {
						panic("um: kvm PF handler: KVM_GET_SREGS failed");
					}
					cr2 = dump_sregs.cr2;
				}

				/*
				 * Audit round-5 F7/1: classify the fault
				 * as ring-0 gadget-mid-store vs ring-3 user
				 * instruction. The faulting RIP lives in the
				 * IDT-pushed iretq frame on the IST stack at
				 * offset +8. If the RIP lies within a gadget
				 * handler that writes to user memory
				 * (clock_gettime / time / getcpu), convert
				 * the #PF into a SYSCALL fallback so the
				 * user-visible result is -EFAULT per POSIX
				 * semantics rather than SIGSEGV.
				 *
				 * Post F5-followon (#230) the IST stack is
				 * its own page mapped at GVA bootstrap_va +
				 * 3*PAGE_SIZE, backed by kvm_bootstrap_page_
				 * stack on the host side. RSP at #PF entry
				 * lies in [bootstrap_va + 3*PAGE_SIZE,
				 * bootstrap_va + KVM_BOOTSTRAP_STACK_TOP);
				 * subtract the page-2 base to get the host
				 * offset.
				 */
				ist_off = (unsigned long)(kregs.rsp -
							  (kvm_bootstrap_va +
							   3 * PAGE_SIZE));
				if (ist_off >= PAGE_SIZE) {
					pr_warn_ratelimited("um: kvm: PF IST out of range\n");
					fatal_sigsegv();
				}
				ist = (u8 *)kvm_bootstrap_page_stack + ist_off;
				/*
				 * Audit round-6 G3: read the CPU-pushed
				 * #PF error code from IST+0 instead of
				 * hardcoding faultinfo->error_code = 4
				 * later. Bits we care about (Intel SDM
				 * vol 3 §6.15):
				 *   bit 0  P  — 1 if protection violation
				 *   bit 1  W  — 1 if write
				 *   bit 2  U  — 1 if user-mode access
				 *   bit 4  ID — 1 if instruction fetch
				 * UML's faultinfo expects the same bits
				 * (mirrors the Linux x86 error_code
				 * convention).
				 */
				fault_error_code = *(u64 *)(ist + 0);
				fault_was_write  = (fault_error_code >> 1) & 1;
				fault_rip = *(u64 *)(ist + 8);

				pr_info_ratelimited("um: kvm #PF: cr2=0x%lx rip=0x%llx ec=0x%llx; fault-in + shadow refill\n",
						    cr2,
						    (unsigned long long)fault_rip,
						    (unsigned long long)fault_error_code);

				gadget_nr = kvm_gadget_fault_nr(fault_rip);
				if (gadget_nr >= 0) {
					/*
					 * F7/1 fallback: a gadget tried to
					 * write to (%rdi/%rsi) and faulted in
					 * ring-0. Restart the SYSCALL through
					 * handle_syscall, which then uses the
					 * standard copy_to_user path:
					 *
					 *   - If cr2 was a lazy-but-valid user
					 *     VA, UML's own fault path fills
					 *     the PTE on access and the syscall
					 *     completes normally (RAX = 0 for
					 *     clock/getcpu, seconds for time).
					 *   - If cr2 was genuinely bad, copy_
					 *     to_user returns -EFAULT and the
					 *     syscall return value propagates
					 *     back to the guest as -EFAULT.
					 *
					 * SYSCALL-entry regs to restore:
					 *   RAX = gadget_nr (NR; F7/2 preserves
					 *         it across clock but time /
					 *         getcpu handlers clobber it,
					 *         so we restore from the
					 *         range-derived NR here).
					 *   RCX = user RIP (SYSCALL saved it
					 *         there at dispatch time; gadget
					 *         handlers preserve RCX for the
					 *         sysretq tail).
					 *   R11 = user RFLAGS (SYSCALL saved;
					 *         gadget handlers preserve).
					 *   RSP = IST+32 (user RSP at fault
					 *         time; preserved across gadget
					 *         entry).
					 *
					 * Args RDI / RSI (the output pointers
					 * that would-have-faulted) pass through
					 * unchanged — the gadget only READS
					 * them for the address computation,
					 * never writes them.
					 */
					regs->gp[HOST_AX]     = gadget_nr;
					regs->gp[HOST_IP]     = regs->gp[HOST_CX];
					regs->gp[HOST_SP]     = *(u64 *)(ist + 32);
					regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11];
					regs->is_user = 1;
					kvm_decode_syscall(regs, &kregs, vcpu_fd);
					goto out_read_regs;
				}

				if (cr2 && current->active_mm &&
				    current->active_mm->pgd) {
					struct mm_struct *m2 =
						current->active_mm;
					int code_out = 0;
					int hpf_rc;

					/*
					 * Task #238 — direct handle_page_fault
					 * call.
					 *
					 * Prior versions did `copy_from_user(&
					 * probe, cr2, 1)` (and copy_to_user for
					 * write faults) to indirectly trigger
					 * UML's fault path. That worked when
					 * cr2 was a user VA already in UML's
					 * vmas, but had three problems:
					 *
					 *   1. It depended on copy_*_user's
					 *      side-effect rather than calling
					 *      the real fault path. Bypassed
					 *      the proper VM_FAULT_RETRY
					 *      machinery.
					 *
					 *   2. For first-time access to a page
					 *      that's in UML's vma but not yet
					 *      in the logical pgd (e.g. binary
					 *      text on first instruction
					 *      fetch), copy_from_user could
					 *      fail because the kernel-mode
					 *      copy stub also requires a
					 *      mapping it can't always create.
					 *
					 *   3. It was paired with the eager
					 *      kvm_touch_all_user_vmas walk
					 *      that ran on every kvm_enter_
					 *      guest just to mask case (2),
					 *      costing O(mm-size) per syscall.
					 *
					 * Calling handle_page_fault directly
					 * gives us the same semantics UML's
					 * SIGSEGV trap path uses (arch/um/
					 * kernel/trap.c:377): it locks the mm,
					 * finds the vma, validates VM_WRITE /
					 * VM_READ / VM_EXEC, calls handle_mm_
					 * fault with the right flags, and
					 * returns 0 on success or -EFAULT /
					 * -EACCES / -ENOMEM on failure. The
					 * is_user=1 + is_write=fault_was_write
					 * pass-through makes the fault
					 * accountable as a real user-mode
					 * fault, which gates the retry +
					 * killable handling correctly.
					 */
					hpf_rc = handle_page_fault(cr2,
								   fault_rip,
								   fault_was_write,
								   1,
								   &code_out);
					if (hpf_rc == 0)
						touched = true;
					(void)kvm_shadow_fill_from_uml_pgd(m2->pgd);
				}

				/*
				 * If the touch couldn't reach cr2 (NULL
				 * deref, unmapped-beyond-vma, protection
				 * violation), treat it as a real fault:
				 * populate faultinfo + dispatch SIGSEGV.
				 * Same code path KVM_EXIT_MMIO uses above.
				 * After SIGSEGV dispatch we return to the
				 * outer userspace() loop rather than re-
				 * entering KVM_RUN — the faulting insn
				 * would just fault again + spin us. UML's
				 * signal-delivery + scheduler machinery in
				 * the outer loop notices the queued SIGSEGV
				 * and either terminates the task or handles
				 * it; on the next run_userspace iteration
				 * regs reflect the post-signal state.
				 */
				if (!touched) {
					struct faultinfo *fi =
						UPT_FAULTINFO(regs);

					/*
					 * Audit round-6 G3: propagate the
					 * actual CPU-pushed error code (W /
					 * U / I/D bits) instead of hardcoding
					 * `4` (user-mode-only). Otherwise UML's
					 * trap.c can't distinguish a
					 * read-from-RO from a write-to-RO from
					 * an instruction-fetch fault; SIGSEGV
					 * delivery to the guest task lacks the
					 * info debuggers / stress-testers need
					 * to reason about the fault.
					 */
					fi->trap_no    = 14;
					fi->error_code = (u32)fault_error_code;
					fi->cr2        = cr2;
					/*
					 * is_user MUST be set BEFORE
					 * sig_info[SIGSEGV] dispatches —
					 * UML's segv_handler / segv() reads
					 * UPT_IS_USER(regs) and panics with
					 * "Kernel tried to access user
					 * memory" if it sees is_user=0
					 * with a user-range address.
					 *
					 * At KVM_EXIT_IO from the IDT[14]
					 * #PF handler, our prior CPL-from-
					 * SREGS read returns CPL=0 (we're
					 * in the in-guest ring-0 handler
					 * by then), so regs->is_user came
					 * back 0. But the FAULTING access
					 * was at CPL=3 — the handler is
					 * just delivering on the user
					 * fault's behalf. Tag the regs
					 * accordingly before SIGSEGV
					 * dispatch.
					 */
					regs->is_user = 1;
					(*sig_info[SIGSEGV])(SIGSEGV, NULL,
							     regs, NULL);
					/*
					 * Drop out to interrupt_end so the
					 * queued SIGSEGV drains before the
					 * outer userspace() loop re-enters.
					 */
					goto out_read_regs;
				}

				/*
				 * Audit A1 round-3: extract user
				 * continuation regs from the IDT-pushed
				 * iretq frame on the IST stack, stash
				 * them in `regs`, and exit to
				 * interrupt_end() per trap (matching
				 * the SYSCALL path above and ptrace /
				 * seccomp's contract). Prior revisions
				 * advanced kregs.rip+2 + KVM_SET_REGS
				 * and `continue`d the for-loop to let
				 * the handler's `iretq` run — that
				 * worked but silently batched PF
				 * events across the inner loop, which
				 * delayed resched + signal drain.
				 *
				 * IST frame layout at `out %al, $0xfb`
				 * VMEXIT (kregs.rsp points at offset
				 * 0):
				 *   +0:  error_code
				 *   +8:  RIP      (user's faulting VA)
				 *   +16: CS
				 *   +24: RFLAGS   (user's, at fault)
				 *   +32: RSP      (user's stack ptr)
				 *   +40: SS
				 *
				 * Read via the bootstrap page's kernel-
				 * VA alias — the IST stack lives in
				 * the bootstrap page (RX-mapped for the
				 * guest, kernel-VA-directly-accessible
				 * for the host). Next kvm_enter_guest's
				 * bootstrap IRETQ dance (post-#272)
				 * resumes ring-3 at HOST_IP (= user RIP),
				 * with HOST_SP restored to user RSP and
				 * HOST_EFLAGS restored to user RFLAGS
				 * (audit round-4
				 * F2). Previously user RFLAGS was
				 * discarded and approximated by a
				 * hardcoded 0x3202 at re-entry time,
				 * which silently clobbered arithmetic /
				 * direction / trap flags across a
				 * recoverable fault — a flag-sensitive
				 * retry (e.g. rep movs after std) could
				 * then execute with the wrong DF. The
				 * CPU-saved IST frame is the
				 * authoritative source for all three
				 * user-visible fields; we read all
				 * three.
				 */
				{
					unsigned long off =
						(unsigned long)(kregs.rsp -
								(kvm_bootstrap_va +
								 3 * PAGE_SIZE));
					u8 *ist;
					u64 user_rip, user_rsp, user_rflags;

					if (off >= PAGE_SIZE) {
						pr_warn_ratelimited("um: kvm: PF IST out of range\n");
						fatal_sigsegv();
					}
					ist = (u8 *)kvm_bootstrap_page_stack + off;
					user_rip     = *(u64 *)(ist + 8);
					user_rflags  = *(u64 *)(ist + 24);
					user_rsp     = *(u64 *)(ist + 32);
					regs->gp[HOST_IP]     = user_rip;
					regs->gp[HOST_SP]     = user_rsp;
					regs->gp[HOST_EFLAGS] = user_rflags;
				}
				goto out_read_regs;
			}
			if (run->io.port == UM_KVM_GP_PORT) {
				/*
				 * Audit round-7 P1 follow-on (task #272
				 * IRETQ hardening): in-guest IDT[13] (#GP)
				 * handler fired. Most likely cause: the
				 * bootstrap IRETQ tried to pop a non-
				 * canonical user RIP or user RSP from the
				 * iretq frame, raising #GP during the
				 * cross-CPL transition. Other possibilities
				 * include the user task hitting a privileged
				 * instruction (CLI/STI/HLT/etc) or a
				 * reserved-RFLAGS-bit violation.
				 *
				 * Treat the user task as faulted: build a
				 * faultinfo, dispatch SIGSEGV via the
				 * standard sig_info path. The user task gets
				 * killed cleanly; the host kernel doesn't
				 * panic. is_user=1 because the offending
				 * state is the user's RIP/RSP/RFLAGS, even
				 * though the #GP fired during ring-0 IRETQ.
				 *
				 * IST frame layout for #GP: same as #PF
				 * (CPU-pushed error_code at +0, RIP at +8,
				 * CS +16, RFLAGS +24, RSP +32, SS +40). We
				 * extract them for the diagnostic.
				 */
				struct faultinfo *fi = UPT_FAULTINFO(regs);
				unsigned long ist_off;
				u64 gp_user_rip = 0, gp_user_rsp = 0;
				u64 gp_user_rflags = 0;
				u64 gp_error_code = 0;

				ist_off = (unsigned long)(kregs.rsp -
							  (kvm_bootstrap_va +
							   3 * PAGE_SIZE));
				if (ist_off < PAGE_SIZE) {
					u8 *ist = (u8 *)kvm_bootstrap_page_stack
						+ ist_off;
					gp_error_code  = *(u64 *)(ist + 0);
					gp_user_rip    = *(u64 *)(ist + 8);
					gp_user_rflags = *(u64 *)(ist + 24);
					gp_user_rsp    = *(u64 *)(ist + 32);
				}
				pr_warn_ratelimited("um: kvm #GP: ec=0x%llx rip=0x%llx rsp=0x%llx rflags=0x%llx (delivering SIGSEGV)\n",
						    (unsigned long long)gp_error_code,
						    (unsigned long long)gp_user_rip,
						    (unsigned long long)gp_user_rsp,
						    (unsigned long long)gp_user_rflags);
				fi->trap_no    = 13;
				fi->error_code = (u32)gp_error_code;
				fi->cr2        = 0;
				regs->is_user = 1;
				regs->gp[HOST_IP]     = gp_user_rip;
				regs->gp[HOST_SP]     = gp_user_rsp;
				regs->gp[HOST_EFLAGS] = gp_user_rflags;
				(*sig_info[SIGSEGV])(SIGSEGV, NULL,
						     regs, NULL);
				goto out_read_regs;
			}
			if (run->io.port == UM_KVM_DF_PORT) {
				/*
				 * Task #269: in-guest IDT[8] (#DF) handler
				 * fired. The CPU pushed the iretq frame onto
				 * IST[1] (the same stack as #PF), with the
				 * RIP saved being the RIP of the original
				 * fault delivery — i.e. the instruction that
				 * was about to fault when its #PF handler
				 * couldn't be reached.
				 *
				 * Read CR2 + the IST frame for diagnostics,
				 * dump the cause-of-cascade info, then
				 * panic the host UML kernel because #DF is
				 * unrecoverable. Without this case, a #DF
				 * during #PF delivery would have triple-
				 * faulted and surfaced as the opaque
				 * KVM_EXIT_SHUTDOWN with no useful info.
				 *
				 * Don't try to deliver SIGSEGV to the user
				 * task: a #DF during #PF means the IDT
				 * dispatch itself is broken, which is a
				 * host-kernel bug, not a user fault.
				 */
				struct kvm_sregs df_sregs;
				unsigned long df_cr2 = 0;
				u64 df_user_rip = 0, df_user_rsp = 0;
				u64 df_user_rflags = 0;
				unsigned long ist_off;

				if (os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
						     (unsigned long)&df_sregs) >= 0)
					df_cr2 = df_sregs.cr2;

				ist_off = (unsigned long)(kregs.rsp -
							  (kvm_bootstrap_va +
							   3 * PAGE_SIZE));
				if (ist_off < PAGE_SIZE) {
					u8 *ist = (u8 *)kvm_bootstrap_page_stack
						+ ist_off;
					df_user_rip    = *(u64 *)(ist + 8);
					df_user_rflags = *(u64 *)(ist + 24);
					df_user_rsp    = *(u64 *)(ist + 32);
				}
				panic("um: kvm #DF: cr2=0x%lx fault_rip=0x%llx fault_rsp=0x%llx fault_rflags=0x%llx kregs_rsp=0x%llx (double-fault: #PF handler unreachable; check shadow PT for IST stack + IDT page)\n",
				      df_cr2,
				      (unsigned long long)df_user_rip,
				      (unsigned long long)df_user_rsp,
				      (unsigned long long)df_user_rflags,
				      (unsigned long long)kregs.rsp);
			}
			panic("um: kvm run_userspace: KVM_EXIT_IO port=0x%x (unknown)",
			      run->io.port);

		case KVM_EXIT_HLT:
			/*
			 * Guest HLT. Jump to out_read_regs so
			 * interrupt_end() drains pending resched +
			 * signals before the next kvm_run_userspace call
			 * re-enters the guest. A plain `break;` would
			 * only exit the switch (not the for-loop) and
			 * silently loop back into KVM_RUN, skipping
			 * interrupt_end entirely — that was the A1
			 * partial-fix defect flagged in the 2026-04-24
			 * audit round. is_user was set from the observed
			 * CPL above (A2); leave it. HOST_IP points past
			 * the HLT.
			 */
			goto out_read_regs;

		case KVM_EXIT_INTR:
			goto out_read_regs;

		case KVM_EXIT_MMIO: {
			/*
			 * EPT-level fault: the guest walked its pgd to
			 * a valid gpa, but no memslot backs that gpa.
			 * Route through UML's common fault handler so
			 * mmap-on-demand / swap-in / SIGSEGV delivery
			 * all use the same code path as the ptrace +
			 * seccomp backends.
			 *
			 * Faultinfo mapping (memo 08 sub-commit #3):
			 *   trap_no = 14  (X86 #PF — SEGV_IS_FIXABLE)
			 *   error_code = bit 1 set if write
			 *                bit 2 set if user-mode access
			 *                (regs->is_user already true
			 *                 once the inner loop ran once)
			 *   cr2 = gpa + uml_physmem — the host-VA
			 *         equivalent under the Policy A
			 *         identity memslot. In UML this is
			 *         the address `segv` interprets as the
			 *         faulting-VA; vma lookup uses it.
			 *
			 * Phase III Lift #1d's harness decode template
			 * landed the same shape; this is that logic
			 * lifted into production with regs + fault
			 * handler hooked up.
			 */
			struct faultinfo *fi = UPT_FAULTINFO(regs);
			bool is_user;

			/*
			 * Audit A2 follow-up: pick is_user for the
			 * fault error code from the live CPL read at
			 * the top of the loop when we have it
			 * (sregs_valid), not from a stale `regs->
			 * is_user` that a prior exit may have left
			 * behind. A KVM_GET_SREGS failure (sregs_
			 * valid = false, rare) falls back to the
			 * pre-fault regs->is_user; harmless for
			 * ring-3 userspace workloads because sregs
			 * rarely fails, but the explicit branch
			 * documents the intent and the fallback
			 * matches every other backend's "always
			 * user-mode" assumption on EPT faults.
			 */
			if (sregs_valid)
				is_user = (exit_sregs.cs.selector & 3) != 0;
			else
				is_user = regs->is_user;

			fi->trap_no    = 14;
			fi->error_code = (run->mmio.is_write ? 2 : 0) |
					 (is_user ? 4 : 0);
			fi->cr2        = (unsigned long)run->mmio.phys_addr +
					 uml_physmem;

			pr_info_ratelimited("um: kvm run_userspace: KVM_EXIT_MMIO gpa=0x%llx cr2=0x%lx len=%u write=%u\n",
					    (unsigned long long)run->mmio.phys_addr,
					    fi->cr2, run->mmio.len, run->mmio.is_write);

			/*
			 * Dispatch through the same sig_info[SIGSEGV]
			 * table-entry seccomp + ptrace use. siginfo is
			 * NULL here — segv_handler only consumes the
			 * faultinfo we just populated.
			 */
			(*sig_info[SIGSEGV])(SIGSEGV, NULL, regs, NULL);

			/*
			 * Re-fill the shadow PT: segv_handler may have
			 * installed a new mapping via mm_map, but the
			 * shadow PT still reflects the pre-fault state.
			 * Walking current->active_mm->pgd again picks up
			 * the new entry. Memo 09 step 3 follow-on:
			 * targeted single-page invalidate is an
			 * optimisation; for MVP the full refill works.
			 */
			if (current->active_mm && current->active_mm->pgd)
				(void)kvm_shadow_fill_from_uml_pgd(
					current->active_mm->pgd);
			/*
			 * MMIO is a clean ring-3 boundary: the SEGV
			 * either got handled (page installed, guest
			 * retries) or got signaled. Drop to
			 * interrupt_end so resched + signals drain
			 * before the outer loop re-enters. Same
			 * for-loop-vs-switch trap as the HLT case;
			 * goto, not break.
			 */
			goto out_read_regs;
		}

		case KVM_EXIT_SHUTDOWN:
		case KVM_EXIT_FAIL_ENTRY:
		case KVM_EXIT_INTERNAL_ERROR:
		case KVM_EXIT_EXCEPTION: {
			struct kvm_sregs dump_sregs;
			int dump_rc;
			u64 guest_cr3 = 0, guest_rip = 0;

			/*
			 * Best-effort state snapshot on the failing
			 * vCPU. Helps diagnose bring-up issues
			 * (triple-fault on first instruction,
			 * unmapped CR3, etc.) from a single failing
			 * boot rather than requiring a kgdb dance.
			 * Unused by normal operation since the
			 * calling path already panics.
			 */
			dump_rc = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
						   (unsigned long)&dump_sregs);
			if (dump_rc >= 0)
				guest_cr3 = dump_sregs.cr3;
			guest_rip = kregs.rip;

			pr_err("um: kvm run_userspace: unrecoverable exit %u (%s)\n",
			       run->exit_reason,
			       kvm_exit_reason_str(run->exit_reason));
			pr_err("um: kvm: guest RIP=0x%llx CR3=0x%llx CS=0x%x CPL=%u is_user=%d\n",
			       (unsigned long long)guest_rip,
			       (unsigned long long)guest_cr3,
			       dump_rc >= 0 ? dump_sregs.cs.selector : 0,
			       dump_rc >= 0 ? dump_sregs.cs.dpl : 0,
			       regs->is_user);
			if (dump_rc >= 0) {
				pr_err("um: kvm: CR0=0x%llx CR4=0x%llx EFER=0x%llx GDTR base=0x%llx limit=0x%x\n",
				       (unsigned long long)dump_sregs.cr0,
				       (unsigned long long)dump_sregs.cr4,
				       (unsigned long long)dump_sregs.efer,
				       (unsigned long long)dump_sregs.gdt.base,
				       dump_sregs.gdt.limit);
			}
			pr_err("um: kvm: bootstrap page va=0x%llx gpa=0x%llx lstar=0x%llx sysret=0x%llx uml_physmem=0x%lx\n",
			       (unsigned long long)kvm_bootstrap_va,
			       (unsigned long long)kvm_bootstrap_gpa,
			       (unsigned long long)(kvm_bootstrap_va +
						    KVM_BOOTSTRAP_LSTAR_OFFSET),
			       (unsigned long long)(kvm_bootstrap_va +
						    KVM_BOOTSTRAP_SYSRET_OFFSET),
			       uml_physmem);
			/*
			 * Walk the guest CR3 for diagnostic VAs so we
			 * can see exactly which mapping is missing on a
			 * triple-fault / SHUTDOWN. Under Policy A
			 * memslot, gpa = hostva - uml_physmem, so each
			 * pgd/pud/pmd/pte physical address in the
			 * tables is a gpa we re-translate via __va() to
			 * a host VA we can safely read. Print the entry
			 * values at each level; a zero entry on the
			 * path means the VA is unmapped in this CR3.
			 *
			 * Audit Finding 5 (2026-04-25): walk THREE
			 * addresses, not just kvm_bootstrap_va — the
			 * actual failing RIP and the cr2 (faulting VA
			 * from the last #PF) are the load-bearing
			 * targets for diagnosing dyn-loader / lazy-fault
			 * cascades.
			 */
			{
				u64 cr3 = guest_cr3 & ~0xfffULL;
				int wi;
				const struct {
					const char *label;
					u64 va;
				} walk_targets[] = {
					{ "bootstrap_va",
					  kvm_bootstrap_va },
					{ "guest_rip",
					  guest_rip },
					{ "cr2",
					  dump_rc >= 0 ?
					  dump_sregs.cr2 : 0 },
				};

				if (!(cr3 && cr3 < physmem_size)) {
					pr_err("um: kvm: CR3=0x%llx out of physmem (size=0x%llx); cannot walk\n",
					       (unsigned long long)cr3,
					       (unsigned long long)physmem_size);
				}
				for (wi = 0; wi < 3 && cr3 && cr3 < physmem_size; wi++) {
					u64 va  = walk_targets[wi].va;
					const char *lbl = walk_targets[wi].label;
					u64 *pgd_va, *pud_va, *pmd_va, *pte_va;
					u64 pgde = 0, pude = 0, pmde = 0, pte = 0;
					unsigned int pgd_i, pud_i, pmd_i, pte_i;

					if (!va)
						continue;
					pgd_i = (va >> 39) & 0x1ff;
					pud_i = (va >> 30) & 0x1ff;
					pmd_i = (va >> 21) & 0x1ff;
					pte_i = (va >> 12) & 0x1ff;

					pgd_va = (u64 *)__va(cr3);
					pgde = pgd_va[pgd_i];
					pr_err("um: kvm: walk %s va=0x%llx pgd[%u]=0x%llx\n",
					       lbl,
					       (unsigned long long)va,
					       pgd_i,
					       (unsigned long long)pgde);

					if ((pgde & 1) && !(pgde & (1ULL << 7))) {
						u64 pud_pa = pgde & 0x000ffffffffff000ULL;

						pud_va = (u64 *)__va(pud_pa);
						pude = pud_va[pud_i];
						pr_err("um: kvm: walk %s pud[%u]@0x%llx = 0x%llx\n",
						       lbl, pud_i,
						       (unsigned long long)pud_pa,
						       (unsigned long long)pude);
					}
					if ((pude & 1) && !(pude & (1ULL << 7))) {
						u64 pmd_pa = pude & 0x000ffffffffff000ULL;

						pmd_va = (u64 *)__va(pmd_pa);
						pmde = pmd_va[pmd_i];
						pr_err("um: kvm: walk %s pmd[%u]@0x%llx = 0x%llx\n",
						       lbl, pmd_i,
						       (unsigned long long)pmd_pa,
						       (unsigned long long)pmde);
					}
					if ((pmde & 1) && !(pmde & (1ULL << 7))) {
						u64 pte_pa = pmde & 0x000ffffffffff000ULL;

						pte_va = (u64 *)__va(pte_pa);
						pte = pte_va[pte_i];
						pr_err("um: kvm: walk %s pte[%u]@0x%llx = 0x%llx%s\n",
						       lbl, pte_i,
						       (unsigned long long)pte_pa,
						       (unsigned long long)pte,
						       (pte & 1) ? "" : " (NOT PRESENT)");
					} else if ((pmde & 1) && (pmde & (1ULL << 7))) {
						pr_err("um: kvm: walk %s 2MB huge page at pmd level\n",
						       lbl);
					}
				}
			}
			/*
			 * Audit Finding 5 follow-on: dump KVM_GET_VCPU_
			 * EVENTS — exposes pending exception state. Tells
			 * us if a #PF / #DF was queued at the moment of
			 * SHUTDOWN, which discriminates "guest faulted
			 * and we never delivered" from "guest delivered
			 * but cascaded".
			 */
			{
				struct kvm_vcpu_events ev;

				if (os_ioctl_generic(vcpu_fd,
						     KVM_GET_VCPU_EVENTS,
						     (unsigned long)&ev) >= 0) {
					pr_err("um: kvm: vcpu_events exc.injected=%u nr=%u has_err=%u err=0x%x interrupt.injected=%u nr=0x%x\n",
					       ev.exception.injected,
					       ev.exception.nr,
					       ev.exception.has_error_code,
					       ev.exception.error_code,
					       ev.interrupt.injected,
					       ev.interrupt.nr);
				}
			}
			if (run->exit_reason == KVM_EXIT_FAIL_ENTRY) {
				u64 hw = run->fail_entry.hardware_entry_failure_reason;

				pr_err("um: kvm: FAIL_ENTRY hw_reason=0x%llx\n",
				       (unsigned long long)hw);
			}
			/*
			 * Dump 16 bytes of guest instruction bytes at the
			 * failing RIP. RIP is a user VA; copy_from_user
			 * reads it. Useful for deciding whether the fault
			 * was at a syscall instruction, a mov-from-memory,
			 * or a computed jump into nothing.
			 */
			{
				unsigned char insn_bytes[16] = { 0 };
				long cr_rc;

				cr_rc = copy_from_user(insn_bytes,
						       (void __user *)(unsigned long)guest_rip,
						       sizeof(insn_bytes));
				if (cr_rc == 0) {
					pr_err("um: kvm: guest insn @ RIP=0x%llx: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					       (unsigned long long)guest_rip,
					       insn_bytes[0], insn_bytes[1],
					       insn_bytes[2], insn_bytes[3],
					       insn_bytes[4], insn_bytes[5],
					       insn_bytes[6], insn_bytes[7],
					       insn_bytes[8], insn_bytes[9],
					       insn_bytes[10], insn_bytes[11],
					       insn_bytes[12], insn_bytes[13],
					       insn_bytes[14], insn_bytes[15]);
				} else {
					pr_err("um: kvm: guest insn @ RIP=0x%llx unreadable (copy_from_user=%ld)\n",
					       (unsigned long long)guest_rip, cr_rc);
				}
			}
			if (run->exit_reason == KVM_EXIT_INTERNAL_ERROR)
				pr_err("um: kvm: INTERNAL_ERROR suberror=%u\n",
				       run->internal.suberror);
			panic("um: kvm run_userspace: unrecoverable exit %u (%s)",
			      run->exit_reason,
			      kvm_exit_reason_str(run->exit_reason));
		}

		default:
			panic("um: kvm run_userspace: unknown exit reason %u (%s)",
			      run->exit_reason,
			      kvm_exit_reason_str(run->exit_reason));
		}
	}

out_read_regs:
	/*
	 * Host-side INTR / EINTR path: kregs may be stale (KVM_RUN
	 * returned before a clean KVM_GET_REGS ran). Pull fresh
	 * guest state so interrupt_end() + the next kvm_enter_guest
	 * operate on accurate regs.
	 */
	if (rc == -EINTR) {
		struct kvm_sregs sregs;
		bool sregs_ok;
		bool in_kernel = false;

		if (kvm_backend_ctx()->sync_regs_caps & KVM_SYNC_X86_REGS) {
			kregs = run->s.regs.regs;
			rc = 0;
		} else {
			rc = os_ioctl_generic(vcpu_fd, KVM_GET_REGS,
					      (unsigned long)&kregs);
		}
		/*
		 * Audit A2: derive is_user from CPL, so a host-signal
		 * interrupt mid-ring-0 (e.g. inside the LSTAR trampoline,
		 * the bootstrap IRETQ gadget, or the #PF handler) doesn't
		 * mask as a user-mode exit. Best-effort — if KVM_GET_SREGS
		 * fails, fall back to user-mode since host signals during
		 * normal workload almost always fire while the guest is
		 * in ring-3.
		 */
		sregs_ok = os_ioctl_generic(vcpu_fd, KVM_GET_SREGS,
					    (unsigned long)&sregs) >= 0;
		if (sregs_ok) {
			regs->is_user = (sregs.cs.selector & 3) != 0;
			in_kernel = !regs->is_user;
		} else {
			regs->is_user = 1;
		}
		/*
		 * Task #272: when the EINTR fired with the guest at CPL=0,
		 * we're mid-bootstrap-transition (IRETQ gadget popping the
		 * iretq frame, LSTAR trampoline mid-`out`, or the #PF
		 * handler returning). kregs.rip in that state is a kernel
		 * VA inside the bootstrap page (e.g. 0x60ade4d0 = IRETQ
		 * gadget), NOT the user's intended resume RIP. Folding it
		 * into regs->gp[HOST_IP] would make the next kvm_enter_
		 * guest re-enter with frame[0] = bootstrap kernel address
		 * → ring-3 fetch fault on a US=0 page (ec=0x15).
		 *
		 * The user's actual continuation RIP is whatever
		 * regs->gp[HOST_IP] held before kvm_enter_guest installed
		 * the bootstrap shape — and the user's GPRs likewise
		 * weren't touched mid-transition. Skip the marshal back.
		 *
		 * For CPL=3 EINTR (real mid-user signal), the kregs DO
		 * reflect user state and we want to preserve them through
		 * to the next entry; do the marshal.
		 */
		if (rc >= 0 && !in_kernel)
			kvm_regs_to_uml_regs(regs, &kregs);
	}

	/*
	 * Drain resched + pending signals + resume work, matching
	 * the contract seccomp/ptrace backends honor (audit A1 /
	 * decisions-log D70). Each kvm_run_userspace call = one
	 * trap + one interrupt_end, same shape as
	 * seccomp_run_userspace line 157 +
	 * ptrace_run_userspace line 254.
	 */
	interrupt_end();
}

#else /* !CONFIG_UM_BACKEND_KVM_INTEGRATED */

void kvm_run_userspace(struct uml_pt_regs *regs)
{
	int vcpu_fd = kvm_backend_vcpu0_fd();
	struct kvm_run *run = kvm_backend_ctx()->run0;
	int rc;

	(void)regs;

	if (vcpu_fd < 0 || !run) {
		panic("um: kvm run_userspace: vCPU not initialized (vcpu_fd=%d run=%p)",
		      vcpu_fd, run);
	}

	/*
	 * First call registers the memslot now that arch_setup() has
	 * populated uml_physmem / physmem_size. Subsequent calls are
	 * idempotent no-ops. If registration fails we still attempt
	 * KVM_RUN so the panic below reports the real KVM exit reason
	 * rather than hiding behind a memslot-unavailable message.
	 */
	rc = kvm_ensure_memslot();
	if (rc < 0)
		pr_warn_once("um: kvm run_userspace: memslot registration failed (%d)\n",
			     rc);

	rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);

	/*
	 * D-04a scaffold: no SREGS / CR3 / LSTAR yet, so KVM_RUN
	 * returns either an error or a fail-entry / shutdown exit
	 * immediately. Neither is "correct" guest behavior; both are
	 * expected until D-04b lands. Panic with the exit_reason so
	 * the failure mode is diagnosable in dmesg.
	 */
	panic("um: kvm run_userspace: ioctl rc=%d, exit_reason=%u (%s) — D-04b SREGS/CR3 setup pending",
	      rc, run->exit_reason, kvm_exit_reason_str(run->exit_reason));
}

#endif /* CONFIG_UM_BACKEND_KVM_INTEGRATED */
