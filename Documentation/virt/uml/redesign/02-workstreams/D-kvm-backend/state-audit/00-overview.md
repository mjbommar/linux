# UML KVM v2 — State Ownership Audit

**Created:** 2026-05-01
**Tip at creation:** `e5f231656ead`
**Status:** SCAFFOLDING — to be filled in by structured audit

## Purpose

The mt-mini SMP T=8 stress flake (~50% PASS, all symptoms consistent
with cross-task register/stack contamination) has resisted multiple
fix attempts:

1. Naive cross-vCPU TLB IPI broadcast — regressed T=4 to 30%
2. cmpxchg dedup'd kick — regressed T=4 to 77%
3. v1-pattern targeted kick (gen + mm narrowing + cmpxchg) — regressed T=4 to 70%
4. preempt_enable-before-dispatch — no improvement
5. Per-vCPU dispatch mutex — hard hang (scheduler deadlock)

Three independent investigations (2× opus subagents + codex CLI
gpt-5.5 xhigh) converged on the *symptom* (cross-task contamination
via shared per-host-CPU vCPU pool resources) but no patch has hit the
*mechanism*.

This audit takes a different approach: **enumerate every piece of
guest/host state v2 manages, every operation that touches it, and
build an ownership matrix.** Every cell of the matrix must answer:
"who is responsible for save/restore at this transition, and is the
v2 implementation correct?"

The hypothesis is that mechanical enumeration will surface the bug
that ad-hoc fix attempts missed.

## Scope

x86-64 KVM-v2 only. Not seccomp, not v1-archive (those are reference
points but not under audit).

## Methodology

### Layer 1: state inventory ([01-state-inventory.md](01-state-inventory.md))

Enumerate every piece of state v2 must manage. Categories:

- Guest GP registers (RAX–R15, RIP, RFLAGS) — 18 items
- Guest control regs (CR0, CR2, CR3, CR4, CR8) — 5 items
- Guest segment regs (CS, DS, ES, FS, GS, SS, TR, LDT — selector + base + limit + attr) — 32 items
- Guest MSR-anchored bases (FS_BASE, GS_BASE, KERNEL_GS_BASE) — 3 items
- Programmed MSRs (LSTAR, STAR, FMASK, EFER, etc.) — ~20 items
- Debug registers (DR0–DR3, DR6, DR7) — 6 items
- FPU/XSAVE state (x87 + XMM/YMM/ZMM + MXCSR + XCR0) — feature-mask dependent
- Pending event state (exception, interrupt, NMI, SIPI, SMI) — 5 fields
- Descriptor tables (IDT, GDT, LDT, TR base+limit) — 5 pairs
- v2-private per-task state (arch_thread.kvm_v2.{fpu, iotrap_fpu, ist_frame, saved_cr2, last_seen_tlb_gen, current_mm}) — 7 items
- v2-private per-vCPU state (struct kvm_v2_vcpu.{vcpu_fd, kvm_run, ist_stack_kva, tss_kva, kick_pending, ...}) — ~10 items
- v2-private per-mm state (mm_context.{tlb_gen, sync_tlb_range, deferred_free, ...}) — ~6 items

Total: ~80–120 state items. Finite, enumerable.

For each: where it LIVES (per-task / per-vCPU / per-mm / per-host-CPU /
per-VM), what AUTHORITY owns it, what the SHADOW copies are.

### Layer 2: operations inventory ([02-operations-inventory.md](02-operations-inventory.md))

Enumerate every operation that touches state, in order, with the
state items each one reads/writes:

- Hardware-driven: VMRUN, vmexit, SYSCALL, SYSRET, IDT delivery, IRETQ
- KVM-driven (host): __set_sregs, __set_regs, kvm_vcpu_ioctl_run, sync_regs, store_regs, vcpu_enter_guest
- v2-driven (UML kernel): kvm_v2_load_user_sregs, marshal_to/from_kvm_regs, kvm_v2_handle_io_pf, kvm_v2_handle_pf_eintr_inline, kvm_v2_ist_frame_*, kvm_v2_fpu_*, kvm_v2_tlb_kick_others
- UML-kernel-driven (cross-cutting): schedule, switch_to, do_signal/setup_rt_frame, sigreturn, interrupt_end, do_exit
- Signal-driven: SIGALRM (timer preempt), IPI_SIGNAL (cross-vCPU kick), SIGSEGV (delivered to user), KVM_SET_SIGNAL_MASK (which signals can interrupt KVM_RUN)

For each: the **operation contract** — exactly which state items it
must read or write, and the synchronization (preempt_disable, mutex,
nothing) that protects access.

### Layer 3: ownership matrix ([03-state-ownership-matrix.md](03-state-ownership-matrix.md))

Two-dimensional table:
- Rows: state items from layer 1
- Columns: operations from layer 2
- Cells: a 2-tuple `(R/W/-, save-restore-rule)`

For every cell, the v2 implementation MUST satisfy:
- The save-restore rule is honored (we save before context switch,
  restore on return, etc.)
- No cross-task or cross-vCPU contamination is possible: if the cell
  says "R" and the source is per-vCPU shared, the operation must be
  protected by serialization sufficient to prevent interleaved writes
  from another task.

### Layer 4: per-suspect-register deep audit ([04-suspect-register-audits.md](04-suspect-register-audits.md))

For the 4 registers that show up corrupted in mt-mini-diag captures:

- **RBP** (corrupted to other thread's stack address)
- **RAX** (sometimes 0, the MMAP_NULL signature)
- **RDX** (small int, the slow_memset write target → SIGSEGV at low addr)
- **FS_BASE** (TLS base — corruption explains wrong TLS reads)

For each: enumerate every read/write site in `arch/um/backend/kvm-v2/`
and audit each for cross-task safety.

### Layer 5: toolkit ([05-toolkit.md](05-toolkit.md))

Concrete cscope/ast-grep/ripgrep/bpftrace/ftrace recipes to
mechanically enumerate the matrix and verify each cell at runtime.

### Layer 6: findings ([99-findings.md](99-findings.md))

What bugs the audit surfaces. Filled in as the matrix is completed.

## Acceptance criteria

The audit is complete when:

1. Every state item in layer 1 has a single authoritative HOME and a
   complete list of save/restore points across operations.
2. Every operation in layer 2 lists every state item it reads/writes,
   with file:line references.
3. The ownership matrix is filled in for every (state, operation)
   pair — even cells marked "doesn't apply" must be explicit.
4. Layer 4's per-register audits identify EVERY site that could write
   the wrong value, and either (a) prove the site is safe, or (b)
   flag it as a bug-candidate.

## Process

This document and the per-layer files are SCAFFOLDS. They will be
filled in by:

- 3 parallel opus subagents — one per layer (1, 2, 4)
- 1 manual layer (5 — toolkit, since recipes are session-specific)
- Final consolidation in layer 3 (matrix) + layer 6 (findings)

Each subagent's output goes in its layer's file, with file:line
citations for every claim. No subagent modifies code.

## Why this approach is different

Previous fix attempts started from a hypothesis ("cross-vCPU TLB
stale", "cross-task IST clobber") and patched specific code paths.
Each fix either didn't help or caused regressions because the
hypothesis was incomplete.

This audit is **bottom-up**: enumerate every state item and operation,
check each for correctness, surface bugs by mechanical comparison.
Even if no single bug jumps out, the matrix becomes the v2 design
spec — the canonical answer to "how does v2 manage state?"

## Anti-pattern to avoid

Do NOT propose architectural changes (per-task vCPU, mutex
serialization, etc.) until the matrix is COMPLETE and a specific
matrix cell is shown to be incorrect. Premature architecture changes
have already burned this session.
