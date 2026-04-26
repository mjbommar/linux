---
title: Memo 18 — KVM backend architectural fix plan (post-memo-17 residual)
date: 2026-04-26
status: PLAN — execution starts immediately
input:
  - memo 16 (architecture review)
  - memo 17 (multi-task investigation + Phase A-J landed fixes)
  - external review priorities (priorities 1-9 below)
target: cpython parity gate 21/21 stable; import unittest 30/30 deterministic
---

# Architectural fix plan

## Diagnosis (the honest version)

Memo 17 Phase A-J landed 11 keystone correctness fixes that moved the
cpython parity gate from 0/21 → 17/21 (median, with 14-18 variance).
The residual ~10-15% SIGSEGV-flake on Python startup is NOT one more
small bug — it is the consequence of an architecture that shares too
much state across UML tasks on a single vCPU.

**What's shared and racy:**

| Resource | Shared by | Race manifests as |
|---|---|---|
| `vcpu0_fd` + `kvm_run0` mmap | All UML tasks | Cross-task vCPU state contamination |
| `kvm_bootstrap_page_stack` (IRETQ frame staging) | All UML tasks | Task A runs with task B's user RIP/RSP/RFLAGS — wild SIGSEGV |
| `cached_cr3_gpa` / `cached_fs_base` / `cached_gs_base` / `sregs_primed` | All UML tasks | Skip-SREGS predicate hits with stale cache → no TLB flush → stale read |
| `os_map_memory` parent-VA mappings | All UML mms | `copy_*_user` from kernel side hits wrong mm's mapping |
| Pending `kvm_vcpu_events` exception queue | All UML tasks (until Phase E/H fix) | Wrong-task #PF delivered |
| KVM_EXIT_INTR exit path | All exit reasons | At-CPL=0 marshal corrupts user RIP/RSP/RFLAGS with bootstrap values |

**What seccomp/ptrace do differently** (and why they don't have these
flakes): each mm has its own host stub-child process. mm-state is
isolated by host kernel. Per-task register state is in the stub
child's own context. `os_map_memory` writes into the stub child's
address space, not the parent UML's.

**What gVisor/Firecracker do differently**: pool of vCPUs sized to
NumCPU; one active task per vCPU during its run. No singleton
staging buffers. KVM-native uaccess (don't depend on host VA
mappings for guest-memory access).

**The architecture this codebase has converged on**: 1 vCPU + N
tasks + manual save/restore of "what we noticed needs saving." The
manual save/restore approach has now caught FPU + VCPU_EVENTS +
SREGS-cache (Phase D/E/H), but the IRETQ frame and parent-VA
mappings are structurally singleton.

**Conclusion**: surgical patches on the singleton design have
diminishing returns. The remaining residual requires structural
de-singletonification.

---

## Plan structure

Five phases, each with a clear deliverable, success criterion, and
reversibility plan. Each phase MUST run the parity gate + import
unittest reliability harness BEFORE moving to the next. Don't pile
phases on top of broken state.

```
Phase 1 (1-2 days):  KVM_EXIT_INTR CPL gate + diagnostic toggles
Phase 2 (2-3 days):  Per-mm IRETQ-frame storage
Phase 3 (3-5 days):  Per-mm vCPU + per-mm SREGS cache
Phase 4 (1-2 weeks): KVM-native uaccess (eliminate os_map_memory dependency)
Phase 5 (multi-week, optional): Per-task vCPU pool / SMP
```

After Phase 3, parity should be 20-21/21 stable. Phase 4-5 are for
correctness completeness and eventual SMP.

---

## PHASE 1 — KVM_EXIT_INTR CPL gate + diagnostic toggles (1-2 days)

External review priority 1. Phase K (this session) attempted this
narrow fix and reverted because the implementation had two separate
problems:
  - Skip-marshal-entirely lost user CPL=3 progress
  - CPL=0 gate broke SYSCALL/PF/MMIO paths that legitimately need
    the marshalled regs

The CORRECT fix lifts the SREGS read above the marshal AND
distinguishes between "CPL=0 with iretq frame on IST stack" (PF
case: rebuild from IST) vs "CPL=0 with no frame to recover" (LSTAR
mid-OUT: preserve previous regs and let bootstrap re-execute).

### 1.1 Move SREGS read above marshal
- `arch/um/backend/kvm/thread.c::kvm_run_userspace`
- Read `exit_sregs` and derive `is_user` BEFORE
  `kvm_regs_to_uml_regs(regs, &kregs)`
- Set `regs->is_user` from the CPL bit

### 1.2 Implement per-exit-reason CPL-aware marshal gate
The marshal is needed for cases where the dispatcher reads the bulk
regs (SYSCALL via kvm_decode_syscall reads RAX/RCX/R11, MMIO and HLT
read RIP). The marshal is HARMFUL when CPL=0 AND the dispatcher
doesn't use the regs (KVM_EXIT_INTR, IRQ_WINDOW_OPEN, etc).

Decision tree:
  - CPL=3 always: marshal (user state)
  - CPL=0 + KVM_EXIT_IO (port==SYSCALL/PF): marshal (kvm_decode_syscall
    needs RAX/RCX/R11; PF case rebuilds RIP/SP/EFLAGS from IST after)
  - CPL=0 + KVM_EXIT_MMIO: marshal (MMIO case overwrites RIP/SP/
    EFLAGS via faultinfo)
  - CPL=0 + KVM_EXIT_HLT: marshal (HLT case overwrites RIP)
  - CPL=0 + KVM_EXIT_INTR: DO NOT marshal — preserve regs as set by
    kvm_enter_guest entry. Next entry resumes at the same bootstrap
    sequence which restarts cleanly.

### 1.3 Add diagnostic toggles
Three module parameters for narrow-window investigation:
  - `kvm_diag_force_pge_toggle=1` — toggle CR4.PGE every entry
    regardless of dirty (validates whether missed-dirty-producer is
    still happening)
  - `kvm_diag_disable_sync_regs=1` — disable KVM_SYNC_X86_REGS,
    fall back to KVM_GET_REGS / KVM_SET_REGS ioctls (validates
    whether stale sync-regs is corrupting state)
  - `kvm_diag_no_sregs_skip=1` — disable the SREGS-skip cache
    entirely (validates whether the singleton cache is the
    remaining race source)

### Success criterion
- read_test5 still 8/8
- import unittest reliability ≥ 28/30 across 3 trials
- cpython parity gate ≥ 17/21 (no regression from Phase J)

### Reversibility
Single commit. Reverts to Phase J state on failure.

---

## PHASE 2 — Per-mm IRETQ-frame storage (2-3 days)

External review priority 2. This eliminates the singleton bootstrap_
page_stack race.

### 2.1 Add per-mm IRETQ-frame page allocation

In `arch/um/backend/kvm/lifecycle.c::kvm_shadow_mm_alloc`:
  - Allocate a fresh page via `__get_free_page(GFP_KERNEL | __GFP_ZERO)`
  - Store the kernel VA in `shadow->iretq_frame_va`
  - Calculate the GPA: `__pa(shadow->iretq_frame_va)`
  - Store the GPA in `shadow->iretq_frame_gpa`

In `kvm_shadow_mm_free`:
  - `free_page(shadow->iretq_frame_va)`

### 2.2 Map per-mm IRETQ frame into the shadow PT
The IRETQ frame must be at a guest-accessible VA (shadow PT must
map it). Two choices:
  - **(a)** Add it to the bootstrap-alias range (extend from 4
    pages to 5 pages, 5th being the per-mm IRETQ frame)
  - **(b)** Allocate a fresh shadow VA per mm in the kernel-half
    range and install it in the per-mm shadow PGD only

Choice (b) is cleaner — different mms get different VAs naturally
since each shadow_mm has its own PGD. Use a fixed VA per
shadow_mm: `shadow->iretq_frame_va_guest = ALIGN_DOWN((u64)shadow,
PAGE_SIZE) | 0xff_0000_0000UL` (top of canonical kernel half,
unique per shadow_mm pointer).

In `kvm_shadow_pgd_alloc`, install a leaf at the chosen guest VA
mapping to `shadow->iretq_frame_gpa`. This is in the kernel half
(slot 256+), so not touched by the user-half clear/install passes.

### 2.3 Use the per-mm frame in kvm_enter_guest
- `arch/um/backend/kvm/thread.c::kvm_enter_guest`
- Replace `frame = (u64 *)kvm_bootstrap_page_stack;` with
  `frame = (u64 *)current_shadow->iretq_frame_va;`
- Replace `kregs.rsp = kvm_bootstrap_va + 3 * PAGE_SIZE;` with
  `kregs.rsp = current_shadow->iretq_frame_va_guest;`

### 2.4 Per-task within an mm
Threads sharing an mm still share the per-mm IRETQ frame. Two
options:
  - **(a)** Block signals across the frame-write → KVM_RUN window.
    Signals queue, fire after KVM_RUN returns. Simple but adds latency.
  - **(b)** Per-task offset into the per-mm frame page (each task gets
    a 64-byte slot). Then kregs.rsp = frame_va + task_slot_offset.

Option (a) is simpler and we have evidence (Phase F) it doesn't
break things. The Phase F signal-block didn't help PARITY because
the dominant remaining bug at that point was elsewhere (Phase H+I+J
were not yet landed). With per-mm frame eliminating CROSS-mm
contamination, signal-block on same-mm SHOULD close the residual.

Start with (a). If parity isn't 21/21 after Phase 2, add (b).

### Success criterion
- read_test5 still 8/8
- import unittest reliability ≥ 29/30 across 5 trials
- cpython parity gate ≥ 19/21 across 3 runs
- The `kvm_diag_force_pge_toggle` test from 1.3 should show no
  improvement (proves the dirty discipline is correct in isolation
  from the IRETQ-frame race)

### Reversibility
Three commits (storage, shadow PT install, use). Each reverts
cleanly.

---

## PHASE 3 — Per-mm vCPU + per-mm SREGS cache (3-5 days)

External review priorities 3, 7. This is the structural fix that
eliminates ALL cross-mm vCPU state contamination, removes the
singleton SREGS-skip cache, and brings the design closer to gVisor's
per-active-task vCPU model.

### 3.1 Per-mm KVM_VCPU
- Replace `vcpu0_fd` singleton with `shadow->vcpu_fd`.
- `kvm_shadow_mm_alloc`: `KVM_CREATE_VCPU` with vcpu_id derived from
  a per-mm counter (limit: KVM caps at ~288/512 vCPUs per VM —
  enough for typical UML workload of 10s-100s of mms; if hit, fall
  back to lazy create-on-first-run).
- `kvm_shadow_mm_free`: close the vcpu_fd.
- `kvm_run_userspace`: use `current->active_mm->context.id.kvm_shadow->vcpu_fd`.

### 3.2 Per-mm kvm_run mmap
Each vCPU has its own kvm_run mmap. Store in `shadow->vcpu_run`.
Update all `run0` references.

### 3.3 Move SREGS-skip cache into shadow_mm
- `cached_cr3_gpa`, `cached_fs_base`, `cached_gs_base`,
  `sregs_primed` move from `kvm_um` (singleton) to
  `kvm_shadow_mm` (per-mm).
- The skip-SREGS predicate now reads per-mm cache, which only
  reflects the LAST task that ran ON THIS MM's vCPU. Same-mm
  cross-task can still desync within an mm — but that's a
  narrower problem than cross-mm.

### 3.4 Per-mm bootstrap programming
The LSTAR / IDT / TSS / per-mm bootstrap is currently programmed
once at first entry. With per-mm vCPU, each vCPU needs its own
programming. Move `kvm_enter_guest_init_bootstrap` to per-mm
init at vCPU create time.

### 3.5 Audit singleton callers
`grep -n "vcpu0_fd\|run0\|cached_cr3_gpa\|cached_fs_base\|cached_gs_base\|sregs_primed"` → audit every reference, replace with per-mm.

### Success criterion
- read_test5 still 8/8
- import unittest reliability 30/30 across 5 trials
- cpython parity gate 19-21/21 across 5 runs (median ≥ 20)
- Cross-mm parity tests (test_subprocess, test_multiprocessing) move
  from broken to working

### Reversibility
This is invasive. Do it in feature branch with all old code
commented out behind `#ifdef KVM_LEGACY_SINGLETON_VCPU` for the
first iteration. Once the new path is proven, delete the legacy.

---

## PHASE 4 — KVM-native uaccess (eliminate os_map_memory dependency)
(1-2 weeks)

External review priority 7 (long term). Currently `kvm_mm_map` calls
`os_map_memory` into the SINGLE UML host process VA space. This is
the parent-VA contamination class — `copy_to_user` from the kernel
side dereferences a host VA that may belong to a DIFFERENT mm than
the current one.

### 4.1 Audit current usage of os_map_memory under KVM
Per the comment at `arch/um/backend/kvm/mm.c:124-134`, `os_map_memory`
is currently load-bearing under integrated KVM despite the
copy_to_user code path going through `page_address(pte_page(*pte))`
(kernel direct map). Find what specifically depends on it (the
comment suggests "io_uring fixed-buffer setup" — verify).

### 4.2 Replace io_uring / DMA paths with KVM-native equivalents
KVM provides `KVM_TRANSLATE` to translate guest VA to GPA without
going through host VA. For paths that need bulk transfer, use
`KVM_GET_DIRTY_LOG` / direct memslot access via the user-set
memslot HVA (which is uml_physmem + GPA — this is HVA, but it's
the SAME mapping for ALL mms via the singleton memslot).

### 4.3 Drop `os_map_memory` from `kvm_mm_map`
Once 4.2 is complete, remove the `os_map_memory` call entirely.
Each mm's user-VA range becomes invisible to host kernel-mode code
EXCEPT through the singleton physmem memslot (which is per-PFN, not
per-mm).

### 4.4 Add PROT_NONE canary diagnostic
Per memo 17 finding 4 + the user's external review: add a
diagnostic mode that mprotect(PROT_NONE)s the user-VA range before
KVM_RUN, catching any accidental parent-VA access during guest run.

### Success criterion
- All existing parity tests still pass
- New test: cross-mm copy_to_user stress doesn't crash
- PROT_NONE diagnostic doesn't fire under normal workloads

---

## PHASE 5 (optional, multi-week) — Per-task vCPU pool / SMP

If after Phase 1-4 there's still residual flake from same-mm
threading, add per-task vCPU. This matches gVisor's model.

### 5.1 vCPU pool
- Pool of N vCPUs (start with 8). Grow lazily.
- Each task that wants to run user code gets a vCPU from the pool.
- Released after KVM_RUN returns.

### 5.2 SMP support
- UML normally runs ncpus=1. With per-task vCPU we naturally support
  SMP — multiple vCPUs can run concurrently.
- Need careful audit of all "ncpus=1" assumptions in shadow PT
  handling (most assumed cooperative).

This is a real refactor and only worth doing if 1-4 don't reach
21/21.

---

## Order of operations (concrete commits)

```
Phase 1.1 — Move SREGS read above marshal
Phase 1.2 — Per-exit-reason CPL-aware marshal gate
Phase 1.3 — Diagnostic toggles
  → run parity gate × 3, import unittest × 30 × 3
  → if ≥ Phase J baseline, proceed

Phase 2.1 — Per-mm IRETQ frame storage in shadow_mm
Phase 2.2 — Map per-mm frame into shadow PT
Phase 2.3 — Use per-mm frame in kvm_enter_guest
Phase 2.4 — Block signals across frame-write → KVM_RUN window
  → run parity gate × 3, import unittest × 30 × 5
  → if ≥ 19/21 median, proceed

Phase 3.1 — Per-mm KVM_VCPU
Phase 3.2 — Per-mm kvm_run mmap
Phase 3.3 — Move SREGS-skip cache into shadow_mm
Phase 3.4 — Per-mm bootstrap programming
Phase 3.5 — Audit singleton callers + cleanup
  → run parity gate × 5
  → target: 20-21/21 median

Phase 4 — only if Phase 3 doesn't reach 21/21 stable
Phase 5 — only if Phase 4 doesn't reach 21/21 stable
```

## Validation harnesses (use after every phase)

```bash
# Smoke
/tmp/uml-kvmint/linux backend=force=kvm ... init=/tmp/read_test5
# expect: 8/8 byte-perfect

# Reliability
for i in $(seq 1 30); do
  /tmp/uml-kvmint/linux backend=force=kvm ... init=/usr/bin/python3 -- /tmp/imp_ut.py
done | grep -c IMPORT_UT_OK
# expect: ≥29/30 after Phase 2; 30/30 after Phase 3

# Parity gate
UML_BINARY=/tmp/uml-kvmint/linux \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh
# expect: ≥17/21 after Phase 1; ≥19/21 after Phase 2; ≥20/21 after Phase 3
```

## Discipline rules

- After each phase, REPORT THE NUMBERS. Don't move forward without
  data.
- If a phase regresses parity, REVERT and fix the regression before
  proceeding.
- Don't try to combine phases. They're sized to be debuggable
  individually.
- The diagnostic toggles from Phase 1.3 are the source of truth for
  WHAT bug class is responsible — use them.
