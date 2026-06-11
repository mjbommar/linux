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

The remaining likely gap is vector2's lack of legacy vector's `sendmmsg()` TX
batching.

## Remaining Gate

This does not close P4.3 performance parity or P4.5 multiqueue fairness.  The
next work is vector2 TX batching, then rerunning the same gate:

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
