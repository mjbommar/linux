# UML Redesign — planning mini-project

A green-field architectural plan for `arch/um/` that gives you
**near-native performance, full instrumentation, and time-travel
determinism from a single source tree** — the way the host kernel
already gives you KASAN + ftrace + KVM + KCOV from one tree.

This is a planning project, not a code project. The deliverable is
the plan: scoped enough that a maintainer can pick up any leaf node
and start work, opinionated enough that the architecture survives
contact with reviewers.

## Where to start

| If you are... | Read |
|---|---|
| Skimming for the idea | [00-vision.md](00-vision.md) |
| A maintainer evaluating feasibility | [01-architecture/](01-architecture/) |
| An engineer picking up work | [02-workstreams/](02-workstreams/) |
| A reviewer questioning scope | [04-risks/](04-risks/) |
| A planner sequencing | [06-sequencing/](06-sequencing/) |
| Looking for cited prior art | [07-references/](07-references/) |

## The architecture in one paragraph

Three layers. Layer 1 is a **backend ops table** (`struct
um_backend_ops`) that abstracts the trap mechanism — ptrace,
seccomp, KVM. Layer 2 is **static-key gates** on every hot path —
syscall entry, page fault, context switch — so that tracing,
coverage, time-travel, and record-replay can be flipped on
*without rebuilding* and cost ~1 ns/gate when off. Layer 3 is
**compile-time wraps** for sanitizers (KASAN/KMSAN/KCSAN/KFENCE)
that compose with any backend. Defconfig profiles select
combinations: `prod-fast`, `research`, `fuzz`, `sandbox`, `library`,
`embedded`, `time-travel`.

## Layout

```
docs/uml-redesign/
├── README.md                         this file
├── 00-vision.md                      what we're building, what we're not, why now
│
├── 01-architecture/                  load-bearing design
│   ├── README.md                     overview of layers
│   ├── three-layers.md               the backbone — backend ops × static keys × wraps
│   ├── invariants.md                 properties that hold across every profile
│   ├── conflicts.md                  things that genuinely don't compose, with resolutions
│   └── data-flow.md                  walk a syscall through every layer, every backend
│
├── 02-workstreams/                   four workstreams, ~24 engineer-months total
│   ├── README.md                     dependency graph + critical path
│   ├── A-backend-abstraction/        struct um_backend_ops + ptrace/seccomp impls
│   ├── B-static-key-hot-paths/       gate every hot path, add debugfs controls
│   ├── C-profiles-and-gaps/          defconfigs + port KFENCE/KCSAN/kprobes/ftrace/JIT
│   └── D-kvm-backend/                third backend implementation, gVisor-style
│
├── 03-profiles/                      one spec per shipped profile
│   ├── README.md                     the matrix
│   ├── prod-fast.md                  KVM + zero hooks; the production target
│   ├── prod-with-hooks.md            KVM + hooks compiled in; flippable at runtime
│   ├── research.md                   debugger-friendly + all sanitizers
│   ├── fuzz.md                       KCOV + snapshot/restore + syzkaller
│   ├── fuzz-deep.md                  fuzz + record-replay + KCSAN
│   ├── sandbox.md                    minimal TCB, seccomp-only, no debug surfaces
│   ├── library.md                    LKL-mode liblinux.a, sibling artifact
│   ├── embedded.md                   ptrace-only, no host KVM/seccomp dependency
│   └── time-travel.md                deterministic clock + record-replay
│
├── 04-risks/                         what could derail this
│   ├── README.md
│   ├── political-lkml-acceptance.md  Tazaki's RFC stalled at v8 — why, and how we don't
│   ├── technical-rwx-vs-readonly.md  ftrace patches text, KVM wants RO text
│   ├── perf-regressions.md           how we don't regress prod-fast as we add features
│   ├── ci-resource-cost.md           N profiles × M tests = ?
│   └── decisions-log.md              every architectural choice, why, who
│
├── 05-validation/                    how we know it works
│   ├── README.md
│   ├── benchmarks.md                 syscall cycles, boot time, fuzz iters/sec
│   ├── conformance-tests.md          LTP, kselftest, syzkaller corpus reproduction
│   ├── ci-matrix.md                  profile × backend × arch × kernel-version
│   └── upstream-strategy.md          how patches land in mainline without a 7-year RFC chain
│
├── 06-sequencing/                    when and how
│   ├── README.md
│   ├── 24-month-plan.md              quarter-by-quarter
│   ├── milestones.md                 verifiable checkpoints
│   ├── critical-path.md              what blocks what
│   └── parallelism-map.md            what can run concurrently
│
└── 07-references/
    ├── README.md
    ├── prior-art.md                  gVisor, LKL, Firecracker, crosvm — what we steal
    └── sources.md                    URLs from the original research agents
```

## Status of this plan

- [x] Architecture layers committed (`01-architecture/`)
- [x] Workstream decomposition (`02-workstreams/`)
- [x] Profile matrix (`03-profiles/`)
- [x] First-pass sequencing (`06-sequencing/`)
- [ ] Decisions log open and ready to grow
- [ ] Open question resolution (see each workstream)
- [ ] Upstream strategy validation (talk to maintainers)

## How to extend this plan

When new information arrives:

1. **A new constraint or fact** → log in `04-risks/decisions-log.md`
   with date, source, impact.
2. **A workstream task gets refined** → expand the relevant
   workstream's `tasks.md`. Don't edit the workstream README
   unless the design itself changes.
3. **A profile changes** → edit `03-profiles/<profile>.md` and
   verify the matrix in `03-profiles/README.md` still holds.
4. **An assumption breaks** → revisit `01-architecture/invariants.md`
   first, then propagate.

## How to use this plan

Engineers picking up work should:

1. Read `00-vision.md` and `01-architecture/three-layers.md`.
2. Pick a workstream from `02-workstreams/`.
3. Read its README, then its `tasks.md`.
4. Pick the lowest-numbered unstarted task.
5. Write the design doc for that task before writing code.
6. Add findings to the decisions log as you go.

Reviewers (LKML, internal, etc.) should:

1. Read `00-vision.md`.
2. Skim `01-architecture/conflicts.md` and `04-risks/`.
3. Look at the relevant workstream's task breakdown.
4. Send feedback to the decisions log, not as freeform mail.

## Original research input

Five parallel Opus research agents (web search, ~2 min each) on
2026-04-17 produced ground truth on UML, gVisor, LKL, Firecracker,
Cloud Hypervisor, Kata, Unikraft, MirageOS, seL4, Bao, Nabla,
WASI, BULKHEAD, WSL1/WSL2, KCOV/KASAN/KMSAN/KCSAN/KFENCE/eBPF.

Reviewer #2 then critiqued the v1 synthesis (`../UML-REDESIGN.md`)
and prompted the multi-profile reframing that this plan adopts.

A second design conversation with the project owner pushed the
plan beyond reviewer #2's scope into the static-key + layered
abstraction architecture documented here.

Sources are in `07-references/sources.md`. The v1 synthesis is
preserved at `../UML-REDESIGN.md` for historical context.
