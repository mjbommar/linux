# UML KVM v2 — Layer 17: SMP-T30 H2-narrow A/B INCONCLUSIVE

**Date:** 2026-05-03
**Production tip:** `c8eaaca8687d`
**Experimental binary:** `~/src/uml-builds/uml-smp-h2narrow/linux` (source reverted in working tree)
**Status:** **NOT LANDING.** Trend in the right direction but statistically inconclusive at n=30.

## Headline

opus-subagent TLB-mechanism audit (2026-05-03) proposed H2-narrow:
re-introduce `os_map_memory`/`os_unmap_memory` in
`kvm_v2_mm_region_added/removed` (commented out by `f77a31d1fbe4` on
2026-04-30) to test whether the dropped per-region map removed an
mmu-notifier-equivalent path for KVM TDP coherence.

A/B test results under matched concurrent fork-malloc load:

| Configuration  | mt-mini SMP T=8 ×30 | flake rate |
|----------------|---------------------|-----------|
| Production (`uml-smp`)        | 22 ALL_OK / 8 VERIFY_FAIL | 27% |
| H2-narrow (`uml-smp-h2narrow`) | 25 ALL_OK / 5 VERIFY_FAIL | 17% |

Difference: +10 pp. 95% binomial CI of difference includes zero
(`[-11pp, +31pp]`), so **the trend is not statistically significant
at this sample size.** A larger n (≥200) would distinguish, but the
expected payoff (closing some-but-not-all of mt-mini residual) does
not justify (a) the engineering cost of carrying H2-narrow as a
permanent change, (b) the risk of regressing other test classes that
were the original motivation for dropping `os_map_memory` (commit
`f77a31d1fbe4` documented mt-mmap-stress 0/30 → 19/30 from the
DROP — the reverse direction).

Regression check: threaded-fork-malloc × 5 boots × 4000 forks each
= **20000/20000 forks PASS, 0 fails, 0 aborts** under H2-narrow.
So the change wouldn't regress the dominant fork+exec workload, but
neither does it produce a clear win on mt-mini.

## Why we believed H2-narrow might fix mt-mini

The opus audit's H2-narrow hypothesis: with `os_map_memory` dropped,
KVM's TDP cache for slot 0 has no mmu_notifier callback to
invalidate when UML's pgd PTEs change. KVM walks via slot 0
(`uml_physmem`-based), but slot 0's per-page EPT entries can survive
a UML-side PTE change. Without a per-region memslot or a per-page
mmu_notifier hook, the only way KVM's TDP cache coherence is restored
is via the per-dispatch `CR4.PGE` toggle (which queues
`KVM_REQ_TLB_FLUSH_GUEST` → INVVPID before vmenter — invalidates
GVA→GPA TLB entries but not necessarily the EPT shadow at the
PML4-page level).

The mt-mini failure (`got=0 expect=N` after a same-thread
`memset`+`verify` back-to-back on a freshly-mmap'd page) is consistent
with the reader resolving its read via a stale slot-0 EPT entry that
maps the GVA to a stale physical page that was zero-aliased before
the writer's mmap landed.

## Why H2-narrow doesn't actually close it

Putting `os_map_memory` back means MAP_FIXED'ing each user-VA range
into the spawner mm at every region_added call. KVM's TDP walk now
finds an actual VMA at `userspace_addr=region->va` rather than going
through the slot-0 fallback — but this gets re-faulted in by KVM's
TDP just-in-time, not invalidated atomically with the PTE write.

So the deeper bug (UML PTE write ordering vs KVM TDP refill on a
different host CPU) is not addressed by H2-narrow alone. The ~10pp
trend probably reflects a smaller window for the race (per-region
mapping is more localized than slot-0 fallback) but not a closure.

## What would actually close it

The architectural fix is one of:

1. **Real mmu_notifier in v2** — register an `mmu_notifier_ops` on
   the spawner mm so KVM gets `invalidate_range_start/end` callbacks
   when UML's pgd PTEs change. This is what KVM normally relies on
   for TDP coherence under guest mm changes; v2 currently doesn't
   provide it because we don't go through KVM's standard memslot
   per-region path.
2. **Per-mm `KVM_SET_USER_MEMORY_REGION` per UML region (revert E.5)**
   — re-add per-region memslots so KVM has an explicit per-range
   coherence anchor. Costs the per-region churn that motivated E.5.
3. **Force a KVM-level TDP invalidate from UML's tlb.c drain** — add
   a backend op `tlb_invalidate_tdp(mm, va, len)` that issues
   `KVM_INVALIDATE_TDP_MMU` (KVM 6.6+ ioctl) or equivalent. Most
   targeted, doesn't add VMA churn.

All three are bigger lifts than H2-narrow. Defer to a future
SMP-T31+ task once the user prioritizes mt-mini residual over other
work.

## Don't repeat (lessons)

- **n=30 isn't enough to distinguish 10pp differences** for this
  test class. Future A/B's targeting marginal effects need n≥100.
- **Statistical thinking matters.** Initial 9/10 from a low-load
  isolation run looked like "90% means H2-narrow fixes it!" but the
  matched-load 25/30 reveals the effect was ~10pp, not 30pp.
- **Reversing a documented improvement is suspect.** `f77a31d1fbe4`
  showed dropping `os_map_memory` improved mt-mmap-stress from 0/30
  to 19/30. Re-adding it should have been weighed against that prior
  datapoint, not just the opus audit.
- **Audit + experiment + statistics > audit alone.** opus's hypothesis
  framing was good (concrete, testable, falsifiable). The empirical
  test was the right next step. The negative-but-trending result is
  a CONTRIBUTION, not a failure.

## Open: mt-mini residual remains deferred

Production-tip mt-mini SMP T=8 ~27% (n=30, matched load). Same
`got=0 expect=N` cross-vCPU TLB stale class documented in earlier
state-audit memos. Architectural fix path documented above; not
prioritized for this session. Does NOT gate any user-facing
workload (cpython-parity 21/21, threaded-fork-malloc 0/116000,
threaded-subprocess-wait 10/10, substrate=seccomp).
