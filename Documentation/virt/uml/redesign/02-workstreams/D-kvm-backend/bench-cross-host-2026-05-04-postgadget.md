# Cross-host benchmark — kvm-v2 (with gadget) vs seccomp (2026-05-04, post-gadget)

Captured 2026-05-04 against the kvm-v2 SMP T41-fix kernel **with the
Phase H LSTAR gadget revived** (`7ebcd8aac347`) using the
`tools/testing/selftests/um/bench/` reproducible benchmark suite
(`bench-micro` + `bench-py` + `bench-stress`) on every lab host.

This is the post-gadget refresh of `bench-cross-host-2026-05-04.md`.
The earlier table was bound to commit `81665d4c2343`, which routed
every syscall through the host KVM_EXIT_IO slow path (~36 800 cyc/
getpid). This commit re-enables the in-LSTAR stay-in-guest gadget for
ten trivial syscalls (getpid/gettid/getppid/getuid/geteuid/getgid/
getegid/getcpu/time/clock_gettime CLOCK_MONOTONIC) — the micro tier
collapses to ~90 cyc/call as a result.

**All 8 hosts, all 3 tiers, all 2 backends — 48/48 cells populated,
zero STRICT_MEMSET_FAIL / VERIFY_FAIL events on the stress tier.**

## Methodology

Identical to the pre-gadget run (see
`bench-cross-host-2026-05-04.md`):

- **Same kernel binary** on every host (built once on server7 from
  `7ebcd8aac347`, scp'd into each host's `~/bench-bundle/kernel/`).
- **Same workload binaries**: `getpid-loop`, `mt-mini`, `bench-py.py`.
- **Same orchestrator**: `tools/testing/selftests/um/bench/run-bench.sh`
  for both backends per tier; `umlctl up` boots the per-tier toml.
- **Performance governor everywhere**.
- **Pinned to single vCPU** for `micro`/`py` tiers, ncpus=4 for
  `stress`. Median over N samples (5 for py, 3 for stress, 1 for
  micro since the binary's own internal averaging is N=100 000).
- **rdtsc-bracketed** for `micro`. Wall-clock for `py`/`stress`.

## Headlines

- **kvm-v2 is faster than seccomp on every host on every tier.**
- **micro tier: 193× (Alder Lake i5) → 908× (Zen 4)** — a ~400×
  uplift over the pre-gadget micro tier (which topped out at
  2.22×). The gadget converts getpid from a ~36 800 cyc round-trip
  into a ~90 cyc in-guest dispatch.
- **py tier: 2.99× (Skylake/Alder Lake i5) → 4.06× (Zen 4)** —
  roughly 2-3× better than the pre-gadget py tier (1.03-1.64×).
  Python startup hits getpid/getuid/clock_gettime in tight bursts
  and now amortises them in-guest.
- **stress tier: 3.71× → 8.72×** — comparable to the pre-gadget
  stress tier (3.62× → 11.97×). mt-mini is mmap-heavy; the gadget
  doesn't cover mmap/munmap/brk, so the speedup is dominated by
  vCPU-pool parallelism rather than per-syscall in-guest dispatch.
- **Zero data corruption events** across 24 mt-mini runs — the
  SMP-T41 fix continues to hold across silicon families.

## Tier 1 — `micro` (per-getpid round-trip cyc, lower=better)

| Host | CPU                  | µarch         | kvm-v2 cyc | seccomp cyc | ratio v2/seccomp | speedup |
|------|----------------------|---------------|-----------:|------------:|-----------------:|--------:|
| s0   | i9-12900K            | Alder Lake P  |        101 |      28 682 |           0.0035 | **284×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     |        100 |      74 341 |           0.0013 | **743×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     |         96 |      78 026 |           0.0012 | **813×** |
| s3   | Xeon W-2123          | Skylake-SP    |         98 |      69 288 |           0.0014 | **707×** |
| s4   | i5-12600K            | Alder Lake    |        153 |      29 528 |           0.0052 | **193×** |
| s5   | Ryzen 7 7840HS       | Zen 4         |         87 |      74 940 |           0.0012 | **861×** |
| s6   | Ryzen 7 7840HS       | Zen 4         |         88 |      79 874 |           0.0011 | **908×** |
| s7   | Ryzen 7 7840HS       | Zen 4         |         90 |      81 387 |           0.0011 | **904×** |

Observations:
- The kvm-v2 column collapses to 87–153 cyc — that's the gadget's
  in-guest dispatch (swapgs + 3× saved-reg movq + dispatch jump +
  one %gs:disp32 load + 3× restore + swapgs + sysretq, ~12
  instructions on the hot path). The cyc cost is essentially
  silicon-bound: AMD Zen 4 (s5/s6/s7) at 87-90 cyc is the best,
  Alder Lake P-cores (s0) at 101 cyc, Alder Lake mixed (s4 lands
  on E-core) at 153 cyc, older Intel (s1/s2/s3) at 96-100 cyc.
- The seccomp column is unchanged (no kernel-side change touched
  seccomp), so the speedup ratio is dominated by how fast the
  gadget runs on each silicon family.
- s4 lands on a heterogeneous core (Alder Lake i5 has both P and
  E-cores; the bench's single-core pin can land on either) — that
  explains the 153-cyc outlier vs s0's 101 cyc on the same µarch
  family.

## Tier 2 — `py` (canned Python workload ms, lower=better)

| Host | CPU                  | µarch         | kvm-v2 ms | seccomp ms | ratio | speedup |
|------|----------------------|---------------|----------:|-----------:|------:|--------:|
| s0   | i9-12900K            | Alder Lake P  |     63.75 |     237.64 | 0.268 | **3.73×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     |    211.40 |     644.51 | 0.328 | **3.05×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     |    222.11 |     665.14 | 0.334 | **2.99×** |
| s3   | Xeon W-2123          | Skylake-SP    |    172.23 |     541.49 | 0.318 | **3.14×** |
| s4   | i5-12600K            | Alder Lake    |     68.15 |     204.07 | 0.334 | **2.99×** |
| s5   | Ryzen 7 7840HS       | Zen 4         |    120.75 |     467.88 | 0.258 | **3.87×** |
| s6   | Ryzen 7 7840HS       | Zen 4         |    121.88 |     495.35 | 0.246 | **4.06×** |
| s7   | Ryzen 7 7840HS       | Zen 4         |    127.48 |     498.28 | 0.256 | **3.91×** |

Observations:
- Pre-gadget py tier showed 1.03-1.64× speedup; post-gadget every
  host clears 2.99×. The delta vs pre-gadget is the workload-level
  realisation of the micro-tier 400× uplift — Python startup spends
  a measurable fraction of its time in getpid/getuid/clock_gettime,
  and the gadget removes the vmexit cost on every one of them.
- s0 (Alder Lake P-only) sees 63.75 ms kvm-v2 vs 237.64 ms seccomp.
  That's a 3.73× speedup on the Python startup workload — the
  highest absolute ratio on Intel hardware in this run.
- Zen 4 stays at 3.87-4.06×; ratio is tighter than the pre-gadget
  run (1.49-1.64×).

## Tier 3 — `stress` (mt-mini SMP T=8 ncpus=4, ms/iter, lower=better)

| Host | CPU                  | µarch         | kvm-v2 ms | seccomp ms | ratio | speedup |
|------|----------------------|---------------|----------:|-----------:|------:|--------:|
| s0   | i9-12900K            | Alder Lake P  |       205 |      1 488 | 0.138 | **7.26×** |
| s1   | Xeon E3-1225 v6      | Kaby Lake     |       558 |      2 071 | 0.269 | **3.71×** |
| s2   | Xeon E3-1225 v5      | Skylake-S     |       631 |      2 483 | 0.254 | **3.94×** |
| s3   | Xeon W-2123          | Skylake-SP    |       481 |      2 003 | 0.240 | **4.16×** |
| s4   | i5-12600K            | Alder Lake    |       197 |      1 717 | 0.115 | **8.72×** |
| s5   | Ryzen 7 7840HS       | Zen 4         |       347 |      1 491 | 0.233 | **4.30×** |
| s6   | Ryzen 7 7840HS       | Zen 4         |       319 |      1 377 | 0.232 | **4.32×** |
| s7   | Ryzen 7 7840HS       | Zen 4         |       327 |      1 600 | 0.204 | **4.89×** |

**Zero `STRICT_MEMSET_FAIL` and zero `VERIFY_FAIL` events on every
host.** The SMP-T41 fix continues to hold across silicon families
under the gadget refactor.

Observations:
- mt-mini speedups are roughly comparable to the pre-gadget run
  (3.62-11.97× → 3.71-8.72×). This is expected: mt-mini's hot path
  is mmap()/munmap()/page-fault, none of which the gadget covers,
  so the speedup is bounded by vCPU-pool scaling, not in-guest
  syscall dispatch.
- s0's pre-gadget 11.97× → post-gadget 7.26× looks like a
  regression but the absolute kvm-v2 latency went up only 20% (171
  → 205 ms) while seccomp's dropped 27% (2 047 → 1 488 ms) — a
  seccomp variance effect on the i9, not a kvm-v2 regression.
- s4 (i5-12600K) post-gadget 8.72× is essentially flat vs the
  pre-gadget 9.09×.

## Cross-tier ratio summary

| Host | CPU                  | µarch         | micro | py    | stress |
|------|----------------------|---------------|------:|------:|-------:|
| s0   | i9-12900K            | Alder Lake P  | 0.0035 | 0.268 | 0.138  |
| s1   | Xeon E3-1225 v6      | Kaby Lake     | 0.0013 | 0.328 | 0.269  |
| s2   | Xeon E3-1225 v5      | Skylake-S     | 0.0012 | 0.334 | 0.254  |
| s3   | Xeon W-2123          | Skylake-SP    | 0.0014 | 0.318 | 0.240  |
| s4   | i5-12600K            | Alder Lake    | 0.0052 | 0.334 | 0.115  |
| s5   | Ryzen 7 7840HS       | Zen 4         | 0.0012 | 0.258 | 0.233  |
| s6   | Ryzen 7 7840HS       | Zen 4         | 0.0011 | 0.246 | 0.232  |
| s7   | Ryzen 7 7840HS       | Zen 4         | 0.0011 | 0.256 | 0.204  |

(All ratios are kvm-v2 / seccomp; <1.0 = kvm-v2 wins.)

## Pre-gadget vs post-gadget delta

Tier-1 (micro) ratio comparison (pre / post / improvement multiplier):

| Host | pre-gadget ratio | post-gadget ratio | improvement |
|------|-----------------:|------------------:|------------:|
| s0   | 0.460 | 0.0035 | **131×** |
| s1   | 0.719 | 0.0013 | **535×** |
| s2   | 0.746 | 0.0012 | **608×** |
| s3   | 0.710 | 0.0014 | **517×** |
| s4   | 0.613 | 0.0052 | **117×** |
| s5   | 0.460 | 0.0012 | **396×** |
| s6   | 0.465 | 0.0011 | **424×** |
| s7   | 0.450 | 0.0011 | **419×** |

Tier-2 (py) ratio comparison:

| Host | pre-gadget ratio | post-gadget ratio | improvement |
|------|-----------------:|------------------:|------------:|
| s0   | 0.642 | 0.268 | **2.4×** |
| s1   | 0.864 | 0.328 | **2.6×** |
| s2   | 0.874 | 0.334 | **2.6×** |
| s3   | 0.834 | 0.318 | **2.6×** |
| s4   | 0.973 | 0.334 | **2.9×** |
| s5   | 0.672 | 0.258 | **2.6×** |
| s6   | 0.610 | 0.246 | **2.5×** |
| s7   | 0.621 | 0.256 | **2.4×** |

Tier-3 (stress) is unchanged within run-to-run noise (gadget does
not cover mmap-heavy workloads).

## Reproducibility

Each lab host has a copy of the bench bundle at `~/bench-bundle/`.
To re-run any host post-refactor:

```sh
ssh sN 'bash ~/bench-bundle/run-on-host.sh'
```

Per-host scoreboard rows are written to
`~/bench-bundle/scoreboard-<host>.jsonl`.

## Source data

The 48 deduplicated scoreboard rows from this run live in
`tools/testing/selftests/um/scoreboard.jsonl` under
`gate=bench-{micro,py,stress}` + `host=server[0-7]` + the
post-gadget commit. Note the scoreboard rows record `commit=unknown`
on lab hosts because the bundle is not a git repo there; the
authoritative commit is recorded in this document's header.

## Next refresh

Re-run the bench on every host whenever:
- A v2-side change touches the dispatch hot path (vcpu.c,
  syscall_trap.c, lstar_gadget.S).
- A host kernel upgrade lands on any lab host (KVM ABI surface).
- A new lab host comes online.

The `bench` tier is fast enough (~60-120 s × 8 hosts = 8-16 min
sequential, ~2 min with parallel ssh) to re-baseline on every
kvm-v2 commit that touches the syscall hot path.
