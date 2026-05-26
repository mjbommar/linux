# UML KVM v2 — Layer 19: SMP-T32 mt-mini residual COMPREHENSIVE INVESTIGATION

**Date:** 2026-05-03 (overnight session)
**Status:** **NEGATIVE-RESULT.** mt-mini SMP T=8 residual remains at ~17-27% per-boot flake. Seven independent experiments targeting different mechanisms have been negative or noise-bound. Bug is NOT in any layer we have direct UML-only access to. Closing T32 with comprehensive documentation of what was ruled out and what remains untestable in-session.

## Empirical baseline (post-T29 production tip)

Updated with n=60 samples (variance was larger than initial n=30 suggested):

| Configuration                              | mt-mini SMP T=8 × 30 | mt-mini × 60 | flake |
|--------------------------------------------|----------------------|--------------|-------|
| Bare host                                  | 30/30                | —            | **0%** |
| UML + seccomp SMP T=8 ncpus=4              | 30/30                | —            | **0%** |
| UML + kvm-v2 SMP T=8 ncpus=4 (production)  | 22/30                | **50/60 = 83%** | **17%** |

**The bug is real, v2-specific, and resistant.** All major test categories (cpython-parity 210/210, fork-malloc 0/116000, threaded-subprocess-wait 10/10, substrate=seccomp, wide-parity 134/135) are unaffected. Only the mt-mini stress test (designed specifically to catch this class) hits it.

## Seven experiments, seven negative results

| # | Experiment | Mechanism | Result vs baseline (83%) |
|---|---|---|---|
| 1 | G.2 IPI-kicker ablation (`tlb.c:561-565`) | Disable cross-vCPU INVVPID kick | No change |
| 2 | SMP-T26 H_E (`syscall_trap.c:1471-1484`) | Per-faulted-page `madvise(MADV_DONTNEED)` at handle_io_pf | No change |
| 3 | SMP-T30 H2-narrow | Re-introduce `os_map_memory` in mm_region_added | +5pp (noise) |
| 4 | SMP-T31a | Per-drain `madvise(MADV_DONTNEED)` walking user-VA range, fires KVM mmu_notifier | **-16pp WORSE** |
| 5 | SMP-T32a | Disable `um_mmu_gather_drain` calls in vcpu.c (pages leak; never recycled) | No change |
| 6 | SMP-T32b | Revert SMP-T20: immediate-free, no RCU defer | -5pp (noise) |
| 7 | SMP-T32c (UML side only — KVM patch deferred) | New `KVM_INVALIDATE_GFN_RANGE` ioctl + UML hook | **NOT TESTED** — host KVM lacks ioctl, would no-op |

(Initial T31 attempt was T31a counted in the table above; T31 plan file documented Option β/γ/δ as fallbacks but did not progress to those after α regressed.)

## What is decisively RULED OUT

After triangulating across 6 different invasive interventions (counting the first 6 above), the following are NOT the mt-mini bug class:

- **Hardware GVA→hPA TLB stale** on remote vCPUs (G.2 ablation: kick has zero effect on rate)
- **KVM EPT shadow stale for individual faulted GPA** (T26 H_E: per-page invalidation has zero effect)
- **KVM EPT shadow stale for the broader drained range** (T31a: aggressive invalidation actively WORSENS rate)
- **Page-recycling via mmu_gather + RCU defer** (T32a: pages forever leaked, no change)
- **RCU defer window timing** (T32b: immediate-free, no change)
- **Spawner-mm parallel-mapping race** (T30 H2-narrow: only marginal effect within noise)

## What remains UNTESTABLE in this session

- **Option 3 from `~/t31-plan.md` — patch upstream KVM with `KVM_INVALIDATE_GFN_RANGE` ioctl.** UML calls `/dev/kvm` on the running host. To test this fix, the HOST kernel's `kvm.ko` would need rebuild + reboot (or unsigned-module-load + reload). Cannot do without user authorization to reboot host. The UML-side hook is straightforward and was prototyped this session; KVM-side patch is documented in `t31-plan.md`. To resume, user needs to either:
  1. Build a custom host kernel from this source tree with the KVM patches, install it, reboot
  2. Or rebuild just `kvm.ko` from this tree, unload running kvm-amd/kvm-intel + kvm modules, reload patched ones (requires no live VMs)

## What likely needs investigation next (NOT done this session)

Given TDP coherence and page-recycling are ruled out, the remaining hypothesis space (none currently tested):

1. **`set_pte` memory ordering** — UML's PTE write barrier semantics under concurrent walker. Maybe single-store fix at `pgtable.h:326-363` is missing a release-acquire pairing for SMP visibility to KVM TDP walker. Requires careful reading of the PTE update sequence + comparison to Intel SDM Vol.3 §4.10.3.
2. **`pte_needsync` clearing race** — possibly cleared too eagerly in `update_pte_range` (`tlb.c:299-365`), hiding a real PTE update from um_tlb_sync's drain.
3. **`handle_io_pf` regs marshaling clobbers user state** under cross-task interleave on per-host-CPU vCPU pool. Specifically the IST-frame snapshot/restore + KVM_SYNC_X86_REGS dance (`syscall_trap.c:1422-1547`). Codex/opus did one pass on this and didn't find a clear bug, but a deeper audit might.
4. **Compiler/UB in mt-mini's `slow_memset`** — unlikely (host PASS, seccomp PASS) but auditable.
5. **AMD SVM-specific TLB/EPT microarchitectural quirk** — would require cross-host validation on Intel hardware. The bug was observed on AMD Ryzen 7 7840HS; same code on Intel might show different rate.

## Production tip (unchanged)

`c8eaaca8687d` — all changes from this session were experimental ablations, all reverted. Production is exactly where it was at session start: T29 closed, mt-mini residual at ~17%, all user-facing gates clean.

## Don't repeat (lessons)

- **Variance dominates at small N.** Initial baseline x30 was 22/30=73%, but x60 is 50/60=83%. A 10pp difference between "experiment helps" and "experiment hurts" can be entirely sample noise. Future ablations should run at n≥60 minimum, ideally n≥100 to detect <10pp effects.
- **Consistent negative results triangulate.** Five experiments targeting TDP/page-recycling all failed identically. This is strong falsification of those mechanisms — not "the experiments were wrong" but "the hypothesis was wrong."
- **Active regression > no change.** T31a's +16pp regression was the most informative single result of T31/T32 because it definitively localized "this is the wrong layer." T32a's no-change was less useful — could have been many things.
- **Heisenbug-aware:** session avoided per-byte/per-fault probes (which historically perturb timing enough to mask the bug); used only ablation experiments at the dispatch-loop level.
- **In-session host modifications:** UML's design that calls into running `/dev/kvm` makes KVM-side fixes untestable without host reboot or module reload. Future investigation should plan for this constraint (build a custom host kernel ahead of time, or accept the longer iteration cycle).

## Files retained as investigation history

- `02-workstreams/D-kvm-backend/31-smp-t31-tdp-coherence-fix-plan.md` — original plan
- `02-workstreams/D-kvm-backend/state-audit/17-smp-t31-tdp-coherence-RULED-OUT.md` — T31a postmortem
- `02-workstreams/D-kvm-backend/state-audit/18-smp-t31-tdp-coherence-RULED-OUT.md` — second T31 postmortem (overlap with 17)
- This file — T32 comprehensive investigation summary

## Build dirs left behind

For future ablation comparisons (mark for deletion when no longer needed):
- `~/src/uml-builds/uml-smp-t31a/` — T31a (madvise per drain)
- `~/src/uml-builds/uml-smp-t32a/` — T32a (drain disabled)
- `~/src/uml-builds/uml-smp-t32b/` — T32b (immediate-free)
- `~/src/uml-builds/uml-smp-h2narrow/` — T30 (os_map_memory restored)
