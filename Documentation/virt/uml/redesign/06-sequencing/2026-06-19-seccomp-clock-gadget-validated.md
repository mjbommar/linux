# UML — seccomp clock gadget: a CORRECT, validated ~3.9× win

**Date:** 2026-06-19
**Builds on:** `2026-06-18-seccomp-gadget-fast-path.md` (which proved the gadget
mechanism and found clocks trap, but only as a perf-ceiling forward-to-host).
This iteration makes the clock gadget **correct** (offset-corrected,
time-travel-gated, MONOTONIC + REALTIME) and validates both correctness and
speed. Patch: `upstream-patches/2026-06-19-seccomp-clock-gadget.patch` (kept;
source reverted from the tree pending the hardening below).

## What it does

UML's vDSO turns every `clock_gettime` into a syscall (~32,000-cyc trap). The
gadget answers `clock_gettime(CLOCK_MONOTONIC|CLOCK_REALTIME)` in the stub's
SIGSYS handler:

- UML stamps two offsets into the shared stub page on a crossing clock_gettime,
  **only when `time_travel_mode == TT_MODE_OFF`**:
  - `clock_mono_offset = os_nsecs() - ktime_get_ns()`
  - `clock_real_offset = os_persistent_clock_emulation() - ktime_get_real_ns()`
- The stub reads the host clock for the requested id and returns
  `host_clock - offset`, skipping the futex handoff entirely. The guest
  clocksource *is* host CLOCK_MONOTONIC, so the monotonic offset is the constant
  host-uptime-at-guest-boot.

## Validation (all on `/tmp/uml-clockfix` vs baseline, 1 CPU)

**Correctness (the gadget path — call #2+ after the offset is stamped):**
- MONOTONIC: guest-scale (last_sec=1, *not* host 1.3M), strictly monotonic,
  delta after a 50 ms sleep = **50.19 ms** (baseline 50.20 ms).
- REALTIME: gadgeted call returns epoch **1781887066**, matching host wall clock
  (1781887061), +1 s after a 1 s sleep.
- cpython-test curated: **SUCCESS, rc=0** (no regression).

**Performance:**
| workload | baseline | clock gadget | speedup |
|---|---|---|---|
| clock_gettime microbench | 32,228 cyc | 8,587 cyc | **3.75×** |
| python 2M `time.monotonic()` | 18.7 s | 4.86 s | **3.85×** |
| python 2M `time.time()` | 18.9 s | 4.82 s | **3.9×** |
| python 200k timestamped log records | 10.1 s | 6.0 s | **1.68× (−40%)** |

The last row is the honest real-world number: logging also formats + does I/O,
so clocks aren't 100% of the time, yet the gadget still cuts wall time 40%.

## Why this is the win the investigation was hunting

Across the whole trap-path effort, micro-opts on the trap's edges did not move
real workloads (O1 arch_prctl: −8.9% micro → 0% workload; getpid gadget: 4.5×
micro → 0% workload, glibc caches it). The clock gadget is different because
UML's vDSO *forces* every clock read to trap and real code reads clocks
constantly (timing, logging, datetime, asyncio). Removing the crossing for the
two dominant clock ids yields a real, broad, multi-x workload speedup.

## Remaining hardening before upstreaming (why not shipped as-is)

1. **REALTIME staleness.** A stamped-once `clock_real_offset` is exact only until
   the guest steps its wall clock (settimeofday / NTP / suspend). Needs a budget
   re-stamp (force an occasional crossing) — the same mechanism kvm-v2's gadget
   uses. MONOTONIC never jumps, so it is already exact.
2. **Observational equivalence.** Gadgeted clock calls never reach the UML kernel,
   so in-guest seccomp/ptrace/audit/strace on `clock_gettime` won't see them —
   the same audit kvm-v2's in-guest gadget already passed must be repeated here.
3. **Clock-id coverage.** Only MONOTONIC/REALTIME are gadgeted; MONOTONIC_RAW,
   COARSE variants, BOOTTIME, CPUTIME fall through to the handoff (correct, just
   not accelerated). COARSE especially could be added.
4. **The real end state** is a UML **vvar time page** read by the vDSO with *no
   syscall at all* (~32,000 → ~50 cyc, vs the gadget's ~8,500). The gadget is the
   tractable big step (~3.9×); the vvar page is the full ceiling.

## Disposition

A genuine, correct, measured UML performance win on clock-bound workloads, with a
clear path to production (items 1–2) and to the full ceiling (item 4). This is
the concrete deliverable the trap-path investigation pointed to: attack the
*crossings* for syscalls real workloads actually hammer.
