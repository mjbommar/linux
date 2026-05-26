# UML cross-backend benchmark suite

Three reproducible benchmark tiers for tracking UML backend
performance over time. Designed to run identically on every host
in the lab — same source tree, same workloads, same harness — so
ratios are comparable across machines + commits.

## Tiers

| Tier      | Workload                                       | Tier budget | What it measures                                |
|-----------|------------------------------------------------|-------------|-------------------------------------------------|
| `micro`   | `getpid-loop` (100 000 raw `SYS_getpid`)        | <10 s       | Pure trap-mechanism cyc/getpid (rdtsc-bracketed) |
| `py`      | Canned Python script: hash + fs + getpid + sock | <30 s       | Realistic mixed-syscall workload                 |
| `stress`  | mt-mini SMP T=8 ncpus=4, MT_STRICT_MEMSET=1     | <60 s       | Concurrency + first-touch #PF; SMP-T41 canary   |

Each tier emits one machine-readable summary line:

    BENCH_MEDIAN: tier=<tier> samples=<N> elapsed_ms_p50=<X> ...
    PERF_GETPID:  ...                  cyc_per_call=<C>             # micro tier

The host-side runner wraps each tier × each backend, computes
`ratio_v2_over_seccomp`, and appends a row to the scoreboard.

## Running

Prereqs: a UML kernel built with both `kvm-v2` and `seccomp`
backends co-selected. The current default (`make ARCH=um defconfig`
+ standard SMP build) satisfies this.

```sh
KERNEL=$HOME/src/uml-builds/uml-smp-t41fix/linux

# Single tier, both backends
bash tools/testing/selftests/um/bench/run-bench.sh \
     --tier micro \
     --kernel "$KERNEL"

# All three tiers, append to scoreboard
for tier in micro py stress; do
    bash tools/testing/selftests/um/bench/run-bench.sh \
         --tier $tier \
         --kernel "$KERNEL" \
         --out tools/testing/selftests/um/scoreboard.jsonl
done

# With a regression gate
bash tools/testing/selftests/um/bench/run-bench.sh \
     --tier micro --kernel "$KERNEL" --max-ratio 0.6
# Exits non-zero if kvm-v2:seccomp ratio > 0.6
```

The runner pins to the `performance` cpufreq governor. Run
`sudo cpupower frequency-set -g performance` first if your host
defaults to `powersave`.

## Adding a host to the cross-host table

Per-host comparison uses the **ratio**, not absolute cycles
(cycle counts aren't portable across µarch). Each host owns one
JSON baseline file:

  Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/
    bench-baseline-<cpu-slug>.json

To add your host:

```sh
# 1. Build the kernel locally with both backends
make ARCH=um O=$BUILD defconfig
make ARCH=um O=$BUILD -j$(nproc)

# 2. Run all 3 tiers, accumulate rows
SCOREBOARD=$PWD/tools/testing/selftests/um/scoreboard.jsonl
for tier in micro py stress; do
    bash tools/testing/selftests/um/bench/run-bench.sh \
         --tier $tier --kernel "$BUILD/linux" --out "$SCOREBOARD"
done

# 3. Extract this host's rows into a baseline JSON
HOST=$(hostname)
grep "host\":\"$HOST" "$SCOREBOARD" | tail -6 > /tmp/my-rows.jsonl
# (commit per existing convention; see existing baseline JSONs
#  for the schema)
```

Commit the baseline file and the relevant scoreboard rows.

## Schema

Each scoreboard row:

```json
{
  "ts":       "<ISO-8601 UTC>",
  "gate":     "bench-micro" | "bench-py" | "bench-stress",
  "backend":  "kvm-v2" | "seccomp",
  "commit":   "<short sha>",
  "host":     "<hostname>",
  "cpu":      "<model name from /proc/cpuinfo>",
  "governor": "<scaling_governor for cpu0>",
  "metrics": {
    "p50":          <number>,    // cyc for micro, ms for py/stress
    "strict_fails": <int>,       // stress only; must stay 0
    "verify_fails": <int>        // stress only; must stay 0
  }
}
```

## Why these three tiers

- **micro**: the cleanest cross-backend signal (pure trap mechanism,
  rdtsc-bracketed in user code, dominated by KVM/seccomp roundtrip).
  Ratio ≈ 0.5 on Zen 4 today (kvm-v2 2× faster than seccomp).
- **py**: representative mixed workload. Catches regressions that
  hide in the micro tier because the trap mechanism isn't on the
  critical path (e.g., python startup is dominated by libc/libdl
  init). Ratio ≈ 0.6 on Zen 4 today.
- **stress**: regression canary for the byte[0]=0 bug class
  (SMP-T41). Doesn't measure speed primarily — measures
  `strict_fails_total` and `verify_fails_total`. Both must stay 0.

## File layout

```
tools/testing/selftests/um/bench/
├── README.md            # this file
├── bench-py.py          # Python workload (stdlib only)
├── bench-stress.sh      # mt-mini wrapper (in-guest)
├── bench-micro.toml     # umlctl config: tier=micro
├── bench-py.toml        # umlctl config: tier=py
├── bench-stress.toml    # umlctl config: tier=stress
└── run-bench.sh         # host-side wrapper
```

Each `bench-*.toml` is a Umlfile; the runner clones it per backend
(rewriting `instance.name` and `kernel.backend`), boots UML with
that toml, and parses the resulting BENCH_MEDIAN line.

## Cross-link

- Per-host historical timing table:
  `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  `measurements.md` (legacy v1 columns) +
  `measurements-2026-05-post-T41.md` (current v2 era).
- A-07 baseline file convention:
  `Documentation/virt/uml/redesign/02-workstreams/A-backend-abstraction/`
  `README-perf-baseline.md`.
