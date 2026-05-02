# UML KVM v2 — Layer 14: SMP-T26 glibc heap corruption residual CHARACTERIZED

**Date:** 2026-05-02
**Tip with diagnostics:** `6a3b989cd7d5` (branch `umlctl-deploy`, post-T25)
**Status:** **CHARACTERIZED, not closed.** Residual ≈5% (1/20) on
threaded-subprocess-wait.py. Different bug class from Bug B; T25
LSTAR-EINTR fix is irrelevant here. Strong correlation with
KVM_V2_TLB_LAG counters in the thousands.

## Problem statement

Post-T25, `threaded-subprocess-wait.py × 20` PASS rate is 19/20.
The 1 fail (run 14 of `/tmp/smp-t25-py-validate/`) shows:

```
[w0 iter=56]  PYRC1: rc=-6 stdout=b'' stderr=b'free(): invalid size\n'
[w1 iter=107] PYRC1: rc=-6 stdout=b'' stderr=b'malloc(): corrupted top size\n'
```

- `rc=-6` = SIGABRT (`128 - 6 = 122` is `WTERMSIG()` value 6 = SIGABRT,
  reported as `-6` by `subprocess.Popen.returncode`).
- `BUG_B = 0` — no kernel-half RIP fault, distinct from T22/T25 class.
- No segfault printk, no kernel WARN, no BUG_C / BUG_PR escalation
  (BUG_PR fires only at boot for init.sh demand-paging).
- Failure happens INSIDE the spawned Python child interpreter at
  startup, well before the `import time; time.sleep(0.001)` runs.
- Both fails this run involved different parent worker iterations
  (w0 iter=56, w1 iter=107) — non-deterministic timing.

## Key correlation: KVM_V2_TLB_LAG saturation

The same run's `kernel.log` is dense with TLB lag warnings:

```
KVM_V2_TLB_LAG cpu=0 pid=58 mm=0000000061a12d00 last=25  cur=1896 lag=1871
KVM_V2_TLB_LAG cpu=0 pid=61 mm=0000000061a12d00 last=21  cur=1916 lag=1895
KVM_V2_TLB_LAG cpu=0 pid=59 mm=0000000061a12d00 last=110 cur=1921 lag=1811
KVM_V2_TLB_LAG cpu=0 pid=61 mm=0000000061a12d00 last=80  cur=1927 lag=1847
...
```

**Decoded:** per-mm tlb_gen is at 1927; this vCPU's last-seen for
that mm was 80; lag = 1847 generations missed. That mm has had ~1800
mappings change since this vCPU last flushed — meaning thousands of
pages might have been freed + recycled with new content while this
vCPU's TLB still holds the old translations.

This is exactly the symptom that G.2 cross-vCPU TLB kicks (commits
77cc1821c595 + 7e1c255a09ad) were designed to address — but
activation was deferred (task #143) because the un-throttled
sledgehammer caused IPI storms (T=4 100% → 30%).

## Hypotheses (ranked)

### H1 — Cross-vCPU TLB stale during fork+exec (LEADING, ~70%)

Mechanism:
1. Parent (worker thread) does fork() → COW marks all PTEs RO.
2. Child execve()s — replaces the mm; COW pages get freed via
   `um_mmu_gather`. RCU-deferred (T20), then released to buddy.
3. New mm allocates fresh pages; same physical pages may come back.
4. Parent vCPU still has TLB entries pointing to old GVA → old PA.
   Lag now grows.
5. Parent worker writes to its address space; the write hits a
   stale TLB entry mapping an old GVA → a now-recycled PA that
   belongs to a different mm's heap.
6. From the parent's point of view, glibc heap metadata in some
   `malloc_chunk` looks intact — it was, at the time of the cached
   translation. But the underlying PA now holds the child's heap
   data. Next malloc/free trips a corruption check.

Why glibc-heap is the visible symptom: malloc puts size/prev_size
metadata at chunk boundaries; any wrong byte fails the integrity
check. Stack/code regions are likely also corrupted but die later
(or not at all if the corrupted byte is in unexecuted code).

**Predictions:**
- Reproducer in pure C with malloc-heavy fork+exec child should
  trigger the same.
- Disabling G.2 RCU defer (task #189) would NOT fix it (defer is
  the right behaviour; the missing piece is the cross-vCPU sync).
- Activating G.2 IPI kicks WITH proper throttling should drop
  fail rate.

### H2 — execve mm-swap cr2/sregs leak (MEDIUM, ~20%)

T23 fixed cr2 zeroing on cross-mm transition. Could there be other
sregs (FS_BASE, GS_BASE, CR3-related cached state) that leak across
execve? E.g., a stale FS_BASE from the parent points into the
child's heap region after execve replaces the mm.

**Predictions:**
- Reproducer would NOT depend on multi-thread (single-threaded
  fork+exec would suffice).
- Symptom would be more "wild jump to wrong address," not metadata
  corruption confined to heap.

### H3 — mmu_gather drain timing edge case at execve (LOW, ~10%)

T20 RCU-deferred page free runs `um_mmu_gather_drain` after at
least one CR4.PGE-flushed dispatch. But if the dispatch is
EINTR-only (no IO exit), maybe the drain timing is subtly wrong.

**Predictions:**
- Strong correlation with EINTR-heavy workloads.
- Disabling RCU defer (revert T20) might mask the symptom.

## Distinguishing experiments

| Experiment | H1 prediction | H2 prediction | H3 prediction |
|---|---|---|---|
| C repro w/ malloc-heavy child | Repro | No repro | Repro |
| Single-threaded fork+exec stress | No repro | Repro | Maybe |
| Disable RCU defer (revert T20) | Worse (more concurrent unmap) | Same | Better |
| Activate G.2 IPI kick (throttled) | Better | Same | Same |
| Add page-poison on free | Confirms via poison-byte read | No (different addr range) | Confirms |

## Concrete next steps

### Step 1: Build C reproducer

Extend `tools/testing/selftests/um/fork-tree-3level/repros/threaded-fork-exec.c`
or write `threaded-fork-malloc.c`: N pthreads × M iters, each iter
forks and execve's a small program that does ~100 malloc/free cycles
of varying sizes (16 B to 4 KB) before exiting. Glibc integrity
checks should fire if heap is corrupted.

Predicted outcome under H1: reproduce within a few × 20-iter
sweeps. If repro, commit + run against existing T25 build to
quantify rate.

### Step 2: Page-poison-on-free instrumentation

Add a debug-only path in `um_mmu_gather_drain`'s RCU callback to
memset freed pages to a known pattern (e.g., `0xDEADBEEF` repeating)
BEFORE returning to buddy. If the user-space SIGABRT shows the
poison pattern in the corrupted chunk's metadata, it's a confirmed
TLB-stale-recycled-page read.

### Step 3: G.2 throttled activation (third attempt)

Prior attempts (commits 9f0ff6257e8b et al) tried per-vCPU dedup
flag. The TLB_LAG counters suggest a smarter activation:
- Kick only when `lag > THRESHOLD` (e.g., 64 generations).
- Use `mm_cpumask` to target only vCPUs that actually ran this mm
  recently.
- Coalesce multiple lag misses into a single batched kick at the
  next dispatch boundary.

### Step 4: codex / opus subagent investigation

Hand the v1 archive's TLB-flush pattern + this T26 characterization
to a subagent for independent root-cause confirmation. The v1 code
at `kvm-v1-archive/thread.c` (TLB-shootdown flow) was referenced in
the SMP-T11 fix; need to see if v1 had cross-vCPU sync that v2 is
missing.

## Workload notes

- The Python repro is heavy (Python startup mallocs ~50-100 MB
  even for `import time`); fork+exec churn × 200 × 2 workers is
  ~400 fork+exec operations. Each fork doubles the COW PTE count.
- A C repro doing 100 mallocs in the child × 4 workers × 100 iters
  = same total fork+exec count but vastly more allocations — should
  be MORE sensitive if H1 is correct.
- mt-mini (T=8) doesn't use fork+exec — only mmap stress within a
  single mm. That's why mt-mini PASS=20/20 post-T25 even though
  threaded-subprocess-wait still flakes: T26 needs the cross-mm
  page recycling that fork+exec produces.

## Open questions

1. Why does mt-mini SMP T=8 pass 20/20 post-T25 but
   threaded-subprocess-wait still flakes? Both stress the same SMP
   bug-shape but with different operations (mmap-only vs fork+exec).
   → Suggests T26 is fork+exec-specific, not just same-mm thread
   stress.

2. Is the failure rate dependent on `ITERS_PER_WORKER` or
   `N_WORKERS`? Need to vary and measure. Higher N_WORKERS → more
   concurrent fork+exec → more cross-mm page churn → if H1, more
   failures.

3. Does running with `kvm_v2_trace_enable=0` change the rate?
   State-trace ring overhead may be slowing dispatches enough to
   reduce TLB_LAG growth → masking the bug. Validate against
   trace-OFF baseline.
