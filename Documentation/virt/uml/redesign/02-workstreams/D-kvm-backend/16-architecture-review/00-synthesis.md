# KVM Backend Architecture Review — Synthesis

Date: 2026-04-26

This document synthesizes findings from five Opus sub-agent reviews of
the integrated KVM backend, conducted after ~40 commits failed to move
the cpython-parity gate from 0/21. Empirical ground truth (measured
this session): ptrace and seccomp are 10/10 across every workload
(hashlib, import_os, import_unittest, import_json, import_subprocess,
test.test_struct), with or without ASLR. KVM is 80-90% on small
workloads and **0% on every medium workload**.

Source reviews:

- `01-mm-architecture.md` — shadow PT, mm_id flow, deferred-vs-direct chain
- `03-bootstrap-bytes.md` — LSTAR/IDT/IST/gadget bytecode review
- `04-concurrency.md` — locking, atomic-context safety, races
- `05-observability.md` — diagnostic blindspots
- `06-backend-comparison.md` — KVM vs ptrace vs seccomp structural diff
- (`02-vcpu-state.md` — pending; will integrate when complete)

## The convergent diagnosis

All five reviews independently identified variations of the same
structural problem: **the KVM backend maintains multiple representations
of guest memory state, each mutable by a different actor, with no
single-source-of-truth discipline**. The reviews differ on which
representation is most likely the immediate fault site, but agree
on the architectural pattern.

```
                 PARENT UML HOST PROCESS
   ┌─────────────────────────────────────────────────────────┐
   │                                                          │
   │   ┌─────────────────┐    ┌─────────────────┐            │
   │   │  UML LOGICAL    │    │   HOST VA       │            │
   │   │      pgd        │←──→│   MAPPING       │            │
   │   │ (Linux mm)      │    │ (os_map_memory) │            │
   │   └────────┬────────┘    └────────┬────────┘            │
   │            │                       │                     │
   │            │ kvm_um_pte_to_x86     │ mmu_notifier        │
   │            ↓                       ↓                     │
   │   ┌─────────────────┐    ┌─────────────────┐            │
   │   │  SHADOW PT      │    │   KVM EPT       │            │
   │   │  (per-mm)       │←──→│  (managed by    │            │
   │   │                 │    │   KVM)          │            │
   │   └────────┬────────┘    └────────┬────────┘            │
   │            │                       │                     │
   │            └─────────┬─────────────┘                     │
   │                      ↓                                   │
   │            ┌─────────────────┐                           │
   │            │   GUEST CR3     │                           │
   │            │   (vCPU0)       │                           │
   │            └─────────────────┘                           │
   │                                                          │
   │   FIVE WRITERS to "what should the guest see"            │
   │   ─────────────────────────────────────────              │
   │   1. set_pte_at      → UML pgd                           │
   │   2. set_ptes hook   → shadow direct sync                │
   │   3. pte_clear hook  → shadow direct sync                │
   │   4. flush_tlb_*     → kvm_shadow_sync_va_atomic         │
   │   5. um_tlb_sync     → ops.mm_map → invalidate           │
   │   6. kvm_enter_guest → kvm_shadow_fill_from_uml_pgd      │
   │                       (transactional clear+install)      │
   │                                                          │
   │   ZERO observation points for cross-view divergence.     │
   │                                                          │
   └─────────────────────────────────────────────────────────┘
```

Compare with the working backends:

```
   ptrace / seccomp
   ────────────────────────────
   PARENT UML HOST PROCESS         STUB CHILD HOST PROCESS
   ┌──────────────────────┐         ┌──────────────────────┐
   │  UML LOGICAL pgd     │ ──────→ │  CHILD HOST pgd      │
   │  (Linux mm)          │ os_*    │  (only writer:       │
   │                      │ via     │   um_tlb_sync)       │
   └──────────────────────┘ stub    └──────────┬───────────┘
                                                │
                            ┌───────────────────┘
                            ↓
                   ┌──────────────────────┐
                   │  REAL HOST CPU runs  │
                   │  user code via       │
                   │  PTRACE_SYSEMU /     │
                   │  seccomp filter      │
                   └──────────────────────┘

   TWO VIEWS, ONE WRITER per view, the host CPU enforces
   coherence via real page-table walks. Cannot diverge
   structurally — only via UML pgd → child pgd sync errors,
   and those errors are LOUD because the child's MMU faults.
```

The KVM backend has THREE views on the parent process side
(UML pgd / host VA mapping / shadow PT) plus EPT cached by KVM,
all maintained by separate writers. Divergence is silent.

## Top-priority concrete bugs identified (ranked by reviewer confidence)

### B-RACE-E [PROVEN — Agent 4] missing memory barrier + plain load in SREGS-skip

**Location:** `arch/um/backend/kvm/shadow_sync.c:267-268`,
`arch/um/backend/kvm/thread.c:2164`

`kvm_shadow_sync_pte` writes the leaf via `WRITE_ONCE(*spte, new)` then
`WRITE_ONCE(shadow->dirty, true)` with **no smp_wmb between them**.
`kvm_enter_guest`'s SREGS-skip predicate at thread.c:2164 reads
`!shadow->dirty` with a plain load, not `READ_ONCE`.

A SIGALRM-driven preemption between the leaf write and the dirty write
leaves dirty=false momentarily; the next entry's SREGS-skip sees
`!dirty`, skips KVM_SET_SREGS (= no CR3 reload = no guest TLB flush),
and the guest reads through a stale TLB entry. Frequency scales with
mutation density — exactly matches "intermittent on small workloads,
deterministic on large".

**Fix:** ~10 lines. `smp_wmb()` between writes; `READ_ONCE` on the
predicate.

### B-ALIAS-INSTALL [PROVEN — Agent 3] fill's install pass overwrites bootstrap aliases

**Location:** `arch/um/backend/kvm/lifecycle.c:1216-1331` (fill install
pass)

Fill's CLEAR pass (lines 1213-1252) explicitly preserves the bootstrap
VA range `[kvm_bootstrap_va, +4*PAGE_SIZE)`. The INSTALL pass (1266-1331)
walks the UML pgd and unconditionally calls `kvm_shadow_map_page` for
every present leaf — overwriting any bootstrap shadow leaf at a colliding
VA. `kvm_bootstrap_va` lives in PGD slot 0 (low addresses, ~0x60000000),
which user pgd entries can collide with.

**Fix:** ~5 lines. Mirror the alias-range guard in the install pass.

### B-GS-PRIMED [PROVEN — Agent 3] kernel_gs_base_primed is one-shot

**Location:** `arch/um/backend/kvm/thread.c:1644`

`kernel_gs_base_primed` short-circuits MSR_KERNEL_GS_BASE re-programming
after first success. `KVM_SET_SREGS` and `arch_prctl(ARCH_SET_GS)` can
both modify this MSR underneath us. After drift, every gadget swapgs
reads from the wrong base and `%gs:0x08` returns garbage that flows
downstream as a fake pid/uid — exactly the wild-pointer pattern.

**Fix:** ~1 line. Drop the short-circuit; pay the per-entry MSR write
cost.

### B-FPU-HASH-UAF [PROVEN — Agent 4] FPU per-task hash never frees + uses task_struct as key

**Location:** `arch/um/backend/kvm/thread.c:104-130`

The hash table `kvm_fpu_hash[]` (added in commit a6041ebb) keys on
`task_struct *` and never removes entries. When a task exits, its
slab is freed and may be reused by a different task. Lookup walks
through the freed task's stale `task` field — UAF read. Plus
`GFP_ATOMIC` failures are silent.

**Fix:** ~30 lines. Allocate per-task slot in `init_new_context` (not
`GFP_ATOMIC`); free in `exit_mmap` / `arch_release_task_struct`.

### B-MULTI-WRITER [PROVEN — Agent 1, 6] direct-sync grafted on top of deferred chain

**Location:** entire shadow PT subsystem.

The deferred chain (`set_pte_at` → `_PAGE_NEEDSYNC` → `um_tlb_sync` →
`ops.mm_map` → `kvm_shadow_invalidate_va_range` → next
`kvm_enter_guest`'s `kvm_shadow_fill_from_uml_pgd`) ALL still runs for
every PTE transition. PLUS direct sync runs (`set_ptes` /
`pte_clear` / `flush_tlb_*` hooks). PLUS the transactional fill
clear+install runs. Five writers, no owner. Each "fix" closes one
inconsistency edge while opening another.

**Fix:** STRUCTURAL. Either remove the deferred shadow path entirely
under CONFIG_UM_BACKEND_KVM_INTEGRATED (memo 15 Phase 3) or remove the
direct sync. Keep one writer.

### B-PARENT-VA-CONTAMINATION [PROVEN — Agent 6] guest runs in same host process as UML kernel

**Location:** `arch/um/backend/kvm/mm.c:135` (os_map_memory MAP_FIXED)

`kvm_mm_map` does `os_map_memory(MAP_FIXED, ...)` into the parent UML
process's VA space. The parent VA also holds UML kernel binary, libc,
allocator state. If a guest pointer is wild and the wild value lands
on UML's own structures, the guest writes corrupt UML's kernel state.
The 45-bit garbage addresses (e.g. `0x441f0f66e0ff`) and the
PyList_Append's-own-text writes (cr2=0x5164a0 = PyList_New text) are
consistent with this contamination.

**Fix:** STRUCTURAL. Per-mm host worker process (memo 15 Phase 4
Option B). Substantial refactor.

### B-PMD-CLEAR-BYPASS [PROVEN — Agent 1] parent-level clears bypass direct sync

**Location:** `arch/um/include/asm/pgtable.h:158`,
`arch/um/include/asm/pgtable-4level.h:84`

`pmd_clear`/`pud_clear`/`p4d_clear` are macros that ONLY set
`_PAGE_NEEDSYNC`. They do NOT call `kvm_shadow_sync_pte` or any
shadow update. Generic mm code clearing intermediate levels (free_pgtables,
mremap, exec_mmap) leaves the shadow holding present leaves below
freed parent tables.

The earlier "audit conclusion" that flush_tlb_* covers this is WRONG
for cases where the parent-level clear is followed by no flush, OR
where flush_tlb_mm's vma walk doesn't include the cleared range.

**Fix:** ~30 lines. Convert macros to inline functions taking mm/addr;
call `kvm_shadow_sync_range_atomic` for the parent's full range.

### B-THREE-VIEW-BLINDSPOT [PROVEN — Agent 5] no diagnostic checks the host VA mapping

**Location:** all existing diagnostics.

`kvm_shadow_audit_va` compares (UML pgd PTE, expected shadow, actual
shadow). It never inspects the host VA mapping (the third coordinate).
40 commits of diagnostics have all said EQUAL because shadow does
match pgd — but neither matches the host VA mapping that copy_to_user
actually writes through.

**Fix:** ~50 lines. At fatal fault, walk the parent's host pgtable
for cr2's host VA and log its physical page alongside shadow's PFN
and pgd's PFN. The first time these three diverge tells us
definitively which writer dropped the ball.

## Per-agent "what I would do first" recommendations

| Agent | First recommended action |
|-------|--------------------------|
| 1 — MM | Collapse to single source of truth: gut the deferred shadow path, make direct sync the sole writer, kvm_enter_guest a verifier |
| 3 — Bootstrap | Add alias-range guard to fill's install pass + drop kernel_gs_base_primed short-circuit + LSTAR byte CRC32 |
| 4 — Concurrency | Add `smp_wmb` between leaf write and dirty=true; `READ_ONCE` on the SREGS-skip predicate |
| 5 — Observability | Three-way coherence dump at fatal fault (shadow PFN, pgd PFN, host-VA PFN) |
| 6 — Backend comparison | mprotect parent's [0, task_size) PROT_NONE around KVM_RUN — discriminator diagnostic |

These five are mostly independent and can be done in parallel. They
either fix the bug directly (Race E, alias install, GS-primed) or
make the next attempt actionable (three-view dump, PROT_NONE
discriminator).

## Recommended new architecture (high-level)

The only design that survives all five reviews is a **single-source-of-
truth + isolation** model:

```
   PROPOSED KVM BACKEND
   ════════════════════════════════════════════════════════════
   PARENT UML HOST PROCESS              PER-MM HOST WORKER
   ┌──────────────────────┐             ┌──────────────────────┐
   │  UML LOGICAL pgd     │             │  HOST VA MAPPINGS    │
   │  (Linux mm)          │             │  (only writer:       │
   │                      │   RPC      │   worker, via        │
   │  ↓ on every          │ ─────────→ │   os_map_memory)     │
   │  set_pte_at /        │ mutation   │                      │
   │  pte_clear           │ stream     └──────────┬───────────┘
   │                      │                       │
   │  ↓ direct sync       │            ┌──────────────────────┐
   │  (atomic, no fill,   │            │  KVM VM (per-mm)     │
   │  no deferred chain)  │            │  - own memslot       │
   │                      │            │  - own vCPU0         │
   │  ↓                   │            │  - shadow CR3 from   │
   │  ┌────────────────┐  │            │    the UML pgd via   │
   │  │ SHADOW PT      │  │            │    direct sync       │
   │  │ - sole writer: │  │            └──────────┬───────────┘
   │  │   direct sync  │  │                       │
   │  │ - CR3 source   │ ─┼───────────────────────┘
   │  │   for vCPU     │  │                       
   │  └────────────────┘  │            
   │                      │            
   │  kvm_enter_guest:    │            
   │  VERIFIER ONLY       │            
   │  (assert shadow      │            
   │   matches pgd)       │            
   └──────────────────────┘            
   
   ─ Single writer per view (UML pgd, host VA, shadow PT)
   ─ Per-mm worker = stub-child equivalent → matches ptrace/seccomp
   ─ Per-mm KVM VM = isolated memslot, no cross-mm collision
   ─ Wild guest pointers can't reach UML kernel state (different VA space)
   ─ Fault at the verifier panics LOUDLY with a coherence diff
```

This is a significant refactor (4-6 weeks of focused work). It is
also the only configuration where the failure modes that have
plagued this session would be either impossible (per-mm isolation)
or LOUD (verifier panic on divergence).

## The phased implementation plan

### Phase 0 — Five-fix sprint (this week, ~150 lines total)

These are concrete actionable items from the reviews, each small
enough to implement and validate against the parity gate independently.

| Item | Lines | Owner-question | Files |
|------|-------|----------------|-------|
| P0-1 Race E barrier + READ_ONCE | ~10 | Does parity move? | shadow_sync.c, thread.c |
| P0-2 Bootstrap-alias install guard | ~5 | Does parity move? | lifecycle.c |
| P0-3 Drop kernel_gs_base_primed | ~1 | Does parity move? | thread.c |
| P0-4 Three-view coherence dump | ~50 | What does diff show? | thread.c, shadow_sync.c |
| P0-5 PROT_NONE discriminator | ~50 | Does PROT_NONE catch a write? | thread.c |

After Phase 0, ONE OF:
- Parity moved → identify which fix did it; commit cleanly; understand why
- Parity didn't move + three-view dump shows divergence → know exactly
  which view is wrong; fix proceeds from there
- Parity didn't move + dump shows no divergence + PROT_NONE catches a
  write → bug is in a host-VA path we haven't audited

### Phase 1 — Single source of truth (1-2 weeks)

Memo 15 Phase 3 fully implemented:
- Remove `kvm_shadow_fill_from_uml_pgd` from `kvm_enter_guest`'s normal
  path. Only run on `needs_full_resync`.
- Convert `kvm_shadow_invalidate_va_range` from a writer to an audit-
  only function (panic on divergence).
- Convert `pmd_clear`/`pud_clear`/`p4d_clear` from macros to inlines that
  call direct sync.
- Remove `kvm_shadow_clear_range_atomic` (legacy, replaced by sync).

### Phase 2 — Comprehensive observability (1 week)

- ftrace `TRACE_EVENT_DEFINE` for: shadow_sync, mm_map, mm_unmap,
  flush_tlb, vCPU enter/exit, copy_to_user, copy_from_user.
- Three-way audit at every fault (Phase 0's diagnostic, productionized).
- Differential test runner: same script under all three backends, diff
  syscall traces.
- KVM_GET_VCPU_EVENTS + KVM_GET_FPU + complete sregs dump on every
  fatal fault.

### Phase 3 — Per-task vCPU state (1 week)

- Save/restore on context switch: GPRs (already done), FPU (recently
  added; rework with proper lifetime), VCPU_EVENTS, FS/GS base, MSRs,
  debug registers DR0-DR7.
- Single `vcpu0_fd` shared across tasks → per-task state struct in
  thread_struct (not a hash).
- Free at exit (fix the UAF).

### Phase 4 — Per-mm host worker (3-4 weeks; STRUCTURAL)

Memo 15 Phase 4 Option B:
- Per-mm host child process for user-VA mappings.
- Mappings via RPC from UML kernel to worker.
- Worker holds the memslot for its mm.
- UML kernel's copy_to_user routes via PTE walk (memo 15 Option A) or
  via RPC to worker.

### Phase 5 — Drop deferred shadow path entirely (cleanup; 1 week)

After Phase 1-4 prove direct sync owns the world:
- Remove `_PAGE_NEEDSYNC` shadow propagation from
  `kvm_shadow_invalidate_va_range`.
- Remove `kvm_shadow_invalidate_va_range` from `kvm_mm_map`/`unmap`
  entirely; mm_map becomes purely host-VA management.
- The deferred chain still runs for ptrace/seccomp; KVM stops using
  it for shadow.

## Cross-cutting risks the review surfaced

1. **Hand-rolled bootstrap bytecode is brittle.** Agent 3 found the
   bytes byte-perfect but flagged this as inherent risk: silent
   corruption in 8 bytes of asm. Recommend: assemble from `.S` source
   under build, runtime CRC32 check, KUnit test for bytecode equality.

2. **The `init_new_context` short-circuit (T13)** is a heuristic
   (`virt==0 && len >= 1<<46`). May be too aggressive (skipping
   legitimate work) or too narrow (missing other catastrophic calls).
   Replace with explicit per-mm-init flag passed through ops.

3. **No formal model of the shadow-PT/direct-sync state machine.**
   Without a state diagram, every reviewer found different races. Build
   one as part of Phase 1.

4. **No KVM-specific kselftest for the actual failure modes.** All
   existing kselftests pass while the bug persists. Build a kselftest
   that specifically reproduces "import unittest under kvm" and
   asserts no segfault — make it RED so future work is gated on it.

## What the reviews did NOT converge on

- **Whether fixing Race E (B-RACE-E) is sufficient.** Agent 4 thinks
  high probability. Agents 1, 6 think it's a contributing factor at
  most.
- **Whether per-mm host worker (Phase 4) is necessary or just nice-
  to-have.** Agent 6 strongly recommends; Agents 1, 5 view it as the
  "if Phase 1-3 don't fix it" backstop.
- **The role of FPU.** Agent 4 found the UAF in the hash; Agent 6
  thinks per-task vCPU state more broadly is the bigger issue.

The Phase 0 sprint is designed to disambiguate these — each item
narrows the search.
