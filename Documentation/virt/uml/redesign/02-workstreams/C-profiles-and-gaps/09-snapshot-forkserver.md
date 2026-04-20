# C-09: Snapshot/forkserver host launcher

**Status:** design (2026-04-20); implementation commits to follow
**Effort:** 6 weeks
**Dependencies:** A (backend ops) and B (Layer 2 static-key
                  infrastructure) — both landed
**Blocks:** fuzz profile achieving <50 ms restart; the entire
            "shippable fuzz" milestone (M8); syzkaller's
            `RunSnapshot` API contract (see Prior art §4)

## Goal

Reach steady-state `>=1000` fuzz iterations per second against
`uml/fuzz` by replacing per-iteration cold boot with an in-process
forkserver. The concrete target is **<50 ms from "syz-executor
sends testcase" to "result byte ready for syz-manager"**,
averaged over a sustained 10-minute syzkaller run with the
existing in-tree fuzzing corpus.

The 50 ms target is not arbitrary: it is the threshold at which
UML+snapshot becomes competitive with QEMU+Nyx (Schumilo et al.,
USENIX Sec '21) for the kernel-fuzzing niche. Nyx reaches that
number via a modified KVM dirty-page bitmap; UML reaches it for
free via host `fork()` COW. See §"Prior art" below.

## The model: forkserver first, CRIU-style v2 later

**Chosen v1: AFL-style forkserver over a cooperative in-kernel
seam.** The UML guest boots once to a user-named "ready point,"
quiesces vCPU threads and I/O to a barrier, and `fork()`s itself.
Parent = forkserver (pristine). Child = worker that runs one
testcase and exits. Guest RAM and hostfs page cache inherit via
copy-on-write automatically — only pages the testcase actually
touches cost a minor fault. **KASAN shadow requires one
mmap-flag flip to inherit** (see Q2 below and pull-forward #6);
without that flip the shadow is `MADV_DONTFORK` and the worker
SEGVs on first KASAN access.

**Explicitly not in v1: CRIU-style snapshot-to-disk.** See D35
for the full decision. Short version: UML is one cooperative
process that owns its memory and fds, so parasite injection,
pagemap walking, and TCP-repair are all unnecessary. Everything
CRIU pays for is solved by `fork()` + a small post-fork re-init.
Snapshot-to-disk stays parked as a v2 "survive host reboot"
feature; M8 does not need it.

## Signal-gating contract (D41)

Every C-09 critical section that runs a blocking host syscall
from inside UML kernel context uses UML's per-thread
`signals_enabled` flag as its primitive — NOT host
`sigprocmask`. See D41 for the full rationale and the
historical investigation that led to this decision.

The one-line statement: *host sigprocmask stops host delivery
but not UML's in-kernel IRQ dispatch; `signals_enabled` is what
the dispatch actually consults, so that is what gates UML
kernel critical sections.*

C-09 code obligations:

- Parent forkserver loop (commit 3a+): surround the loop body
  with `os_snapshot_block_iter_signals()` /
  `os_snapshot_unblock_iter_signals()`. These internally use
  `um_set_signals()` per D41.
- Worker `um_snapshot_worker_init()` (commit 3c+): operates
  with `signals_enabled == 0` inherited from the parent's
  pre-fork block. Forget-helpers (commit 3c) don't touch the
  gate. Rebuild-helpers (commit 3d-b) must register new
  handlers / fds / timers FIRST and open the gate LAST.
- Worker return path (commit 3d-c): the tail of
  `um_snapshot_worker_init()` calls `um_set_signals(1)` so
  SIGALRM / SIGIO / SIGCHLD start driving the worker's timer,
  I/O, and stub-reap paths before guest code resumes.
- Ready-point assertions (`um_snapshot_assert_ready()`):
  require `signals_enabled == 1` at ready-point entry so we
  fail loud if a future caller enters an already-gated
  critical section and our block-iter call degenerates into
  a no-op.

Canonical in-tree examples of this pattern:
`os_kill_process` and `os_kill_ptraced_process` in
`arch/um/os-Linux/process.c`, which wrap their `waitpid()`
with `block_signals()` / `unblock_signals()`.

## Approach

### 1. Kernel-side seam (this workstream; in-tree)

One new file, `arch/um/kernel/snapshot.c`, gated by
`CONFIG_UM_FUZZ_HOOKS` (already reserved in `03-profiles/fuzz.md`):

```c
/* arch/um/kernel/snapshot.c (sketch; implementation in commit 1) */

/* Layer 2 gate — 0 ns when fuzz mode is off, matches invariant I1
 * and the three-layers model. */
DEFINE_STATIC_KEY_FALSE(um_snapshot_enabled);

/* Called from init code once the kernel has reached the user-named
 * ready point. Quiesces vCPU threads to a barrier, parks kthreads
 * that would dirty pages (writeback, ksoftirqd), stops the UML
 * timer, closes or CLOEXECs fds on the "must not inherit" list,
 * then fork()s. Parent returns to the forkserver main loop;
 * child returns to the caller and continues execution. */
void um_snapshot_ready(const char *named_point);

/* Called in the worker child right after fork() returns. Re-creates
 * per-process vCPU pthreads and seccomp stub children (which
 * fork() does NOT inherit). Restarts UML's timerfd and signal
 * plumbing. Flips um_snapshot_enabled back off inside the child so
 * the worker runs with zero per-syscall forkserver overhead. */
void um_snapshot_worker_init(void);
```

The `um_on_snapshot_ready()` call site lands in one place only —
a late-stage initcall plus a debugfs/trigger path — so the static
key fires exactly once on the ready path. It is not a hot-path
gate; the `DEFINE_STATIC_KEY` is for compile-out in non-fuzz
profiles (see §5), not for per-syscall cost.

### 2. The trigger — how the guest learns the ready point

Three channels, selected at boot by presence:

| Channel | When it fires | Fuzz profile has it? |
|---|---|---|
| `/sys/kernel/debug/um/snapshot_ready` | write `1` from guest userspace | yes if DEBUGFS=y |
| AFL fd handshake on inherited fds 198/199 | init_post parses env `__AFL_SHM_ID` / fds | yes (compiled in) |
| `mconsole snapshot-ready` command | host sends mconsole command | **no** (fuzz disables MCONSOLE) |

The AFL fd handshake is the required path for the fuzz profile
(because MCONSOLE is off there, per `03-profiles/fuzz.md` line
46). The debugfs entry is the convenient path for research
profile users experimenting with snapshot semantics. The
mconsole command is the nice-to-have for operators running
`prod-with-hooks` who want to snapshot without rebuilding.

All three paths feed into the same `um_snapshot_ready(point)`
entry point. The only channel this workstream commits to
landing in v1 is the AFL fd handshake; debugfs and mconsole are
opportunistic follow-ups.

### 3. Per-iteration protocol (AFL-compatible)

After `um_snapshot_ready()` returns in the parent, the
forkserver loop is the canonical AFL protocol:

```
handshake (once):
    parent  -> 199: [4 bytes: 'AFL\0' | version]   // 'I'm up'

per iteration:
    198 ->  parent: [4 bytes: testcase descriptor or length]
    parent ->  199: [4 bytes: worker pid]
    parent ->       fork() -> child = worker
    worker <-  198: [testcase payload, shmem or pipe]
    worker runs testcase
    worker exit(status)
    parent reaps via waitpid, -> 199: [4 bytes: status]
```

Twelve bytes per iteration; no execve, no linker, no libc init.
Wire format matches AFL++ exactly so `syz-manager`'s existing
`SnapshotInstance` code path (Prior art §4) binds without a new
protocol.

### 4. Post-fork worker re-init

`um_snapshot_worker_init()` handles the five things fork() does
not inherit:

1. **vCPU pthreads.** Only the thread that called `fork()`
   survives. Re-create the N-1 other vCPU host threads and
   restart SMP IRQ delivery. UML already does this on boot — the
   code is in `arch/um/kernel/skas/process.c` and will be
   refactored (commit 1, see below) into a callable
   `um_vcpus_start_all()` so snapshot restore reuses the same
   path.

2. **Seccomp stub children.** The seccomp backend
   (`arch/um/os-Linux/skas/*`) keeps per-guest-mm stub processes
   as separate host pids. `fork()` of the UML kernel does not
   inherit those. Mitigation: kill any inherited stubs (defensive,
   they'll be stale by the time fork returns) and lazily
   re-create them via the existing `userspace()` path the first
   time the worker schedules guest userspace. Cost: ~1–3 ms per
   stub × 1 per guest-mm ≈ 1–3 ms for the typical syzkaller
   testcase that does not create guest-userspace processes.

3. **Timerfd + signalfd.** UML's tick and I/O notification paths
   use timerfd and signalfd. These inherit across fork (they're
   just fds), but the parent and child would then race on the
   same underlying subscriber. Close-and-recreate on the worker
   side; a dedicated `um_timer_reinit()` lives next to the
   existing `um_timer_setup()`.

4. **Host fd hygiene.** A "must not inherit" allowlist:
   forkserver control socket, syzkaller status socket, log fd,
   any mconsole socket if compiled in. Enforced by marking each
   with `FD_CLOEXEC` at creation (cheapest) or by an explicit
   close list in `um_snapshot_ready()` pre-fork.

5. **Pending signals.** Drain any SIGIO / SIGALRM queued in the
   parent before fork; re-arm timers in the worker.

### 5. Compile-time scope

```
CONFIG_UM_FUZZ_HOOKS=y      # fuzz, fuzz-deep profiles
  └── select UM_SNAPSHOT_FORKSERVER
         └── compiles arch/um/kernel/snapshot.c

CONFIG_UM_FUZZ_HOOKS=n      # all other profiles
      arch/um/kernel/snapshot.o not built; no symbols, no gates
```

This keeps `prod-fast`, `sandbox`, `research`, and friends at
exactly zero cost for the forkserver path (compile-out, not
runtime-gate). Matches invariant I1 ("gates that are off are free").

### 6. Host-side launcher — deferred

The host-side pieces in the original spec (`tools/uml/snapshot/`,
`uml-forkserver` binary) live outside
`arch/um/` and `Documentation/virt/uml/`, and therefore need
explicit user sign-off per the AGENT-PROMPT cross-subsystem rule
before landing. The kernel seam above does NOT depend on
`tools/uml/` existing — a syzkaller-or-equivalent host process
that dup2's the two AFL fds and speaks the protocol is enough to
exercise it, and the `kprobes-stress`-style selftest model
scales to drive it directly from a shell script under
`tools/testing/selftests/um/`.

The `tools/uml/snapshot/` design remains planned; it is commit
N (TBD) and will open a separate design thread when we get
there.

## Deliverable

**In-tree, in this workstream (commits land on
`uml-redesign-plan`):**

- `arch/um/Kconfig`:
  - New `config UM_SNAPSHOT_FORKSERVER` under
    `UM_FUZZ_HOOKS` with help text pointing here.
- `arch/um/include/asm/um-mmaps.h` (new, pull-forward #1):
  `struct um_mmap_region` + a table enumerating every kernel
  mmap region (physmem_fd RAM, KASAN shadow, vmalloc runs,
  stub pages, time-travel shm). Consumed by snapshot.c, the
  FD hygiene sweep, and the KASAN MADV_DOFORK flip. Also the
  v2 writer's input when v2 opens.
- `arch/um/kernel/snapshot.c` (new):
  - `um_snapshot_ready()` + `um_snapshot_worker_init()` as
    sketched in §1.
  - Static-key definition + trigger plumbing.
  - Strict ready-point assertions (pull-forward #2).
  - `/sys/kernel/um/state_version` sysfs node, value = 1
    (pull-forward #4).
- `arch/um/kernel/Makefile`: conditional build under
  `UM_SNAPSHOT_FORKSERVER`.
- `arch/um/kernel/physmem.c`: inline assert that `physmem_fd`
  stays `MAP_SHARED` + file-backed + RWX, plus a comment
  explaining the contract v2 depends on (pull-forward #5).
- `arch/um/os-Linux/mem.c`: the KASAN-shadow MADV_DONTFORK
  → MADV_DOFORK flip under `CONFIG_UM_FUZZ_HOOKS`
  (pull-forward #6 / Q2 fix).
- `arch/um/os-Linux/file.c` audit: every host-fd creation site
  marked `O_CLOEXEC` or added to the snapshot "close list",
  with per-FD disposition tag (`INHERIT_ACROSS_FORK`,
  `SERIALIZE_CONTENT`, `RECONSTRUCT_BY_PATH`, `SKIP`) — pull-
  forward #3. Mechanical sweep, one patch, Coccinelle-eligible.
- `arch/um/os-Linux/skas/process.c` refactor: carve out
  `um_vcpus_start_all()` so both boot and `worker_init()` share
  it.
- `arch/um/configs/profiles/fuzz.config`:
  `CONFIG_UM_FUZZ_HOOKS=y`, `CONFIG_UM_SNAPSHOT_FORKSERVER=y`.
- `Documentation/virt/uml/snapshot.rst`: user doc (like
  `kprobes.rst` for C-04) covering how to drive the forkserver
  from a minimal host harness.
- `tools/testing/selftests/um/snapshot-smoke/`: AFL-protocol-
  speaking smoke test that boots UML, performs the handshake,
  forks 10 workers, verifies each reports a distinct pid and
  exits cleanly, asserts `/sys/kernel/um/state_version` == 1.
  Models `kprobes-stress/`.

**Explicitly deferred (needs user sign-off before landing):**

- `tools/uml/snapshot/uml-forkserver` host-side CLI wrapper.
- `tools/uml/snapshot/uml-snapshot save|restore` CRIU-style
  snapshot-to-disk (v2, see D35 "Revisit if").

## Validation

- **Functional:** `snapshot-smoke` selftest — 10 forks, 10
  distinct pids, all children exit(0), dmesg clean
  (no "snapshot drift" warnings, no orphan stubs).
- **Latency:** median fork-to-first-syscall-return <50 ms on a
  KASAN-enabled fuzz profile (the intended config). Measured with
  `clock_gettime(CLOCK_MONOTONIC)` around the AFL 4-byte wire
  exchange; reported in selftest output; tracked in
  `05-validation/benchmarks.md` alongside C-04/C-05 numbers.
- **Throughput:** 1000 iter/s sustained over 10 minutes with
  `snapshot-smoke --iters=600000`. Not a syzkaller run — that's
  C-08 — but a representative forkserver workload.
- **Quiescence:** at `um_snapshot_ready()` entry, no kthread is
  in `TASK_RUNNING` other than the caller. Assert this in
  `snapshot.c`; `WARN_ON_ONCE` if violated. Per-risk-#6 drift
  detection.
- **KASAN compatibility:** research + fuzz-deep profiles boot,
  hit the ready point, fork, and run a KASAN-triggering testcase
  in the worker without splats in the parent.

## Open questions

Six concrete engineering risks that can each blow the 50 ms
budget; each becomes a tracked item in `04-risks/`:

- **Q1: Post-fork page-fault storm.** A worker that touches 64
  MB of fresh RAM pays ~16k minor faults × ~2 µs = ~32 ms on
  the fault path alone. Plan: `madvise(MADV_DONTFORK)` on
  regions that don't need guest-visibility (stub state, KASAN
  shadow for unmapped kernel VAs); pre-fault the guest working
  set in the parent with `madvise(MADV_WILLNEED)`;
  `MAP_SHARED` for genuinely read-only regions.
- **Q2: KASAN shadow + fork() interaction
  (correctness, not perf).** `arch/um/os-Linux/mem.c:49` sets
  `MADV_DONTFORK` on the KASAN shadow region. Contrary to an
  earlier reading of this risk (D35's Q2 text), fork() does
  not COW the shadow — it **unmaps** the shadow range in the
  child. Workers under fuzz profile (which has
  `CONFIG_KASAN=y`) would therefore SEGV on first KASAN-
  instrumented kernel access. Plan (pull-forward item #6 from
  D37): under `CONFIG_UM_FUZZ_HOOKS`, flip the shadow mapping's
  disposition to `MADV_DOFORK` so the child inherits shadow via
  COW like RAM does. Alternative considered: re-call
  `kasan_map_memory()` in `um_snapshot_worker_init()` and
  reconstruct shadow — rejected because it loses valid
  poisoning state, meaning KASAN would miss any use-after-free
  on slabs that were freed before snapshot-ready. The COW
  amplification concern from D35's original Q2 text is real but
  secondary (typical fuzz testcases touch well under 32 MB of
  kernel VA per iteration; amplification costs << 1 ms per iter).
- **Q3: Stub-child respawn latency.** `clone + seccomp_install +
  stub mmap` ≈ 1–3 ms per stub × N vCPUs × guest-userspace
  processes. Plan: pool pre-created stubs in the parent and pass
  via `SCM_RIGHTS`; also keep guest-userspace execution out of
  the fuzz hot path (syzkaller testcases are syscall sequences,
  not userspace code — usually fine).
- **Q4: Host fd hygiene.** Any fd leaked into the worker is a
  correctness bomb or a cleanup-time sink. Plan: mechanical
  sweep of every `socket()` / `open()` / `eventfd()` in
  `arch/um/os-Linux/` (est. ~30 sites) to add `O_CLOEXEC`, plus
  an explicit close-list for fds that must remain open in the
  parent.
- **Q5: Reaping / pid pressure at 1000 iter/s.** `wait4` + pid
  allocation contention on a shared pid namespace shows up
  above ~500 iter/s. Plan: `prctl(PR_SET_CHILD_SUBREAPER)`;
  batch reaping; `CLONE_PIDFD` + `waitid(P_PIDFD)` to avoid
  the pid table.
- **Q6: Snapshot drift.** AFL persistent-mode bugs ("stability
  < 100%") return here if any parent-side mutation leaks
  between forks — a timer firing dirtying a page, a kthread
  running, an RCU callback invoking. Plan: freeze the parent
  at the barrier: stop the timer, park kthreads, drain RCU,
  and assert those conditions at snapshot-ready time. This is
  the hardest of the six; it is the work the 6-week estimate
  actually budgets for.

## Prior art (source of the model)

1. **AFL / AFL++ forkserver.** lcamtuf's `technical_details.txt`
   and AFL++ `src/afl-forkserver.c`. The fd-198/199 wire
   protocol, `__AFL_LOOP(N)` persistent mode and its "stability
   %" warning (source of Q6 above).
2. **Nyx: Greybox Hypervisor Fuzzing** (Schumilo et al., USENIX
   Sec '21, <https://www.usenix.org/system/files/sec21-schumilo.pdf>).
   Proof that 1000+ iter/s fuzz-reset is achievable in a VM; Nyx
   does it via KVM dirty-log. UML should reach it more cheaply
   via host `fork()` COW.
3. **gVisor checkpoint/restore**
   (<https://gvisor.dev/docs/user_guide/checkpoint_restore/>).
   The closest architectural analog: gVisor owns its "kernel"
   (Sentry) and serializes directly without CRIU, streaming
   memory via demand-faulted page file. The approach UML adopts
   for the (deferred) v2 snapshot-to-disk path.
4. **syzkaller snapshot API**
   (<https://pkg.go.dev/github.com/google/syzkaller/vm>). The
   `Env.Snapshot bool` + `Instance.SetupSnapshot(input) error`
   + `Instance.RunSnapshot(input) (result, output, err)` contract.
   We are **implementing an existing syzkaller contract**, not
   proposing a new one. C-08's `vm/uml` backend binds onto these
   two methods once C-09 lands.
5. **CRIU** (<https://criu.org/Checkpoint/Restore>). Not used;
   its ptrace-parasite + pagemap-walk + TCP-repair machinery is
   overkill for a single cooperative process. See D35.
6. **v2 snapshot-to-disk design.** Parked at
   `08-future-phases/02-snapshot-to-disk.md` with format
   decision in D36 (ELF64 core + UML `PT_NOTE` types) and the
   pull-forward items it surfaced in D37. v1 and v2 share the
   kernel-side seam, the mmap enumeration table, the ready-
   point contract, the FD disposition annotations, and the
   state_version sysfs node.

## Pull-forward items from v2 design (D37)

The v2 snapshot-to-disk design in
`08-future-phases/02-snapshot-to-disk.md` surfaced five pieces
of infrastructure plus one v1 correctness bug (Q2 above) that
are load-bearing for v2 but also cheap and useful to land now.
D37 records the decision to pull them into v1 rather than
retrofit. Each folds into a specific v1 commit below; none
requires a new dedicated commit.

1. **Kernel mmap enumeration table** — new
   `arch/um/include/asm/um-mmaps.h` with a
   `struct um_mmap_region { name, base, len, disposition,
   flags }` array populated from every `mmap` call site in
   arch/um. Feeds the Q2 KASAN fix and the fd hygiene sweep.
   Lands in commit 1.
2. **Strict ready-point contract (assert-on-entry)** —
   `um_snapshot_ready()` entry `WARN_ON_ONCE`s on dirty
   inodes, pending RCU callbacks, non-caller kthreads running,
   IRQ in flight, or pending non-SIGCHLD signals. Promotes
   Q6 from "should probably check" to "check loudly, always".
   Lands in commit 2.
3. **FD allowlist extended with disposition annotation** —
   `INHERIT_ACROSS_FORK`, `SERIALIZE_CONTENT`,
   `RECONSTRUCT_BY_PATH`, `SKIP`. One-tag-per-site addition
   to the commit-4 FD_CLOEXEC sweep. Unblocks v2 fd handling
   with no extra kernel work now. Lands in commit 4.
4. **`/sys/kernel/um/state_version` sysfs node** — one
   integer, schema version = 1 at v1 landing. Lands in commit
   5 alongside the selftest (selftests assert this).
5. **`physmem_fd` MAP_SHARED invariant documented + asserted**
   — `BUILD_BUG_ON`-style assert at `create_mem_file()` +
   comment explaining the contract. Lands in commit 1.
6. **KASAN shadow `MADV_DONTFORK` fix (Q2 above)** — flip to
   `MADV_DOFORK` under `CONFIG_UM_FUZZ_HOOKS`. Lands in
   commit 3 alongside `um_snapshot_worker_init()`, which is
   the call site that depends on the fix.

## Commit plan

1. **commit 1 (landed b78df759dfbd):** skeleton + Kconfig +
   **mmap enumeration table** (pull-forward #1) + **physmem_fd
   invariant documented in setup_physmem()** (pull-forward #5).
   No behavior change yet. Builds `UM_FUZZ_HOOKS=y` clean;
   `um_snapshot_ready()` / `um_snapshot_worker_init()` are
   WARN-stubs. The `um_vcpus_start_all()` refactor originally
   scoped here moves to commit 3c where it is first used.
2. **commit 2 (landed c29a9ed9960c):** `um_snapshot_ready()`
   quiesce-and-fork path + **strict ready-point assertions**
   (pull-forward #2). AFL-compatible handshake on fds 198/199,
   single fork, worker `exit_group(0)`. Parent returns to the
   debugfs-write caller. Tested: handshake byte-correct,
   worker pid delivered. Known limitation: parent is unstable
   after return because UML's internal host-thread model
   doesn't expect to have been the parent of a fork; that path
   disappears in commit 3a, which makes parent loop forever.
3. **commit 3 split into 3a/3b/3c/3d per D39.** Commit 3 as
   originally scoped in D35 had four distinct risk tranches —
   parent loop (trivial), KASAN DOFORK (surgical), worker
   reinit (gnarly), worker-runs-guest-code (very gnarly) —
   with wildly different dev costs. Shipping them as one
   commit broke the bisectability rule; splitting into sub-
   commits is mechanical and lets each land on its own merits:
   - **commit 3a:** parent loops forever. Replace one-shot
     body with `while (true) { handshake_iteration() }`. Worker
     still exits immediately. Fixes commit 2's "parent returns
     and panics" limitation by construction: parent never
     returns. Low risk; half a day. Enables multi-iteration
     testing with a trivial host harness.
   - **commit 3b:** KASAN `MADV_DOFORK` fix (D37 pull-forward
     #6) + populate mmap registry with physmem_fd region +
     KASAN shadow region. Under `CONFIG_UM_FUZZ_HOOKS`, flip
     `arch/um/os-Linux/mem.c:49` from DONTFORK → DOFORK so
     workers inherit shadow via COW instead of SEGVing on first
     KASAN access. Low risk; half a day.
   - **commit 3c:** `um_snapshot_worker_init()` minimum — the
     no-crash reinit. Abandon parent-inherited host threads
     (IRQ driver, timer, mconsole), kill inherited seccomp
     stub children, close-and-recreate timerfd/signalfd,
     re-install signal handlers where fork-inheritance isn't
     correct. Worker still exits at end of um_snapshot_ready;
     no guest code yet. Validates that reinit itself doesn't
     crash. Where commit 2's post-fork parent-panic is
     *fundamentally* resolved: even if caller did return,
     reinit has swept up the inconsistent state. High risk;
     2-5 days.
   - **commit 3d split into 3d-a/3d-b/3d-c/3d-d per D40.**
     The originally-scoped 3d combined four high-risk pieces
     — parent waitpid, worker rebuild, worker-returns,
     worker-runs-guest-code — each with distinct failure
     modes and test signals. The same bisectability argument
     that drove D39's 3-split applies: shipping one monolith
     makes any regression bisect to "commit 3d" when in fact
     the useful diagnostic is "bisect to 3d-b vs 3d-c."
     Sub-commits:
     - **commit 3d-a:** parent-side only. After writing worker
       pid, call `os_snapshot_waitpid_status(pid)` + write
       4-byte status to fd 199 per AFL protocol. Worker still
       `exit_group(0)`. Tests commit-2's waitpid-crash
       hypothesis in the commit-3c-stable world.
     - **commit 3d-b:** add the REBUILD half of worker reinit
       that pairs with the FORGET half from commit 3c. New
       helpers: `os_sigio_worker_rebuild`,
       `os_timer_worker_rebuild`, plus `mm_list` clear.
       Worker still exits at end; tests "rebuild itself
       doesn't crash" in isolation. Per D41, rebuild helpers
       must not flip `signals_enabled` to 1 prematurely —
       handlers must be re-registered first, THEN the gate
       opens. End-of-3d-b state: `signals_enabled` stays 0
       in the worker (worker still exits).
     - **commit 3d-c:** flip the worker path from
       `os_snapshot_worker_exit(0)` to `return 0`; worker
       unwinds through `um_snapshot_ready` and continues
       guest execution. Init script: `echo hello; exit 0`.
       Highest-risk sub-commit: RCU / scheduler / kthread
       state inherited from parent may not survive the
       unwind. Bugs here may require changes outside
       `arch/um/` and trigger AGENT-PROMPT cross-subsystem
       sign-off. Per D41, the tail of this commit's worker
       path must `um_set_signals(1)` before returning guest-
       side so SIGALRM/SIGIO/SIGCHLD start driving the
       worker's timer, I/O, and stub-reap paths. This
       commit is also where the waitpid retry lives — the
       parent's state during the wait window changes
       substantively under 3d-b's rebuild, possibly
       unblocking the crash documented in D41; if it does
       not, fallback strategies are named there (pidfd +
       poll, SIGCHLD-driven IRQ reap, or no-status-byte).
     - **commit 3d-d:** scheduler sanitization — necessary but not
       sufficient (v1 limit). After commit 3d-c proved the worker
       CAN run guest code (echo, /bin/true) but crashes on the next
       voluntary `schedule()` at `__set_next_task_fair` because the
       CFS runqueue still references parent-enqueued kthreads, 3d-d
       adds one small exported helper in `kernel/sched/core.c`
       (`sched_worker_detach_other_tasks()`) that strips non-
       current tasks from `rq->cfs_tasks` under `rq_lock_irqsave`
       via `deactivate_task()`. Called from
       `um_snapshot_worker_init()` between the forget and rebuild
       helpers. Per D42 this is the one explicit cross-subsystem
       touch the C-09 v1 makes.
       **Observed outcome:** the helper builds + links on gcc and
       clang, the worker continues to run 1-2 guest syscalls
       (`/bin/echo`, `/bin/true`) per 3d-c, but the KASAN slab-OOB
       at `__set_next_task_fair+0x11b` persists on the first
       `schedule()` after the helper returns. The scheduler has
       invariants beyond `rq->cfs_tasks` list membership (observed
       secondary signal: a `WARN` at `fair.c:5637` complaining
       about `sched_delayed` state). That means detaching from the
       CFS list alone is not enough to sanitize the inherited
       scheduler state; the `sched_entity` in-tree linkage, the
       per-task `thread.switch_buf` jmp_buf, or both, remain
       load-bearing. The v1 helper is kept in place because (a) it
       documents the shape of the problem in one place, (b) it is
       a precondition for any future fix, and (c) ripping it back
       out would hide the fact that we actually need a deeper fix.
       v2 (when someone needs sustained fuzz on blocking guest
       syscalls) replaces it with a freezer-cgroup pre-fork barrier
       + per-task re-clone path per D41's revisit triggers; at that
       point this narrow helper becomes dead code and is removed
       in the same series.
     **Current C-09 v1 ceiling:** a worker runs short non-blocking
     guest programs (trivial execve + exit paths — the 3d-c
     demonstration). Sustained fuzz with execve+wait+filesystem
     I/O remains gated on the v2 freezer-cgroup design. This is
     an honest v1 delivery, not the hypothesised 3d-d breakthrough.
4. **commit 4:** fd hygiene sweep in `os-Linux/` + **per-FD
   disposition annotations** (pull-forward #3). One patch
   touching every `socket()` / `open()` site; largely
   mechanical; Coccinelle eligible per AGENT-PROMPT §2.
5. **commit 5:** `snapshot-smoke` kselftest + user doc
   `Documentation/virt/uml/snapshot.rst` + **state_version
   sysfs node** (pull-forward #4). Validates commits 3a-4
   end-to-end; emits latency and throughput numbers. Selftest
   reads `/sys/kernel/um/state_version` and asserts == 1.
6. **commit 6:** fuzz defconfig enables `UM_SNAPSHOT_FORKSERVER`;
   status-header flip to `landed` in this file.

Commits 1, 2 landed; 3a-3d are the core; 4 is defensive
hygiene; 5 is the Q1 bar; 6 is the wire-up. Each commit
builds+boots clean on both gcc and clang per the AGENT-PROMPT
bisectability rule. Pull-forward items fold into commits 1-5
without adding dedicated commits — see D37 for the rationale
and D39 for the 3-split rationale.

**Realistic timeline to fuzz-works:** commits 3a-3d are the
gating set; best-case 3-5 weeks of disciplined work (with the
commit-3d UML-internals unknown as the dominant risk).
Commits 4-6 add another ~1 week. Then C-08 (syzkaller
`vm/uml` backend, external to linux.git) adds ~4 weeks + LKML/
syzkaller review cycles. Total from today to "researcher runs
`syz-manager --vm=uml`": **2-4 months**, with commit-3d as the
dominant unknown.

## Risk (summary)

Low on the model choice — forkserver over cooperative fork is
a well-trodden path (AFL for 10+ years, Nyx for recent VM
work). Medium on drift (Q6) — getting parent-quiescence right
is subtle and is the bulk of commit 2's effort. Low on
syzkaller integration — the API already exists; we are
implementing a published contract, not negotiating one.

**Mitigation:**

- Every risk above maps to a concrete commit-level test.
- Commit 6 wires fuzz defconfig only after commits 1–5 pass;
  gate can slip a commit if Q1 or smoke gate regresses.
- Snapshot-to-disk (CRIU-like) stays explicitly deferred per
  D35; its risk profile is re-entered only if forkserver
  proves insufficient.

## Shipped-commit audit (post-D41)

Recorded after commit 3d-a's waitpid investigation surfaced
that UML's `signals_enabled` TLS gate is the right primitive
for snapshot-forkserver critical sections (see D41). Each
shipped commit audited for signal-gating correctness; the
conclusion for each is noted inline.

| Commit | SHA | Touches UML signal state? | Correct per D41? |
|---|---|---|---|
| 1 | `b78df759dfbd` | No | N/A — skeleton + Kconfig + registry; no signal path |
| 2 | `c29a9ed9960c` | No (handshake + fork; fork is raw syscall) | Correct |
| 3a | `2bf287b64b16` | Yes (forkserver loop critical section) | Was using `sigprocmask` initially; fixed in `b2e391348e80` hardening |
| 3b | `56a1b0961e44` | No (KASAN DONTFORK→DOFORK; registry populate) | N/A |
| 3c | `8f5e8b2159ea` | Yes (worker_init forget path) | Correct: runs with `signals_enabled == 0` inherited from parent's pre-fork block; forget helpers don't touch signal state; worker exits before the gate matters |
| 3d-a | `a0328b6011ed` | Yes (same critical section as 3a) | See hardening below |
| 3d-a hardening | `b2e391348e80` | Yes (same critical section) | Correct: switched to `um_set_signals` per D41; raw `wait4` via `syscall(__NR_wait4)` to bypass glibc cancellation point |

No shipped commit is retroactively incorrect under D41's framing.
The only code change that the audit warrants is the hardening
already landed in `b2e391348e80` (switches
`os_snapshot_block_iter_signals` /
`os_snapshot_unblock_iter_signals` from `sigprocmask` to
`um_set_signals`). Commit 3d-a's waitpid-crash remains open;
D41 names the concrete fallback strategies for commit 3d-c to
try if the primitives alone don't unblock it.

Follow-up code change (this series): extend
`um_snapshot_assert_ready()` to assert
`signals_enabled == 1` at ready-point entry. Fails loud when a
future caller enters an already-gated critical section; the
assertion is cheap and has zero false-positives today.
