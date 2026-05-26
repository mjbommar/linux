# Conformance tests

Exhaustive tests that prove the architecture is real Linux,
not Linux-shaped.

## Test corpora

### LTP (Linux Test Project)

Run subsets per profile:

- `runtest/syscalls` — every profile
- `runtest/fs` — every profile (sanity)
- `runtest/io` — research, fuzz (long-running)
- `runtest/mm` — research, fuzz (catches mm bugs)
- `runtest/net` — research, fuzz (with network stack)
- `runtest/sched` — research (with schedstats)

Pass rate target: ≥95% (on par with QEMU-KVM as target).

### kselftest

Run all `tools/testing/selftests/` subsets that aren't
explicitly skipped for UML:

- `x86` — basic
- `mm`
- `net`
- `cgroup`
- `bpf` (after C-06 BPF JIT)
- `ftrace` (after C-05 ftrace)
- `kvm` — runs *under* UML; tests host KVM API

### syzkaller crash reproducer corpus

Maintain a corpus of historical syzbot reproducers (~100
testcases representing diverse bug classes). Each must
reproduce identically across backends.

This is invariant I4 + I5 enforcement at scale.

### KASAN/KMSAN/KCSAN positive tests

The `test_kasan.ko`, `test_kmsan.ko`, `test_kcsan.ko` modules.
Each must report the synthetic bug they trigger.

### KFENCE positive tests

`kfence_kunit_test`.

### kprobes / ftrace / BPF integration tests

Standard `samples/kprobes/`, `samples/ftrace/`, `samples/bpf/`
all loadable and functional.

## Cross-backend equivalence

Cornerstone of invariant I4. The same workload on different
backends produces the same observable behavior.

Tests:

- Run `LTP/runtest/syscalls` on ptrace; capture pass/fail per
  test.
- Run on seccomp; compare. Diff must be empty.
- Run on KVM; compare. Diff must be empty.

If any test passes on one backend and fails on another, that's
a backend bug (not a test bug). File and fix.

## Profile-specific conformance

### research

- Boot with KASAN; deliberate use-after-free; KASAN report
  matches expected
- Attach kprobe to `do_sys_open`; verify it fires
- Attach gdb; break on `sys_close`; verify breakpoint hits

### fuzz

- syzkaller `vm/uml` backend boots and runs
- KCOV records edges per syscall
- Snapshot+restore cycle <50 ms (M-snapshot)
- Forkserver model survives 10k iterations

### fuzz-deep

- KCSAN positive on synthetic race
- Record-replay produces deterministic output across 10 runs

### sandbox

- `strace -p <launcher pid>` shows only whitelisted host syscalls
- Known guest-kernel CVE exploit doesn't escape the device jails
- `perf stat -e syscalls` confirms low host syscall rate

### library

- LKL-style program reads `/etc/passwd` correctly
- A fuzzer harness against `liblinux.a` runs at >100k iter/s
- KASAN-instrumented build catches a synthetic VFS bug

### time-travel

- Two UMLs in `time-travel=ext` mode produce bit-identical
  output across 5 runs
- Captured event log replays bit-identically
- Determinism confirmed via cryptographic hash of stdout

### embedded

- Boots on Linux 3.0 host (older than SECCOMP_FILTER)
- Boots in container without `/dev/kvm`
- LTP runtest/syscalls passes (slowly)

## Test maintenance

- New conformance tests added when invariants are extended
- Skipped tests periodically reviewed (don't accumulate)
- Flakiness budget: any test flaking >0.5% is quarantined
  immediately and fixed before next release
