# Substrate Gate Tightening

Date: 2026-06-11

Branch: `next`

## Purpose

The UML v2 completion plan requires the validation surface to reflect current
functionality, not stale historical floors. The `regrtest-substrate` gate still
allowed the old pre-fix KVM v2 substrate line with only five passes, even
though current `next` now runs the full substrate repro set at or above the
seccomp pass floor.

## Current Evidence

Command:

```text
UML_BINARY=$PWD/linux BACKEND=seccomp TIMEOUT=180 \
  bash tools/testing/selftests/um/regrtest-repros/run-regrtest-repros.sh
```

Result:

```text
PASS=25 FAIL=3 EXPECTED_FAIL=3
```

Command:

```text
UML_BINARY=$PWD/linux BACKEND=kvm-v2 TIMEOUT=180 \
  bash tools/testing/selftests/um/regrtest-repros/run-regrtest-repros.sh
```

Result:

```text
PASS=27 FAIL=3 EXPECTED_FAIL=1
```

Both invocations use the current `./linux` binary on `next` with
`CONFIG_UM_BACKEND_KVM_V2=y` and the KVM v2 record/replay KUnit options enabled.
The harness exits nonzero when `FAIL > 0`, so the gate descriptor intentionally
keeps `require_clean_exit = false` and enforces the parsed counts instead.

The `umlctl gate run` wrapper was also run with a temporary scoreboard after
the threshold update:

```text
regrtest-substrate backend=seccomp pass=25 fail=3 expected_fail=3 exit=1 status=PASS
regrtest-substrate backend=kvm-v2 pass=27 fail=3 expected_fail=1 exit=1 status=PASS
```

## Gate Change

`tools/testing/selftests/um/gates/regrtest-substrate.toml` now uses:

```text
min_pass = 25
max_fail = 3
require_clean_exit = false
```

That keeps the gate aligned with the current worst-backend pass floor while
still allowing the three known ordinary class-a-env failures. Any pass count
below 25 or any additional ordinary failure is now a regression.

## Remaining Work

The substrate gate is not a complete final matrix by itself. The plan still
requires broader KVM v2 dynamic-userspace coverage, vector2 Tier 3 coverage,
record/replay deterministic workload policy, KMSAN runtime closure, and the
final integration matrix before UML v2 can be declared complete.
