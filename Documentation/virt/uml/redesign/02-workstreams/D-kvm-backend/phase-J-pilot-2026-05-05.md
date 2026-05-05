# Phase J pilot soak — design + first-pilot result (2026-05-05)

> Companion: `tools/testing/selftests/um/soak/README.md` (operator-facing
> rig docs). This memo is the *why* + the *result*; the README is the *how*.

## Context

Phase J in the original 24-month plan is "validation: 24 h continuous + Tier
1/2/3 + soak". Pre-2026-05-04, mt-mini SMP T=8 N=400 was the only soak gate;
yesterday's SMP-T54 fix (worker-socketpair fd leak) brought it to 400/400 =
100.0 %. With a clean baseline the obvious next question is **what other
realistic workloads expose flake at the 1 % rate the mt-mini gate now
forecloses?**

The answer wanted three properties:

1. **Realistic** — actual production-shape workloads (compile, regrtest,
   IPC stress), not synthetic kernel-side reproducers.
2. **Diverse syscall coverage** — MM beyond mt-mini's mmap loop, IPC beyond
   threaded-fork-malloc's wait4, page-cache + writeback beyond mt-mini's
   anon-only memory.
3. **Sweepable** — kvm-v2 vs seccomp side-by-side, so any regression / flake
   delta surfaces directly without re-running the whole soak under each.

## Workload selection

A research subagent identified six candidates; trimmed to five for the
pilot, with apt mirror unreachable removing two upstream tools:

| Picked | Workload | Replaces / based on | Why |
|--------|----------|---------------------|-----|
| ✓ | `memcheck` (50 LoC C) | memtester | anon-mmap CoW + addressing wiring at GB-scale; deterministic patterns; trivial dep tree |
| ✓ | `iocheck` (80 LoC C) | fio | tmpfs write/fsync/read/verify with per-block deterministic key; catches pagecache + writeback ordering bugs |
| ✓ | `stress-ng` (apt) | — | the canonical Linux stress amplifier; ~30 stressors in the picked classes, `--verify` is the mt-mini analog at scale |
| ✓ | `cpython-soak` | extends existing `cpython-parity/` | regrtest curated subset (signal/io/mmap/fork/etc); stresses v2 EINTR + threading |
| ✓ | `kbuild-tiny` | — | tinyconfig UML build; fork-storm + pipe + file I/O at high rate |
| ✗ | LTP syscalls | — | ~30 min build + ~hour run; out of scope for the pilot |
| ✗ | trinity | — | non-deterministic seeds contaminate Wilson CI methodology |
| ✗ | xfstests | — | hostfs short-circuits the interesting paths |
| ✗ | glibc `make check` | — | huge build, dominated by hostfs / locale edge cases |
| ✗ | redis / sqlite | — | sqlite gives the same coverage with less moving parts; redis needs unix socket setup |

The two C tools (`memcheck`, `iocheck`) exist because the host's apt
mirror was offline at pilot time and `memtester`/`fio` couldn't install.
Writing minimal replacements turned out to be ~130 LoC total and gave
better control over the pattern + verify semantics anyway — kept them.

## Harness

`tools/testing/selftests/um/soak/run-pilot.sh` drives the loop:

- One Umlfile template per workload (`{{KERNEL}}` + `{{BACKEND}}`
  substituted at run time)
- Per-workload backend sweep (`kvm-v2`, `seccomp`)
- `umlctl gate loop -W <workers> -M <iters>` for parallel boots with
  per-iteration verdict
- **Thermal throttle**: max(`k10temp`/`coretemp`/`zenpower`) sensor read;
  pause when ≥ `THERMAL_PAUSE_C` (default 88 °C), resume at ≤
  `THERMAL_RESUME_C` (default 75 °C). The `acpitz` thermal_zone wasn't
  reliable on AMD — Zen 4 hits 92 °C package well before acpitz reads
  >50 °C.
- `COOLDOWN` seconds between workloads (default 30)
- CSV summary at exit

Default W=2 (50 % host CPU load). W=4 hit 92 °C in pilot smoke and
prompted adding the throttle.

## Pass/fail discipline — two harness bugs the pilot caught

Both same root cause: bash semantics around exit-code propagation.
Documented in the rig README; reproducing here because the pattern
will bite future templates if not internalized:

**Bug 1 — pipefail loss.** The phase-wrapper's `eval $cmd; rc=$?`
captures the last simple-command's exit, not the pipeline's. Templates
that did this:

```bash
# kbuild-tiny (broken): make ... vmlinux | tail -10; echo KBUILD_RC=$?
# stress-ng (broken):   stress-ng ... ; echo STRESS_NG_RC=$?
```

…always reported rc=0 from the trailing `echo`, masking real failures.
First pilot run got "1/1 PASS in 2 s" for kbuild — the build had silently
failed (no `vmlinux` produced) but the harness counted it as a pass.

**Bug 2 — stress-ng cwd is RO.** UML's hostfs root is read-only from the
guest. stress-ng tries to drop sentinel files in cwd unless told
otherwise, so it aborted immediately with `temp-path '.' must be readable
and writeable` — but again the trailing `echo` masked the failure.

Fix in every template now:

```bash
cd /tmp && cmd args && echo TOOL_OK || (tail -10 /tmp/log; echo TOOL_FAIL; exit 1)
```

This propagates non-zero to the phase wrapper → init.sh aborts → no
`REPRO_DONE rc=0` marker → gate-loop counts a real FAIL.

## First-pilot result

W=2, M=20, 3 short workloads (memcheck / iocheck / stress-ng) × 2 backends.
12 min wall-clock, max package temp 86 °C (under the 88 °C throttle):

| Workload | kvm-v2 | seccomp | v2 elapsed | seccomp elapsed |
|----------|-------:|--------:|-----------:|----------------:|
| memcheck (anon-mmap, 256 MB × 2 iters × 8 patterns) | 40/40 | 40/40 | 86 s | 58 s |
| iocheck (write/fsync/read/verify, 64 MB × 4 iters @ 4 K blocks) | 40/40 | 40/40 | 93 s | 58 s |
| stress-ng (futex+pipe+switch IPC, 10 s × 6 stressors, --verify) | 40/40 | 40/40 | 215 s | 216 s |
| **TOTAL** | **120/120** | **120/120** | | |

240 / 240 = 100.0 %. Wilson 95 % CI [98.5 %, 100.0 %].

Perf observations from the elapsed numbers (~boot + ~workload, summed
over all iters):

- `memcheck` and `iocheck` are ~1.5× slower under v2 than seccomp.
  Both are MM-heavy workloads where v2's per-syscall gadget +
  per-mm worker IPC dispatch costs add up. Expected — these are the
  "every syscall" path, not the gadget fast path.
- `stress-ng` is symmetric (~216 s both backends). The IPC stressors
  (futex_wake, pipe read/write, sched_yield) hit the gadget hot path
  on v2 and the seccomp trap path on seccomp; both pay one trap per
  syscall, gross numbers come out close.

## Open finding — SMP-T57

`stress-ng --vm 1 --vm-bytes 16M --verify` produces

```
stress-ng: info: 0x0000000000000000 not readable
stress-ng: info: 0x0000000000000010 not readable
…
stress-ng: error: vm: terminated with an error, exit status=2
```

**only on kvm-v2**. seccomp passes. mt-mini, memcheck, iocheck all pass on
v2.

Hypotheses (not yet bisected):

1. **Real v2 MM-coherence residual** triggered specifically by stress-ng's
   vm-method patterns (CoW + madvise-poison + walk). The simpler tools
   don't combine those operations the way stress-ng does.
2. **stress-ng / hostfs interaction** — some probe path that depends on
   /proc/$pid/maps sizes or /sys layout that UML reports differently.

Reproducer-narrowing path (deferred):

```sh
for m in zero ones flip walk-0 walk-1 prime-incdec rowhammer stripe \
         move grayflip galpat-0 galpat-1; do
  stress-ng --vm 1 --vm-bytes 16M --vm-method $m --verify --timeout 10s \
    --log-file /tmp/sn-$m.log
  echo "$m: rc=$?"
done
```

Whichever vm-method trips becomes the focus of a minimal C reproducer.

Tracked as task #243 (SMP-T57). Disabled in the pilot stress-ng.toml
template's IPC-only profile so the flake-rate measurement isn't
contaminated by it.

## Status against M12

M11 (KVM bookend ~100 ns) is **closed and exceeded** (~30 ns / ~90 cyc on
Zen 4 — 3.3× under target). M12 (all profiles ship) is the umbrella;
this rig is the first concrete piece of "realistic-workload soak" that
isn't `mt-mini` or `cpython-parity`.

What's still gated for full Phase J:

- **24 h continuous run** — the current rig is fine for the multi-hour
  scale; needs a daemon-mode wrapper + persistent scoreboard for 24 h.
- **Tier 1 / 2 / 3** — current pilot is Tier 1 (smoke) shape. Tier 2
  (per-merge with paths-changed-aware subset) + Tier 3 (nightly full
  matrix incl. cpython-soak + kbuild-tiny) needs CI integration, not
  just the rig.
- **LTP syscalls** — ~1500-test correctness oracle. Build + curate skip
  list; one good rainy-day session.
- **SMP-T57 closure** — separate from M12 but a real flake we discovered;
  closing it tightens the v2 confidence interval.

## How to extend the rig

Adding a workload:

1. Drop a `tools/testing/selftests/um/soak/<name>.toml.template` with
   `{{KERNEL}}` and `{{BACKEND}}` placeholders + at least one phase that
   prints `REPRO_DONE rc=0` on success.
2. Use the `cmd && echo OK || (echo FAIL; exit 1)` pattern in every
   phase that calls a binary whose exit code matters.
3. Add an entry in `run-pilot.sh`'s `case "$WORKLOAD"` switch (workload
   name + per-workload timeout).
4. Smoke at W=1 M=1 first; then W=2 M=10.

Adding a backend (e.g., a future v3): just pass `--backend v3` through
to the gate-loop runner. The Umlfile template's `{{BACKEND}}` is
already parametric.

## File layout

```
tools/testing/selftests/um/soak/
├── README.md              # operator docs (run, knobs, output layout)
├── run-pilot.sh           # driver
├── memcheck.c             # anon-mmap pattern verifier
├── iocheck.c              # write/fsync/read/verify
├── memcheck.toml.template
├── iocheck.toml.template
├── stress-ng.toml.template
├── cpython-soak.toml.template
└── kbuild-tiny.toml.template
```

## References

- Pilot harness commit: `95c95267202e`
- Pre-pilot mt-mini 100 % achievement: `467aa7d142c0` (SMP-T54)
- Cross-host bench (post-gadget): `bench-cross-host-2026-05-04-postgadget.md`
- Milestone definitions: `06-sequencing/milestones.md`
- SMP-T57 (open): task #243
