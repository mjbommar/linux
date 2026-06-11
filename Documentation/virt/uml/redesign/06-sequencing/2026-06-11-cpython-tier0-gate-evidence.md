# CPython Tier-0 Gate Evidence

Date: 2026-06-11

Branch: `next`

## Purpose

The completion plan calls for broader KVM v2 dynamic-userspace evidence beyond
`/bin/true`, `dyn-loader`, and the substrate reproducers. This slice records
the current `cpython-tier0` gate result through the `umlctl gate run` wrapper on
the same `./linux` binary used for the substrate tightening slice.

## Gate

Gate descriptor:

```text
tools/testing/selftests/um/gates/cpython-tier0.toml
```

Harness:

```text
tools/testing/selftests/um/cpython-tier0/run-cpython-tier0.sh
```

The gate boots UML with hostfs root, runs the curated CPython tier-0 test
sweep as init, and extracts the hashlib SHA-256 metric from the gate log.

## Current Evidence

Command shape:

```text
tools/uml/uml-launcher/target/debug/umlctl gate run \
  --source-root . \
  --file tools/testing/selftests/um/gates/cpython-tier0.toml \
  --kernel "$PWD/linux" \
  --backend <seccomp|kvm-v2> \
  --scoreboard <temporary-jsonl>
```

Results:

```text
cpython-tier0 backend=seccomp pass=1 fail=0 expected_fail=0 exit=0 status=PASS
cpython-tier0 backend=kvm-v2 pass=1 fail=0 expected_fail=0 exit=0 status=PASS
```

Both rows extracted the same metric:

```text
hashlib_sha256_emptystring=2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
```

## Interpretation

This is a focused dynamic-userspace gate, not the final workload matrix. It
does prove that the current KVM v2 tree can run the curated CPython tier-0 path
through the same gate wrapper as seccomp with matching parsed metrics.

Remaining KVM v2 workload breadth still includes broader CPython parity,
vector2 Tier 3 networking, long soak, and final matrix coverage.
