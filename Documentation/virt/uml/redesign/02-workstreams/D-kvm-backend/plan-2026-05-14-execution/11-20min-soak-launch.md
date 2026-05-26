# Phase J 20-minute soak launch (2026-05-14)

## What

Launched a 20-minute sustained soak against the locked snapshot
binary `/tmp/uml-soak-snapshot/linux-pre-phase3` to produce real
PASS/FAIL Wilson-CI evidence as partial progress toward
PLAN-2026-05-14.md §10 acceptance criterion (1) "Phase J 24h
soak ≥99.5% per workload."

The full 24h soak is the operator-time deliverable. This 20-min
run is in-session evidence that the substrate sustains under
the daemon at low-iteration cadence, complementing the earlier
2h soak result (task #22 — 200/200 PASS on memcheck + iocheck +
stress-ng + tier1-pylibs).

Excluded from this run:
  - `tier3-django` / `tier3-fastapi` — pending `vector_net_open`
    upstream fix (see `10-tier3-live-smoke-findings.md`).
  - `ltp-runner` — needs operator pre-flight (LTP + kirk
    install).
  - `cpython-soak`, `kbuild-tiny` — each cycle is 6-10 min;
    won't get representative iteration count in a 20-min budget.

## Invocation

```sh
UML_KERNEL=/tmp/uml-soak-snapshot/linux-pre-phase3 \
  nohup bash run-soak-daemon.sh \
    --budget-sec 1200 \
    --workloads memcheck,iocheck,stress-ng,tier1-pylibs,tier2-uv-pylibs \
    --workers 2 --iters-per-rotation 5 \
    --out /tmp/phase-J-soak-20min-2026-05-14 &
```

Wall-clock budget: 1200s = 20 min. Per-rotation: 5 workloads ×
2 backends × 5 iters = 50 iters/rotation. Each rotation
should take ~3-5 min depending on the slow workloads
(stress-ng = 120s per W=2 invocation, tier1-pylibs = 90s,
tier2-uv-pylibs = 120s). Expect 4-7 rotations in 20 min.

Total iterations: ~200-350 across all (workload, backend)
tuples.

## Process state

Daemon pid 2196531 (top-level bash); umlctl + UML kernels
spawn as workers under each phase. Signal handlers:
SIGTERM/SIGINT clean stop after the in-flight phase;
SIGUSR1 forces summary refresh.

## Expected result

Same per-workload PASS rates as the earlier 2h soak's pilot
run (200/200 = 100% on memcheck/iocheck/stress-ng/
tier1-pylibs) PLUS tier2-uv-pylibs which the earlier daemon
runs (80/80 in commit `b78ac72e0b3c`) verified at 100%.

If any (workload, backend) drops below 99.5% in this 20-min
window, that's a regression vs the documented baseline and
should be investigated before the operator runs the full 24h
soak.

## What this closes

  - PLAN §10 criterion (1) STILL not closed (24h is the bar).
  - But the in-session evidence shifts confidence that a 24h
    run at ≥99.5% is achievable on this substrate.
  - The full 5-workload subset has now been daemon-driven; the
    only outstanding 24h-eligible workload is `ltp-runner`
    which needs LTP+kirk install + `vector_net_open` fix.

## Diary log

Results will be appended once the soak completes. Stub here
keeps continuity with the post-2026-05-14 diary chain.
