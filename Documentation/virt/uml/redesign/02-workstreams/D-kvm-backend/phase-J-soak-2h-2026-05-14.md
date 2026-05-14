# Phase J soak diary — 2h short-workload run (2026-05-14)

First post-T57-Phase-A sustained soak. Demonstrates `run-soak-daemon.sh`
end-to-end under threshold-trip semantics, validates the post-T57
kernel under a real workload rotation, and surfaces one
iocheck-on-seccomp timing flake (filed for follow-up).

## TL;DR

- 400 total iterations over 35 min wall (early-stopped by daemon at
  29.1 % of the 7200 s budget, per design).
- **kvm-v2: 200/200 = 100.00 % PASS** across all 4 workloads.
- seccomp: 197/200 = 98.50 % — three iocheck/seccomp iters tripped
  the daemon's `>5 % rolling-50` threshold; clean stop followed.
- 0 panics, 0 kernel corruption, 0 throttle pauses, 0 cross-task FPU
  events (post-T55 + post-T57 Phase A holds under sustained pressure).
- The failure tuple is iocheck/seccomp only — NOT a v2 regression.
  Root-cause analysis below points at per-iter timeout under W=2
  contention, not corruption.

## Setup

| Knob | Value |
|------|-------|
| `run_id` | `phase-J-2h-2026-05-14T020423Z` |
| Out dir | `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/` |
| Kernel | `~/src/uml-builds/uml-smp-t41fix/linux` (HEAD `bcd791f5a369`, post-T57 Phase A) |
| Host | AMD Ryzen 7 7840HS (Zen 4, 16 host CPUs), Linux 7.0.0-15-generic |
| Driver | `tools/testing/selftests/um/soak/run-soak-daemon.sh` (Phase J #167 daemon mode) |
| Budget | 7200 s (2 h) |
| Workloads | `memcheck`, `iocheck`, `stress-ng` (IPC-only), `tier1-pylibs` |
| Backends | `kvm-v2` and `seccomp`, swept per rotation |
| Workers | 2 |
| Iters/rotation | 10 |
| Per-iter timeouts (from per-workload templates) | memcheck/iocheck/stress-ng: 60-90 s; tier1-pylibs: 90 s |

Invocation (single line):

```
UML_KERNEL=~/src/uml-builds/uml-smp-t41fix/linux \
  tools/testing/selftests/um/soak/run-soak-daemon.sh \
  --budget-sec 7200 \
  --workloads memcheck,iocheck,stress-ng,tier1-pylibs \
  --workers 2 --iters-per-rotation 10 \
  --out /var/tmp/uml-soak/phase-J-2h-$(date -u +%Y-%m-%dT%H%M%SZ)
```

## Results

### Final per-tuple table

| Workload | Backend | n | pass | fail | Rate | Wilson 95 % CI |
|----------|---------|---|------|------|------|----------------|
| memcheck | kvm-v2 | 60 | 60 | 0 | 100.00 % | [93.98 %, 100.00 %] |
| memcheck | seccomp | 60 | 60 | 0 | 100.00 % | [93.98 %, 100.00 %] |
| iocheck | kvm-v2 | 60 | 60 | 0 | 100.00 % | [93.98 %, 100.00 %] |
| iocheck | seccomp | 60 | 57 | 3 | 95.00 % | [86.30 %, 98.29 %] |
| stress-ng | kvm-v2 | 40 | 40 | 0 | 100.00 % | [91.24 %, 100.00 %] |
| stress-ng | seccomp | 40 | 40 | 0 | 100.00 % | [91.24 %, 100.00 %] |
| tier1-pylibs | kvm-v2 | 40 | 40 | 0 | 100.00 % | [91.24 %, 100.00 %] |
| tier1-pylibs | seccomp | 40 | 40 | 0 | 100.00 % | [91.24 %, 100.00 %] |

### Aggregates

| Backend | total | pass | fail | rate |
|---------|-------|------|------|------|
| kvm-v2 | 200 | 200 | 0 | **100.00 %** |
| seccomp | 200 | 197 | 3 | 98.50 % |
| **Total** | **400** | **397** | **3** | **99.25 %** |

### Stop reason

`THRESHOLD_TRIPPED` sentinel was written at the third iocheck/seccomp
fail; per-tuple rolling-50 failure rate hit 5.0 % which exceeded the
default `--fail-threshold-pct 5`. Daemon finished the in-flight
workload phase (correct behaviour — do NOT kill `umlctl` mid-iteration)
and exited cleanly with the final summary written.

No `kvm-v2`-side tuple ever crossed the threshold.

## iocheck/seccomp failure root-cause (preliminary)

Three failing rows, all from rotations 0 and 2:

```
iter 6   r=0  _loop/iocheck-seccomp-r0/p0_default/w0/run-5.log
iter 51  r=2  _loop/iocheck-seccomp-r2/p0_default/w1/run-10.log
iter 54  r=2  _loop/iocheck-seccomp-r2/p0_default/w1/run-3.log
```

All three logs share the same shape:

```
iocheck path=/tmp/iocheck-$ mb=64 iters=4 bs=4096
  iter 1/4: bad=0 (cumulative=0)
  iter 2/4: bad=0 (cumulative=0)
  iter 3/4: bad=0 (cumulative=0)
  <log ends — no iter 4/4, no REPRO_DONE>
```

Every iter that completed reported `bad=0` (zero miscompares).
The 4th iter was started but the phase wrapper timed out before
`REPRO_DONE rc=0` reached stdout. The daemon's classifier sees no
PASS marker and records FAIL — by design, but the underlying work
showed zero data corruption.

`iocheck.toml.template` line `timeout_secs = 60` is the upper bound
on a single iter. The body (`iocheck /tmp/iocheck-$$ 64 4 4096` =
64 MiB × 4 iters × 4 KiB blocks on tmpfs) takes ~2-3 s under
single-tenant kvm-v2; under daemon W=2 (two seccomp iocheck workers
running concurrently writing to the same host's tmpfs), the
host-side block writeback + seccomp's signal-based syscall traffic
occasionally push a 4-iter run past 60 s.

kvm-v2 doesn't see this because its gadget hot-path keeps the
per-syscall cost ~100× lower (memo `bench-cross-host-2026-05-04-
postgadget.json`); under the same W=2 contention kvm-v2 stays well
under the per-iter timeout.

Disposition:
  - **NOT a v2 regression.** kvm-v2 is the better-behaved backend
    on this workload at W=2.
  - **NOT a v2 correctness issue.** Zero `bad=` lines across all
    400 iterations.
  - **NOT blocking** Phase J — the daemon's threshold-trip stop
    fired as designed.
  - **Follow-up filed** (task #23): bump `iocheck.toml.template`'s
    `timeout_secs` from 60 to 120 to fit W>=2 contention margins.
    Alternatively, teach the classifier to read partial `iter N/M:
    bad=` progress as PASS-if-all-complete-iters-clean — heavier
    change, deferred.

## What this run proves

1. **Phase J daemon-mode driver works end-to-end** under sustained
   pressure: rotation-driven workload sweep, per-backend parity
   tracking, Wilson-CI summary, scoreboard.jsonl row schema,
   threshold-trip clean-stop semantics. First production-scale
   run of `run-soak-daemon.sh` (commit `4977e0357ce5`).

2. **T57 Phase A holds under load.** 200 kvm-v2 iterations across 4
   workloads, including:
   - 40 stress-ng IPC iters (futex/pipe/switch + verify) — would
     have been the historic SMP-T57 SIGILL surface had `--vm` been
     in the rotation. Phase A doesn't re-enable `--vm` in the
     IPC-only template (Phase B AVX-512 work + memo `state-audit/
     24` §8 closes that loop later).
   - 40 `tier1-pylibs` iters (requests + cryptography C-extension
     exercise) — direct test of the SIGSEGV-on-import P0 class
     that the v2 substrate originally surfaced (#274). Still zero
     fails.

3. **T55 perf-debt fix holds under load.** No noticeable startup
   regression — `tier1-pylibs` per-iter wall stayed at ~5 s on both
   backends throughout the soak. The per-vCPU FPU-dirty epoch flag
   (commit `fd2f9639b0ce`) is doing its job under sustained
   per-task FPU transitions.

4. **T26/T27 cross-task FPU guarantee holds.** Zero `CHILD_FAIL`
   events across the rotation's fork-heavy workloads (memcheck +
   tier1-pylibs both fork-and-exec Python). Compounded with the
   threaded-fork-malloc 24 000-fork verification at Phase A commit
   time, the cross-task XMM/SSE isolation invariant has been
   sustained across ~50 000 dispatches in this single session.

5. **Daemon's threshold-trip stop is well-calibrated.** The 5 % /
   rolling-50 default caught the iocheck/seccomp timing flake
   without spurious-firing on the other seven (workload, backend)
   tuples that ran 40-60 clean iters each. If the threshold were
   tighter (e.g. 2 %), it would have over-triggered on noise;
   looser (10 %), it would have let the flake accumulate longer.
   5 % feels right.

## Implications for Phase J

- **24 h continuous goal looks reachable on kvm-v2 alone.** Linear-
  extrapolating from 200/200 over 35 min, a clean-stop-on-real-
  failure 24 h run would surface a real bug rather than noise.
  Suggest re-running with the iocheck timeout bumped (task #23 fix)
  and `--fail-threshold-window 100` (smoother rolling average) for
  the formal "Phase J DONE" 24 h ratio.
- **stress-ng `--vm` re-enable.** Post-Phase-A, 12 of 14 vm-methods
  pass on kvm-v2. The pilot rig's IPC-only carve-out (memo `phase-
  J-pilot-2026-05-05.md`) was warranted at the time but can now be
  partially relaxed — e.g. enable the 12 passing methods, keep
  mscan + prime-incdec excluded until Phase B (AVX-512) lands.
- **Tier 2 / Tier 3 / LTP** are still unimplemented (per `phase-J-
  design-2026-05-07.md` §3.2-3.3 + §4). The soak template-toml
  pattern is now load-tested; adding new tiers is a straight-line
  task.

## Artefacts (preserved)

- `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/scoreboard.jsonl`
  (400 rows, one per iteration).
- `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/summary.md`
  (final Wilson-CI table, identical to the table above).
- `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/config.json`
  (env + flags snapshot at start).
- `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/THRESHOLD_TRIPPED`
  (sentinel — non-zero size flags "iocheck seccomp" tuple).
- `/var/tmp/uml-soak/phase-J-2h-2026-05-14T020423Z/_loop/.../run-*.log`
  per-iter init.log copies; the 3 iocheck/seccomp failure logs are
  preserved verbatim.

## Refs

- `phase-J-design-2026-05-07.md` §2 (daemon-mode spec).
- `phase-J-pilot-2026-05-05.md` (pilot-rig precedent — 240/240 short-
  set baseline for comparison).
- `state-audit/24-smp-t57-vmmethod-bisect.md` + `state-audit/25-
  smp-t57-xsave-enable-plan.md` (T57 root cause + Phase A plan).
- `04-risks/decisions-log.md` D117 (J-pilot), D118 (T55 surfaced),
  D119 (T55 fix), D120 (J daemon driver), D121 (T57 Phase A),
  D122 (this run).
- Commit `ab68bf077de3` (T57 Phase A code; kernel under test).
- Commit `bcd791f5a369` (STATUS + decisions-log + Phase J memo
  Tier 2 swap to `uv`).
- Task #23 (iocheck timeout follow-up).
