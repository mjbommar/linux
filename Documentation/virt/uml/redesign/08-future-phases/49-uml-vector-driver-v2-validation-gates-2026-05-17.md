# UML vector driver v2 — validation-gate roadmap

**Status:** roadmap for the three remaining operator-time gates.
**Date:** 2026-05-17.
**Audit ref:** `46-uml-vector-driver-v2-code-audit-2026-05-17.md`
§P4.3, §P4.4, §P4.5.

The audit's swap-readiness checklist has three remaining gates that
need wall-clock time + benchmarking on the operator's host fleet,
not in-tree code work.  This memo defines what each gate needs and
the acceptance criteria.

## P4.3 — performance parity acceptance gate

### Current state

`36-uml-vector-driver-v2-perf-baseline.md` recorded a repeated
TCP sweep across both directions (guest -> host and host -> guest),
three buffer sizes (1 MiB / 8 MiB / 32 MiB), two repeats per cell.
Findings:

  - **Host -> guest:** vector2 is **much faster** than legacy on
    this host across all three buffer sizes.
  - **Guest -> host:** vector2 is **slower** than legacy on this
    host across all three buffer sizes.
  - **UDP / syscall-rate / CPU-utilisation:** not measured.

### Acceptance criteria for "perf parity"

The gate is *not* "vector2 is faster than legacy in every cell."
It is "regressions are bounded + understood."  Concretely:

  1. **TCP throughput** (BIDI):
     - host -> guest: vector2 >= legacy * 1.0 (no regression)
     - guest -> host: vector2 >= legacy * 0.85 (≤15 % regression
       acceptable for a first replacement; the gap is the cost
       of the queue-ownership rewrite + virtio_net_hdr handling
       and can be tightened later).
  2. **UDP throughput** (1500-byte payload, BIDI):
     - same shape: no regression host -> guest, ≤15 % guest -> host.
  3. **syscall rate** (small-frame TCP, 64-byte payload):
     - measure both backends with `perf stat -e syscalls:sys_*`.
     - vector2 should NOT add net syscalls vs legacy (the
       NAPI rewrite was supposed to consolidate, not multiply).
  4. **CPU utilisation under steady-state load**:
     - measure %user / %sys / %softirq across both backends at
       the same achieved throughput.
     - vector2 may shift work between %sys and %softirq but
       total CPU consumption should not exceed legacy by >20 %.

### What to do

The benchmarks live in
`Documentation/virt/uml/redesign/08-future-phases/36-uml-vector-driver-v2-perf-baseline.md`
already; extending the matrix to cover UDP + syscall + CPU is
operator time.  Acceptance is a judgment call on the resulting
table — this memo defines the bars.

## P4.4 — full 7200-second long-soak gate

### Current state

`45-uml-vector-driver-v2-seccomp-soak-status.md` recorded:
  - 6142-second run (85.3 % of 7200-second budget);
  - 970 / 970 PASS across Django-v2 + FastAPI-v2 on seccomp +
    vector2 fd+tap;
  - operator-stopped (not budget-elapsed).

### Acceptance

Re-run the same configuration but allow the full 7200-second
budget to elapse naturally.  Acceptance:

  - all (workload, backend, queue) tuples remain at ≥99.5 % PASS
    (Wilson 95 % lower bound ≥99.0 %);
  - no fatal kernel signatures in dmesg (panic/BUG/Oops/Kernel
    mode fault/general protection fault/NULL pointer);
  - clean TAP teardown after final iteration;
  - process state empty at end.

### What to do

Re-run the same `tools/testing/selftests/um/soak/run-soak-daemon.sh`
invocation from §45 with `--budget-sec 7200` and let it elapse.
Operator time.

## P4.5 — broader KCSAN matrix + kvm-v2 reruns

### Current state

KCSAN coverage so far:
  - auto-queue fd multiqueue 10/10 (no warning, no data race);
  - one FastAPI vector2 fd workload PASS;
  - concurrent TCP/UDP traffic — initial PASS + three default
    repeats all four TX/RX queues moving;
  - 2-queue/6-flow + paced 8-flow/4-queue PASS.

Missing:
  - kvm-v2 reruns (blocked on KVM-v2's separate Django/socket
    workload flake; once fixed, re-run all the KCSAN profiles);
  - longer fairness profiles (10k iters minimum on each
    queue-count + flow-count combination);
  - additional host/kernel coverage:
    - host: AMD Zen 4 (our dev host) is well-covered; add
      Intel Sapphire Rapids + ARM64 if available;
    - kernel: tip-of-master + 6.6 LTS + 6.12 LTS.

### Acceptance

  - kvm-v2 KCSAN reruns: all profiles PASS at the same rate as
    seccomp, no KCSAN warnings;
  - longer-fairness profiles: 10k iters each, ≥99.5 % PASS,
    queue counters stay within 2x of each other on a uniform
    workload (no starvation);
  - host portability: at least one non-Zen 4 host PASS.

### What to do

Substantial benchmarking + bench-fleet operator work.  Estimate:
several days of wall-clock per host.

## Combined gate-out-of-this-band

When all three gates have data + acceptance:

  - bump the buildout memo's "Status" line to
    `BUILDOUT PLAN - vector v2 swap-eligible pending operator
    cutover`;
  - flip the Kconfig default of `CONFIG_UML_NET_VECTOR_V2=n`
    (today) to `y` (then) in `arch/um/drivers/Kconfig`;
  - mark `CONFIG_UML_NET_VECTOR` as deprecated in the Kconfig
    help text, naming a removal window (e.g. "scheduled for
    removal in Linux 6.20").

This memo + the cmdline migration memo
(`48-uml-vector-driver-v2-cmdline-migration-2026-05-17.md`)
collectively define the path from where v2 is today (audit-
clean experimental driver) to a default replacement for legacy.

## What is *not* in this gate set

  - **Multi-host vhost-user backend** (the sandbox-confined helper
    described in the original buildout memo §"sandbox profile").
    That is a separate workstream — vector2 v1 stops at inproc +
    inherited-fd TAP/FD.
  - **Performance parity vs vhost-net** (the upstream "fast"
    target).  Not in scope; legacy parity is the gate.
  - **Live-migration support.**  Same as above; out of scope.
