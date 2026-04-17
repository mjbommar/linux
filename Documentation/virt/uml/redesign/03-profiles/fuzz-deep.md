# Profile: fuzz-deep

**One-line:** fuzz + KCSAN + record-replay; slower iteration but
catches more bug classes; replayable crashes.

## Intended user

- Researchers hunting concurrency bugs (KCSAN).
- Bug-bounty / disclosure work where reproducibility matters.
- Long-running fuzzing campaigns.

## Defining features

Same as fuzz, plus:

- KCSAN: compiled in, on.
- Record-replay: hook compiled in, off by default; turn on
  per-crash for deterministic replay.
- KMSAN: optional (heavy but valuable).

## Kconfig fragment

```
# Inherits fuzz.config

CONFIG_KCSAN=y
CONFIG_UM_RECORD_REPLAY=y     # gate exists
# CONFIG_KMSAN — opt-in; default off because heavy
```

## Cost

- Binary size: ~110 MB (added KCSAN)
- Boot: ~700 ms
- Snapshot restore: <50 ms (replay buffer adds slight cost)
- Syscall: ~1200 ns

## The replay loop

```
$ uml-forkserver ./linux-fuzz-deep ... &
$ syz-manager -config uml-fuzz-deep.cfg
... fuzz hits a KASAN crash on iter 12345 ...
... record-replay stored event log for last 1000 syscalls ...
$ uml-replay <event-log> ./linux-fuzz-deep
... deterministically reproduces the crash ...
$ gdb -ex 'b kasan_report' --args ./linux-fuzz-deep ...
... step into the bug under deterministic replay ...
```

This combination — fuzzer + KASAN + record-replay + KCSAN — is
not available on any other kernel research platform.

## Validation

- KCSAN reports a known race in synthetic testcase
- Record-replay deterministically reproduces a captured crash
- Replay is bit-exact across runs

## What this profile is NOT

- Not for fast steady-state fuzzing — overhead is
  ~2× of `fuzz` profile.
- Not for production.
