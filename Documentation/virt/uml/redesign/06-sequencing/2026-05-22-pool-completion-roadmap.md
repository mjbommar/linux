# Pool model completion roadmap (working backwards from 100%)

**Date:** 2026-05-22
**Status:** Plan — agreed direction; implementation tracked here.
**Reference docs:**
  * `post-2026-05-19-next-sprint/09-fork-server-snapshot-restore.md` — the vision
  * `post-2026-05-19-next-sprint/09-fork-server-PHASE2A-DESIGN.md` — Phase 2a design (kernel-stack race fix)
  * `post-2026-05-21-pool-architecture-comparison.md` — comparison + decision (1.6 + 1.7 + 1.3)
  * `02-workstreams/D-kvm-backend/state-audit/30-32` — diagnostic chain
  * `2026-05-21-path-a-integration-session-1.md` — day-1 deliverable map

---

## 0. The 100% target (Memo 09 vision)

`syzkaller` uses UML as a fuzzing backend via `umlctl pool`:

  * **Throughput:** 50 takes/sec sustained, no pool drain at
    `min_warm=4`, `refill_concurrency=2`.
  * **Latency:** p50 ≤ 5 ms, p99 ≤ 50 ms for `Pool.Create`.
  * **Memory:** 100 forks from a 128 MiB master ≤ 200 MiB total RSS
    (99 %+ physmem sharing, plus per-member dirty deltas).
  * **Stability:** 24 h soak + 10 000 take/close cycles with no
    memory leak.
  * **Per-member identity:** independent MAC, host TAP, IPv4,
    mconsole socket.

This unblocks Phase 4 (syzkaller `vm/uml` shim) which closes the
five-week-redesign keystone.

---

## 1. Working backwards from 100%

```
100% vision
     ↑
     │
   Step −1   syzkaller vm/uml Go shim + USAGE.md
             ~150 LoC + docs.
             Blocked on: pool model working.
     ↑
     │
   Step −2   pool-bench acceptance gates pass
             (5ms p50, 50ms p99, ≤200MiB RSS at N=100, no leak)
             Selftest scaffold exists from Phase 3.
             Blocked on: sustained N-member dispatch.
     ↑
     │
   Step −3   sustained-smoke flips XFAIL → PASS
             N=3 members each reach MEMBER_DONE with distinct
             identities; ramp to N=100 next.
             Blocked on: per-member state isolation.
     ↑
     │
   Step −4   per-pool-member kernel + userspace state isolation
             Iter N's writes don't propagate to master or
             sibling members.
             Blocked on: per-pool-member physmem backing.
     ↑
     │
   Step −5   per-pool-member physmem backing  ← THE ARCHITECTURAL GAP
             Currently: single global `physmem_fd` MAP_SHARED across
             all forked UML kernels.  Writes from any forked kernel
             propagate to all others including master.
             Path A primitive's MAP_PRIVATE child-stack fixed the
             kernel-stack race specifically; everything else (task_
             struct, signal_struct, mm_struct, bash heap/stack) still
             shares.
     ↑
     │
   ▼ Step −6 (we are here, 2026-05-22)
             Single-iter pool member runs end-to-end.
             Path A primitive + `start_userspace_fresh` (per-mm
             memfd-backed stub_data) + `um_skas_force_resync_mm`
             + state restoration.
             3 selftests: pivot 20/20 PASS, pool-member PASS
             (init.sh → MEMBER_DONE + identity-parsed), sustained
             N=3 XFAIL on shared physmem.
```

---

## 2. The gap: shared physmem across forked UML kernels

### 2.1 What's true today

  * `arch/um/kernel/physmem.c::setup_physmem` creates one tmpfs
    file (`physmem_fd`) and maps it via `os_map_memory`.
  * `arch/um/os-Linux/process.c::os_map_memory` always uses
    `MAP_SHARED | MAP_FIXED | MAP_POPULATE`.
  * On `fork(2)`, the child UML kernel inherits the same
    MAP_SHARED mapping.  Writes from either process land in
    the shared backing — there is NO copy-on-write at the
    process level for `MAP_SHARED` file-backed mappings.
  * All UML kernel allocations (slab, page allocator, kernel
    stacks) and all "guest physical" pages (bash heap, libc,
    stack) live in this single tmpfs.

### 2.2 What was already fixed

  * Commit `9c6c4948dc70` (2026-05-20) identified the MAP_SHARED
    physmem race as the root cause of the Phase 2a M-fork-child
    crash, and proposed three fixes:
    * (a) `__NR_clone` with private MAP_PRIVATE child stack
    * (b) Pre-fork copy of kernel stack into private mmap
    * (c) Refactor UML to allocate kernel stacks outside physmem
  * Commit `d057cf28f482` (2026-05-20) shipped option (a) for the
    M-fork child's kernel stack specifically.
  * `start_userspace_fresh` (2026-05-22, commit `06968e3f5f27`)
    extends this to per-mm memfd-backed `stub_data` — addresses
    stub<->kernel communication channel sharing.

### 2.3 What was not fixed

  * Kernel slab allocations (`task_struct`, `signal_struct`,
    `mm_struct`) for init.sh and other tasks → still in shared
    `physmem_fd`.
  * Guest userspace pages (bash text, data, heap, stack;
    libc.so.6) → still in shared `physmem_fd`.
  * When iter N's child runs init.sh's exit path, `PF_EXITING`
    is set on the `task_struct`, `signal->live` decremented,
    bash's stack mutated.  All visible to master and to iter
    N+1's child via shared backing.

### 2.4 Decision-doc gap

The 2026-05-21 pool architecture comparison memo §1.3 stated
the design assumes "each child is a CoW duplicate at the
SIGSTOP point."  The wording presumes fork CoW's the master's
physmem.  It does not — that's a documentation/design gap that
this roadmap supersedes.

---

## 3. The architectural fix: per-pool-member physmem backing

### 3.1 Two viable approaches

**Option A: MAP_PRIVATE flip.**

Change `os_map_memory`'s physmem flags from `MAP_SHARED` to
`MAP_PRIVATE`.  Fork() then gets host-kernel page-table CoW
for free.  Each forked UML kernel writes to its own private
copy of any page it modifies.  Identical reads still share
underlying pages until first write.

  * **Pro:** O(1) per fork at fork time; host kernel does the
    work via standard CoW PTE bits.  Memory amplification stays
    at ~"sum of dirty deltas" instead of "N × master size".
  * **Pro:** Hits the latency target (5 ms p50) trivially.
  * **Con:** Breaks D36 v2 snapshot-to-disk path, which reads
    `physmem_fd` directly to serialize guest RAM.  With
    MAP_PRIVATE, the fd content is the pre-write template; the
    actual process state is in CoW'd private pages.
  * **Con:** May break other consumers that expect to read
    physmem via the backing fd.
  * **Mitigation for snapshot writer:** add a separate path that
    reads via `/proc/self/mem` (or `process_vm_readv`) to capture
    the post-write process state, not the template.

**Option B: per-member memfd at fork time.**

In `child_entry_pool_member`, create a fresh memfd, copy
master's physmem content into it via `copy_file_range`, and
remap the physmem VA range to back-onto the new memfd.

  * **Pro:** No global flag change.  D36 snapshot writer
    unaffected (master's fd is still the source).
  * **Pro:** Each pool member's backing is explicitly private;
    no surprise sharing.
  * **Con:** ~50 ms memcpy per fork for 128 MiB physmem.  Misses
    the 5 ms p50 target by 10×.
  * **Con:** Memory amplification grows linearly with member
    count (each member holds a full copy in its memfd).  Misses
    the ≤200 MiB at N=100 target.
  * **Mitigation:** lazy-copy via reflink (`copy_file_range`
    falls back to dense copy on tmpfs; on btrfs/zfs it's O(1)).
    Move tmpfs → reflink-capable backing.  Adds operator
    complexity.

### 3.2 Recommendation

**Land Option A (MAP_PRIVATE) gated behind a Kconfig
(`CONFIG_UM_POOL_MEMBER_FORK=y`) so the snapshot-to-disk path
stays default.**

  * The pool model is the production target for the syzkaller
    integration; the snapshot-to-disk path is a separate
    feature.
  * MAP_PRIVATE matches the memo 09 vision's stated assumption
    ("each child is a CoW duplicate").
  * The implementation is a single-line change in
    `os_map_memory` + Kconfig + a new fork-time path that
    `madvise(MADV_DONTFORK)` regions registered as
    `UM_MMAP_REMAP_IN_WORKER` (i.e., KASAN shadow).
  * If/when D36 snapshot-to-disk needs to coexist, add a
    `process_vm_readv`-based writer that reads the running
    UML kernel's post-CoW pages.

---

## 4. Concrete implementation plan

### 4.1 Step A — flip physmem to MAP_PRIVATE (gated)

**Files:**
  * `arch/um/Kconfig` — add `CONFIG_UM_POOL_MEMBER_FORK`
  * `arch/um/os-Linux/process.c` — `os_map_memory`: gate the
    flags between `MAP_SHARED` (default) and `MAP_PRIVATE`
    (when Kconfig set + caller indicates physmem)
  * `arch/um/kernel/physmem.c` — `setup_physmem`: pass a hint
    flag to `os_map_memory` so only physmem (not other mmaps)
    flips
  * `arch/um/include/asm/um-mmaps.h` — extend
    `UM_MMAP_INHERIT_COW` semantics to reflect MAP_PRIVATE
    behavior under the Kconfig

**Acceptance:**
  * `defconfig` build unchanged (MAP_SHARED stays default).
  * `defconfig + CONFIG_UM_POOL_MEMBER_FORK=y` builds clean.
  * `template-pause-pool-member-smoke` still PASSES (single-iter
    uses the new flag, master writes don't propagate).

### 4.2 Step B — sustained-smoke flip XFAIL → PASS

**Selftest:** `tools/testing/selftests/um/template-pause-pool-sustained-smoke/`

Rebuild kernel with `CONFIG_UM_POOL_MEMBER_FORK=y`.  Run
selftest.  Expected outcome:

  * iter 1: PASS (init.sh → MEMBER_DONE with member-1 identity)
  * iter 2: PASS (member-2 identity, no leakage from iter 1)
  * iter 3: PASS (member-3 identity)
  * No kernel panic, no v1-ceiling regression, no zap_pid_ns BUG.

**Acceptance gate:**
  ```
  SUSTAINED_MEMBER_DONE : 3
  Kernel panic          : False
  PASS
  ```

Update selftest to expect PASS by default; remove the XFAIL
sentinel logic.

### 4.3 Step C — ramp + perf

**Files:**
  * `tools/testing/selftests/um/pool-bench/` (scaffold exists)

**Validate:**
  * N=10 → N=100 sustained.
  * Measure p50/p99 take latency.  Target: ≤ 5 ms / ≤ 50 ms.
  * Measure RSS amplification.  Target: 100 members from
    128 MiB master ≤ 200 MiB total RSS (CoW dirty deltas only).
  * 24 h soak: 10 000 take/close cycles, no leak.

If perf misses:
  * Profile fork → child_entry_pool_member → first vcpu_run.
  * If `force_resync_mm` is the bottleneck (walks all PTEs),
    switch to lazy fault-driven mapping for the per-mm memfd
    path (CoW-only via stub-side mmap-on-fault).
  * If memcpy / page fault cost dominates, consider reflink-
    capable backing per Option B.

### 4.4 Step D — close out

**Phase 4 ship:**
  * `syzkaller/vm/uml/uml.go` — ~150 LoC Go shim (lives in
    syzkaller repo).
  * `Documentation/virt/uml/redesign/06-sequencing/post-2026-
    05-19-next-sprint/09-fork-server-USAGE.md` — operator guide.
  * `gh pr create` for syzkaller integration.

**Series 7 (kvm-v2 backend) LKML send:**
  * Already audited (`task #6` complete).
  * Unblocks once pool model proves stable under 24 h soak.

---

## 5. Risks and mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|------------|
| 1 | MAP_PRIVATE flip breaks a consumer we don't know about | medium | high | Gate behind Kconfig; full bisect of physmem readers before flip |
| 2 | KASAN shadow + MAP_PRIVATE interaction breaks fuzz builds | low | medium | KASAN shadow is already MADV_DONTFORK'd in non-fuzz; verify fuzz path under new Kconfig |
| 3 | force_resync_mm cost dominates take latency | medium | medium | Replace with lazy fault-driven push or persistent PTE-marker |
| 4 | Per-member identity_apply has unresolved bugs (we deferred netdev test) | medium | low | Already split log + apply; tap reopen path exists, just untested under per-member backing |
| 5 | seccomp filter on the stub binary doesn't tolerate per-mm memfd | low | high | Already validated in start_userspace_fresh path: stub binary mmaps the override fd successfully (verified in iter 1) |

---

## 6. Decision needed from project owner

**Question:** MAP_PRIVATE flip is the cleanest path forward.  It
trades D36 snapshot-to-disk's "read backing fd" property for
pool-model correctness.

Two responses unblock implementation:

  * **(a)** "Go with MAP_PRIVATE Kconfig-gated.  D36's writer
    becomes `process_vm_readv`-based in a follow-up.  Pool
    model is the production target."
  * **(b)** "Preserve D36's backing-fd path.  Use per-member
    memfd + reflink instead; accept the perf gap."

Either unblocks Step A.

---

## 7. Status snapshot (2026-05-22)

| Item | State |
|------|-------|
| Path A primitive | landed (`e56dff591079`) |
| Path A selftest 20/20 PASS | landed (`af8ebcf75773`) |
| Single-iter pool member | landed; PASS end-to-end |
| `start_userspace_fresh` (per-mm stub_data memfd) | landed (`06968e3f5f27`) |
| `um_skas_force_resync_mm` (eager VMA push) | landed (`85227a0552d9`) |
| Kernel state restoration across iters | landed (`aabfd4082413`) |
| AFL preconditions in `assert_fork_safety` | landed (`757888e5680c`) |
| Identity blob parse/apply split | landed (`4d8b6bc65d3d`) |
| Pre-SIGSTOP host signal block | landed (`a1d6d0020ccc`) |
| Sustained-smoke XFAIL → PASS | **PENDING — gated on Step A** |
| Pool-bench N=100 acceptance | pending |
| syzkaller vm/uml shim | pending |
| Series 7 LKML send | gated on soak |
