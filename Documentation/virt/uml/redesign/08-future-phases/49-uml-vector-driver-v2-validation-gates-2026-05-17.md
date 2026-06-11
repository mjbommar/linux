# UML vector driver v2 — validation-gate roadmap

**Status:** current gate tracker; not replacement approval.
**Initial date:** 2026-05-17.
**Last updated:** 2026-06-11 against `next`.
**Audit ref:** `46-uml-vector-driver-v2-code-audit-2026-05-17.md`
§P4.3, §P4.4, §P4.5.

This memo is the current vector2 validation-gate tracker.  It supersedes
older default-flip status summaries that were tied to short-lived branches.
The current `next` tree keeps `CONFIG_UML_NET_VECTOR_V2` opt-in, keeps the
legacy vector driver available, and does not claim vector2 is replacement
ready.

The runtime transport claim is intentionally narrow: current vector2 netdevs
support TAP and launcher-owned inherited fd paths.  GRE and L2TPv3 remain
parser/header-helper coverage only; raw, proxy, VDE, BESS, and hybrid are not
implemented runtime netdev transports.

## Current `next` status

| Gate | Current status | Remaining work |
| --- | --- | --- |
| Kconfig/publication claim | Current and bounded: v2 remains `default n`, legacy vector is not deprecated, and docs avoid saying vector2 supersedes legacy vector. | Do not flip defaults or deprecate legacy until the replacement gates below pass. |
| Runtime transport scope | TAP and inherited fd are the only current vector2 runtime netdev transports. Parser-only transports fail explicitly from the netdev open path. | New runtime transports need backend code plus live smokes before entering the claim. |
| Launcher-owned fd handoff | PASS on 2026-06-11 through `vector2-fd-handoff-smoke` on rebuilt `98166580dc4f`. | Keep in CI/preflight. |
| Pool-member TAP handoff | PASS on 2026-06-11 through `vector2-pool-tap-smoke` on rebuilt `98166580dc4f`. Per-take pool fd handoff is retired from the current completion claim. | Keep validating the TAP reopen path used by pool members. |
| fd multiqueue smoke | PASS on 2026-06-11 through `vector2-fd-multiqueue-smoke` on rebuilt `98166580dc4f`. | Add fairness and performance acceptance data. |
| Trusted in-process TAP | PASS on 2026-06-11 through `vector2-inproc-tap-smoke` on rebuilt `98166580dc4f`. | Keep explicit; do not treat it as sandboxed mode. |
| Sandbox audit | PASS on 2026-06-11 through `vector2-sandbox-audit` on rebuilt `98166580dc4f`. | Keep CI/preflight coverage aligned with launcher-managed configs. |
| Failed-open validation knob | PASS on 2026-06-10 through `vector2-failed-open`; `fail_open_after=N` is documented as validation-only. | Leave unset for normal workloads. |
| Seccomp Tier 3 soak | Strong evidence but not final: `45-uml-vector-driver-v2-seccomp-soak-status.md` records a requested-stop 6142/7200 second run with 970/970 PASS. | Let the same 7200-second seccomp/vector2 soak complete naturally. |
| KVM v2 Tier 3 | Partial current-head smoke: rebuilt `98166580dc4f` passed one KVM-v2/vector2 iteration each for `tier3-django-v2` and `tier3-fastapi-v2`, including `SERVER_READY`, `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, and `REPRO_DONE rc=0`. | Run the same full Tier 3 networking coverage on KVM v2 with the final vector2 stack. |
| Perf/fairness/KCSAN breadth | Current guest-to-host TCP gate passes after the TX/RX NAPI scheduling fix. Existing evidence covers TCP perf baseline, KCSAN multiqueue traffic, and several smoke profiles. The current tree now builds the TCP `net-bench` helper through kselftest and keeps the TAP benchmark scripts as explicit operator-run tools. A 2026-06-11 guest-to-host run initially failed throughput parity: vector2 median 18.95 Gbps vs legacy 39.81 Gbps, ratio 0.476 below the 0.85 gate. Scatter-gather TX improved the follow-up ratio to 0.573, and a bounded `sendmmsg()` prototype did not improve the ratio. The subsequent TX/RX NAPI scheduling fix passed the normal gate: vector2 median 37.12 Gbps vs legacy 40.08 Gbps, ratio 0.926. A current fixed-byte bidirectional TCP refresh now clears guest-to-host at 1/8/32 MiB and host-to-guest at 8/32 MiB, but host-to-guest 1 MiB remains below legacy after a focused rerun. The harness now captures guest `ip`/route/ethtool diagnostics, and vector2 reports inherited-fd vnet-header state through ethtool. | Finish the rest of P4.3: host-to-guest small-transfer follow-up, UDP, syscall-rate, CPU-utilisation, longer multiqueue fairness profiles, and broader host/kernel coverage. |

The sections below preserve the original three gate definitions and their
acceptance bars.

## 2026-06-11 Current-HEAD Refresh

`06-sequencing/2026-06-11-vector2-current-head-validation.md` records a
current-head vector2 refresh on rebuilt `98166580dc4f`
(`7.1.0-rc7-00188-g98166580dc4f`):

  - `um_vector2_*` KUnit: 84 pass, 0 fail, 2 trusted-TAP skips;
  - `vector2-fd-handoff-smoke`: PASS;
  - `vector2-fd-multiqueue-smoke`: PASS;
  - `vector2-inproc-tap-smoke`: PASS;
  - `vector2-sandbox-audit`: PASS, `PASS=1/1 FAIL=0 TIMEOUT=0`;
  - `vector2-pool-tap-smoke`: PASS; and
  - bounded KVM-v2/vector2 Tier 3 smoke: Django-v2 1/1 PASS and
    FastAPI-v2 1/1 PASS, both with `SERVER_READY`,
    `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, and `REPRO_DONE rc=0`.

This refresh improves confidence in the current stack but does not close the
replacement gates: P4.3 perf parity, P4.4 natural 7200-second seccomp soak,
and full P4.5 KVM-v2/fairness breadth remain open.

## P4.3 — performance parity acceptance gate

### Current state

`36-uml-vector-driver-v2-perf-baseline.md` recorded a repeated
TCP sweep across both directions (guest -> host and host -> guest),
three buffer sizes (1 MiB / 8 MiB / 32 MiB), two repeats per cell.
Findings:

  - **Host -> guest:** the original May 2026 sweep showed vector2 much faster
    than legacy across all three buffer sizes, but the current June 11 rerun is
    mixed: vector2 clears the larger 8 MiB and 32 MiB cells while 1 MiB remains
    below legacy.
  - **Guest -> host:** the original May 2026 sweep showed vector2 slower than
    legacy across all three buffer sizes.  The June 11 TX/RX NAPI scheduling
    fix now clears 1 MiB, 8 MiB, and 32 MiB in the fixed-byte harness and the
    duration-based `net-bench` gate.
  - **UDP / syscall-rate / CPU-utilisation:** not measured.

`06-sequencing/2026-06-11-vector2-net-bench-integration.md` records a
current-head tooling cleanup: `tools/testing/selftests/um/net-bench` now builds
the `tcp-send` helper through kselftest, removes developer-local absolute paths
from the benchmark template, and keeps the privileged TAP throughput scripts as
explicit operator-run gates rather than default selftests.  The first
current-head guest-to-host run after that cleanup passed all functional
iterations but failed the throughput gate: legacy vector median 39805.4 Mbps,
vector2 median 18949.8 Mbps, ratio 0.476 against the 0.85 bar.  The
scatter-gather TX fix in
`06-sequencing/2026-06-11-vector2-tx-scatter-gather.md` improved the follow-up
run to vector2 median 22953.7 Mbps versus legacy 40041.7 Mbps, ratio 0.573.
A follow-up bounded `sendmmsg()` prototype did not improve the ratio and was
not committed.  The benchmark diagnostics then pointed at TX-driven RX buffer
preparation churn.  The TX/RX NAPI scheduling fix filters empty TX IRQ
reschedules, honors `netdev_xmit_more()` while the TX ring has space, and
gates RX buffer preparation on RX readiness.  The normal guest-to-host TCP gate
now passes: legacy vector median 40080.5 Mbps, vector2 median 37116.0 Mbps,
ratio 0.926.  The benchmark records guest link, route, feature, and
ethtool-counter diagnostics around each sender, and vector2's ethtool
`vnet_hdr_enabled` stat reflects inherited-fd runtime state.

`36-uml-vector-driver-v2-perf-baseline.md` was also refreshed on current
`next` with the fixed-byte bidirectional TCP harness:

  - guest -> host best observed host-side ratios:
    - 1 MiB: 0.966;
    - 8 MiB: 1.007;
    - 32 MiB: 0.925;
  - host -> guest best observed host-side ratios:
    - 1 MiB: 0.483 in the bidirectional run and 0.579 in a focused four-repeat
      rerun;
    - 8 MiB: 2.002;
    - 32 MiB: 0.901.

This confirms that the current guest-to-host TCP regression is fixed in both
the duration-based gate and the fixed-byte harness.  It also leaves a concrete
host-to-guest small-transfer follow-up before P4.3 can be closed.

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
  - full kvm-v2 reruns on the current final vector2 stack; the June 11
    one-iteration Django-v2/FastAPI-v2 smoke proves the path is runnable, but
    it does not replace full KVM-v2/vector2 Tier 3 evidence;
  - longer fairness profiles (10k iters minimum on each
    queue-count + flow-count combination);
  - additional host/kernel coverage:
    - host: the current AMD Zen 4 development host is well-covered; add
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
