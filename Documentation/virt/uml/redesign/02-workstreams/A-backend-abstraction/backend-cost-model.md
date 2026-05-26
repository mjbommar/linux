# Backend cost model

A living artifact tracking measured cost per backend. Updated as
benchmarks change. The numbers in this document are **baselines**;
A-07 perf-CI compares future runs against these and gates patches
that regress >5% (invariant I2 in `01-architecture/invariants.md`).

## How these numbers are produced

Each measurement is reproducible from the kernel source root:

```
Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh   # builds
Documentation/virt/uml/redesign/scripts/uml-perf.sh          # measures
```

The host context (CPU, glibc, gcc) is part of the reading. Cross-host
comparisons are uncertain; intra-host A/B (this commit vs. previous)
is the actionable signal.

## Current host

- Hardware: 4-CPU, 61 GiB RAM
- OS: Ubuntu 26.04 (Resolute)
- Compiler: gcc 15.2.0
- glibc: 2.43-2ubuntu1
- Kernel-tree: branch `uml-redesign-plan` HEAD
- Date: 2026-04-18

## Boot wall-time per backend (init=/bin/true)

The boot exercises every HOT op (run_userspace, mm_map/unmap,
context_switch, read_clock_ns) many times during early init plus
a single guest syscall round-trip when init=/bin/true exits.
Wall-time captures any systemic slowdown.

```
PTRACE_ONLY          avg=2448385 µs over 5 iter   (~2.45 s)
SECCOMP_ONLY         avg=2473354 µs over 5 iter   (~2.47 s)
DYN/ptrace           avg=2460632 µs over 5 iter   (~2.46 s)
DYN/seccomp          avg=2481217 µs over 5 iter   (~2.48 s)
```

**Observation:** boot wall-time is dominated by host setup,
runtime initialization (KASLR, devtmpfs, NET protocol families,
posixtimers, etc.), and the host fork/exec cost — *not* by the
trap mechanism. The expected ~3-4× seccomp speedup over ptrace
doesn't show up here because init=/bin/true issues only a handful
of syscalls before exiting. The numbers are useful as a regression
baseline (any backend going past ~2.6 s is news), not as a
microbenchmark of trap cost.

A cycle-accurate microbenchmark (rdtsc-timed `getpid()` loop
inside a guest init binary) lands in workstream A-07. That will
populate the per-syscall numbers we actually care about for the
prod-fast invariant.

## Binary size per backend

`size` output for each `vmlinux`:

```
Build         text     data     bss      total
PTRACE_ONLY   ?        ?        188844   ?
SECCOMP_ONLY  ?        ?        188844   ?
DYNAMIC       ?        ?        188844   ?
```

(To be filled in: `size /tmp/uml-matrix-*/vmlinux` — placeholder
table, will populate when next matrix run completes. The earlier
post-A-02-COLD-3 PTRACE_ONLY measured at text=5077655, data=1247032,
bss=188844; updated numbers go here as the migrations stabilize.)

## Per-op microbenchmarks (planned, A-07)

The HOT ops will get individual cycle-accurate microbenchmarks once
A-07 lands the per-op timing harness:

```
op                  ptrace        seccomp       kvm (target)
─────────────────  ────────────  ────────────  ──────────────
run_userspace      ~1-5 µs       ~300-500 ns   ~100 ns
mm_map (one page)  ~1-5 µs       ~300-500 ns   ~50 ns (EPT)
mm_unmap           ~1-5 µs       ~300-500 ns   ~50 ns
context_switch     ~50 ns        ~50 ns        ~5 ns (CR3 swap)
read_clock_ns      ~30 ns        ~30 ns        ~30 ns (host clock)
```

These figures are from the architecture document
(`01-architecture/three-layers.md`); A-07 confirms them with
real measurements.

## Update protocol

When the cost model changes (op signature change, hot-path
optimization, KVM backend lands):

1. Re-run `uml-perf.sh` and update the boot wall-time table.
2. If a HOT op was touched, re-run the A-07 microbenchmark suite
   and update the per-op table.
3. Note the date + host context above.
4. If the change is a regression (any number got worse by >5%),
   the patch series should explain why.

## Cross-references

- `01-architecture/invariants.md` — the I2 invariant (≤5% perf
  regression on prod-fast)
- `02-workstreams/A-backend-abstraction/05-contract.md` — the
  conformance suite that consumes these numbers
- `02-workstreams/A-backend-abstraction/07-perf-ci.md` — the CI
  gates that compare future runs against this baseline
