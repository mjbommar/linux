# Workstreams

Four concurrent workstreams totaling ~24 engineer-months. They
have non-trivial dependencies; the dependency graph determines
critical path.

## The four workstreams

| Workstream | Owner role | Effort | Bookend |
|---|---|---|---|
| [A — Backend abstraction](A-backend-abstraction/) | Architect | 6 EM | Defines the contract every backend implements |
| [B — Static-key hot paths](B-static-key-hot-paths/) | Performance eng | 4 EM | Adds gates without regressing prod-fast |
| [C — Profiles + gap-fill](C-profiles-and-gaps/) | Multiple porters | 8 EM | The widest workstream; many parallel ports |
| [D — KVM backend](D-kvm-backend/) | Hypervisor eng | 6 EM | The third backend; gVisor-style |

## Dependency graph

```
A (backend ops table)  ──────────────► defines contract
       │                              ┌──► A1 ptrace refactor
       │                              ├──► A2 seccomp wrap
       │                              └──► (D depends here)
       ▼
B (static-key gates)   ──────────────► uses A's hooks
       │                              ┌──► B1 audit hot paths
       │                              ├──► B2 add gates
       │                              └──► B3 debugfs controls
       ▼
C (profiles + gaps)    ──────────────► uses B's gates, A's backends
       │                              ┌──► C1..C8 sanitizer/tracer ports
       │                              ├──► C9 syzkaller backend
       │                              └──► C10 host launcher (crosvm)
       ▼
D (KVM backend)        ──────────────► implements A's contract
       │                              parallel to C, depends on A only
       │
       ▼
Final integration: ship 8 defconfigs, validate the matrix.
```

## Critical path

A → B → C is the critical path. D parallels C but depends only
on A.

If you have a small team:

- **2 engineers**: one drives A then B; the other drives C in
  parallel after A's first milestone (the ops table interface
  freeze). D is unscheduled until C is well underway.
- **4 engineers**: one each on A, B, C, D in parallel after
  A delivers the interface freeze (~month 2).
- **6 engineers**: above, plus a syzkaller specialist on C9
  and a Rust VMM specialist on C10 starting month 6.

## Sequencing rules

1. **A's interface freeze gates everything else.** B can't add
   gates calling into the backend until the backend interface is
   stable. C can't write profile defconfigs until backend selection
   exists. D can't implement a backend until the contract is
   defined.

2. **B's first gate is on A's `run_userspace`.** This validates
   that gates compose with backend ops. After this works once,
   the rest are mechanical.

3. **C ports are independent of each other.** KFENCE doesn't
   need KCSAN. Kprobes doesn't need ftrace. Parallelize freely.

4. **D's first deliverable is a syscall round-trip.** Get one
   syscall through KVM backend; everything else extends from
   that.

## Workstream-level milestones

Each workstream has one Big Demo at its midpoint that proves the
abstraction is real:

- **A: Demo at month 2.** ptrace and seccomp backends both pass
  the conformance suite via the ops table. No external visible
  change.
- **B: Demo at month 3.** Toggle KCOV on/off via debugfs while
  running LTP. Coverage data appears or doesn't, kernel keeps
  running.
- **C: Demo at month 6.** Build all 6+ defconfigs from one
  checkout. Run profile-appropriate test for each.
- **D: Demo at month 9.** `getpid()` round-trips through KVM
  backend in <100 ns measured with `rdtsc`.

If a milestone slips by >50% of its budget, raise it to the
sequencing review.

## What can go wrong

**A slips:** everything slips. A is the foundation. If the ops
table design is wrong, the layering doesn't work and the project
restarts.

**B slips:** profiles don't differentiate. We can ship `prod-fast`
and `research` but they look the same because no gates exist.

**C slips:** the architecture works but no users see it because
the profile defconfigs aren't there. Mitigation: ship one or two
profiles partially and grow.

**D slips:** we don't get to ~100 ns syscall on bare metal.
seccomp backend is the realistic floor for prod-fast. Acceptable
fallback; the "10×" promise becomes "5×".

## How tasks are tracked

Each workstream has a `tasks.md` with numbered tasks. Tasks
follow the template in
`/nas4/data/workspace-infosec/kernel-network-simulatory/docs/SUBMISSION-PLAYBOOK.md`-
adjacent style: Goal, Approach, Deliverable, Validation, Open
questions.

Status is tracked in the workstream READMEs as a checklist.
When a task is started, its file is updated with **Status: in
progress**, owner, and start date. When done, **Status:
complete** and the merged commit/PR.

## Cross-workstream coordination

Two synchronization points:

1. **Backend interface freeze** (after A's first 2 months): A
   commits to `struct um_backend_ops`. B and D can begin.
2. **Profile matrix freeze** (after C's first 4 months): C
   commits to which 6+ defconfigs ship. Validation work in
   05-validation/ can be planned against these.

Both freezes are versioned. Breaking changes after freeze require
explicit sign-off from all workstream owners.

## See also

- [04-risks/decisions-log.md](../04-risks/decisions-log.md) — every
  architectural decision and its rationale
- [06-sequencing/24-month-plan.md](../06-sequencing/24-month-plan.md) —
  the calendar version of the dependency graph
- [05-validation/benchmarks.md](../05-validation/benchmarks.md) —
  what each workstream's deliverable must measure
