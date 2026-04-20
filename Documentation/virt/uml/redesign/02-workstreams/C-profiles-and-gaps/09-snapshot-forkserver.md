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
testcase and exits. Guest RAM, KASAN shadow, and hostfs page
cache inherit via copy-on-write automatically — only pages the
testcase actually touches cost a minor fault.

**Explicitly not in v1: CRIU-style snapshot-to-disk.** See D35
for the full decision. Short version: UML is one cooperative
process that owns its memory and fds, so parasite injection,
pagemap walking, and TCP-repair are all unnecessary. Everything
CRIU pays for is solved by `fork()` + a small post-fork re-init.
Snapshot-to-disk stays parked as a v2 "survive host reboot"
feature; M8 does not need it.

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
- `arch/um/kernel/snapshot.c` (new):
  - `um_snapshot_ready()` + `um_snapshot_worker_init()` as
    sketched in §1.
  - Static-key definition + trigger plumbing.
- `arch/um/kernel/Makefile`: conditional build under
  `UM_SNAPSHOT_FORKSERVER`.
- `arch/um/os-Linux/file.c` audit: every host-fd creation site
  marked `O_CLOEXEC` or added to the snapshot "close list".
  Mechanical sweep, one patch.
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
  exits cleanly. Models `kprobes-stress/`.

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
- **Q2: KASAN shadow COW amplification.** Every kernel VA the
  testcase dirties also dirties 1/8 of that in shadow; scatter
  over 256 MB VA → 32 MB shadow → ~8k extra COWs. Plan: in the fuzz
  hot loop, accept SW_TAGS or classic KASAN with the smaller
  shadow; KASAN+KMSAN together go to a "triage" profile that
  explicitly skips the forkserver loop.
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

## Commit plan

1. **commit 1:** skeleton + Kconfig + refactor
   `um_vcpus_start_all()` out of boot code. No behavior change
   yet. Builds `UM_FUZZ_HOOKS=y` clean but does nothing when
   `um_snapshot_enabled` is off.
2. **commit 2:** `um_snapshot_ready()` quiesce-and-fork path.
   Parent enters a minimal forkserver loop; worker exits
   immediately. Enough to hit the AFL handshake from a host
   harness.
3. **commit 3:** `um_snapshot_worker_init()` — vCPU re-create,
   stub respawn, timerfd/signalfd recreate. Worker now runs
   actual guest code.
4. **commit 4:** fd hygiene sweep in `os-Linux/`. One patch
   touching every `socket()` / `open()` site; largely
   mechanical; Coccinelle eligible per AGENT-PROMPT §2.
5. **commit 5:** `snapshot-smoke` kselftest + user doc
   `Documentation/virt/uml/snapshot.rst`. Validates commits 2–4
   end-to-end; emits latency and throughput numbers.
6. **commit 6:** fuzz defconfig enables `UM_SNAPSHOT_FORKSERVER`;
   status-header flip to `landed` in this file.

Commits 1–3 are the core; 4 is defensive hygiene; 5 is the Q1
bar; 6 is the wire-up. Each commit builds+boots clean on both
gcc and clang per the AGENT-PROMPT bisectability rule.

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
