---
title: Memo 17 — multi-task residual diverge synthesis + fix plan
date: 2026-04-26
status: ACTIVE — fixes in flight
inputs:
  - 01-per-task-vcpu-state.md (Agent 1, per-task vCPU state)
  - 02-cross-mm-shadow-design.md (Agent 2, shadow PT design)
  - 03-empirical-failure-modes.md (Agent 3, empirical bisection)
  - 04-tlb-invalidation-discipline.md (Agent 4, TLB-flush mechanism)
---

# Synthesis

## Where we are

Post-keystone (`901213a8d2d1`), cpython parity gate is **17/21**.
Five modules still diverge under KVM:

- `test_struct.test_endian_table_init_subinterpreters`
- `test_decimal` DocTestCase 731
- `test_bytes.FreeThreadingTest.test_free_threading_bytearrayiter`
- `test_list.test_repr_deep` (NULL-deref during deep recursion)
- `test_set.TestWeirdBugs.test_merge_and_mutate`

Plus `import unittest` is 8/10 — residual ~20% flake on a single
Python process.

## Convergent root-cause picture

Three of the four agents independently arrived at the same
mechanism for the residual flakes:

> **The keystone fix was incomplete on both sides of the
> producer/consumer pair.** It correctly added `WRITE_ONCE` +
> `smp_wmb` to the direct-sync writer (`shadow_sync.c::kvm_shadow_sync_pte`),
> but six parallel sites in `lifecycle.c` still write
> `shadow->dirty = true` (and `shadow->synced`) via plain stores.
> Symmetrically, the consumer in `kvm_enter_guest`'s SREGS-skip
> predicate uses `READ_ONCE` with no `smp_rmb` pair. Under
> SIGALRM-driven preemption, the consumer can read stale
> `dirty=false` after a producer leaf-write but before its
> dirty-store retires, take the SREGS-skip, and KVM_RUN with
> stale TLB.

The fourth agent (empirical) confirms the signature: every captured
fault prints `needs_full_resync=0` (kernel believes shadow is in
sync) and the user-space data is corrupted with the host-physmem
0xAA initial-fill pattern (page reads stale before the install
becomes visible). Pure-C pthread harnesses are 100% green —
threading per se is not the bug; the bug is shadow-PT-update
ordering exposed more often by threading because SIGALRM fires
more often.

Agent 1 finds an ADDITIONAL, independent hazard:
**`KVM_GET/SET_FPU` only saves the legacy 512 B FPU area** — YMM
upper-128 (AVX), ZMM upper-256, opmask, and PKRU live in the
XSAVE area beyond `kvm_fpu` and are NEVER round-tripped on
context switch. `KVM_GET/SET_XSAVE2` covers all of them. This
is independent of the memory-ordering bug above and is the
likely mechanism behind the bug-B "TLS slot reads as 0xff..ff"
(actually FS_BASE-keyed AVX register file pollution via TLB).

Agent 4 also flags my CR3-toggle keystone hack as architecturally
fragile and proposes **CR4.PGE-toggle** as the disciplined
replacement: one ioctl instead of two, no risk of failing
`kvm_vcpu_is_legal_cr3` validation under future CR3 layout
changes (PCID, LAM), same `KVM_REQ_TLB_FLUSH_GUEST` outcome via
`mmu_reset_needed=1` on CR4 mismatch at
`arch/x86/kvm/x86.c:12487-12488`.

## Specific code citations

### Producer-side memory-ordering gap (Agent 2 + 4)

Six sites in `arch/um/backend/kvm/lifecycle.c` write the dirty/synced
fields via plain stores:

- **lifecycle.c:1261** — `kvm_shadow_fill_from_uml_pgd` clear-pass
  sets `shadow->dirty = true` after clearing user-half stale leaves
- **lifecycle.c:1376-1377** — same fn install-pass sets
  `shadow->synced = true; shadow->synced_pgd_va = (u64)pgd_va;`
- **lifecycle.c:1441** — `kvm_shadow_map_page` sets `shadow->dirty = true`
  after a leaf install (this is the lazy-fill path, called from
  `kvm_enter_guest`'s repair pass)
- **lifecycle.c:1494** — `kvm_shadow_pgd_clear_user` (now a stub
  post-#275, but still touches dirty)
- **lifecycle.c:1560-1561** — `kvm_shadow_invalidate_va_range` sets
  `shadow->dirty = true; shadow->synced = false;` after clearing leaves

Each must mirror the keystone-fix discipline: `smp_wmb` between the
last leaf write and the dirty-flag store, and `WRITE_ONCE` for
the dirty/synced writes themselves.

### Consumer-side memory-ordering gap (Agent 4)

In `arch/um/backend/kvm/thread.c`:

- **thread.c:2235-2240** — SREGS-skip predicate reads
  `READ_ONCE(shadow->dirty)` with no preceding `smp_rmb`
- **thread.c:2150** — analogous skip-fill predicate reads
  `READ_ONCE(shadow->needs_full_resync)`

Both should be `smp_load_acquire` (or `READ_ONCE` after an
explicit `smp_rmb`) to pair with the producer-side `smp_wmb`.

### Stale TLB-flush keystone hack (Agent 4)

- **thread.c:2339-2351** — current CR3 XOR sentinel hack. Replace
  with CR4.PGE-toggle sentinel: one ioctl, cleaner failure semantics.

### vCPU XSAVE state leak (Agent 1)

- **thread.c:142-172** — `kvm_fpu_save_for_task` / `kvm_fpu_restore_for_task`
  use `KVM_GET/SET_FPU` (legacy 512 B). Replace with
  `KVM_GET/SET_XSAVE2` (full XSAVE area).
- Plus enable CR4.OSXSAVE in `sregs.c:261` and program XCR0 = 0x7
  via `KVM_SET_XCRS` (Agent 1 §4.1, "G-AVX fix recipe").

## Fix plan (in order)

### Phase A — Memory-ordering keystone (Race-I fix) — IMMEDIATE

Estimated: ~30 LOC, ~30 minutes work + parity gate run.

1. `lifecycle.c`: convert all 6 dirty/synced writes to
   `WRITE_ONCE`, with `smp_wmb` paired with the preceding leaf write.
2. `thread.c::kvm_enter_guest`: change the two consumer-side
   READ_ONCE reads to `smp_load_acquire` (or add explicit smp_rmb).

This is the most likely candidate to move parity 17 → 20-21.

### Phase B — CR4.PGE-toggle replacement for CR3-toggle hack

Estimated: ~10 LOC.

1. Replace the CR3 XOR sentinel at `thread.c:2339-2351` with a
   CR4.PGE-bit-toggle sentinel.
2. Update the keystone comment to cite the actual KVM code path
   (`__set_sregs_common` → `mmu_reset_needed` →
   `KVM_REQ_TLB_FLUSH_GUEST`) instead of the wrong `kvm_set_cr3` claim.
3. Stop swallowing the first ioctl's return code.

This is a hygiene improvement that should not change parity but
makes future changes safer.

### Phase C — Validate with parity gate

Build, run cpython-parity gate, run `read_test5`, run `import re`/
`import unittest` × 10 each. Acceptance: parity ≥ 18/21 (Agent 4
predicts 18+; Agent 2 predicts 21).

### Phase D — XSAVE2 + VCPU_EVENTS (per-task vCPU state) — IF NEEDED

Estimated: ~150 LOC.

Only execute if Phase A+B don't reach 21/21. Per-task XSAVE2 +
KVM_GET/SET_VCPU_EVENTS in `kvm_context_switch` (Agent 1 §3 and §4).

### Phase E — Empirical regression suite

Capture the 14 Python harnesses from Agent 3 into
`tools/testing/selftests/um/cpython-parity/` so the harness flake
rate is tracked across future changes.

## Out of scope (for now)

- **Per-mm host worker (memo Phase 4 Option B)** — Agent 2's analysis
  shows this is real architectural correctness for parent-VA
  contamination but is not blocking the parity gate. Defer until 1-4
  above are insufficient.
- **Per-page INVLPG via gadget injection** (Agent 4 Option d) — only
  beats full flush above ~500 invalidations per entry. Not warranted.
- **Memo 15 Phase 3 (collapse fill out of hot path)** — clean
  architectural improvement, but defer until Phase A+B prove the
  current architecture is workable.

## Order of operations

1. Write this synthesis. ✓ (you are here)
2. Apply Phase A (lifecycle.c + thread.c memory ordering). Build. Test.
3. Apply Phase B (CR4.PGE replacement). Build. Test.
4. Run parity gate. Decide whether Phase D is needed.
5. Commit each phase as a separate commit on `uml-redesign-plan`.

The empirical harnesses from Agent 3 (`/tmp/uml-diag/h_*.py`,
`cthread.c` etc.) become the regression suite for each phase.
