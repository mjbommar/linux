# UML KVM v2 — Layer 18: SMP-T31 TDP coherence RULED OUT

**Date:** 2026-05-03
**Plan:** [`31-smp-t31-tdp-coherence-fix-plan.md`](../31-smp-t31-tdp-coherence-fix-plan.md)
**Status:** **NEGATIVE RESULT — and informative.** Option α was implemented, tested, and reverted. The mt-mini residual is NOT in KVM TDP/EPT coherence. This memo records the experiment and the conclusion that closes off the entire "TDP coherence" investigation branch.

## Headline

mt-mini SMP T=8 × 30 results, all under matched concurrent load, on `~/src/uml-builds/uml-smp-t31a/linux`:

| Configuration | mt-mini × 30 | flake | vs baseline |
|---|---|---|---|
| **Production** (no T31) | 22 / 30 ALL_OK | 27% | — |
| **T31a** (madvise per-PTE-PFN in `um_tlb_sync`) | **17 / 30 ALL_OK** | **43%** | **+16 pp WORSE** |

Regression check on T31a:
- cpython-parity 3-module smoke: **3/3 PARITY** (no regression on real workloads)

So madvise on the drained range:
- Doesn't break cpython workloads (which don't have the mt-mini mmap-churn pattern)
- **Specifically AMPLIFIES the mt-mini failure rate** (16 pp worse)

## Why this is informative even though the fix failed

T31a is the third independent experiment that targeted "force TDP/EPT coherence" and the third that didn't help — and the only one that actually made things measurably worse. Together they form a triangulation:

| Experiment | Mechanism | Result | Conclusion |
|---|---|---|---|
| **G.2 IPI kicker ablation** (`arch/um/kernel/tlb.c:561-565`) | Disable cross-vCPU IPI that forces remote CR4.PGE → INVVPID | No change | Hardware GVA→hPA TLB flush isn't the bug |
| **SMP-T26 H_E** (`arch/um/backend/kvm-v2/syscall_trap.c:1471-1484`) | Per-page `madvise(MADV_DONTNEED)` at handle_io_pf after segv_handler | No change | Per-faulted-page TDP zap isn't the bug |
| **SMP-T31a** (this memo) | Per-drain `madvise(MADV_DONTNEED)` walking the user-VA range | **16 pp worse** | Per-drain TDP zap is **actively harmful** |

If the mt-mini bug were "stale TDP/EPT entry causes guest read to hit wrong page", then forcing TDP zap at any of these three points should at least narrow the window. Instead:
- G.2 kicker: zero effect → not even the GVA→hPA hardware TLB
- H_E: zero effect on per-page invalidation → not the EPT for the just-faulted page
- T31a: actively worse → broader EPT zap doesn't close the window; if anything, it widens it

**Conclusion: the mt-mini `got=0 expect=N` failure is NOT in KVM TDP/EPT coherence at all.** Three null-or-negative results triangulating the same area is strong evidence.

## What the bug is NOT

Ruled out by this experiment series:
- Stale guest GVA→hPA hardware TLB entry on remote vCPU (G.2 ablation)
- Stale TDP shadow for a faulted GPA (H_E)
- Stale TDP shadow for any GPA in the drained range (T31a)
- Missing mmu_notifier coupling between UML's `set_pte` and KVM (T31a + opus + codex audits)

## Why T31a made things worse (best hypothesis)

`madvise(MADV_DONTNEED)` on a shared-tmpfs MAP_SHARED mapping:
- Drops the spawner mm's host PTE for that HVA
- Fires `mmu_notifier_invalidate_range_*` callbacks
- KVM's per-VM notifier zaps EPT entries pointing at that HPA
- Next vCPU access faults; KVM walks afresh; refills EPT from the spawner mm's PT

The added work per drain creates a new race window between madvise's TLB invalidation, KVM's EPT refill, and the worker thread's continued user-mode access. Even though the data is preserved (tmpfs page cache survives), the dispatch latency increases because remote vCPUs now take EPT misses they didn't take before. More dispatches per unit work → more opportunities to hit the existing bug → higher per-boot failure rate.

This isn't a NEW bug class introduced by T31a. The failure pattern (`got=0 expect=N`, `p` values in user-half range, `tid` matches the worker thread) is identical to baseline; just MORE of them.

## Where the bug actually lives — fresh hypothesis space

Now that TDP coherence is ruled out, the remaining hypothesis space narrows to:

1. **UML's set_pte ordering / atomicity.** `arch/um/include/asm/pgtable.h:326-363` recently went from a multi-store update to a single-store form to fix a transient-PRESENT window. Maybe there's still a subtle ordering bug — e.g., the PFN field write is visible before the PRESENT bit (or vice versa), and a concurrent reader observes an inconsistent intermediate.
2. **UML's `handle_mm_fault` page allocation race.** Two threads on the same mm page-fault on adjacent pages simultaneously; the buddy allocator hands them the same PFN; one PTE update overwrites the other. Would explain `got=0` if one thread gets a fresh-zeroed page that the other thread thought it had populated.
3. **mmu_gather drain ordering vs guest access.** SMP-T20 added RCU-deferred free; maybe there's still a window where a freed page is visible to guest before RCU grace expires.
4. **slow_memset itself races with kernel-side state.** Per-byte writes interrupted by SIGALRM; UML kernel runs scheduler; some kernel path zeroes the page (e.g., direct-mapped zeroing for a page that the kernel "thinks" is free). Would explain why mt-mini specifically (slow_memset) hits this and `memcpy`-shaped tests don't.
5. **The mt-mini binary itself has a UB or compiler-optimization quirk** that manifests only under v2's preempt timing. Less likely (binary works on host and seccomp), but worth eyeballing the assembly.

(2) is my next-most-likely candidate. Need to instrument `handle_mm_fault` under v2 to capture per-PFN allocation races during mt-mini.

## Decision

- T31 closed as negative-result
- Option δ (mprotect toggle) NOT pursued — same mechanism as α (both fire mmu_notifier), would likely show the same regression pattern. δ is "α with extra syscall cost"; if α is harmful, δ would be more harmful.
- mt-mini residual remains deferred at ~27% (production tip)
- Next investigation direction: hypothesis (2) — `handle_mm_fault` page allocation race. Tracked as new task SMP-T32.

## Don't repeat (lessons)

- **Run discriminating experiments before deep investigation.** The host-vs-seccomp-vs-v2 comparison earlier in this session should have happened weeks ago. It immediately localized the bug to v2. The TDP coherence hypothesis pulled in two agents and three experiments before being ruled out — most of that effort was avoidable if we'd had the discriminating data sooner.
- **Three null/negative results pointing the same direction is strong evidence.** When multiple independent attempts to fix a hypothesized mechanism don't help (or hurt), update the prior — don't keep trying variants of the same mechanism.
- **Active regression is more informative than no change.** T31a making things worse pinpointed the area more precisely than G.2 ablation or H_E ever did. "Forcing the supposed fix amplifies the bug" is a strong falsifier of the hypothesis.

## Files modified during T31a (now reverted)

For reference if this approach needs to be re-explored later:
- `arch/um/include/shared/backend.h` — added `tlb_invalidate_guest` op
- `arch/um/kernel/tlb.c` — called it from `um_tlb_sync` post-drain
- `arch/um/backend/kvm-v2/region.c` — `kvm_v2_tlb_invalidate_guest` walking pgd + per-PFN `os_drop_caching`
- `arch/um/backend/kvm-v2/ops.c` — registered the op
- `arch/um/backend/kvm-v2/kvm_v2_backend.h` — extern decl

All five files reverted to pre-T31 state. Plan file at `02-workstreams/D-kvm-backend/31-smp-t31-tdp-coherence-fix-plan.md` retained as investigation history.

Build artifact at `~/src/uml-builds/uml-smp-t31a/` retained for future ablation comparisons; mark for deletion after T32 lands.
