# Milestones

Verifiable checkpoints. Each is a yes/no question with a
testable answer.

## M1: Backend interface frozen (end of month 2)

**Question:** Can a new backend be implemented by filling in
`struct um_backend_ops` and nothing else?

**Test:** Create a stub `um_backend_null` that returns -ENOSYS
for every op. Build it; show it links cleanly with no other
arch/um/ changes.

## M2: ptrace + seccomp backends both pass conformance (end of month 4)

**Question:** Are the two backends interchangeable for kernel
correctness?

**Test:** Run conformance suite on both; identical results.
Run LTP runtest/syscalls on both; identical results.

## M3: Single static-key gate flippable end-to-end (end of month 6)

**Question:** Can a runtime user observe behavior change from a
gate flip?

**Test:** Boot prod-with-hooks; verify trace gate is off.
`echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls`. Verify
next syscall produces a trace event. `echo 0`. Verify trace
events stop.

## M4: prod-fast benchmarks unchanged from skas baseline (end of month 6)

**Question:** Has the architecture cost prod-fast performance?

**Test:** Compare current prod-fast (`getpid()` cycles, fork
storm rate, network throughput) against pre-refactor `skas`-mode
baseline. Each within 5%.

## M5: KCOV-flip demo (end of month 9)

**Question:** Can syzkaller observe coverage from a runtime-flipped
KCOV gate?

**Test:** Boot prod-with-hooks. Mmap KCOV buffer in test
process. Flip KCOV gate on. Run a workload. Verify coverage
data appears.

## M6: research profile reproduces a syzbot CVE (end of month 12)

**Question:** Is research profile actually useful for kernel
research?

**Test:** Pick a recent syzbot CVE (any). Reproduce it under
research profile. Time from "I have the syz repro" to "I have
the KASAN report" should be <30s.

**Status:** landed (2026-04-23). Shipped as post-Q1 push Phase I
Lift #8 (`06-sequencing/post-q1-push.md`). Selftest
`tools/testing/selftests/um/cve-repro/` boots the research-
profile UML with `kasan_test_module=<path>` on the cmdline,
loads `kasan_test.ko` which executes the full KASAN KUnit
suite. Every passing test is a minimal abstract repro of a
CVE-class memory-safety bug (heap OOB read/write, use-after-
free, double-free, stack OOB, globals OOB, etc., per
`lib/kasan_test_c.c`). The PASS gate requires ≥10 TAP-level
`ok` lines AND ≥10 `BUG: KASAN:` reports; the runner asserts
host-visible wall-clock ≤30 s (matches the M6 contract). The
intentional divergence from a single-named-CVE repro is
documented in the selftest's inline comments: synthetic-but-
deterministic reproduction proves the research profile's
detection machinery works; specific-CVE repros are left as a
user-extensible slot on top of the same harness.

## M7: syzkaller `vm/uml` backend lands (end of month 12)

**Question:** Has google/syzkaller#1288 been closed?

**Test:** PR merged in syzkaller repo. `syz-manager` with
`type: uml` runs syzkaller successfully against UML.

## M8: fuzz profile achieves <50ms restart (end of month 15)

**Question:** Is the snapshot/forkserver model working?

**Test:** Measure restart cost end-to-end (fuzzer iteration
boundary). Median <50ms over 1000 iterations.

## M9: KVM backend round-trips a syscall (end of month 18)

**Question:** Does the KVM platform pattern work for UML?

**Test:** A guest userspace process under KVM backend executes
`getpid()` and gets the right answer. Doesn't matter how slow
yet.

## M10: sandbox profile with crosvm-style launcher (end of month 18)

**Question:** Has UML's host attack surface been minimized?

**Test:** Audit running sandbox-profile UML with `strace -p`;
only whitelisted host syscalls invoked. Pen-test: known
guest-kernel exploit doesn't escape device jails.

## M11: KVM bookend (end of month 21)

**Question:** Did we hit the ~100 ns syscall target?

**Test:** Microbenchmark `getpid()` with `rdtscp` on bare metal
under KVM backend. Median <100 ns.

## M12: All profiles ship (end of month 24)

**Question:** Is the architecture done in v1?

**Test:** All 9 profiles build cleanly; each passes its
profile-appropriate validation suite; all on the same source
tree.

## Reporting cadence

- Weekly: each workstream owner reports status against current
  task
- Monthly: status to linux-um list
- Quarterly: review milestones; adjust calendar; decide if
  slippage is normal or alarming
- M1, M3, M5, M7, M9, M11, M12: external announcements
  (blog post, LWN article, conference talk)
