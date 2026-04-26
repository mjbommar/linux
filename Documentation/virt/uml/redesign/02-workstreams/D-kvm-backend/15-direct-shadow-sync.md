# Alternative KVM Architecture: Direct Shadow Synchronization

Status: alternative design / replacement candidate

Date: 2026-04-26

Scope: integrated UML/KVM backend memory coherency, especially the
remaining CPython parity failures where initialized userspace objects are
later observed with NULL or stale fields.

## Summary

The current KVM design has improved through several local fixes, but the
remaining failures are better explained as a structural coherency problem than
as one missing bookkeeping update.

Today there are three memory views that must agree:

- UML's logical page tables, mutated by generic mm code.
- The host process virtual mappings, updated later through `um_tlb_sync()`.
- The KVM shadow page table, invalidated indirectly and refilled later.

The proposed architecture makes UML's logical PTE mutation the single
authoritative event. When a PTE changes, the KVM shadow for that exact UML
`mm` is updated or fail-safe-cleared synchronously, before the mutating helper
returns. Full shadow walks remain available for initial attach, debug audit,
and recovery from allocation failure, but they stop being the hot-path
correctness mechanism.

This is an incremental architecture. It keeps the existing one-KVM-VM,
per-UML-mm shadow-PGD model, but replaces the deferred chain

```
set_pte_at() -> _PAGE_NEEDSYNC -> um_tlb_sync() ->
ops.mm_map/mm_unmap -> kvm_shadow_invalidate_va_range() ->
kvm_enter_guest() full fill
```

with a direct shadow update path:

```
PTE mutation -> kvm_shadow_sync_pte(mm, va, new_pte) ->
next KVM entry consumes shadow->dirty and flushes CR3 if needed
```

## Current Failure Model

The current code has several signs of a split-brain memory model.

- `arch/um/include/asm/pgtable.h:270:set_pte()` marks every PTE update with
  `_PAGE_NEEDSYNC`, then `arch/um/include/asm/pgtable.h:300:set_ptes()` only
  records a pending range with `um_tlb_mark_sync()`.
- `arch/um/kernel/tlb.c:173:um_tlb_sync()` later walks that pending range and
  calls backend `mm_map`/`mm_unmap`. This means PTE mutation and shadow mutation
  are separated by scheduling, fault recovery, and context-switch ordering.
- `arch/um/backend/kvm/mm.c:119:kvm_mm_map()` and
  `arch/um/backend/kvm/mm.c:183:kvm_mm_unmap()` still use host `mmap`/`munmap`
  as the trigger point for shadow invalidation. That makes the KVM shadow a
  side effect of maintaining another address-space representation.
- `arch/um/backend/kvm/thread.c:1675:kvm_enter_guest()` still contains the
  expensive repair step: map bootstrap aliases, then at
  `arch/um/backend/kvm/thread.c:1952:kvm_shadow_fill_from_uml_pgd()` refill the
  shadow from the logical pgd when the cache says it is stale.
- `arch/um/backend/kvm/lifecycle.c:1126:kvm_shadow_fill_from_uml_pgd()` tries
  to repair stale leaves transactionally, but it is still a derived full-tree
  reconstruction instead of the original mutation event.
- `arch/um/backend/kvm/lifecycle.c:1057:kvm_um_pte_to_x86()` now handles the
  dirty-bit half of UML's software A/D model by withholding `RW` from clean
  writable PTEs. That is the right direction, but the accessed-bit half is
  still not modeled cleanly.
- `arch/um/backend/kvm/lifecycle.c:1345:kvm_shadow_map_page()` forcibly ORs in
  `KVM_X86_PTE_P | KVM_X86_PTE_A` at the leaf. That helper cannot represent
  "mapped in UML but intentionally inaccessible until an accessed fault". It
  also means future translator changes that return a non-accessed leaf can be
  silently overridden.
- `arch/um/backend/kvm/lifecycle.c:1540:kvm_shadow_audit_va()` masks A/D bits
  during comparison. That is reasonable for hardware-maintained x86 A/D bits,
  but risky for UML because `_PAGE_ACCESSED` and `_PAGE_DIRTY` encode software
  permission state, not just observational status.

The user-visible symptom is consistent with this model: CPython reaches code
like `PyMethod_New` or `PyList_Append` and then reads a zero or stale
`ob_type`. That is not a natural syscall-return corruption pattern. It is what
we expect when writes went to one physical page, later reads used a stale
shadow leaf, or page reclaim/COW treated a page as clean or old because the
shadow let KVM bypass UML's emulated A/D faults.

## Bookkeeping Versus Architecture

Some bugs in this area are simple bookkeeping bugs:

- wrong target shadow, such as using `current` instead of the target `mm_id`;
- failing to clear `shadow->synced` after invalidation;
- failing to mark `shadow->dirty` before a needed CR3 reload;
- advancing batched PTE PFNs incorrectly;
- marking `_PAGE_NEEDSYNC` drained after a backend failure.

Those are real, and many have already been fixed.

The deeper problem is that the architecture makes these bugs likely. Correctness
is distributed across PTE helpers, pending range coalescing, host `mmap`, KVM
shadow invalidation, full shadow refill, CR3-cache invalidation, and page-fault
recovery. A missed edge in any one of those layers can leave a stale executable
shadow mapping even though the UML pgd is correct.

The direct-sync design narrows the invariant:

> Before returning to KVM_RUN, every shadow leaf for the current mm is either
> protection-equivalent to the UML PTE or absent in a fail-safe way that will
> fault and repair. It must never retain an older PFN or more-permissive write
> permission than the current UML PTE.

## Proposed Architecture

### 1. Add a Direct Shadow Sync API

Add a small KVM-only synchronization API, preferably in a new file:

- `arch/um/backend/kvm/shadow_sync.c`
- declarations in `arch/um/backend/kvm/kvm_backend.h`
- lightweight call stubs in a new `arch/um/include/asm/kvm_mmu_sync.h`

Suggested internal API:

```c
int kvm_shadow_sync_pte(struct mm_struct *mm, unsigned long addr, pte_t pte);
int kvm_shadow_sync_pteval(struct mm_struct *mm, unsigned long addr, u64 pteval);
int kvm_shadow_clear_pte(struct mm_struct *mm, unsigned long addr);
int kvm_shadow_clear_range(struct mm_struct *mm, unsigned long start,
			   unsigned long end);
int kvm_shadow_resync_range(struct mm_struct *mm, unsigned long start,
			    unsigned long end);
```

The public helper used from inline pgtable code must be fail-safe because
`set_ptes()` cannot return an error. The rule should be:

1. Resolve `mm->context.id.kvm_shadow`.
2. Under `shadow->fill_lock`, clear any existing leaf for `addr` first.
3. Translate the new UML PTE into the expected KVM shadow state.
4. If the new state should be present, allocate missing intermediate tables and
   install the leaf.
5. On allocation failure, leave the leaf absent, set `shadow->synced = false`,
   set `shadow->dirty = true`, record `shadow->last_sync_errno`, and let
   `kvm_enter_guest()` repair or return the error before KVM_RUN.

The important safety property is that a direct-sync failure cannot leave the
old mapping in place.

### 2. Translate UML PTEs By Protection Semantics

Rename or wrap `kvm_um_pte_to_x86()` so the contract is explicit:

```c
enum kvm_shadow_leaf_action {
	KVM_SHADOW_LEAF_ABSENT,
	KVM_SHADOW_LEAF_PRESENT,
};

enum kvm_shadow_leaf_action
kvm_um_pte_to_shadow(u64 um_pte, u64 *gpa, u64 *flags);
```

The translation should match `arch/um/kernel/tlb.c:52:update_pte_range()` and
UML's software A/D semantics, not a literal bit mapping.

Recommended v1 semantics:

- If `_PAGE_PRESENT` is clear, shadow leaf is absent.
- If `_PAGE_PROTNONE` is set, shadow leaf is absent.
- If `_PAGE_ACCESSED` is clear, shadow leaf is absent. x86 cannot represent a
  present user leaf that denies reads, so absence is the safest way to force a
  #PF and let UML mark the PTE young. This is intentionally stricter than the
  current host-VA path, which can ask `mmap()` for execute without read/write;
  a KVM hardware page-table leaf has no equivalent "execute but still force the
  UML accessed fault" state.
- If `_PAGE_ACCESSED` is set, install a present leaf.
- Set `US` only when `_PAGE_USER` is set.
- Set `RW` only when both `_PAGE_RW` and `_PAGE_DIRTY` are set.
- Preserve NX if UML ever sets it.
- Treat shadow A/D bits as local hardware status. They are not authoritative
  for UML PTE state.

This requires changing `kvm_shadow_map_page()` or adding a lower-level helper
that does not unconditionally OR `KVM_X86_PTE_A` into leaf entries. It is fine
for non-leaf page-table entries to stay permissive. Leaf entries must preserve
the translated permissions exactly.

### 3. Hook All PTE Mutation Paths, Not Only `set_ptes()`

The first implementation should hook `arch/um/include/asm/pgtable.h:300:set_ptes()`.
That captures `set_pte_at()` because generic `include/linux/pgtable.h` maps
`set_pte_at()` to `set_ptes(..., 1)`.

That is necessary but not sufficient. UML currently relies on generic helpers
for several mutation paths:

- `mm/pgtable-generic.c:70:ptep_set_access_flags()`
- `include/linux/pgtable.h:493:ptep_test_and_clear_young()`
- `include/linux/pgtable.h:610:ptep_get_and_clear()`
- `mm/pgtable-generic.c:97:ptep_clear_flush()`
- `include/linux/pgtable.h:1032:ptep_set_wrprotect()`
- `include/linux/pgtable.h:1566:ptep_modify_prot_start()`
- `include/linux/pgtable.h:1579:ptep_modify_prot_commit()`

Most generic helpers eventually call `set_pte_at()` or `pte_clear()`, so a
careful audit may show that `set_ptes()` plus `pte_clear()` coverage is enough
for v1. Do not assume that. Add KUnit or boot-time counters proving every PTE
transition observed by `um_tlb_mark_sync()` is also observed by direct shadow
sync. Higher-level clears (`pmd_clear()`, `pud_clear()`, `p4d_clear()`, and
`pgd_clear()`) must also map to `kvm_shadow_clear_range()` when they remove an
entire subtree; otherwise a parent-level teardown can bypass the per-PTE direct
path and leave stale leaves below it.

If gaps remain, define UML arch overrides:

```c
#define __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
#define __HAVE_ARCH_PTEP_TEST_AND_CLEAR_YOUNG
#define __HAVE_ARCH_PTEP_GET_AND_CLEAR
#define __HAVE_ARCH_PTEP_SET_WRPROTECT
#define __HAVE_ARCH_PTEP_MODIFY_PROT_TRANSACTION
```

Each override should update the UML PTE and direct-sync the exact affected VA.
For range/batch helpers, prefer a batched shadow operation so mapping a large
file does not take a lock/unlock per page.

### 4. Keep Deferred TLB Sync For Host VA Only

Do not delete `_PAGE_NEEDSYNC` or `um_tlb_sync()` immediately. The existing
ptrace/seccomp backends still use the deferred host-VA/stub mapping model, and
KVM may still need host mappings while residual direct-deref paths are audited.

Instead, split responsibilities:

- Direct shadow sync maintains KVM's CR3 view.
- `um_tlb_sync()` maintains host VA mappings for backends that need them.
- KVM `mm_map`/`mm_unmap` stops being the primary shadow invalidation path.

In v1, `arch/um/backend/kvm/mm.c:119:kvm_mm_map()` may continue to call
`os_map_memory()` and then call a conservative shadow clear as a safety net.
Once direct-sync coverage is proven, the KVM shadow invalidation in
`kvm_mm_map()`/`kvm_mm_unmap()` should become an assertion/audit path rather
than the normal mechanism.

### 5. Change KVM Entry From Repair To Verification

`arch/um/backend/kvm/thread.c:1675:kvm_enter_guest()` should eventually stop
running a full `kvm_shadow_fill_from_uml_pgd()` as part of normal entry.

The new entry discipline:

1. Ensure memslot and bootstrap/gadget pages exist.
2. Ensure fixed bootstrap aliases are mapped in the current mm's shadow.
3. If `shadow->last_sync_errno` or `shadow->needs_full_resync` is set, repair
   by targeted range sync or full pgd walk before KVM_RUN.
4. If `shadow->dirty`, reload CR3 through `KVM_SET_SREGS` to flush the vCPU TLB.
5. In debug builds, audit a bounded number of recently changed VAs.

`kvm_shadow_fill_from_uml_pgd()` remains valuable, but only as:

- initial shadow population for a newly attached mm;
- fallback repair after direct-sync allocation failure;
- explicit debug/audit command;
- panic-time diagnostics.

### 6. Add Mutation-Centric Observability

The current logs mostly observe repair points. Add observability at the source
of truth: the PTE mutation.

Recommended trace fields:

- `mm`
- `shadow`
- `addr`
- old UML PTE
- new UML PTE
- old shadow PTE
- new shadow PTE
- action: clear, install, absent-old, absent-protnone, absent-nosync,
  alloc-failed, skipped-backend-inactive
- caller or static callsite tag

Implementation options:

- proper tracepoints under `include/trace/events/uml_kvm.h`;
- or default-off dynamic-debug/ring-buffer logging in `shadow_sync.c`.

Add counters to `struct kvm_shadow_mm`:

```c
u64 direct_sync_install;
u64 direct_sync_clear;
u64 direct_sync_absent;
u64 direct_sync_alloc_fail;
u64 direct_sync_repair_full;
int last_sync_errno;
bool needs_full_resync;
```

The counters should be dumpable on parity-gate failure. The question we need
to answer quickly is: did the corrupt VA ever receive a direct shadow update,
and what did the shadow leaf become?

## Host VA Mapping Policy

The KVM backend still has a larger architectural question around host virtual
address mappings.

`arch/um/backend/kvm/mm.c:119:kvm_mm_map()` maps user pages into the UML host
process with `MAP_FIXED` via `os_map_memory()`. That is dangerous because the
parent process also contains the UML kernel binary, libc, stacks, and allocator
state. It is also unlike the natural KVM model, where the vCPU should execute
through KVM's memslot and shadow CR3, while the host kernel should access user
memory by walking UML PTEs to physical pages.

There are two plausible v2 directions:

### Option A: KVM-Native Uaccess

Make KVM builds stop depending on host user VA mappings. Audit all user access
and route it through UML PTEs and physical pages, matching the existing shape in
`arch/um/kernel/skas/uaccess.c:46:maybe_map()` and
`arch/um/kernel/skas/uaccess.c:64:do_op_one_page()`.

Likely code changes:

- Add KVM-specific assertions that `raw_copy_from_user()`,
  `raw_copy_to_user()`, `strncpy_from_user()`, `strnlen_user()`, and futex
  atomics never dereference user VAs directly.
- Audit syscall and signal-frame code for direct user-pointer dereferences.
- Make `kvm_mm_map()` skip `os_map_memory()` after direct-sync coverage is
  proven, but keep a debug mode that compares host-VA and PTE-walk behavior.
- Keep ptrace/seccomp unchanged.

This is the cleaner long-term architecture if the direct-deref audit is small.

### Option B: Per-mm Host Mapping Worker

Reintroduce an isolated host address space per UML mm, closer to the
ptrace/seccomp stub-child model, and keep KVM's shadow CR3 separate.

Likely code changes:

- Extend `struct mm_id` with a KVM host-mapping worker identity.
- Move `os_map_memory()`/`os_unmap_memory()` for KVM into that worker instead
  of the parent UML process.
- Add an RPC path for rare operations that genuinely require host-VA access.
- Keep KVM_RUN in the parent process and keep shadow CR3 as the vCPU view.

This is heavier and risks recreating much of the existing stub machinery. It is
only attractive if the direct-deref audit shows many unavoidable host-VA users.

Recommendation: implement direct shadow sync first, then attempt KVM-native
uaccess. Only build the per-mm host worker if direct physical-page uaccess
turns out to be impractical.

## Concrete Implementation Plan

### Phase 0: Current-State Diagnostics

- Add a default-off tracepoint around `kvm_um_pte_to_x86()` or the new
  translator.
- Add a one-shot audit mode that checks recently mutated VAs after every
  `kvm_enter_guest()` skip-fill path.
- Extend `kvm_shadow_audit_va()` to support two comparisons:
  hardware-equivalent and UML-protection-equivalent. The latter must not mask
  UML A/D semantics.

### Phase 1: Exact Leaf Helpers

- Add `kvm_shadow_lookup_leaf(shadow, va, &spte, create)` to share the walk
  logic currently embedded in `kvm_shadow_map_page()` and
  `kvm_shadow_invalidate_va_range()`.
- Add `kvm_shadow_install_leaf_exact()` that installs exactly the translated
  leaf flags and does not force `A`.
- Change `kvm_shadow_map_page()` to be an explicit bootstrap/helper API, or
  make its "force P|A" behavior opt-in and impossible to call from UML PTE sync.
- Add `kvm_um_pte_to_shadow()` with the protection semantics above.

### Phase 2: Direct Sync From PTE Mutation

- Add `arch/um/include/asm/kvm_mmu_sync.h` with inline no-op stubs when KVM
  integrated is disabled or not selected.
- Call the helper from `arch/um/include/asm/pgtable.h:300:set_ptes()`.
- Add direct coverage for `pte_clear()` or replace the macro with an inline
  helper that calls `kvm_shadow_clear_pte()`.
- Add direct range-clear coverage for parent-level page-table clears or prove
  that existing `flush_tlb_*()` paths always reach `kvm_shadow_clear_range()`
  before KVM_RUN.
- Audit the generic helper paths listed above and add UML overrides where the
  `set_ptes()`/`pte_clear()` hooks do not cover the mutation.

### Phase 3: Make Entry Trust The Direct State

- In `kvm_enter_guest()`, only call `kvm_shadow_fill_from_uml_pgd()` when
  `shadow->needs_full_resync` or `shadow->last_sync_errno` is set.
- Keep the current full fill behind a boot knob for bisectability:
  `kvm_shadow_force_full_fill=1`.
- Treat an unexpected fill/audit divergence as a correctness bug and log the
  last mutation records for that VA.

### Phase 4: Decouple Host VA Mapping

- Make `kvm_mm_map()`/`kvm_mm_unmap()` stop serving as shadow invalidation
  triggers.
- Add a boot knob that disables KVM parent-process `os_map_memory()` while
  retaining direct shadow sync.
- Run the CPython parity suite both ways. If disabling host mapping still
  fails, inspect the trace to find direct host-VA consumers and decide between
  KVM-native uaccess and a host worker.

### Phase 5: Broader State Completeness

This page-table architecture does not solve all KVM correctness by itself. Keep
separate work items for:

- FPU/XSTATE save/restore or conservative CPUID masking;
- FS/GS readback if FSGSBASE is ever exposed;
- signal frame and `sigreturn` state completeness;
- SMP/vCPU ownership if `ncpus > 1` is enabled later.

## Validation Plan

Add focused tests before widening the parity gate.

- Translator KUnit:
  - absent UML PTE -> absent shadow;
  - PROT_NONE -> absent shadow;
  - old/accessed-clear PTE -> absent shadow;
  - clean writable PTE -> present read-only shadow;
  - dirty writable PTE -> present writable shadow;
  - NX preservation.
- Shadow sync KUnit:
  - stale present leaf is cleared before any install attempt;
  - allocation failure leaves no stale leaf;
  - direct sync marks `dirty` and clears `synced`;
  - exact leaf helper does not force `A`.
- Mutation coverage tests:
  - `set_pte_at()`;
  - `ptep_set_access_flags()`;
  - `ptep_test_and_clear_young()`;
  - `ptep_get_and_clear()`;
  - `ptep_set_wrprotect()`;
  - `mprotect()`-style modify-prot transaction.
- Runtime tests:
  - anonymous page read then write, validating first write faults and dirty
    transition reaches the shadow;
  - COW private mapping write;
  - `mmap`/`mprotect`/`munmap` stress with VA reuse;
  - CPython `hashlib` and `import unittest` parity gates.

## Recommendation

The next serious fix should be direct shadow synchronization, not another round
of cached-fill heuristics.

The current design has accumulated enough guardrails to show the problem:
correctness depends on delayed reconstruction of a page table that already had
a precise mutation event earlier. Moving the KVM shadow update to that event
reduces the number of states that can diverge, gives us better logs at the
point of corruption, and lets `kvm_enter_guest()` become a verifier and TLB
flush point instead of a best-effort repair pass.
