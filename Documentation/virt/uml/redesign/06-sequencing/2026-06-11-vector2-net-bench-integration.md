# UML vector2 net-bench integration

Status: current-head tooling cleanup complete; TCP performance gate still
failing after first implementation fix.
Date: 2026-06-11.
Tree: `next`.

## Purpose

The vector2 replacement plan requires performance and fairness evidence before
any replacement-ready claim.  The existing TCP throughput scripts were useful
operator tools, but they were not wired into the kselftest build surface and
one template carried a developer-local absolute path.

This change makes the TCP benchmark harness reviewable and reproducible without
turning privileged TAP benchmarking into an automatic selftest.

## What Changed

- Added `tools/testing/selftests/um/net-bench/Makefile`.
- Added `net-bench` to `tools/testing/selftests/um/Makefile` so the helper is
  built by the UML selftest target.
- Kept the benchmark scripts as installed files, not default `TEST_PROGS`,
  because they require a UML kernel, TAP setup, and operator-controlled runtime.
- Built the C TCP sender through kselftest as `TEST_GEN_PROGS := tcp-send`.
- Removed developer-local default paths from the throughput wrappers and TOML
  template.
- Added repo-relative defaults for the UML kernel, local `umlctl` discovery, and
  the TCP sender helper.
- Tightened wrapper preflight and empty-result handling so missing prerequisites
  or failed driver runs do not become bogus throughput medians.

## Validation

Commands run:

```sh
make -C tools/testing/selftests/um/net-bench clean
make -C tools/testing/selftests/um/net-bench
make -C tools/testing/selftests/um/net-bench OUTPUT=/tmp/um-net-bench
bash -n tools/testing/selftests/um/net-bench/run-tcp-throughput.sh \
        tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh
python3 -m py_compile tools/testing/selftests/um/net-bench/exec-uml-fd.py
```

Result:

- `tcp-send` builds cleanly through the net-bench Makefile.
- `tcp-send` also builds with an out-of-tree kselftest `OUTPUT` directory.
- Shell wrappers pass syntax validation.
- `exec-uml-fd.py` compiles through Python bytecode validation.

## 2026-06-11 TCP Gate Run

After the harness cleanup, the short guest-to-host TCP gate was run on the
current tree:

```sh
OUT=/tmp/uml-net-bench-run-1781173986 \
KERNEL=$PWD/linux \
tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
        --duration 8 \
        --reps 3
```

Functional result:

- legacy vector: 3/3 `umlctl gate loop` iterations passed;
- vector2: 3/3 `umlctl gate loop` iterations passed;
- host sink received all six TCP streams cleanly.

Performance verdict:

| Driver | Per-rep Mbps | Median Mbps |
| ------ | ------------ | ----------- |
| legacy vector | 40421.9, 36973.3, 39805.4 | 39805.4 |
| vector2 | 18949.8, 19679.2, 17902.6 | 18949.8 |

The vector2/legacy median ratio was `0.476`, below the `0.85` acceptance bar:

```text
VERDICT: FAIL
```

This is a real P4.3 failure for current guest-to-host TCP throughput, not a
harness failure.  The functional datapath is alive, but vector2 remains far
from replacement-ready on this gate.

## Follow-Up Scatter-Gather Fix

`2026-06-11-vector2-tx-scatter-gather.md` records the first implementation
response: vector2 fd/TAP TX now writes the virtio header, skb head, and skb
frags through a scatter-gather iovec instead of forcing skb linearization.

The same TCP gate improved but still failed:

- legacy vector median: 40041.7 Mbps;
- vector2 median: 22953.7 Mbps;
- vector2/legacy ratio: 0.573, below the 0.85 gate.

An uncommitted bounded `sendmmsg()` prototype was tested after this fix and
did not improve the gate, so batching alone is not the obvious remaining
answer.

## Follow-Up Diagnostics

The current benchmark template now records guest-side network diagnostics
around the TCP sender:

- `ip -d link show` for the active guest interface;
- `ip -s link show` before and after the sender;
- guest route state;
- `ethtool -k` feature state when the tool is available; and
- `ethtool -S` driver counters when the driver exposes them.

The vector2 ethtool `vnet_hdr_enabled` stat is now runtime-aware for inherited
fd channels.  It reports the actual `TUNGETIFF` result that the fd backend
probed, instead of treating all fd transports as non-vnet.  That makes future
net-bench logs capable of distinguishing a real datapath bottleneck from an
offload-negotiation failure.

Validation for this diagnostic slice:

```sh
make ARCH=um -j$(nproc)
timeout 180s ./linux mem=256M \
        kunit.filter_glob='um_vector2_ethtool' \
        kunit_shutdown=halt
python3 tools/testing/kunit/kunit.py parse \
        /tmp/um-vector2-ethtool-diag-kunit.log
timeout 240s ./linux mem=256M \
        kunit.filter_glob='um_vector2_*' \
        kunit_shutdown=halt
python3 tools/testing/kunit/kunit.py parse \
        /tmp/um-vector2-diag-kunit.log
make -C tools/testing/selftests/um/net-bench clean
make -C tools/testing/selftests/um/net-bench
bash -n tools/testing/selftests/um/net-bench/run-tcp-throughput.sh \
        tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh
python3 -m py_compile tools/testing/selftests/um/net-bench/exec-uml-fd.py
```

The focused ethtool KUnit reported 6/6 pass, including the new inherited-fd
vnet-header stat case.  The broader `um_vector2_*` KUnit run reported
87 pass, 0 fail, and 2 trusted-TAP skips.

A short expected-fail diagnostic TCP run was also executed:

```sh
OUT=/tmp/uml-net-bench-diag-1781175988 \
KERNEL=$PWD/linux \
BENCH_PORT=5312 \
TAP=tcpdiag-tap1 \
tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
        --duration 2 \
        --reps 1
```

It still failed the throughput gate, as expected:

- legacy vector: 39576.9 Mbps;
- vector2: 21739.7 Mbps;
- ratio: 0.549.

The important result is the new diagnostic evidence in
`loop-vector2/p0_default/w0/run-1.log`: vector2 reported
`vnet_hdr_enabled: 1`, TX checksum/GSO/TSO/scatter-gather enabled, no
TX drops/errors, and after the two-second sender `tx_xmit_calls` and
`tx_ring_completed` both reached 88127 for 5441634350 transmitted bytes.
This points the next investigation away from basic offload negotiation and
toward syscall count, queue/NAPI scheduling, and per-packet transmit overhead.

## Remaining Gate

This does not close P4.3 performance parity or P4.5 multiqueue fairness.  The
next work is measurement-driven bottleneck isolation, then rerunning the same
gate after each narrow datapath change:

```sh
make -C tools/testing/selftests/um/net-bench
tools/testing/selftests/um/net-bench/run-tcp-throughput-via-umlctl.sh \
        --kernel "$PWD/linux" \
        --duration 8 \
        --reps 3
```

Acceptance still requires the broader matrix from
`08-future-phases/49-uml-vector-driver-v2-validation-gates-2026-05-17.md`:
bidirectional TCP, UDP, syscall-rate, CPU-utilisation, and longer multiqueue
fairness profiles.  Until those runs pass, vector2 remains opt-in and not a
replacement for legacy vector.
