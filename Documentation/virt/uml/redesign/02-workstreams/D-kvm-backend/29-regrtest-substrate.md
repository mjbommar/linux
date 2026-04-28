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
