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

### 3.1 Three approaches, two ruled out empirically

**Option A: MAP_PRIVATE flip (ruled out).**

Change `os_map_memory`'s physmem flags from `MAP_SHARED` to
`MAP_PRIVATE`.  Idea: fork() then gets host-kernel page-table
CoW for free.

  * **Why it doesn't work:** UML kernel↔stub coherence relies
    on both ends mapping `physmem_fd` MAP_SHARED.  Concretely,
    `arch/um/kernel/skas/stub.c::syscall_handler` issues
    `STUB_MMAP_NR` with hard-coded `MAP_SHARED | MAP_FIXED`
    (line 54).  If the kernel-side mapping flips to MAP_PRIVATE,
    kernel writes at kernel VAs go to anonymous CoW pages and
    never reach the stub's MAP_SHARED view of the same fd; the
    very first userspace page delivery breaks (observed:
    init.sh segfault at libc 0x4014c490 on iter-1 with private
    kernel mapping).
  * Flipping BOTH ends to MAP_PRIVATE doesn't help either —
    different VMAs of the same file get independent CoW pages,
    so kernel and stub still diverge.

**Option B: per-member memfd at fork time, runtime mmap-FIXED
swap (attempted; empirically broken with dispositive bisect).**

In `child_entry_pool_member`, create a fresh memfd, replicate
master's physmem content into it via mmap+memcpy, and
mmap-FIXED swap the kernel-side mapping over to the new fd.
Update the global `physmem_fd` so new stubs (and future
`phys_mapping` callers) reference the new fd.

  * **Implementation status:** helpers
    (`os_create_memfd`, `os_create_tmpfile`,
    `os_mmap_rw_scratch`, `os_remap_region_shared`,
    `os_dup_file`, `um_pool_replicate_physmem`,
    `um_pool_remap_self_test`) landed.  See
    `arch/um/os-Linux/process.c` and `arch/um/kernel/physmem.c`.

  * **Empirical bisect (2026-05-22):**

    | Variant                                                       | Result            |
    |---------------------------------------------------------------|-------------------|
    | (1) same-fd mmap-FIXED of physmem region (self-test)          | PASS              |
    | (2) dup'd-fd mmap-FIXED (same file, different fd value)       | PASS              |
    | (3) new-fd mmap-FIXED to `memfd_create`-backed fd             | FAIL (SIGALRM)    |
    | (4) new-fd mmap-FIXED to `O_TMPFILE` on /dev/shm or /tmp      | FAIL (SIGALRM)    |
    | (5) skip kernel-VA mmap, just swap global `physmem_fd`        | FAIL (master loop)|
    | (6) anon intermediate (`os_remap_region_via_anon`) → new fd    | FAIL (SIGALRM)    |
    | (7) close inherited io_uring fds, then mmap-FIXED swap (closed=2) | FAIL (SIGALRM)    |
    | (8) post-swap sigtimedwait drain of pending signals (drained=0) | FAIL (SIGALRM)    |
    | (9) post-swap timer_gettime + os_busy_wait_ns(200ms)            | FIRES (timer disarmed, ov=0) |
    | (10) post-swap 50ms periodic timer + 525ms busy-wait             | FIRES (itv cycles at ~50ms, ov stays 0 = signals delivered) |
    | (11) replicate + arm 50ms periodic + diag through start_userspace_fresh | ARMED through stub creation (itv stays ~50ms post-startfresh) |
    | (12) plain replicate, no instrumentation                          | FAIL (bash hangs in sleep) |
    | (13) replicate + post-swap timer_worker_forget+rebuild+one_shot   | FAIL (bash hangs in sleep) |
    | (14) variant 13 + post-swap os_idle_prepare() (rebuild signalfd)  | FAIL (bash hangs in sleep) |
    | (15) replicate + 200 ms in-kernel SIGALRM drain + timer reprime    | FAIL (bash hangs in sleep) |
    | (16) replicate + arm 1s one-shot + 1.5s busy_wait → diagnose        | FIRES (itv 1s→0, ov 0→0 = 1s timer fires + signal delivered) |
    | (17) MINIMAL LINUX-ONLY REPRO (no UML, `tools/testing/selftests/um/mmap-fixed-sigalrm-repro/`) | **PASS** (post_swap_sigalrms=1) → **dispositive proof the bug is NOT in the host kernel's signal/timer subsystem; it is UML-specific** |
    | (18) replicate + cond_resched + schedule_timeout_interruptible(1ms) | FAIL (bash hangs in sleep) — scheduler hygiene yield does not recover |
    | (19) replicate + TEMPORARY printk in try_to_wake_up + hrtimer_start_range_ns + hrtimer_interrupt + wait_stub_done_seccomp | **DISPOSITIVE LOCALIZATION:** bash (pid=1) never appears in `try_to_wake_up` post-MEMBER_ALIVE_1; bash's `sleep(1)` NEVER reaches `hrtimer_start_range_ns` (40 hrtimer_start events: 39 are ksoftirqd, 1 is bash's early 1ms boot one_shot); `HRTIMER_INT next=KTIME_MAX` confirms rb-tree empty. The chain is broken **upstream of UML's hrtimer wheel** — bash never traps into the kernel for nanosleep. Stub-side SIGSYS handler path (postmortem candidate (1) clone-CLONE_VM stub interaction) is the next focused investigation target. |
    | (20) replicate + start_userspace/userspace_tramp trace (FRESH + SU + TRAMP + INIT_NEW_CTX printks) | **FURTHER LOCALIZED:** The FIRST new stub (start_userspace_fresh path, per-mm memfd override) WORKS and services bash's syscalls (`echo` writes go through, including TPPM_POST_PAUSE + TPPM_MEMBER_ALIVE_1).  The SECOND new stub (start_userspace path for /bin/sleep's forked mm, default phys_mapping → new physmem_fd) clones() OK but **never completes execve → never sends its first SIGSYS back to kernel** (kernel stuck in wait_stub_done_seccomp for the new stub).  Baseline (no replicate) gets 50+ wait_stub events; with replicate, stops at 16 = the new stub's init wait.  Hypothesis: the post-replicate VM state breaks the second stub's execveat path (stub_exe_fd inheritance, init_data socket transfer, or uml-userspace binary's MAP_SHARED on new physmem_fd at the second stub's stub_data offset). |

    Variants (1) and (2) prove the mmap-FIXED operation itself
    is benign; (3) and (4) prove the regression is tied to the
    file/inode identity of the new mapping; (5) proves that
    kernel↔stub coherence cannot be preserved by skipping the
    kernel-VA swap; (6) proves the regression survives an
    intermediate MAP_ANONYMOUS|MAP_SHARED mapping at the
    target VA; (7) disproves the io_uring TIF_NOTIFY_SIGNAL
    hypothesis — closing 2 io_uring fds (ubd + hostfs writeback)
    before the swap does not restore SIGALRM delivery; (8)
    initially suggested the "stuck pending signal" hypothesis
    (drained=0 via sigtimedwait post-swap), BUT variants (9)
    and (10) DISPROVED that interpretation.  Variant (9) showed
    a single 50ms one-shot timer fires correctly post-swap
    (it_value drops to 0, overrun=0 = signal-delivered-once).
    Variant (10) ran a 50ms PERIODIC timer over 525ms and
    observed it_value cycling at ~50ms with overrun stable at
    0, proving SIGALRM is delivered every tick.

    **The bug is NOT in signal delivery.**  The SIGALRM-after-
    swap dispatch is fine.  The actual breakage is downstream:
    UML's scheduler / hrtimer wake-up path doesn't propagate
    the tick to bash's userspace task even though the kernel-
    side timer handler is running.

  * **Symptom of (3)/(4):** bash reaches `TPPM_MEMBER_ALIVE_1`
    then `sleep 1` never returns — host SIGALRM stops being
    delivered to this thread.  The first SIGALRM after the
    swap arrives (count goes from 8→9 during the 100 ms
    replicate window) but no further deliveries occur.
    `os_timer_one_shot` is called repeatedly post-swap (counter
    grows) but the host timer's SIGALRMs never reach our
    handler.  Re-issuing `sigaltstack` + `sigaction(SIGALRM)` +
    `timer_create` does NOT restore delivery.

  * **Symptom of (5):** master enters a fast SIGCONT loop
    (count=233+ in seconds) because the kernel-VA mapping
    still points at master's fd while `phys_mapping` returns
    the new fd; subsequent stub mmaps populate user-VAs from
    a different page set than the kernel sees, breaking the
    SKAS contract.

  * **Conclusion:** the host kernel binds something to the
    inode underlying the mmap'd VMA in a way that disrupts
    POSIX-timer-based SIGALRM delivery when the binding
    changes mid-process.  Workarounds tried (sigaltstack
    reinstall, sigaction reinstall, timer recreate, all in
    combination) do not recover.  Root cause not yet
    isolated; candidates: io_uring's pinned-page registry
    (UML uses io_uring for ubd and hostfs writeback —
    inherited across fork via CLONE_FILES with the original
    fd's pages registered), hostfs writeback bdi binding,
    or a kernel reverse-mapping cache.

  * **Call site disabled** in `child_entry_pool_member`.
    Helpers stay in tree.  Next investigation step: strace
    the UML process during the swap window and identify the
    host syscall failing or the in-flight io_uring request
    breaking.

**Option C: per-member memfd, set up at master boot via
Kconfig.**

Have master's `setup_physmem` create the memfd-backed file from
the start, behind `CONFIG_UM_POOL_MEMBER_FORK=y`.  Each forked
member is then a CoW (MAP_SHARED inherited via fork inherits
the same fd reference; the member can re-`open(/proc/self/fd/N)`
to detach).  This avoids the runtime mmap-FIXED swap entirely.

  * **Pro:** No runtime mapping disruption; signal/timer paths
    untouched.
  * **Pro:** Master's mapping is always MAP_SHARED on memfd;
    snapshot-to-disk (D36) continues to read the same file
    directly.
  * **Con:** Doesn't solve sharing across forks by itself —
    fork inheritance of MAP_SHARED still aliases members to
    master.  Combine with `dup`-then-`copy_file_range`-to-new-
    memfd in child to break the aliasing (with cross-fs EXDEV
    handled by mmap+memcpy fallback, as in the Option B
    helpers).  The CRITICAL difference vs Option B: do the
    replication BEFORE master ever maps physmem at kernel VAs
    (i.e., during master boot for the per-member template), so
    no mmap-FIXED swap is ever needed at member-fork time.
  * **Status:** designed but not implemented.  Recommended
    next step.

### 3.2 Recommendation (revised after empirical findings)

**Option C is the most promising path forward.**

Reasoning:

  * Option A (MAP_PRIVATE flip) is fundamentally incompatible
    with UML's hard-coded stub `MAP_SHARED` mmap in
    `stub.c::syscall_handler`.  Cannot be fixed without a
    significant stub rewrite.
  * Option B (runtime mmap-FIXED swap) is implemented and
    correct in theory, but empirically disrupts UML's
    signal/timer subsystem.  Until the disruption is root-
    caused, this approach is blocked.
  * Option C (per-member memfd, set up at boot, replicated
    in child via the existing helpers) avoids the runtime
    mmap swap.  The replication can still use the
    `os_create_memfd` + `os_mmap_rw_scratch` + memcpy
    primitives, but at MASTER BOOT TIME (before any kernel-
    side mapping is established).  Member fork inherits via
    standard fork CoW on master's MAP_SHARED memfd mapping;
    member then `dup` + replicate to a new memfd and the
    kernel mapping at member's address space stays MAP_SHARED
    on the (now per-member) fd.  The KEY trick: the kernel-VA
    mapping is established ONCE per process via the normal
    boot path, never re-established via MAP_FIXED.
  * Estimated cost: 5–50 ms per member fork depending on
    sparse-page density (most of physmem is zeros during
    syzkaller-style boots).  Acceptable for p99.

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
| Replication primitives (`os_create_memfd`, `os_create_tmpfile`, `os_mmap_rw_scratch`, `os_remap_region_shared`, `os_dup_file`) | landed (`0d323527e32d`, `8369c1bbd448`) |
| `um_pool_replicate_physmem` + `um_pool_remap_self_test` | landed (`0d323527e32d`, `8369c1bbd448`); call site unwired |
| Step A — Option A (MAP_PRIVATE) | RULED OUT: stub `MAP_SHARED|MAP_FIXED` is hard-coded |
| Step A — Option B (runtime mmap-FIXED swap) | IMPLEMENTED, regresses SIGALRM; dispositively bisected (`8369c1bbd448`) — host kernel binds something to the inode underlying the VMA |
| Step A — Option C (boot-time per-member memfd) | designed; per the bisect, same root cause likely applies (any mmap-FIXED-to-different-inode breaks SIGALRM) |
| Step A — next debug step | strace UML kernel across the swap with proper PID tracking (UML kernel is strace's CHILD; attach via `/proc/<strace_pid>/task/*`); inspect `io_uring_register`/`io_uring_enter` for fd-pinning evidence |
| Sustained-smoke XFAIL → PASS | **PENDING — Step A unresolved** |
| Pool-bench N=100 acceptance | pending |
| syzkaller `vm/uml` shim | pending (lives in syzkaller repo; spec at `11-syzkaller-shim-spec.md`) |
| Pool USAGE operator guide | landed (`7de8423ddd48`) |
| Series 7 LKML send | gated on 24 h soak; previous soak SIGTERM'd at 1 h |
