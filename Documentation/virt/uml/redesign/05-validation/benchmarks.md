# Benchmarks

What we measure. Numbers go in `tools/testing/selftests/uml/perf/`.

## Microbenchmarks

### M-syscall: syscall round-trip cost

```
for (int i = 0; i < 1_000_000; i++) getpid();
```

Measure with `rdtscp`. Report median + p99.

Targets:
- prod-fast (KVM): <100 ns
- prod-fast (seccomp, nested): <500 ns
- research: <1000 ns
- library: <50 ns (no trap)
- embedded (ptrace): <3000 ns

### M-pgfault: page-fault delivery

```
for (int i = 0; i < N; i++) {
    void *p = mmap(NULL, 4096, ...);
    *(int*)p = 1;  // triggers fault
    munmap(p, 4096);
}
```

Targets per backend, similar tier.

### M-ctx: context switch

`pipe(2)` between two threads, ping-pong. Report
context-switches/sec.

### M-fork: process creation

`fork() + exit()` rate. Critical for shell-heavy workloads
and snapshot validation.

### M-net: network throughput

`iperf3` over each transport (tap, raw, GRE, vhost-user-net).

### M-block: block I/O

`fio` against UBD and virtio-blk-via-vhost-user.

### M-boot: boot time

Time from kernel exec to `/bin/true` exit. Per profile.

### M-snapshot: snapshot-restore cycle

For fuzz profile: time from "trigger snapshot" to "restored
state ready for next test".

Target: <50 ms.

## Macrobenchmarks

### MA-LTP-runtest: LTP runtest/syscalls suite

Runtime of standard suite. Per profile, per backend.

### MA-kbuild: build a kernel inside UML

Time `make defconfig && make` of a small kernel inside UML.
Catches regressions in fork-heavy, fs-heavy, schedule-heavy
workloads.

### MA-fuzz-throughput: syzkaller iter/sec

For fuzz and fuzz-deep profiles. With C-08's `vm/uml` backend.

### MA-tmpfs-stress: filesystem stress

`stress-ng --tmpfs-fill 90% --timeout 60`. Measures
allocator + VFS path.

### MA-network-stress: parallel TCP connections

10k concurrent TCP echo connections. Measures network stack
under load.

## Per-gate microbenchmarks (Layer 2)

For each static-key gate:

```
for (int i = 0; i < 10_000_000; i++) trip_the_gate();
```

In two configurations: gate off, gate on. Report cost per
trip in each.

Targets (off-state):
- All gates: <2 ns per gate (invariant I3)
- Combined cost of all gates per syscall: <20 ns

Targets (on-state) — informational, not enforced:
- trace_syscalls: ~50 ns
- kcov_enabled: ~10 ns
- record_replay: ~200 ns
- time_travel_active: variable (~500 ns when coordinator queried)

## Per-backend cost decomposition

For each backend, measure the cost of each ops-table op
individually:

- syscall_dispatch
- page_fault
- context_switch
- ipi_send
- read_clock_ns

Report as a table; updates to A-06's documentation.

## Reporting

CI generates a benchmark report per build:

```
$ uml-bench report
Profile: prod-fast (seccomp backend)
Host: Intel Xeon E5-2670 v3 @ 2.30GHz, Linux 7.0.0

Microbench:
  M-syscall:    312 ns (median), 425 ns (p99)  [vs baseline 305 ns: +2.3%]
  M-pgfault:    1840 ns (median)               [vs baseline 1810 ns: +1.7%]
  ...

Macrobench:
  MA-LTP-runtest: 247 s                         [vs baseline 245 s: +0.8%]
  ...

Verdict: PASS (all within 5% of baseline)
```

## Baseline management

Baselines stored in
`tools/testing/selftests/uml/perf/baselines/<profile>.json`.

Updated explicitly when:
- An intentional perf change lands (annotated in commit)
- A new hardware target is added (re-baseline on it)
- Quarterly drift review (sometimes baselines need bumps)

Never updated automatically. Bot-driven baseline updates lead to
slow regressions sneaking in.
