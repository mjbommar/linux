# UML KVM v2 Record/Replay R/R-1 Validation

Date: 2026-06-11

Branch: `next`

Validation commit: `02d2d30a3bac`

UML binary tested: `7.1.0-rc7-00261-gbb092119158b`

Purpose: rerun the focused KVM v2 record/replay R/R-1 gates after the branch
was pushed with the current UML v2 status and `umlctl` evidence. The tested
kernel binary was rebuilt from `bb092119158b`; commits above that point were
documentation or selftest ignore-file updates and did not change the UML
kernel image.

## Commands

```sh
make -C tools/testing/selftests/um/kvm-record-smoke
UML_BINARY=$PWD/linux timeout 240s \
  tools/testing/selftests/um/kvm-record-smoke/run-kvm-record-smoke.sh
UML_BINARY=$PWD/linux timeout 120s \
  tools/testing/selftests/um/kvm-record-clock-bench/run-kvm-record-clock-bench.sh
```

## Results

| Gate | Result | Evidence |
| --- | --- | --- |
| `kvm-record-smoke` helper build | PASS | Static helper variants for task-owned replay, mismatch, raw-time, timestamp, signal, randomness, and external-I/O legs rebuilt. |
| `kvm-record-smoke` KUnit leg | PASS | `KUnit=24/24`. |
| `kvm-record-smoke` live debugfs leg | PASS | `entries=3066`, `syscalls=3066`, `used=294336`, `dropped=0`. |
| `kvm-record-smoke` task-owned replay leg | PASS | `pid=1`, `entries=393`, `syscalls=393`, `same=393`, `other=0`, `payload_entries=8`, `payload_bytes=528`, `replayed=393`. |
| `kvm-record-smoke` strict mismatch legs | PASS | `live-mismatch=4/4` for `getcwd(2)`, `clock_gettime(2)`, `gettimeofday(2)`, and `time(2)`. |
| `kvm-record-smoke` signal policy leg | PASS | `live-signal=1`, with `um_backend_kvm_v2_sigmask_install` replay mask/restore evidence. |
| `kvm-record-smoke` raw-time payload leg | PASS | `live-time=1`, with direct-syscall and current UML-vDSO-wrapper raw-time payload replay. |
| `kvm-record-smoke` timestamp-instruction legs | PASS | `live-rdtsc=1`, `live-rdtscp=1`; replay mode faults direct timestamp reads instead of observing host time. |
| `kvm-record-smoke` strict unsupported syscall legs | PASS | `live-negative=1` for `getrandom(2)` and `live-external-io=4/4` for `openat(2)`, `read(2)`, `write(2)`, and `ioctl(2)`. |
| `kvm-record-clock-bench` | PASS | `N=100`, `observed=100`, `replayed=100`, `mismatches=0`. |

The final smoke summary was:

```text
KVM_RECORD_SMOKE: PASS (KUnit=24/24 live-debugfs=1 task-owned=1 live-mismatch=4/4 live-signal=1 live-time=1 live-rdtsc=1 live-rdtscp=1 live-negative=1 live-external-io=4/4)
```

The final clock-bench summary was:

```text
KVM_RECORD_CLOCK_BENCH: PASS (N=100 observed=100 replayed=100 mismatches=0)
```

## Disposition

The focused R/R-1 validation remains green. This supports the current
experimental record/replay claim: task-owned snapshot start, bounded syscall
and payload replay, strict mismatch detection, raw-time syscall payload replay,
timestamp-instruction fail-closed behavior, replay SIGALRM masking, and
fail-closed representative randomness/external-I/O syscalls.

This does not close the broader record/replay completion blocker. Whole-system
replay, replayable asynchronous signal ordering, persistent record files,
native VVAR-style fast paths, arbitrary device/network/hostfs event replay,
and broader workload coverage remain outside the current R/R-1 claim.
