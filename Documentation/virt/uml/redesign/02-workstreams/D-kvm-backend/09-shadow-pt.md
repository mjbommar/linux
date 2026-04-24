# D-kvm-backend #M: shadow page table (D66 prerequisite)

**Status:** design memo (2026-04-24) — blocking memo-08
sub-commits #4 / #5b / #6 / #7 per decisions-log D66.
**Effort:** 3-4 weeks as its own sub-workstream.
**Dependencies:** D-03b (per-mm `kvm_um` context, landed),
memo 08 sub-commits #1-#5a (state materialization + SYSRETQ
bootstrap, landed), D66 (this memo's motivation).
**Blocks:** memo 08 #4 (HLT → scheduler), #5b (IDT), #6 (D-06
`getpid()` bookend), #7 (D-05 nested-virt fallback); D-06
bookend itself.
**Task:** #186.

## Why this memo exists

Sub-commit #5a's pgd-walk diagnostic (commit `e777e5274dfc`,
decisions-log D66) captured the smoking gun: UML's page-table
encoding is software-only. The entry `pgd[0] = 0xaff1e1`
observed under CR3 = `__pa(current->active_mm->pgd)` has bit
7 set — which UML interprets as `_PAGE_ACCESSED` (0x080) but
which x86_64 hardware treats as `PS` (reserved / huge-page,
illegal at PGD level). The CPU rejects the walk, triple-
faults, KVM_RUN returns `KVM_EXIT_SHUTDOWN`.

The observation is not that one entry happens to be malformed;
it's that **UML's entire PTE encoding is incompatible** with
what x86 hardware expects:

| Bit | UML meaning             | x86 hardware meaning |
|-----|-------------------------|----------------------|
| 0   | `_PAGE_PRESENT`         | `P`       ✓          |
| 1   | `_PAGE_NEEDSYNC`        | `R/W`                |
| 2   | (unused)                | `U/S`                |
| 5   | `_PAGE_RW`              | `A`                  |
| 6   | `_PAGE_USER`            | `D` / reserved       |
| 7   | `_PAGE_ACCESSED`        | `PS`                 |
| 8   | `_PAGE_DIRTY`           | `G` / reserved       |

UML's pgd is a software data structure maintained by UML's
own mm code for bookkeeping. Actual memory mappings come from
host-side `mmap`/`mprotect` via `mm_map`/`mm_unmap`, and the
HOST CPU walks the HOST process's pgd to service user-mode
accesses. UML's pgd is never walked by hardware in the
ptrace/seccomp backends.

The KVM backend needs the GUEST vCPU to walk a page table
that is hardware-compatible. That's the shadow PT this memo
scopes.

## Prior art: gVisor's KVM platform

gVisor solves the exact same problem in `pkg/sentry/platform/
kvm/address_space_amd64.go` + `machine.go`:

- Per-"mm" `addressSpace` struct holding a hardware-walkable
  4-level page table.
- Lazy population: faults in the guest cause `KVM_EXIT_MMIO`;
  the host-side fault handler walks the Sentry's logical
  memory-mapping record (`mm.MemoryManager`), and installs a
  matching PTE in the shadow PT.
- Updates on unmap: when the logical mm unmaps a range, the
  corresponding shadow PT range is invalidated + the KVM vCPU
  TLB is flushed.
- Bootstrap: the "sentry stub" (analog of our bootstrap page)
  is eagerly mapped at address-space construction time so
  guest entry can find it.

gVisor's platform-linux code is the reference blueprint;
this memo specifies the UML-specific plumbing.

## Scope

A per-mm shadow page table, maintained alongside UML's
logical pgd, that:

1. Is x86-hardware-walkable (standard PTE encoding).
2. Mirrors the UML logical mapping for the owning mm.
3. Gets the KVM-backend bootstrap page (GDT + LSTAR
   trampoline + SYSRET gadget) eagerly mapped at
   `kvm_mm_attach` time so SYSRETQ + GDT-load + LSTAR-entry
   can execute before any user code runs.
4. Is updated when UML's mm_map/mm_unmap install/remove
   logical mappings.
5. Is swapped as CR3 on context_switch (per-mm).

## Data-structure shape

New fields in `struct kvm_um` (per-mm KVM context, already
exists from D-03b):

```c
struct kvm_um {
    ...existing fields...

    /* Shadow page table (per-mm; memo 09). */
    struct page    *shadow_pgd_page;  /* 4 KiB x86 PGD */
    pgd_t          *shadow_pgd;       /* kernel VA view */
    u64             shadow_pgd_gpa;   /* __pa() for CR3 */
    /* TODO: rwsem / spinlock for shadow PT mutations;
     * TODO: page-cache of intermediate PUD/PMD/PTE tables. */
};
```

Shadow PT is a standard x86_64 4-level page table built with
`_PAGE_*` bits as the HOST kernel understands them (`<asm/
pgtable_types.h>` on x86 hosts, *not* UML's own software
encoding). Every intermediate table is a host page allocated
via `alloc_page(GFP_KERNEL)`; teardown reclaims them.

## Operation: three hot paths

### Path 1 — attach (`kvm_mm_attach`, called on new mm)

1. Allocate the shadow PGD page (`alloc_page(GFP_KERNEL |
   __GFP_ZERO)`).
2. Install fixed bootstrap mappings:
   - Bootstrap page (GDT + LSTAR + SYSRET + future IDT) at a
     fixed VA chosen to be outside TASK_SIZE but inside the
     first 4 GiB so short-offset instructions work.
   - The per-mm vCPU stack (when SMP lands; ncpus=1 for now).
3. Store `shadow_pgd_gpa = __pa(shadow_pgd)` for `kvm_enter_
   guest` to consume as CR3.

### Path 2 — fault-in (`kvm_decode_mmio` → walk UML pgd → install shadow PTE)

The current `kvm_decode_mmio` from sub-commit #3 dispatches
into UML's `segv_handler`. Extend it:

1. Before calling `sig_info[SIGSEGV]`, first attempt a
   shadow-PT fill: walk UML's logical `current->active_mm->pgd`
   for the faulting guest-VA. If the logical mapping is
   present + valid, translate UML's software bits to x86
   hardware bits, install the result in the shadow PT.
2. Re-enter KVM_RUN; guest retries.
3. If the logical mapping is NOT present: fall through to
   `segv_handler` (real fault, UML will signal SIGSEGV or
   do on-demand paging via its normal path; after the
   handler runs + `mm_map` installs the logical mapping,
   the guest retries → step 1 succeeds).

Bit translation:

```c
static u64 um_pte_to_x86_pte(u64 um_pte, bool is_pt_entry)
{
    u64 out = 0;
    if (um_pte & 0x001)  out |= _PAGE_PRESENT;     /* bit 0 */
    if (um_pte & 0x020)  out |= _PAGE_RW;          /* UML bit 5 → x86 bit 1 */
    if (um_pte & 0x040)  out |= _PAGE_USER;        /* UML bit 6 → x86 bit 2 */
    if (um_pte & 0x080)  out |= _PAGE_ACCESSED;    /* UML bit 7 → x86 bit 5 */
    if (um_pte & 0x100)  out |= _PAGE_DIRTY;       /* UML bit 8 → x86 bit 6 */
    /* _PAGE_NEEDSYNC (UML bit 1) has no x86 equivalent;
     * it signals UML's own sync machinery and is safe to drop. */
    out |= um_pte & PAGE_MASK;  /* preserve PFN */
    /* NX (bit 63) copies through with no transform. */
    out |= um_pte & (1ULL << 63);
    return out;
}
```

### Path 3 — unmap (`kvm_mm_unmap` callback)

UML's `mm_unmap` op invalidates a range. Shadow PT must
follow:

1. Walk shadow PT for the range; mark PTEs absent.
2. Issue a KVM-level TLB flush (`KVM_SET_TLB_FLUSH` or re-
   trigger a vCPU re-entry that implicitly flushes on CR3
   change; host-KVM version dependent).
3. Optionally free empty intermediate tables (not mandatory
   for correctness).

## Integration with existing memo-08 sub-commits

- **#1 (state materialization)**: `kvm_enter_guest` changes
  `cr3_gpa = __pa(current->active_mm->pgd)` →
  `cr3_gpa = current->active_mm->kvm_um->shadow_pgd_gpa`.
  Three-line change once #M lands.
- **#2a (LSTAR trampoline + MSRs)**: unchanged. Trampoline
  bytes live in bootstrap page; #M just ensures that page is
  reachable via the guest CR3 walk.
- **#2b (KVM_RUN loop + decode)**: unchanged in shape. The
  decode cases still dispatch syscall / HLT / MMIO / etc.
- **#3 (MMIO decode)**: extended per "Path 2" above — shadow
  PT fill attempted BEFORE fallback to `segv_handler`.
- **#5a (SYSRETQ bootstrap)**: unchanged — it needs the
  bootstrap page reachable, which #M provides.
- **#4 (HLT → scheduler)**: trivially correct once #M exists.
- **#5b (IDT install)**: writes IDT entries into the bootstrap
  page; #M ensures reachable.
- **#6 (D-06 perf bookend)**: directly measurable once #M
  works end-to-end.
- **#7 (D-05 fallback)**: unchanged — path fires when #M
  runs into unrecoverable trouble (e.g. nested virt).

## Risks + mitigations

- **Bit-translation completeness.** Missing a UML software
  bit with a hardware meaning causes silent corruption. **
  Mitigation:** table-driven translator with a KUnit test
  that exercises every UML bit + checks the x86 output +
  runs the CPU walk (under CONFIG_UM_BACKEND_KVM_INTEGRATED)
  with a synthetic pgd to confirm the hardware accepts it.
- **Shadow PT invalidation race.** If UML's mm_map runs while
  a vCPU is mid-KVM_RUN, the shadow PT could read a stale
  mapping. **Mitigation:** take the mm's existing
  `mmap_lock` for read during shadow-PT fills; mm_map/
  mm_unmap already hold it for write. KVM itself serializes
  per-vcpu.
- **TLB flush cost.** Every mm_unmap triggers a KVM TLB
  flush ioctl — syscall overhead. **Mitigation:** batch +
  coalesce flushes when multiple mm_unmap calls happen in
  quick succession; measure under D-06 + optimize if
  profiled.
- **Memory footprint.** One shadow PT per UML mm = 4 KiB
  PGD + O(mapped VA range / 512) for intermediate pages.
  For a typical userspace, <1 MiB per mm. **Mitigation:**
  share intermediate pages via copy-on-write when fork
  duplicates an mm (future optimization; v1 duplicates).
- **Locking in kvm_mm_detach.** Shadow PT teardown must not
  race with a still-running vCPU. **Mitigation:** already
  reference-counted via `kvm_um->mm_refcount` (D-03b);
  teardown happens only after the refcount drops to zero.

## Validation strategy

### Unit tests (KUnit, `arch/um/backend/contract/`)

- `kvm_shadow_pte_translate_test` — exercises every UML
  PTE-bit encoding through the translator; asserts the x86
  output matches a hand-derived expected value per case.
- `kvm_shadow_pgd_attach_test` — allocates a shadow PGD,
  installs the bootstrap page mapping, verifies the walk
  resolves via the test's own walker.
- `kvm_shadow_pgd_teardown_test` — verifies detach drops
  the refcount + frees intermediate pages.

### Selftest

Extend `tools/testing/selftests/um/kvm-smoke/run-kvm-smoke.sh`:

- After #M lands, the "sub-commit #3 pending" / SHUTDOWN
  markers should STOP firing on a fresh boot.
- New markers: "handle_syscall" (real syscall dispatched
  from a real init), "KVM_EXIT_HLT" (guest cleanly
  halted). Threshold rises from 2/6 → 4/6.
- An explicit "KVM_EXIT_MMIO" marker hit confirms shadow-PT
  fault-in is exercising on real user-code loads.

### End-to-end

`backend=kvm force=kvm init=/bin/true` should exit cleanly
(panic with "init returned", same as ptrace/seccomp paths)
once #M lands. That's the D-06 bookend's first requirement.

## Sequencing vs. other D-workstream work

#M is the biggest single remaining KVM lift. Sequence it
before any of #4/#5b/#6/#7 — those all require a working
CR3 walk.

Parallelizable with #M:
- C-08 Go driver interface fingerprint (off-tree, already
  pinned 2026-04-23 Phase VI Lift #6).
- Observability spine O2 (host-side metrics, unrelated).
- LKML hygiene series (ftrace-notrace-generic-v1 etc.).

Serial after #M, in preferred order:
- #5b IDT install (unblocks guest-side #PF handling).
- #4 HLT → scheduler niceties.
- #6 D-06 `getpid()` <100 ns bookend measurement.
- #7 D-05 nested-virt fallback.

## Alternative designs considered + rejected

- **Reuse UML's pgd directly** — D66's finding.
  Rejected: bit encoding incompatible.
- **Extend UML's pgd to also use x86 hardware bits** —
  would require rewriting every UML mm path + invalidating
  assumptions baked into UML's set_pte/set_pmd/etc.
  Rejected: scope creep ≫ shadow PT.
- **Run every vCPU with an identity-paged pgd + use EPT
  exclusively for isolation** — would require the guest to
  see raw host physical memory at VA=PA. Rejected: breaks
  the UML user/kernel boundary model; KVM's EPT alone
  doesn't give us per-process VA space separation at the
  granularity UML needs.
- **Copy-on-write from UML's pgd to shadow PT** —
  considered as an optimization but rejected for v1: fault-
  in is simpler + localized. Revisit if D-06 measurements
  show the fault-in rate is a bottleneck.

## Status flip criteria

This memo leaves "design memo" and becomes "implementation
in flight" when:

1. Shadow PT data structures land in `kvm_um` (per-mm).
2. `kvm_mm_attach` allocates the shadow PGD + installs
   bootstrap page mapping.
3. Unit test `kvm_shadow_pte_translate_test` lands + passes.

When path 1 + path 2 + path 3 all work and the kvm-smoke
selftest reports the "handle_syscall" + "KVM_EXIT_HLT"
markers, memo 09 rolls up into memo 08's #4-#7 sequence.

## Cross-references

- `04-risks/decisions-log.md` D66 — motivation.
- `02-workstreams/D-kvm-backend/08-real-run-userspace.md`
  — the memo this one unblocks.
- `arch/um/backend/kvm/thread.c` commit `e777e5274dfc` —
  diagnostic + finding reproducer.
- `arch/um/include/asm/pgtable.h` — UML's PTE-bit
  definitions.
- gVisor `pkg/sentry/platform/kvm/address_space_amd64.go`
  — prior-art blueprint.

## Out of scope for this memo

- SMP (ncpus>1) shadow PT coherence across vCPUs.
  Hand-wave for now (one shadow PT per mm; vCPUs
  serialize on mm entry); revisit when SMP KVM lands.
- Live migration. Out of scope for v1; never in scope for
  this fork per `00-vision.md`.
- Host-KVM EPT vs software shadow-PT hybrid optimization.
  v1 builds a pure software shadow PT; EPT-only approaches
  are a v2 optimization once baseline perf is measured.
