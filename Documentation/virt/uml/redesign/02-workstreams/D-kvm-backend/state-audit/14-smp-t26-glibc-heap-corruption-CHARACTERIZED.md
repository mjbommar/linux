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

## UPDATE 2026-05-02 (post-investigation, tip b7f70664752b)

### C reproducer landed: tools/testing/selftests/um/fork-tree-3level/repros/threaded-fork-malloc.c

Pure C two-binary repro (parent threaded-fork-malloc + child malloc-stress-child).
At 8 workers × 500 iters = 4000 forks per boot under v2 SMP:

- **6/6 boots fail** with avg ~6 child SIGSEGVs each
- **Deterministic IP**: every failing child segfaults at user IP
  `0x4074ed`, which is in glibc `_int_malloc`:
  ```
  4074e9: mov 0x18(%rdx),%rdi   ; rdi = victim->bk
  4074ed: cmp %rdx,0x10(%rdi)   ; FAULT — rdi=NULL, deref [NULL+0x10]
  ```
  All faults: `cr2=0x10`, `error=4` (P=0, U=1, R=0 = user read of
  not-present page). `victim->bk` is being read as NULL — the chunk's
  back-pointer in some bin (likely unsorted bin) is zero, which is a
  glibc-internal invariant violation.
- **comm = "malloc-stress-c"** — every failing process is a fresh
  execve that ran __libc_start_main → some early malloc → walked the
  bin and tripped the NULL deref.

### Hypothesis updates from ablation experiments

| Test | Result | Implication |
|---|---|---|
| TLB_LAG correlation per failing PID | All failing PIDs had ZERO TLB_LAG entries | H1 weakened |
| G.2 IPI kicker ablation (`if (0 && um_backend->tlb_kick_others)`) | 6/6 fail × ~5 child fails each — IDENTICAL to G.2-active baseline | **H1 RULED OUT** |
| Seccomp baseline (same C repro) | 4/6 PASS, 2/6 fail with DIFFERENT signature (parent NULL deref + UML fatal) | Bug is mostly v2-specific but seccomp has its own residual; v2 rate is higher |

### Discovery during investigation

The opus subagent found that **G.2 IS already activated** (commit
`ad18db7c3768` "activate cross-vCPU tlb_kick_others now that migrate
fix is in"). Earlier task notes saying "G.2 deferred" were stale.
The kicker calls `os_send_ipi(cpu)` from `tlb.c:560` after every
successful drain on a non-init mm. The G.2-disabled ablation above
patched it out and saw NO change in T26 fail rate.

### Subagent's parallel finding

v1 archive evidence (`kvm-v1-archive/thread.c`, `lifecycle.c`,
`shadow_sync.c`) confirms: **v1 NEVER had cross-vCPU IPI**. The signal
slot was reserved (`KVM_UM_KICK_SIGNAL` at `kvm_backend.h:192`) but
no sender exists. v1 used purely passive entry-side check
(per-mm gen counter + per-vCPU last_seen + CR4.PGE-toggle on dispatch).
v2 already does this MORE aggressively than v1 (toggles on every
dispatch, not just same-CR3). So even the IPI-on-drain behavior is a
v2 addition not derived from v1.

### Updated hypothesis ranking (post-FPU + post-CPUID audit)

| Hypothesis | Pre | Post-G2-abl | Post-FPU-abl | Post-CPUID-audit |
|---|---|---|---|---|
| H1 cross-vCPU TLB stale | 70% | **<5%** | <5% | <5% |
| H2 execve mm-swap leak | 20% | ~50% | varies | ~30% |
| H2a per-vCPU FPU/XMM leak | 0% | ~60% | **<5%** (ablation neg) | <5% |
| H3 mmu_gather drain timing | 10% | ~10% | ~10% | ~15% |
| H4 anonymous-page-not-zeroed on alloc | 0% | ~30% | ~40% | **~40%** |
| H5 backend-agnostic | small | small | small | small |
| H6 KVM XSAVE/AVX state leak | 0% | 0% | ~20% | **~0%** (RULED OUT, see below) |
| H7 v2-specific exec.c / start_thread setup race | 0% | 0% | ~25% | **~30%** |
| H8 KVM internal host-side FPU leak (not guest-visible) | 0% | 0% | 0% | **~15%** (NEW) |

### H6 RULED OUT via CPUID audit

`kvm_v2_curate_cpuid` at `arch/um/backend/kvm-v2/vcpu.c:129-185` explicitly
masks OFF the following from guest CPUID:
- Leaf 1 ECX: bits 26 (XSAVE), 27 (OSXSAVE), 28 (AVX), 29 (F16C)
- Leaf 7 EBX: bits 5 (AVX2), 16-31 (AVX512 family)
- Leaf 7 ECX: AVX512 family
- Leaf 7 EDX: AVX512 family
- Leaf 0xD (XSAVE state-component descriptor): zeroed entirely

**Therefore the guest CANNOT use AVX/AVX2/AVX512 — only legacy SSE/SSE2.**
Modern glibc compiled with AVX2 support does runtime CPU-feature
detection via cpuid; with AVX disabled in guest CPUID, glibc falls
back to SSE2-only paths.

KVM_SET_FPU covers the entire legacy fxsave area (XMM0..15, x87 FPR,
MXCSR, FCW, FSW, FTW) — which is everything the guest can address.
So H6 (KVM XSAVE/AVX state leak) is **structurally impossible** in
this configuration. KVM_SET_XSAVE would not improve over KVM_SET_FPU
for this guest.

### Remaining hypotheses ordered by promise

1. **H4 (~40%)**: anonymous-page-not-zeroed on alloc. Test by adding
   page-poison-on-free in mmu_gather drain RCU callback; if user
   SIGSEGV shows the poison pattern in heap metadata, confirmed.
2. **H7 (~30%)**: v2-specific start_thread / binfmt_elf load race.
   Test by tracing the binfmt path between fork and child's first
   KVM_RUN, looking for race windows where the child's mm has PT
   entries that don't match what the elf loader populated.
3. **H8 (~15%)**: KVM internal host-side FPU leak NOT visible at the
   guest XMM level — perhaps host-FPU-context restoration during
   KVM_RUN entry briefly exposes parent-task FPU registers between
   KVM's `fpu_sync_fpstate(vcpu, true)` (load guest FPU) and VMRUN
   instruction. Would need bpftrace on KVM internal functions or an
   independent reproducer using actual XMM probes (mt-xmmprobe
   pattern from the H.1b investigation).
4. **H3 (~15%)**: mmu_gather drain timing edge case at execve.

### Status pause

T25 holds clean across the test matrix:
- mt-mini SMP T=8 × 5: 5/5 PASS
- threaded-fork-exec C × 10: 10/10 PASS
- cpython-tier0: PASS

T26 residual is ~0.15% per-fork in C-malloc-stress, ~5% per-boot in
Python repro. Two ablations done and ruled out (G.2, FPU). T26
investigation paused pending one of:
(a) implementation of H4 page-poison instrumentation, or
(b) fresh subagent investigation focused on H7 / H8 with the new
   evidence (CPUID-disabled-AVX, FPU-ablation-negative).

### UPDATE — User-side SIGSEGV diagnostic (malloc-stress-child-diag.c)

Added a sigaction-based diagnostic version of malloc-stress-child
that captures GPRs + 64 bytes around the chunk pointer + 128 bytes
of arena state at SIGSEGV, then exits 177. Ran 4 boots × 8 workers ×
500 iters = 16000 forks; 26 SIGSEGVs captured.

**Every captured fault has the same structure:**
```
SMP-T26-DIAG SIGSEGV: cr2=0x10 ip=0x415f2d pid=NNNN
  GPRs: rax=<varies>  rdx=<chunk ptr in user heap>  rdi=0  rsi=<size>
        r11=<arena ptr>
  chunk @rdx (victim):
    +0x00: 00 00 00 00 00 00 00 00   (prev_size = 0)
    +0x08: 21 d2 00 00 00 00 00 00   (size = 0xd221, P=1, M=0, A=0)
    +0x10: 00 00 00 00 00 00 00 00   (fd = 0)
    +0x18: 00 00 00 00 00 00 00 00   (bk = 0)  ← READ AS NULL
    +0x20..+0x40: all zeros
  arena @r11 (av):
    +0x00..+0x10: 00 00 ... (mutex, flags — fresh)
    +0x10..+0x70: real pointers (fastbins, top, bins)
```

**Decoded:** the chunk being walked has its `fd` (offset 0x10) and
`bk` (offset 0x18) BOTH zero, but its `size` field at offset 0x08 is
0xd221 = ~53KB (P bit set, valid chunk). The chunk header is
inconsistent: size says "valid 53KB chunk in unsorted bin" but
fd=bk=0 says "uninitialized memory."

For glibc to enter this code path:
- Some bin's `fd` pointed to this chunk (placed there by free() or
  malloc_consolidate())
- That placement should have set chunk->bk to point back to the bin
  head, NOT zero

The all-zeros-except-size pattern is consistent with:
- (a) **Top chunk being incorrectly walked**: glibc's heap top chunk
      has a size field but no fd/bk. If a consolidation bug let the
      top chunk get added to a bin, the bin walk would read fd/bk
      as zero.
- (b) **A page that was zeroed AFTER glibc set up the chunk**: the
      page got the size field written by glibc, then was unmapped
      and re-mapped (zero-init from anonymous mmap), losing fd/bk
      but keeping size. But this requires partial page state, which
      anonymous mmap doesn't do (it's all-or-nothing).
- (c) **Glibc's free() returned early before setting fd/bk**: a
      race in user code OR an interrupt that left malloc state
      inconsistent. Glibc malloc is not signal-safe; a SIGALRM or
      similar mid-free could leave fd/bk unwritten.

(c) is interesting given UML uses SIGALRM internally for timer
delivery. But signal handling at the UML kernel level shouldn't
interrupt user-mode glibc operations.

### Diag tool committed for future investigation

The malloc-stress-child-diag.c source is checked in alongside the
non-diag version. Future investigators can swap it in (rename or
override CHILD_PATH) and rerun to characterize specific failure modes.

### Open: needs deeper kernel-side trace correlation

To proceed, T26 needs to correlate the user-side SIGSEGV PID + chunk
address with kernel-side state-trace events around that PID's
dispatches. Specifically: did this PID see an EINTR mid-free()
(syscall=__NR_munmap or __NR_brk)? Did the PT for the chunk's page
get cleared and re-established? Was there a TLB shootdown event?

This requires either:
1. A kernel-side instrumentation point that prints PT state for a
   specific user VA range when handle_io_pf fires for that PID.
2. Bisecting between current tip and a pre-T20 (no RCU defer) tip
   to see if T20 introduced the residual.

Both are non-trivial and should be the next iteration's focus.

### UPDATE — Aggregate analysis across 26 captures + new ablations

**ALL 26 captures share IDENTICAL register state at fault**:
- `rsi=0x4ee9b0` (alloc size argument — same call site every time)
- `rdx` ranges 0x4d9090..0x4e8390 (multiple PIDs hit the SAME
  chunk address in different boots; e.g. rdx=0x4e3290 hit 4 times)
- The chunk address range is in the BRK heap region (binary RW segment
  ends at 0x4b9248; chunks are 0x4d9000..0x4e8000)
- Chunk size 0xd221 = ~53KB IDENTICAL across all captures
- Chunk content IDENTICAL: prev_size=0, size=0xd221, fd=0, bk=0, body=0

**Strong implications:**
- Bug is at a SPECIFIC step in `_int_malloc`'s large-bin walk
- The corrupted chunk is somewhere consistent — possibly the heap
  top chunk being incorrectly walked
- Multiple PIDs in different boots hit the same chunk address →
  the bug is reproducible at the VA level (not random)

### NEW ablations (this iteration)

| Test | Result | Implication |
|---|---|---|
| **Seccomp baseline** (same C repro, same diag binary) | **0 SIGSEGVs in 5 boots × 4000 forks = 20000 forks** | **Bug is v2-specific** (not glibc/host kernel) |
| T20 RCU-defer ablation (replace `call_rcu` with sync free) | 6/6 fail × ~6 child SIGSEGVs each — IDENTICAL to baseline | T20 is NOT the cause |

### Updated hypothesis ranking

| Hypothesis | Status |
|---|---|
| H1 cross-vCPU TLB stale | RULED OUT (G.2 ablation) |
| H2a per-vCPU FPU/XMM leak | RULED OUT (KVM_SET_FPU ablation) |
| H6 XSAVE/AVX leak | STRUCTURALLY IMPOSSIBLE (CPUID disables AVX) |
| **T20 RCU-defer** | RULED OUT (sync-free ablation) |
| H4 anonymous-page-not-zeroed | UNTESTED (~25%) |
| H7 v2-specific start_thread / load_binary race | UNTESTED (~25%) |
| **NEW: H9 v2-specific BRK / mm_region_added race during heap growth** | **UNTESTED (~30%)** |
| **NEW: H10 v2-specific PT propagation race after fork CoW** | **UNTESTED (~20%)** |

### Concrete next steps for T26

1. Spawn focused subagent on v2 BRK / mm_region_added paths — find a
   v2-only code path that could leave a heap chunk's `fd`/`bk` zero
   while the `size` field is set.
2. Add kernel-side instrumentation to `kvm_v2_handle_io_pf` to log
   the (PID, user VA, host PA) tuples for any fault in the
   0x4d9000..0x4e8000 range. Run repro and look for unusual patterns.
3. Bisect between e4d347ae48d8 (Option B page defer) and tip to find
   when T26 starts reproducing.

T26 investigation continues in next iteration.

### LEADING HYPOTHESIS — H_E (TDP / EPT cache aliasing) — agent #2 finding

Subagent identified the v2-specific mechanism (corroborated by code at
`arch/um/backend/kvm-v2/region.c`, `arch/um/backend/kvm-v2/vcpu.c:1311`,
`arch/um/backend/kvm-v2/vcpu.c:780`):

**Mechanism:**
1. Under v2, `mm_region_added` is essentially a no-op (H.1b A/B test
   stripped both `os_map_memory` and `seccomp_mm_region_added`
   delegation). The only KVM memslot is **slot 0**, covering all of
   `physmem` via spawner-mm `MAP_SHARED|MAP_FIXED` mapping.
2. UML manages guest PTs by writing PTE bytes directly into
   `physmem` (it's just memory inside slot 0). These writes are
   **invisible to KVM's mmu_notifier** because mmu_notifier only
   fires for HOST mm operations on the spawner mm — UML's PT writes
   bypass that entirely.
3. KVM's TDP/EPT cache (the GPA→HPA layer) is therefore NOT
   invalidated when UML changes guest PTEs.
4. CR4.PGE toggle on every dispatch flushes the **guest TLB**
   (GVA→GPA layer), but does NOT flush EPT (GPA→HPA layer).
5. **The aliasing**: when child A's GVA-X mapped to GPA-Pa, then
   child A exits and its mm gets reaped, child B (same vCPU) then
   accesses GVA-X mapped to GPA-Pb (different GPA, fresh mm), KVM
   may still have a cached TDP entry for GPA-Pa→old-HPA. Subsequent
   writes near the fault site land on the cached old-HPA, not the
   correct new-HPA.

**Smoking-gun pattern explained:**
- glibc's first write to a chunk faults; `handle_io_pf` →
  `handle_page_fault` → `handle_mm_fault` allocates a fresh page,
  writes new PTE in physmem (via UML's mm machinery), then re-tries
  the user write. This write succeeds **via the fault path** which
  goes through the spawner mm's HVA, firing mmu_notifier and updating
  TDP. Result: `size` field correctly written.
- The next 1-2 instructions (writing `fd` and `bk` at adjacent
  offsets in the same chunk) hit the SAME page that just got faulted
  in — but those writes go through the **already-cached TDP entry**
  (no fault, no spawner-mm operation, no mmu_notifier fire). If the
  TDP cache had a stale entry for this GPA from a PRIOR mm's
  ownership, the writes land on the wrong physical page.
- Result: chunk has `size=0xd221` (correctly written via fault path)
  but `fd=0`/`bk=0` (writes lost to stale TDP), exactly the captured
  pattern.

**Workload scaling supports the hypothesis:**
- 1 worker × 4000 iters = **0/12000 forks** fail (no concurrent
  mm-create churn → TDP entries naturally evict before reuse)
- 8 workers × 500 iters = **0.150%** fail rate
- 16 workers × 250 iters = **0.092%** fail rate (capped by 4 vCPUs)
- Bug strictly requires multi-worker concurrent mm-create activity

**Why seccomp doesn't have this**: seccomp's stub child IS the
user-mode executor and owns host PTs directly. No second-level
EPT/TDP cache to invalidate.

### Proposed fix candidates (graded by complexity)

1. **MINIMAL**: After `kvm_v2_handle_io_pf` returns success, walk
   UML's pgd to compute the freshly-mapped physical address; call
   `madvise(uml_physmem + phys, PAGE_SIZE, MADV_DONTNEED)` on the
   spawner mm. madvise fires mmu_notifier → KVM invalidates the TDP
   entry → next access re-walks PT and re-populates with correct
   HPA. Cheap, surgical.
2. **MEDIUM**: Add a backend hook in `arch/um/kernel/trap.c`
   `handle_page_fault` (after success) so any backend can invalidate
   its own translation cache. seccomp's hook is a no-op; v2's hook
   does the madvise above.
3. **HEAVY**: Re-enable per-region memslots (revert region.c H.1b
   A/B). Each `mm_region_added` becomes a real
   KVM_SET_USER_MEMORY_REGION call, firing mmu_notifier for every
   PT change. Slowest but most architecturally correct.

### Next iteration

Implement candidate #1 (minimal madvise hook) and test:
- If T26 fail rate drops to ≈0/24000: hypothesis CONFIRMED, ship the
  madvise hook as the fix.
- If T26 fail rate unchanged: hypothesis WRONG, look at #2/#3.

### UPDATE — H_E test result + state-trace capture

H_E madvise(MADV_DONTNEED) test on the freshly-faulted page after
handle_io_pf success: NO change in fail rate (50/24000 = 0.21%, vs
baseline 36/24000 = 0.15%). Hypothesis E as implemented does not fix
T26. Reverted.

**However, the state-trace ring capture (BUG_T26 trigger commit
97a6841bdad3) provides the smoking-gun evidence:**

In a captured failure (pid=2357, cpu=0, seq=4212296, frozen ring
6852 entries), the sequence is:
```
seq=4212287  pid=2357 VCPU_RUN_EXIT  exit=2 port=0xf4
              rip=0x416b6c  rdx=0x4e3290  rsi=0x4b2f38  rdi=0x4e2a80
              rcx=0xb720  rax=0x4b27c8
              cr2=0x4e83a0  cr3=0x304f000
              vmm=619d8000 (PREVIOUS task's mm)
              tmm=619d8a00 (THIS task's mm — DIFFERENT)
              vlast=80 mmgen=75 — cross-mm transition

seq=4212288-4212293: dispatch setup (TLB sync, CR4.PGE toggle,
                     IST restore, FPU install, PRE_KVM_RUN)
              load_user_sregs zeros cr2 (T23 cross-mm guard)
              cr3 changes 0x304f000 -> 0x2d4b000 (?)

seq=4212294  pid=2357 POST_KVM_RUN  exit=2 port=0xf6 (#PF)
              rip=0xffffe00000002158 (PF stub)
              cr2=0x10 (NULL+0x10)
              error_code=4 (P=0, U=1, R=0 = user read of not-present)
              IST frame: [4, 0x415f2d, 0x2b, 0x10246, 0x7f7ffff087f0, 0x23]
              user_rip at fault = 0x415f2d (glibc _int_malloc+0xed)
              register state: rdx=0x4e3290 rdi=0 rsi=0x4ee9b0
              -> user faulted reading victim->bk where bk=0
```

**Critical finding from cross-PID analysis** (multiple PIDs across
multiple CPUs hit the SAME chunk address 0x4e3290 with the SAME
glibc register state):

PID 67 on cpu=3 at seq=1590 ALSO faulted on the same chunk page —
but with DIFFERENT signature: cr2=0x4e32a0, error_code=6 (P=0, W=1,
U=1) = user WRITE to not-present page. And user_rip=0x416b6c (the
SYSCALL-return point in _int_malloc, not the bin-walk address). So
this is a WRITE fault on the chunk's fd field address (0x4e32a0
= 0x4e3290 + 0x10).

So the chunk page at 0x4e3000 was NOT mapped at the moment glibc
first wrote to it. Kernel handles, maps fresh anonymous page (which
is zero-init by POSIX). Glibc re-executes the write, now succeeds.
But then the bin-walk loop reads back fields from the page, and
sees zeros (because the page was just mapped fresh and writes
weren't fully consistent across the page state).

Or more subtly: under v2, the demand-paging path may have a
different content-stability invariant than seccomp. Anonymous
pages SHOULD be zero on first fault, but subsequent writes by the
user code SHOULD persist. If subsequent writes go to a stale TDP
entry pointing at a different physical page (still zero-init), the
writes silently disappear from the user's view.

**Summary of trace-captured smoking gun:**
- Multi-PID, deterministic pattern: same chunk address 0x4e3290,
  same arena address 0x4b27c0, same alloc context (rsi=0x4ee9b0)
- Cross-CPU pattern: hits cpu=0 AND cpu=3 with same register state
- Failure point: glibc _int_malloc bin walk (0x415f2d) reads
  chunk->bk = 0
- Pre-failure: chunk's fd field (0x4e32a0) has been WRITTEN to
  via #PF handler path (different PID, on a different mm with
  the same VA layout)

This is consistent with H_E mechanism but the implemented madvise
fix targets the wrong page. The actual issue may be:
- The VA range that glibc writes to spans MULTIPLE pages
- handle_page_fault only maps the SPECIFIC faulting page; subsequent
  writes to ADJACENT pages still take faults
- Each fresh page mapped = zero content; writes from glibc go to
  the right pages BUT may be invisible to subsequent reads if KVM's
  TDP cache is pointing at a stale page

Need to write a kernel-side fix that ensures ALL pages glibc has
in its cached chunk-pointer view are mapped consistently. Or fix the
TDP-cache invalidation to cover the whole faulting region.

For next iteration: spawn a deeper subagent investigation focused on
v2-side madvise / mmu_notifier / TDP path with this trace evidence.
The core question: when handle_page_fault returns successfully, is the
PT entry visible to KVM's TDP layer immediately, or is there a window
where KVM sees stale GPA→HPA mapping?

### UPDATE — XMM dump + #NM handler audit

**XMM dump from extended diag binary** (commit d486e68e1a7b) at fault time
shows interesting state — XMM0=0, but XMM1-XMM4 contain glibc internal
string data ("PRIVATE\0__libc_e..." in XMM2):
```
XMM0: 00000000 00000000 00000000 00000000  (zeros)
XMM1: f7e13523 00007fff f7e13523 00007fff  (stack-region addr ×2)
XMM2: 56495250 00455441 696c5f5f 655f6362  ("PRIVATE\0__libc_e")
XMM3..7: mix of zeros + small values
```

XMM2's content confirms glibc actively uses XMM for SSE-string ops
during startup. XMM0=0 at fault is consistent with: the failing
instruction `mov 0x18(%rdx),%rdi` (read of bk) doesn't touch XMM,
and an earlier `pxor %xmm0,%xmm0` at 0x41647f explicitly clears it.

**Critical mechanism finding** — `kvm_v2_handle_io_nm` at
`arch/um/backend/kvm-v2/syscall_trap.c:1768-1796`:
```c
static int kvm_v2_handle_io_nm(struct uml_pt_regs *regs,
                               struct kvm_run *run,
                               struct kvm_v2_vcpu *vcpu)
{
    struct kvm_v2_ist_frame frame;
    kvm_v2_ist_frame_read(vcpu, &frame, false);
    regs->gp[HOST_IP]     = frame.user_rip;
    regs->gp[HOST_SP]     = frame.user_rsp;
    regs->gp[HOST_EFLAGS] = frame.user_rflags;
    regs->is_user         = 1;
    /* Host-side `clts` emulation. */
    run->s.regs.sregs.cr0 &= ~X86_CR0_TS;   /* CLEARS TS */
    run->kvm_dirty_regs   |= KVM_SYNC_X86_SREGS;
    /* One-shot bypass: */
    current->thread.arch.kvm_v2.nm_ts_bypass = true;
    interrupt_end();
    kvm_v2_marshal_to_kvm_regs(&run->s.regs.regs, regs);
    run->kvm_dirty_regs   |= KVM_SYNC_X86_REGS;
    return 0;
}
```

**The handler DOES NOT restore the new task's per-task FPU state**.
It only clears CR0.TS so the user can re-execute the trapping FPU
instruction. The XMM/x87 state in the per-host-CPU vCPU at that moment
is **whatever the previous task on this vCPU left**.

For a freshly-execve'd task on its first FPU use:
- Pre-#NM: per-host-CPU vCPU has previous task's FPU state
- User executes XMM op → #NM (because TS=1)
- v2 handler clears TS, returns
- User re-executes XMM op → succeeds with **previous task's XMM data
  in the registers it didn't explicitly write**

This is consistent with the bug observation that v2 fails on
fork+execve workloads (where many fresh tasks share vCPUs) but
seccomp doesn't (each task has its own host-side FPU context).

The H2a ablation (one-shot KVM_SET_FPU(arch_reset_zero) on fresh
execve) didn't help because: glibc's MOVUPS sequence at 0x416458-
0x41646a explicitly sets xmm0 = (r9<<64) | rax via movq+punpcklqdq.
After my arch-reset, xmm0 starts at 0; user runs movq+punpcklqdq
which sets xmm0 properly to (r9<<64)|rax. Then MOVUPS writes valid
data. So bk should NOT be 0.

But bk IS 0. So either:
(a) The MOVUPS write IS losing the high half (bk specifically), even
    when xmm0 is properly set.
(b) Glibc reaches a code path that doesn't go through 0x41646a, and
    the chunk's bk was never written.

Need a DIFFERENT diagnostic: in `kvm_v2_handle_io_pf`, when BUG_T26
fires, do KVM_GET_FPU and dump XMM0. This is the GUEST's XMM at the
moment the user took the page fault — not after glibc has reset xmm0.
This tells us what xmm0 contained when the WRITE was attempted (one
fault back from the read).

Possibly the right fix: in `kvm_v2_handle_io_nm`, the v1 archive's
`kvm_handle_nm` (at `kvm-v1-archive/thread.c:5089-5142`) calls
`kvm_set_fpu_for_task` which restores the per-task FPU state from
`current->thread.arch.kvm.kvm_fpu`. v2 does NOT do this — that's
the architectural difference.

For next iteration: implement v1-style FPU restore in v2's #NM handler
(KVM_SET_FPU from arch_thread.kvm_v2.iotrap_fpu when iotrap_fpu_valid),
or initialize iotrap_fpu to architectural defaults at fork/execve and
KVM_SET_FPU from there in #NM handler.

### FPU ablation experiment (NEW)

Hypothesis: per-host-CPU vCPU's XMM/x87/MXCSR state from previous task
leaks into freshly-execve'd task; glibc's SSE2 strlen/pcmpeqb startup
returns false-zero, corrupts heap arena, NULL bk → SIGSEGV at 0x4074ed.

Test: added one-shot flag `arch_thread.kvm_v2.fpu_arch_reset_needed`,
set true in arch_flush_thread (execve) and false in arch_copy_thread
(fork; fork preserves parent FPU). On first KVM_RUN of a fresh task,
KVM_SET_FPU with architectural-reset values: zeroed XMM/FPR, fcw=0x37f,
mxcsr=0x1f80.

Result: 6/6 boots fail, 42 child SIGSEGVs across 20000 forks (0.21%) —
**identical-or-worse to baseline 0.15%**. Hypothesis ruled out:
KVM_SET_FPU does not prevent the bug. Either:
- (a) FPU isn't the carrier (different state leaks)
- (b) The leak is in XSAVE state (AVX YMM/AVX-512 ZMM) which kvm_fpu
      doesn't cover — would need KVM_SET_XSAVE
- (c) Bug is downstream of FPU restore: e.g., a kernel-side write
      between FPU install and KVM_RUN entry clobbers FPU

(b) is plausible — modern glibc heavily uses AVX2 in startup paths.
KVM_SET_FPU only writes the legacy fxsave area (XMM0..15 low 128b);
YMM0..15 high halves and ZMM0..31 are XSAVE-only.

Branch state: ablation reverted at b277b0b8c0cc..HEAD; tip is clean
post-T25 again. Layer 14 memo retains the failed-fix attempt for
reference.

### Next concrete steps

1. **Test H4**: add page-zero on alloc from buddy in UML's
   physmem-page allocator path. If this fixes T26, the bug is
   stale page contents leaking into newly-mapped user pages.
2. **Test H2 narrowly**: instrument execve path to dump per-vCPU
   state at the FIRST KVM_RUN of a freshly-execve'd task. Check if
   any sregs / FPU / per-task arch_thread state is non-canonical.
3. **codex/opus subagent**: hand the deterministic IP + child-
   fresh-execve fact + ablation results to a fresh agent for an
   independent root-cause hypothesis (suggest: arena_for_init —
   does glibc's first arena read uninitialized memory?).
