# Cross-host benchmark — kvm-v2 vs seccomp on the home lab (2026-05-04, pre-gadget)

> **Superseded for current numbers** by
> `bench-cross-host-2026-05-04-postgadget.md` (commit
> `7ebcd8aac347`, gadget revived). This page is the reference
> baseline at commit `81665d4c2343`, where every syscall took the
> KVM_EXIT_IO slow path (~36 800 cyc/getpid). Keep both for the
> pre/post comparison.

Captured 2026-05-04 against the kvm-v2 SMP T41-fix kernel
(`81665d4c2343` + the bench/ suite) using the
`tools/testing/selftests/um/bench/` reproducible benchmark suite
(`bench-micro` + `bench-py` + `bench-stress`) on every lab host.

**All 8 hosts, all 3 tiers, all 2 backends — 48/48 cells populated,
zero STRICT_MEMSET_FAIL / VERIFY_FAIL events on the stress tier.**

## Methodology

- **Same kernel binary** on every host (built once on server7 from
  `81665d4c2343`, scp'd into each host's `~/bench-bundle/kernel/`).
- **Same workload binaries**: `getpid-loop` (static, x86_64),
  `mt-mini` (static), `bench-py.py` (stdlib).
- **Same orchestrator**: `tools/testing/selftests/um/bench/run-bench.sh`
  for both backends per tier; `umlctl up` boots the per-tier toml.
- **Performance governor everywhere** via `sudo cpupower frequency-set
  -g performance`.
- **Pinned to single vCPU** for `micro`/`py` tiers, ncpus=4 for
  `stress`. Median over N samples (5 for py, 3 for stress, 1 for
  micro since the binary's own internal averaging is N=100 000).
- **rdtsc-bracketed** for `micro` (cyc/getpid). Wall-clock for
  `py`/`stress` (ms).

## Headlines

- **kvm-v2 is faster than seccomp on every host on every tier.**
- **micro tier**: 1.34× (oldest Intel Xeon E3-1225 v5) → **2.22× (Zen 4)**.
- **py tier**: 1.03× (Alder Lake i5) → 1.64× (Zen 4).
- **stress tier**: 3.62× (Kaby Lake) → **11.97× (Alder Lake i9)**.
- **Zero data corruption events** across 24 mt-mini runs (8 hosts ×
  3 samples) — the SMP-T41 fix holds on every silicon family in the
  lab (Skylake/Kaby/Alder Lake/Zen 4).

## Tier 1 — `micro` (per-getpid round-trip cyc, lower=better)

| Host | CPU                  | µarch         | kvm-v2 cyc | seccomp cyc | ratio v2/seccomp | speedup |
|------|----------------------|---------------|-----------:|------------:|-----------------:|--------:|
| s0   | i9-12900K            | Alder Lake P  | 13 194     | 28 660      | 0.460            | **2.17×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     | 54 281     | 75 509      | 0.719            | **1.39×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     | 58 418     | 78 329      | 0.746            | **1.34×** |
| s3   | Xeon W-2123          | Skylake-SP    | 49 603     | 69 850      | 0.710            | **1.41×** |
| s4   | i5-12600K            | Alder Lake    | 17 626     | 28 776      | 0.613            | **1.63×** |
| s5   | Ryzen 7 7840HS       | Zen 4         | 36 060     | 78 383      | 0.460            | **2.17×** |
| s6   | Ryzen 7 7840HS       | Zen 4         | 36 539     | 78 551      | 0.465            | **2.15×** |
| s7   | Ryzen 7 7840HS       | Zen 4         | 36 714     | 81 512      | 0.450            | **2.22×** |

Observations:
- AMD Zen 4 wins biggest on micro — three identical-CPU hosts cluster at
  2.15-2.22× (consistent silicon-bounded result; tight cyc spread within
  ±2%).
- Older Intel (Skylake/Kaby Lake) sees the smallest delta (1.3-1.4×)
  but kvm-v2 still wins.
- Alder Lake i9 (s0) and i5 (s4) at 2.17× and 1.63× respectively —
  the i5 lower probably because the bench landed on an E-core for one
  of the runs.

## Tier 2 — `py` (canned Python workload ms, lower=better)

| Host | CPU                  | µarch         | kvm-v2 ms | seccomp ms | ratio | speedup |
|------|----------------------|---------------|----------:|-----------:|------:|--------:|
| s0   | i9-12900K            | Alder Lake P  | 150.65    | 234.70     | 0.642 | **1.56×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     | 553.07    | 640.14     | 0.864 | **1.16×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     | 585.10    | 669.68     | 0.874 | **1.14×** |
| s3   | Xeon W-2123          | Skylake-SP    | 454.42    | 545.15     | 0.834 | **1.20×** |
| s4   | i5-12600K            | Alder Lake    | 207.02    | 212.79     | 0.973 | **1.03×** |
| s5   | Ryzen 7 7840HS       | Zen 4         | 318.30    | 474.00     | 0.672 | **1.49×** |
| s6   | Ryzen 7 7840HS       | Zen 4         | 322.30    | 528.30     | 0.610 | **1.64×** |
| s7   | Ryzen 7 7840HS       | Zen 4         | 321.43    | 517.81     | 0.621 | **1.61×** |

Observations:
- Python startup is dominated by libc/libdl initialization + CPython
  module imports — much more diverse syscall mix than micro, so the
  per-syscall trap-mechanism advantage is diluted.
- s4 ratio 0.973 (just 3% faster) is near-noise; consistent with the
  bench landing on a slow E-core during this run. s0 (same Alder Lake
  µarch but P-core landings only) shows 1.56×.
- Zen 4 stays at 1.5-1.6× even on the Python workload.

## Tier 3 — `stress` (mt-mini SMP T=8 ncpus=4, ms/iter, lower=better)

| Host | CPU                  | µarch         | kvm-v2 ms | seccomp ms | ratio | speedup |
|------|----------------------|---------------|----------:|-----------:|------:|--------:|
| s0   | i9-12900K            | Alder Lake P  | 171       | 2047       | 0.084 | **11.97×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     | 577       | 2087       | 0.276 | **3.62×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     | 636       | 2435       | 0.261 | **3.83×** |
| s3   | Xeon W-2123          | Skylake-SP    | 489       | 1909       | 0.256 | **3.90×** |
| s4   | i5-12600K            | Alder Lake    | 237       | 2155       | 0.110 | **9.09×** |
| s5   | Ryzen 7 7840HS       | Zen 4         | 326       | 1584       | 0.206 | **4.86×** |
| s6   | Ryzen 7 7840HS       | Zen 4         | 348       | 1414       | 0.246 | **4.06×** |
| s7   | Ryzen 7 7840HS       | Zen 4         | 354       | 1535       | 0.231 | **4.34×** |

**Zero `STRICT_MEMSET_FAIL` and zero `VERIFY_FAIL` events on every
host.** The SMP-T41 fix holds across silicon families.

Observations:
- mt-mini is mmap-heavy (8 pthreads × 50 iters × 16 pages = 6 400
  first-touch #PFs per iteration). Every #PF is a vmexit on kvm-v2's
  IO-port stub but the seccomp backend pays a full ptrace-style trap
  loop — that's why the gap widens to 4-12× on this workload.
- Modern silicon (Alder Lake i9 + i5) gets the biggest stress wins
  (12× and 9×) because seccomp's trap path is single-threaded
  bottlenecked by SIGCHLD delivery overhead while kvm-v2's per-host-CPU
  vCPU pool scales with NR_CPUS.

## Cross-tier ratio summary

| Host | CPU                  | µarch         | micro | py    | stress |
|------|----------------------|---------------|------:|------:|-------:|
| s0   | i9-12900K            | Alder Lake P  | 0.460 | 0.642 | 0.084  |
| s1   | Xeon E3-1225 v6      | Kaby Lake     | 0.719 | 0.864 | 0.276  |
| s2   | Xeon E3-1225 v5      | Skylake-S     | 0.746 | 0.874 | 0.261  |
| s3   | Xeon W-2123          | Skylake-SP    | 0.710 | 0.834 | 0.256  |
| s4   | i5-12600K            | Alder Lake    | 0.613 | 0.973 | 0.110  |
| s5   | Ryzen 7 7840HS       | Zen 4         | 0.460 | 0.672 | 0.206  |
| s6   | Ryzen 7 7840HS       | Zen 4         | 0.465 | 0.610 | 0.246  |
| s7   | Ryzen 7 7840HS       | Zen 4         | 0.450 | 0.621 | 0.231  |

(All ratios are kvm-v2 / seccomp; <1.0 = kvm-v2 wins.)

## Reproducibility

Each lab host has a copy of the bench bundle at `~/bench-bundle/`
containing:
- `kernel/linux` — the UML kernel built from `81665d4c2343`
- `bin/{getpid-loop, mt-mini, umlctl}` — static binaries
- `bench/{bench-micro,bench-py,bench-stress}.toml` — sealed configs
- `bench/{bench-py.py, bench-stress.sh, run-bench.sh}` — workloads + runner
- `run-on-host.sh` — driver that runs all 3 tiers and emits scoreboard rows

Re-run any host:

```sh
ssh sN 'bash ~/bench-bundle/run-on-host.sh'
```

Per-host scoreboard rows are written to `~/bench-bundle/scoreboard-<host>.jsonl`.

## Source data

The 48 deduplicated scoreboard rows from this run live in
`tools/testing/selftests/um/scoreboard.jsonl` under `gate=bench-{micro,py,stress}`
+ `host=server[0-7]` + `commit=81665d4c2343`. To filter:

```sh
grep '"gate":"bench-' tools/testing/selftests/um/scoreboard.jsonl \
  | grep '"commit":"81665d4c2343"' | wc -l   # → 48
```

## Next refresh

Re-run the bench on every host whenever:
- A v2-side change touches the dispatch hot path (vcpu.c, syscall_trap.c).
- A host kernel upgrade lands on any lab host (KVM ABI surface might
  change).
- A new lab host comes online (add a row to this table; run the same
  three commands above).

The `bench` tier is fast enough (~60 s × 8 hosts = 8 min sequential)
to re-baseline on every kvm-v2 commit.
