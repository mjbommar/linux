# UML vector2 net-bench integration

Status: current-head tooling cleanup; performance gate still open.
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

## Remaining Gate

This does not close P4.3 performance parity or P4.5 multiqueue fairness.  The
next operator-run TCP check is:

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
