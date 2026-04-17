# Critical path

The longest chain of strictly sequential work. Determines
shortest possible wall-clock time regardless of team size.

## The chain

```
A-01 (ops table design)             2 wk
  ↓
A-02 (ptrace refactor)              6 wk
  ↓
A-04 (Kconfig single/multi)         2 wk
  ↓
C-01 (defconfig skeleton)           3 wk
  ↓
B-04 (section split)                4 wk     ◀─ blocks ftrace/kprobes
  ↓
C-04+C-05 (kprobes + ftrace)        4 wk     ◀─ shippable research
  ↓
C-08 (syzkaller backend)            4 wk
  ↓
C-09 (snapshot/forkserver)          6 wk     ◀─ shippable fuzz
  ↓
M8 (fuzz milestone reached)
```

Total critical path: **31 weeks ≈ 7 months wall-clock**.

This is the floor. Any team can ship the fuzz profile in 7
months if everything goes right. Adding engineers buys
parallel tasks (B-01..B-03, B-05, B-06; C-02..C-07; A-03;
A-05; A-06; A-07) but not critical-path compression.

## What's on the critical path and why

- **A-01..A-04**: foundation; every workstream depends on
  these.
- **C-01**: defconfigs; profile concept doesn't work until
  these exist.
- **B-04**: section split; ftrace and kprobes can't land
  without it.
- **C-04, C-05**: kprobes and ftrace; research profile isn't
  useful without them.
- **C-08**: syzkaller integration; defines "ship the fuzz
  story".
- **C-09**: snapshot/forkserver; the fuzz profile's
  differentiator.

## What's NOT on the critical path (parallelizable)

- A-03 (seccomp wrap), A-05 (conformance), A-06 (docs), A-07
  (perf CI) — start after A-01.
- B-01..B-03, B-05, B-06 — after A-04.
- C-02 (KFENCE), C-03 (KCSAN), C-06 (BPF JIT), C-07 (KMSAN),
  C-10 (crosvm launcher) — after C-01.
- All of D (KVM backend) — after A only; D's bookend isn't on
  critical path because prod-fast falls back to seccomp without
  KVM.

## Critical path acceleration

If we wanted to compress the 7-month floor:

- **A-02 takes 6 wk**: hardest to compress. It's the refactor.
  Could split among 2 engineers per-op, saving ~3 wk. Carries
  merge-conflict risk.
- **C-09 takes 6 wk**: snapshot/forkserver is the second-
  longest. Could descope to forkserver-only (ship later
  snapshot/restore as v1.1).

If both compressed: 7 mo → 5 mo wall-clock for fuzz. Beyond
that, the critical path is A-01 → A-04 → C-01 → B-04 = 11
weeks ≈ 3 months. That's the absolute floor.

## What blocks D (KVM backend)

Only A-01..A-02. D is wholly parallel to B and C after that.
D-06 (bookend benchmark) is 6 months wall-clock from D start.
If D starts at month 13 (post-Q4), bookend lands at month 19.

D failure is acceptable: prod-fast on seccomp is the realistic
default. KVM is a perf upgrade.

## Bus factor on critical path

If only one engineer can do A-02 (the refactor): they're a bus-
factor risk. Mitigation: pair another engineer on A-02 from week
3 onward, so they can take over.

If only one engineer can do D: D delays. Acceptable since D
isn't on critical path.

If only one engineer can do C-08 (syzkaller backend) or C-09
(snapshot/forkserver): the fuzz milestone delays. These are
specialized roles; recruit appropriately.
