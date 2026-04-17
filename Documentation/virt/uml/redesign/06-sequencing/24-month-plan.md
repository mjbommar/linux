# 24-month plan (single-engineer-pace)

Calendar view assuming 1 EM/month average. Compress proportionally
for larger teams.

## Q1 (months 1-3): foundation

- A-01 (ops table design): 2 wk
- A-02 (ptrace refactor): 6 wk; first ops migrated
- A-04 (Kconfig single vs multi-backend): 2 wk
- A-05 (conformance suite skeleton): in parallel
- C-01 (defconfig design + skeleton): 3 wk

**Q1 milestone**: Ops table designed, ptrace refactor underway,
first defconfig (`prod-fast`) builds (even if it's just the
existing skas mode wrapped).

## Q2 (months 4-6): backend abstraction complete

- A-02 (continued): finish ops migration
- A-03 (seccomp wrap): 4 wk
- A-05 (conformance suite): full
- A-06 (documentation): 2 wk
- A-07 (perf CI): 2 wk
- B-01 (audit hot paths): 2 wk
- B-02 (gate design + first 3 gates): 3 wk

**Q2 milestone**: ptrace and seccomp backends pass conformance.
First 3 static-key gates inserted; off-state cost <2 ns proven.

## Q3 (months 7-9): observability parity

- B-03 (debugfs controls): 2 wk
- B-04 (section split): 4 wk
- B-05 (gate benchmarks): 3 wk
- B-06 (KCOV-flip demo): 2 wk
- C-02 (KFENCE port): 2 wk — easy warm-up
- C-08 (syzkaller backend) start: 2 wk in

**Q3 milestone**: Section split deployed. KCOV-flip demo works.
KFENCE on UML.

## Q4 (months 10-12): tracing parity + first profile ship

- C-04 (kprobes): 4 wk
- C-05 (ftrace): 4 wk
- C-08 (syzkaller backend) finish: 4 wk
- C-09 (snapshot/forkserver) start: 2 wk in

**Q4 milestone**: ftrace + kprobes work in research profile.
syzkaller `vm/uml` backend lands. First public ship: research
profile is usable for kernel-CVE reproduction at par with QEMU.

## Q5 (months 13-15): fuzz capability

- C-09 (snapshot/forkserver) finish: 4 wk
- C-06 (BPF JIT): 3 wk
- C-03 (KCSAN): 4 wk
- D-01 (KVM platform design) start: 4 wk

**Q5 milestone**: fuzz profile shipping. <50ms restart achieved.
KCSAN catches races. KVM design committed.

## Q6 (months 16-18): KVM and sandbox

- D-02 (MSR_LSTAR trap path): 4 wk
- D-03 (page-table mgmt): 6 wk in parallel start
- C-10 (crosvm-style launcher): 6 wk in parallel
- C-07 (KMSAN) start: 2 wk in

**Q6 milestone**: KVM backend boots and round-trips a syscall.
Sandbox profile shipping with crosvm-style isolation.

## Q7 (months 19-21): KVM stabilization

- D-03 (page-table mgmt) finish: 4 wk
- D-04 (ring transitions): 4 wk
- D-05 (nested-virt fallback): 2 wk
- C-07 (KMSAN) continue: 4 wk

**Q7 milestone**: KVM backend functional; conformance approaching;
KMSAN landing.

## Q8 (months 22-24): polish and ship

- D-06 (KVM conformance + bookend): 4 wk
- C-07 (KMSAN) finish: 2 wk
- All-profile validation
- Release: announce to LKML, write up for LWN, present at netdev

**Q8 milestone**: All 9 profiles ship. KVM bookend benchmark
hit. Public announcement.

## Sequencing notes

- **A-02 is the longest single task** (6 wk). Don't try to
  compress it — incremental refactors are inherently slow.
- **B's KCOV-flip demo** at end of Q3 is the most important
  external signal of the architecture. Don't slip it.
- **C-08 (syzkaller)** is intentionally early. Once syzkaller
  targets UML, real fuzzing pressure improves all subsequent
  work.
- **D (KVM)** starts late on purpose. Its design depends on A
  and B being settled. Earlier start would risk rework.

## Slip handling

If a task slips by >50% of its budget:

1. Owner reports to sequencing review.
2. Either: (a) add resources, (b) descope, (c) defer to next
   release.
3. Update this calendar. Don't pretend.

If 2+ tasks slip in the same quarter:

1. Stop and reassess. Either the team is undersized or the plan
   was wrong.
2. Consider shipping an interim release of just what's done.
   Architecture is incremental; partial shipping is fine.
