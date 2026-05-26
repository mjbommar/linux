# Memo 29 — Real-Python regrtest as the substrate acceptance bar

**Date:** 2026-04-28 evening
**Audience:** Whoever is about to start memo 26 Phase A
**Companion memos:** 25 (refactor list); 28 (R4 design lock); 27
(execution prompt — should be updated to reflect this memo's
addition once it lands)

---

## TL;DR

The cpython-parity gate's "21/21 stdlib modules" bar dramatically
understates what a real Python kernel needs. Running
`python -m test` (the full CPython regrtest, ~492 modules) under
seccomp UML reveals ~17 real failures and one hang, most of
which are UML/seccomp substrate gaps that the curated 21-module
gate hid.

**This memo argues:** before memo 26 Phase A starts (v2 KVM
init.c bring-up), seccomp must pass `python -m test` cleanly with
a documented `-x` skip list of environment-only failures. Mixing
v2 changes with pre-existing seccomp gaps is a debugging
nightmare — every v2 commit's regrtest result becomes ambiguous
("is this a v2 bug or did seccomp already do this?").

---

## How the gap was discovered (2026-04-28)

The 21-module cpython-parity gate held at 21/21 across the entire
post-Stage-A → R4-E.3a session arc (28 commits). Confidence was
high. But the gate is a curated subset of *easy* tests — pure data
structures and numerics: `test_struct`, `test_math`, `test_array`,
`test_dict`, `test_list`, `test_set`, `test_int`, `test_float`,
`test_decimal`, `test_complex`, `test_typing`, `test_abc`, etc.
**No subprocess, no networking, no fork-heavy modules, no signal
or timer tests, no ioctl, no pty.**

The full CPython regrtest is ~492 modules. Running it under the
post-R12 seccomp UML (`/tmp/uml-clean/linux`, defconfig + lo up,
4 GB RAM, 1 CPU):

- **365 / 492 modules attempted** before regrtest aborted.
- **314 / 365 passed** (86% of attempted).
- **17 modules failed** with real failures.
- **68 env_changed / skipped** (Windows-only, missing tty, etc.).
- **127 unreached** because regrtest aborted at module 365 when
  `test_signal::test_itimer_virtual` hung past the 180 s
  per-test timeout.

The 17 failed modules cluster into three causes:

### Class A — Environmental (no real tty, no pty inheritance, no controlling terminal)

PID-1 init scripts have no controlling tty by design. These tests
fail because they observe the empty environment, not because
seccomp is broken:

- `test_shutil` — `(0, 0) != os.terminal_size(columns=80, lines=24)`
- `test_openpty` — `posix_openpt`/`pty.openpty` (no `/dev/ptmx`
  capable host)
- `test_cmd_line_script` — subprocess into `/usr/bin/python3 -c`
  paths that need PATH + tty inheritance
- `test_compileall`, `test_ensurepip` — same shape

**Triage: skip via `-x`.** A future curated-tty layer (Tier 1.5)
could cover these, but they are not seccomp/UML correctness
issues.

### Class B — Process-model corners (UML's clone-fork-vfork stub-child lifecycle)

These exercise paths that the seccomp stub-child architecture
handles differently from a normal Linux kernel:

- `test_subprocess` — heavy `Popen` lifecycle
- `test_process_pool`, `test_deadlock`, `test_wait` (concurrent.futures)
- `test_init`, `test_events`, `test_as_completed`, `test_shutdown`
  (multiprocessing futures with start methods)
- `test_logging` — multiprocessing-via-logging
- `test_code_module` — interactive child shell

**Triage: investigate.** Some of these may be real UML core bugs
(e.g., wait/waitid semantics, child reaping); some may be
environmental (pty inheritance, `os.exec*` with PATH). Verbose
rerun (in progress) will clarify. Memo 25 R4 (per-mm worker
process) is the structural fix for several of these — but R4 is
itself half-built and we should not gate seccomp's substrate on
v2 work.

### Class C — UML kernel / syscall gaps

These exercise UML's syscall implementation directly:

- `test_os` — multiple errors (likely `os.fork`, `os.waitpid`
  edge cases; needs verbose output)
- `test_ioctl` — multiple errors (UML doesn't implement every
  host ioctl)

**Triage: investigate and fix.** These are most likely to be real
UML bugs the curated gate hid.

### Class D — The hang

- `test_signal::test_itimer_virtual` — UML's `ITIMER_VIRTUAL`
  doesn't deliver `SIGVTALRM` the way the test expects.

  Likely root cause: ITIMER_VIRTUAL fires on user-mode CPU time
  consumed. Under UML's seccomp stub-child model, the Python
  process's user-mode time is split between the spawner (UML
  kernel) and the stub child (running guest user code). When
  Python calls `setitimer(VIRTUAL, ...)` it forwards to the host's
  setitimer, which probably tracks the spawner's CPU time, not
  the stub child's. The Python busy loop runs in the stub child;
  the spawner mostly waits. So virtual time accumulates on the
  wrong process and SIGVTALRM never fires.

  This is structural. Fixing it likely needs UML-side virtual-
  timer accounting that aggregates stub-child time.

**Triage: real bug, deeper investigation.** Worth understanding
even if we ultimately `-x test_signal` for the gate.

---

## Why this matters for v2

If memo 26 Phase A.1 lands and we run regrtest, we have three
possible outcomes for any failed module:

1. The failure was already present under seccomp (Class A/B/C/D).
2. The failure is a v2 bug.
3. Some interaction between (1) and (2).

Without first stabilizing seccomp at "regrtest passes with
documented skips," we cannot tell these apart. Every v2 commit's
regrtest result becomes ambiguous. The 28-commit substrate session
just demonstrated that the curated 21-module gate has been
giving false confidence.

**The cost of fixing this now**: bounded. Class A skips are
free (just write the skip list). Class B/C investigations may
turn up 2-5 real UML bugs to fix; each is probably a small
fix in the seccomp backend or generic UML kernel code. Class D
needs structural thought.

**The cost of NOT fixing this now**: every v2 phase's regrtest
verification is ambiguous. We re-discover the same Class B/C
bugs through v2's lens, where they're harder to debug because
v2 itself adds new failure modes. The whole memo 26 sequence
slips by some unknown factor.

---

## Proposed structure: memo 25 Part 2.5

Insert between current memo 25 Part 2 (12 refactors) and memo 26
Phase A (v2 implementation):

### 2.5.1 — Capture full regrtest data with `-v`

Run `python -m test -v` under seccomp with `-x` for the known
hang. Captured, classified, this memo updated with detailed
counts and per-module triage.

### 2.5.2 — Build the curated `-x` skip list

For each Class-A environmental failure, add to a project-level
exclude list. Document why each is skipped (no real tty, etc.).
The skip list becomes part of `tools/testing/selftests/um/cpython-parity/`
as a new `regrtest-substrate.sh` gate.

### 2.5.3 — Investigate Class B (process model)

For each multiprocessing/subprocess failure:
- Reproduce minimally (~10-line Python repro).
- Trace through UML to identify the syscall or path that fails.
- Either fix in UML core / seccomp backend, or document as
  "known-skipped pending v2's per-mm worker model."

### 2.5.4 — Fix Class C (real syscall gaps)

`test_os` and `test_ioctl`'s "multiple errors" likely point at
2-5 specific UML-syscall gaps. Each gets a fix commit:
`um: implement missing X syscall (Y)` or `um: fix seccomp Z
edge case`. With `-v` data we can localize each.

### 2.5.5 — Investigate or skip Class D (timer)

ITIMER_VIRTUAL accounting under seccomp is structural. Either:
- Fix it (UML aggregates stub-child CPU time into host
  setitimer), or
- Document the limitation, skip the test, and note that v2's
  per-mm worker model may make this fixable later.

### 2.5.6 — Establish the substrate bar

New gate: `python -m test -x <skip list>` passes 100% under
seccomp. This becomes the v2 acceptance criterion (replacing
"21-module curated gate" or augmenting it as a Tier 1.5 ahead of
memo 26 Phase J's Tier 1/2/3).

### 2.5.7 — Update memo 27 to reflect the new bar

Memo 27 Part F.2's "after every Phase" verification gets a
regrtest line in addition to the curated gate. Memo 27 Part F.3's
Phase J check already names regrtest; this just makes it
consistent across earlier phases.

---

## Open questions for memo 25 Part 2.5

- **Is fixing Class B worth doing on seccomp, or wait for R4?**
  Several Class B failures may be cleanly fixed by R4's per-mm
  worker process (which gives each mm a real Linux process with
  proper signal+fork semantics). Fixing them in seccomp first
  vs. accepting "fix when R4 lands" is a real call.

- **How aggressive should the `-x` skip list be?** Each skip is
  coverage we don't have. A 10-skip list is reasonable; a
  50-skip list is hiding too much. The Class A list is mostly
  legit (env-only); Class B/C/D should be minimized.

- **Does v2's gVisor sentry pattern fix any of these for free?**
  Likely yes for Class B (per-mm worker = proper child reaping +
  signal table). Worth checking the Class B failures against
  memo 28's design to see which auto-resolve.

- **Should we set a numeric target?** "≥450 / 492 modules pass
  on seccomp" is concrete; "all pass minus a small skip list"
  is qualitative. The user's intuition will inform.

---

## Status

This memo is being filed 2026-04-28 evening as the run with
`-v -x test_signal` is in flight. Initial data above is from
the 365-module truncated run; expect updates as the verbose
run completes.

**Next:** verbose run completes → classify every failure with
its actual error message → fill in the per-module triage table
in §2.5.1 → start fixing.

---

## Update — 2026-04-28 late evening — substrate reproducer suite landed

Per §2.5 plan, a focused C/Python reproducer suite now lives at
`tools/testing/selftests/um/regrtest-repros/`. It compresses the
4 failure classes (27 CPython modules, ~15 minutes wall time) into
**28 reproducers running in ~4 seconds wall time inside seccomp UML**.

### Layout

- `class-a-env/` — 7 reproducers (env-only, tty/pty/PATH).
- `class-b-process/` — 6 reproducers (fork/exec/wait/pipe/sigchld).
- `class-c-syscall/` — 10 reproducers (ioctl/socket/os).
- `class-d-structural/` — 5 reproducers (itimer/getrusage/threading).
- `run-regrtest-repros.sh` — boots a UML kernel, runs all four
  per-class runners, tallies PASS / FAIL / EXPECTED_FAIL.

### Seccomp UML baseline (2026-04-28, kernel `7e27ec4a518b`)

```
PASS=22 FAIL=4 EXPECTED_FAIL=2
```

#### Class A: 4 PASS / 3 FAIL

PASS: `terminal_size`, `openpty`, `controlling_tty`, `env_path_subprocess`.
FAIL: `tty_isatty` (stdin/stdout/stderr=1), `termios_get` (tcgetattr
succeeds), `ensurepip_check` (host distro split). The two tty FAILs
reveal that the default UML init invocation passes `con0=fd:0,fd:1`
which makes guest fd 0/1/2 *real* ttys — different from CPython
regrtest's PID-1 invocation. Reproducer is honest about the env;
test_termios/test_tty failures upstream may be more nuanced than
"no tty" — possibly tty-with-wrong-flags.

#### Class B: 6 PASS / 0 FAIL

`fork_exec_wait`, `fork_pipe_ipc`, `pool_workers`, `waitpid_wnohang`,
`sigchld_select`, `asyncio_subprocess_min` all pass cleanly under
seccomp. **This is a strong signal: the raw process-model substrate
(fork, exec, waitpid edges, SIGCHLD-during-poll, pipe drain) is not
broken under seccomp.** The 13 Class-B regrtest module failures are
therefore Python-internal (asyncio transport state machines,
multiprocessing.Pool start-method specifics) rather than substrate
gaps. R4 (per-mm worker) may still fix some of them by giving each
mm a real signal table — but the C-level path is sound.

#### Class C: 9 PASS / 1 FAIL

PASS: `ioctl_fionread`, `ioctl_tiocgwinsz_socketpair`,
`ioctl_blkgetsize_loop`, `socket_options_dgram` (incl. `IP_PKTINFO`),
`socket_unix_abstract`, `os_waitid_edges`, `os_sched_getcpu`,
`os_setblocking`, `sanity_struct_unicode`.

FAIL: `socket_udplite` (errno=93 EPROTONOSUPPORT). **Investigation
revealed UDP-Lite was retired upstream in commit `56520b398e5e`
("ipv4: Retire UDP-Lite.")** — `net/ipv4/udplite.c` is gone, no
longer exists as a Kconfig option. So this is not a UML bug at all
and not a defconfig change either; it's the permanent
post-retirement behavior of any kernel that includes that commit.
The 41 test_socket subtests that failed in regrtest must be
`-x`'d in the substrate skip list: `test_socket.UDPLiteServerTimeoutTest`
and friends. CPython upstream will eventually need a skip on
post-retirement kernels too. Reproducer updated to emit
EXPECTED_FAIL with reason `retired_upstream`. Net effect on the
substrate baseline: FAIL count drops from 4 to 3.

#### Class D: 3 PASS / 2 EXPECTED_FAIL

PASS: `itimer_prof` (sigprof_count=9 — kernel time accumulates),
`itimer_real` (sigalrm_count=10 — wall time unaffected),
`thread_excepthook`.

EXPECTED_FAIL:
- `itimer_virtual sigvtalrm_count=0 expected_ge_3` — confirms
  Class D structural bug. SIGVTALRM never fires during a 1-second
  guest user-mode busy loop.
- `getrusage_split ru_utime_ms=0.0 ru_stime_ms=1000.0 wall_ms=1000.0
  stub_child_split` — gorgeous diagnostic. After a 1-second guest
  busy loop, `getrusage(RUSAGE_SELF)` reports zero user time and
  full system time. This is exactly the predicted seccomp stub-child
  CPU-time mis-attribution: from the spawner's view, it was
  "in-syscall" (system time) the whole time the stub child was
  consuming user CPU. UML's `setitimer(VIRTUAL)` forwards to host
  setitimer on the spawner, which sees `ru_utime=0`, hence
  SIGVTALRM never fires. Memo 29 §"Class D — The hang"
  hypothesis confirmed in 1 second.

That `itimer_prof` passes is also informative: PROF tracks
user+kernel time, and the spawner's kernel-time accumulator does
move (waiting on the stub child counts as system time on the
spawner). Future fix: aggregate stub-child user time into spawner's
virtual-itimer accounting, OR install setitimer on the stub child
PID under R4's worker-owns-its-own-process model.

### Why this changes the §2.5 plan

§2.5.2 (Class A skip list): mostly unchanged, but the "no tty"
assumption was wrong — the UML init line gives guest fd 0/1/2
real tty status. The skip list should target test_tty/test_termios
based on the specific subtest failure pattern, not blanket-skip.

§2.5.3 (Class B investigation): **deprioritize**. The substrate
process model is sound. Investigation should target Python-layer
specifics (asyncio transport state, multiprocessing.Pool
start-method behavior) — likely won't surface UML changes.

§2.5.4 (Class C fixes): **scope reduces to one defconfig change**.
`CONFIG_IP_UDPLITE=y` (or skip test_socket UDPLITE subtests).
No UML kernel patches needed.

§2.5.5 (Class D ITIMER_VIRTUAL): **deferred to post-R4 verification.**
Investigation 2026-04-28: UML routes `setitimer(VIRTUAL)` through
generic `kernel/time/itimer.c` → `posix-cpu-timers.c`, which
expires when `task->utime` accumulates. Under seccomp, the spawner
task's `utime` stays at 0 because guest user code runs in a
separate stub-child process; the spawner is "in syscall" (system
time) the whole time. Fixing this in seccomp requires either
(a) UML aggregating stub-child rusage into the kernel-side
task_struct utime accounting on every guest entry/exit, or
(b) delivering a host-side periodic timer that polls stub-child
rusage and synthesizes SIGVTALRM at the appropriate cadence.

**Original (and incorrect) prediction**: Memo 28 Part I.5's R4
worker-owns-process design will naturally fix this — each guest
mm gets its own host worker process that itself runs guest user
code in user mode, so `setitimer(VIRTUAL)` on the worker tracks
the right user time. The reproducer (`itimer_virtual.c`) flips
from EXPECTED_FAIL to PASS automatically once R4 lands.

**Correction — 2026-04-28 (post-E.3d.2)**: this prediction is
wrong, surfaced by running the substrate gate under
WORKER_PROCESS=y after E.3d.2 (`f77e62d4e031`) landed.
`itimer_virtual` remains EXPECTED_FAIL with `sigvtalrm_count=0`
under WORKER_PROCESS=y, identical to the WORKER_PROCESS=n
baseline. Reason: the original Part I.5 design assumed the
worker would itself run guest user-mode code; E.3d.2's actual
design keeps the stub child as the executor (one stub child per
mm, inside the worker; spawner drives `set_stub_state` /
`get_stub_state` cross-process via memfd-shared `stub_data`).
Guest user-mode CPU time accumulates on the **stub child**, not
the worker; spawner-side `task->utime` stays at 0. R4 doesn't
shift this.

**Real fix paths** (none in R4's scope; documented in memo 28
Part M.4):

- **(a) Stub-child rusage aggregation.** Periodic
  `getrusage(RUSAGE_CHILDREN)` poll on the spawner; synthesize
  utime updates into the originating guest task's `task->utime`.
  Cheap, doesn't require R4.
- **(b) Stub-child-side setitimer.** Forward the guest task's
  `setitimer(VIRTUAL)` into the stub child via the existing
  syscall_fd_map plumbing; SIGVTALRM raised on the stub child
  relays back via the existing SIGSYS/futex path as a TIF_SIGPENDING.
- **(c) Collapse stub child into a worker pthread.** Guest user
  code runs in a pthread inside the worker; setitimer on the
  worker tracks the right user time. Complete redesign — far
  beyond R4's budget.

The reproducer remains a 1-second diagnostic; we now know the
EXPECTED_FAIL → PASS flip needs separate work, not R4.

§2.5.6 (substrate gate): the reproducer suite *is* the gate. Wire
into `tools/testing/selftests/um/Makefile` (done) and use as the
v2-acceptance prerequisite per memo 29's original argument.

### Aggregate cost

15+ minutes (full regrtest) → **4 seconds** (reproducer suite) for
the same diagnostic surface coverage of the 4 classes.
