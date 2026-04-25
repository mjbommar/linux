# Post-Q1 push: sequencing the 10 remaining unknowns

**Date:** 2026-04-23
**Status:** plan (pre-code)
**Author:** claude-code session
**Companion to:** `critical-path.md` (structural blockers),
    `milestones.md` (M1–M12 scoring),
    `04-risks/decisions-log.md` (a D60 entry records this
    sequencing decision and points here)

## Why this file exists

At the 2026-04-23 checkpoint A/B/C-workstream critical-path
work is landed and the six-finding review is closed. The
remaining vision-moving work is captured as ten "big
unknowns / complex engineering lifts" (see §"The ten lifts"
below). Executing 1→10 in the order they were listed is
suboptimal because (a) the lifts have real dependency
structure, (b) several are *unknown-retiring* research
rather than implementation, and (c) one of them (A-Phase-2
extraction) unblocks cleaner execution of the biggest one
(real KVM `run_userspace`). This file records the
dependency-driven execution order so future sessions don't
re-argue it.

## The ten lifts (one-line recap)

In the order they were originally surfaced — NOT execution
order. Execution order is §"Phasing" below.

| # | Lift | Class | Vision-moving weight |
|---|------|-------|----------------------|
| 1 | Real KVM `run_userspace` (D-05/D-06) | implementation | highest |
| 2 | M11 <100 ns syscall / systrap gadget | research spike | high |
| 3 | KMSAN-on-UML redesign (D58) | implementation | medium |
| 4 | A-workstream Phase 2 extraction (D59) | refactor | medium |
| 5 | Snapshot/forkserver real exit-status (Finding #1 v1 ceiling) | forensic + redesign | medium |
| 6 | C-08 Go driver in syzkaller | off-tree | high (gates M7) |
| 7 | LKML upstream acceptance | organizational | existential |
| 8 | M6 syzbot-CVE repro | evidence | high-leverage low-cost |
| 9 | ARM64 / RISC-V host | port | deferred |
| 10 | Context Analysis (clang-22+) | toolchain | deferred |

## Phasing

Execute in phases. Within a phase, items can run in
parallel when independent.

### Phase I — retire cheap uncertainty (≈3 sessions)

**Goal:** convert dormant milestones to landed artifacts
and characterize hazards before we commit to fixing them.

- **Lift #8 — M6 syzbot-CVE repro.** Pick one recent
  syzbot CVE, write a `tools/testing/selftests/um/
  cve-repro/<id>/` harness that boots the research
  profile, plants the trigger, captures the KASAN
  (or equivalent) report, asserts <30 s wall-clock.
  Deliverable: a green selftest + a one-paragraph
  "what the research profile now demonstrates" note
  in `05-validation/benchmarks.md`. Converts M6 from
  budgeted-month-12 to landed; exercises the full
  research stack end-to-end (kprobes + ftrace +
  function_graph + KASAN + KFENCE) and catches any
  regressions across the stack in one shot.
- **Lift #5 — forensic memo only.** NOT a fix. Write a
  scoped selftest that reliably reproduces the SIGALRM
  → `switch_threads()` → stale `jmp_buf` crash the
  Finding #1 attempts hit, plus a root-cause document
  in `04-risks/` (candidate name:
  `signal-reentry-in-fork-window.md`) that describes
  the hazard class, the four failed wait4 variants, and
  the shape a real fix would take. Converts "mystery
  crash" into "characterized hazard" and is a
  prerequisite to any future redesign of the
  snapshot/forkserver exit-status contract.
- **Lift #10 — Context Analysis.** One-line blocker
  note in `scripts/` README and `a-plus-quality-plan.md`
  pointing at the toolchain dependency. No engineering.

### Phase II — A-workstream Phase 2 extraction (Lift #4)

**Goal:** finish the A-01.7 REMOVED disposition for the 12
`using_seccomp` branches in shared os-Linux/ code so the
KVM backend doesn't inherit the Layer-1 leak when it gets
its real `run_userspace` body. Four sub-lifts in risk
order, lowest first:

- **4a. SIGCHLD registration split.** Sites: `signal.c:252`
  (mask SIGCHLD in other handlers) and `process.c:401`
  (register SIGCHLD reaper). Add a per-backend signal-setup
  op (or a capability flag) and route both sites through
  it. Lowest reentry risk because both sites run once at
  init and have no hot-path interaction.
- **4b. `syscall_stub_dump_error` per-backend split.**
  Sites: `skas/mem.c:47` (FD-map printout). Move the
  seccomp-specific dump into
  `arch/um/backend/seccomp/debug.c`; ptrace variant just
  prints the common fields. Debug path — not on the hot
  path.
- **4c. `do_syscall_stub` + `get_stub_fd` +
  `um_stub_mm_map` FD-map split.** Sites: `skas/mem.c:96,
  131, 201, 246`. Move into per-backend
  `arch/um/backend/{ptrace,seccomp}/stub_syscall.c`. Hot
  path — boot matrix required per commit.
- **4d. `start_userspace` clone/wait split.** Sites:
  `skas/process.c:277, 282, 471, 484, 522`. Splits
  `start_userspace()` into `seccomp_mm_attach()` +
  `ptrace_mm_attach()` private helpers. Highest reentry
  risk (touches the fork/wait parent-side signal mask
  interactions that Finding #1 flagged). Do last, after 4a
  demonstrates the test pattern.

Each sub-lift is its own commit, Q1-gated, with a
decisions-log sub-entry if visible behavior changes.
Lift #4 also removes the `extern int using_seccomp;` leak
in the public `skas/skas.h` (A-01.7 item #3) once all
sites are behind per-backend ops.

### Phase III — D-workstream real `run_userspace` (Lift #1)

**Goal:** replace the D-04 `HLT`-on-harness path with a
real ring-3 entry + signal delivery + page-fault handling
+ syscall dispatch loop. Architecturally unblocked
post-Phase II. Decompose into five sub-lifts:

- **1a. Per-mm `kvm_um` attach-cost microbench.**
  **RETIRED by supersession (2026-04-23 — see D60).**
  The framing assumed one KVM VM fd per UML mm; D57
  (one VM fd per UML *process*) pre-empted that by
  making `kvm_mm_attach()` a pure `refcount_inc()` on
  a shared `struct kvm_um`. No KVM ioctls on the
  per-mm attach path, so there is nothing meaningful
  to benchmark against the seccomp `start_userspace`
  baseline. See decisions-log D60 for the full
  reasoning; Phase III execution proceeds directly
  to Lift #1b.
- **1b. Real ring-3 entry + `SYSRETQ`.** Swap the
  dual-memslot harness's `HLT`-on-ring-0 for a real
  `SYSRETQ` into ring 3 with a stub-executable guest
  page. Expect first observable: KVM_EXIT_IO or
  KVM_EXIT_INTERNAL_ERROR on the first SYSCALL out.
- **1c. Syscall dispatch real.** Route the D-04c LSTAR
  trampoline to the kernel's real syscall table, not the
  harness's `getpid`-emulator. Requires stable GDT/IDT
  from 1b.
- **1d. Page-fault handling.** KVM_EXIT_MMIO /
  `KVM_EXIT_SHUTDOWN` decode → UML's existing fault
  path (`arch/um/kernel/trap.c`). This is where UML's
  single-`mm_struct` model starts actually helping — the
  fault address is already in UML's kernel VA, no
  translation needed.
- **1e. Signal delivery.** The harness never delivered
  signals. Wire SIGALRM + SIGIO + SIGUSR1 through
  KVM_INTERRUPT / in-guest IDT. Last because it
  interacts with the time-travel + IPI machinery that
  D-05a/b already landed.
- **1f. Conformance (D-06).** Run `A-05` contract
  KUnit suite against the KVM backend. Must produce
  identical behavior to seccomp on the shared test
  vectors.

This is ~4 weeks wall-clock of focused work, much of
which reuses the existing harness scaffolding.

### Phase IV — M11 systrap gadget feasibility spike (Lift #2)

**Goal:** decide whether <100 ns syscall is achievable
under UML's single-`mm_struct` + one-stub-page model.
Spike, not implementation. Sub-lifts:

- **2a. Gadget-page address space design.** Can a fixed
  guest-VA gadget page live alongside UML's existing
  stub page without colliding with kernel VA or guest
  userspace VA? Paper design; spike harness builds on
  D-04b.2b.
- **2b. Round-trip cost measurement.** Bare spike:
  userspace `syscall` → gadget page → shadow-stack
  update → back to userspace with a spoofed return
  address. Target: measure the actual minimum on the
  current hardware. Compare against the 100 ns
  aspirational target.
- **2c. Go/no-go memo.** Either (a) "gadget lands us
  under N µs, worth building", (b) "gadget hits X µs
  floor on current silicon, not worth it", or (c) "the
  address-space model blocks the gadget pattern; needs
  rework". Entry in decisions-log. No implementation
  decision committed to by this spike.

Gated on Phase III having a real working backend to
compare against.

### Phase V — KMSAN redesign (Lift #3)

**Goal:** unbreak C-07 KMSAN. Needs a workload to
benchmark against (so M6 / Phase I #8 is the
prerequisite). Sub-lifts:

- **3a. VMALLOC quarter-split probe branch.** Implement
  path (a) from D58 in an experimental branch. Boot-test
  research profile. Measure VA-pressure + RSS on a real
  research workload.
- **3b. `task_size` cap probe branch.** Implement path
  (b) similarly. Measure same.
- **3c. Pick + land.** Whichever has lower TCO under
  real workloads. Unbreak the Kconfig gate. Decisions-log
  entry supersedes D58.

### Phase VI — organizational (Lifts #6 + #7)

Parallel with Phase III+. Not gated on Phase II because
the syzkaller driver only cares about the kernel-side
DYNAMIC backend (stable for weeks).

- **Lift #6 — C-08 Go driver.** Off-tree in google/
  syzkaller. Sub-tasks:
  - 6a. Fork syzkaller; add `pkg/vm/uml/uml.go`
    mirroring `pkg/vm/gvisor/gvisor.go`.
  - 6b. Wire the C-09 forkserver fds 198/199 via the
    C-10 `--forkserver=` argument.
  - 6c. Submit PR on google/syzkaller, referencing
    issue #1288.
  - 6d. Land.
- **Lift #7 — LKML upstreaming.** Sub-tasks, in
  submission order:
  - 7a. `kmsan-arch-callback-rfc` (cover letter
    already staged under `upstream-patches/`). Send.
  - 7b. `bpf-hygiene-v1` (two commits already staged).
    Send to BPF + netdev lists.
  - 7c. `notrace` + `-fpatchable-function-entry` fix
    (the C-04 commit 3a strip infrastructure). Package
    as a standalone series for tracing maintainers.
  - 7d. A-workstream backend-ops RFC. Dependent on
    Phase II honesty (the leaked `using_seccomp`
    extern is a reviewer-visible smell).
  - 7e. B-workstream static-key series.
  - 7f. C-series (kprobes, ftrace, KFENCE, KCSAN).
  - 7g. D-workstream KVM backend, once Phase III
    lands.

Each 7x is a 3–6 month wall-clock review cycle.

### Phase VII — deferred (Lifts #9 + #10)

- **Lift #9 — ARM64 / RISC-V host.** Post v1. No
  actionable work now; a note in the 24-month plan is
  the only deliverable.
- **Lift #10 — Context Analysis.** Blocked on clang-22+
  host toolchain. Already noted in Phase I §10.

## Dependency graph

```
Phase I (cheap retirement)
├── #8 M6 CVE repro ─────────────┐
├── #5 forensic memo              │
└── #10 toolchain blocker note    │
                                  │
Phase II (extraction) ──> Phase III (KVM real)
├── 4a SIGCHLD split               │   ├── ~~1a attach-cost bench~~ (retired, D60)
├── 4b dump_error split            │   ├── 1b ring-3 entry
├── 4c stub-syscall split          │   ├── 1c syscall dispatch
└── 4d start_userspace split       │   ├── 1d page-fault
       │                           │   ├── 1e signal delivery
       │                           │   └── 1f D-06 conformance
       │                           │
       └─> Phase IV (spike) ◀──────┘
             └── 2a/2b/2c gadget feasibility
                     │
                     ▼
Phase V (KMSAN redesign) ◀── needs Phase I #8 for workload
├── 3a VMALLOC probe
├── 3b task_size cap probe
└── 3c pick + land

Phase VI (organizational, parallel with III+)
├── #6 C-08 Go driver (off-tree)
└── #7 LKML series (10-month tail)

Phase VII (deferred)
├── #9 ARM64/RISC-V
└── #10 clang-22+
```

## Sequencing rationale

**Why Phase II before Phase III, not after:**
D-workstream adds a third `um_backend_ops` implementation.
If Phase III lands real KVM `run_userspace` while the
12 `using_seccomp` branches are still in shared code, the
KVM backend either (a) inherits the leak (Layer-1
abstraction stays broken), or (b) paper-wraps around it
with `um_backend->kind == UM_BACKEND_KIND_KVM` checks in
the same shared files. Neither is acceptable. Doing
Phase II first costs ~4 commits of delay on Phase III
and keeps the ops-table honesty for all three backends.
*Tradeoff:* Phase III wall-clock pushes ~2 weeks. Accepted.

**Why Lift #8 before Lift #4:**
Lift #8 is a ~1-session deliverable that produces a
durable artifact and exercises every landed C-workstream
feature end-to-end. Any regression in that stack shows
up immediately. Lift #4 is multi-commit structural work;
starting it when M6 is still untested means any boot
regression during 4a–4d could be mistaken for a 4x bug
when it's a latent C-series issue. Doing #8 first
calibrates the baseline.

**Why Lift #5 before Phase II:**
Phase II Lift #4d (`start_userspace` split) touches the
exact signal-mask / fork parent-side interaction that
Finding #1 proved was crash-prone. Having a
characterized-hazard memo in `04-risks/` means the 4d
author knows what they're avoiding before they edit.

**Why Lift #7 (upstream) runs in parallel not after:**
LKML cycles are 3–6 months each. Sequential
upstreaming after all technical work is done means
v1-on-mainline lands ~18 months after v1-on-fork. Running
7a in parallel with Phase III buys back that delay, and
the items in 7a–7c are already self-contained patches
that don't require Phase II honesty to land.

**Why Phase IV (systrap) after Phase III:**
Phase IV measures the gadget against a working backend.
Before a real `run_userspace` exists, the spike can only
measure against the harness, which doesn't include the
real kernel entry overhead. Measuring against the harness
would produce an optimistic floor that misleads the
go/no-go memo. Wait for Phase III.

**Why Phase V (KMSAN) gated on Lift #8:**
The two KMSAN redesign paths have different TCO
signatures. Path (a) takes 1/4 of VMALLOC; path (b) caps
`task_size`. Neither cost is meaningful without a real
research-profile workload to measure against. Lift #8 is
exactly that workload.

## Open sequencing questions (flagged to owner)

Four calls the author should confirm or change:

1. **Phase II before Phase III.** Alternative: do D first
   and eat the `using_seccomp` leak into the KVM backend
   for now (defer Phase II until all three backends are
   real). Author's answer:
2. **Lift #8 as Phase I opener.** Alternative: start with
   Lift #4a (SIGCHLD split) since it's also small and
   more architecturally load-bearing. Author's answer:
3. **Phase V waits for Phase III.** Alternative: run
   Phase V in parallel with Phase III (independent
   workstreams). Author's answer:
4. **Lift #7 (LKML) starts in parallel with Phase III.**
   Alternative: hold all upstreaming until Phase II is
   green, to avoid an "RFC for backend abstraction with
   a still-leaky Layer 1" review. Author's answer:

Once resolved, record the answers in a D60 entry and
proceed.

## Estimated wall-clock

Assuming one AI-assisted session ≈ 1 day of engineering:

| Phase | Optimistic | Realistic |
|-------|------------|-----------|
| Phase I (cheap retirement) | 3 sessions | 5 sessions |
| Phase II (A-Phase-2) | 4 sessions | 8 sessions |
| Phase III (KVM real) | 5 sessions | 10 sessions |
| Phase IV (systrap spike) | 2 sessions | 4 sessions |
| Phase V (KMSAN) | 3 sessions | 6 sessions |
| Phase VI-6 (C-08 off-tree) | 3 sessions | 6 sessions |
| Phase VI-7 (LKML) | parallel, 10 mo wall-clock | parallel, 18 mo wall-clock |

Technical work completion (Phases I–V) realistic: **~40
sessions ≈ 8 weeks of focused AI-assisted engineering**.
LKML tail (Phase VI-7) runs in parallel and is the gating
item on v1-on-mainline — aligned with the 00-vision.md
"honest worry" that organizational velocity, not
engineering velocity, is the constraint.

## What lands after this plan executes

- **M6 (research profile CVE repro):** landed.
- **M7 (syzkaller vm/uml):** landed (off-tree).
- **M8 (fuzz <50 ms restart):** measured.
- **M9 (KVM round-trip):** landed real (not just
  harness).
- **M11 (KVM bookend <100 ns):** either landed via
  systrap gadget, or explicitly supersed'd by measured
  "1–11 µs is the floor" from Phase IV decisions-log
  memo.
- **M12 (all profiles ship):** all nine profiles build,
  boot, pass smoke + Q1. v1 release-ready.

Plus: the 12 `using_seccomp` branches are gone, KMSAN is
unbroken, the snapshot v1 ceiling is characterized,
three or more LKML series are in flight, and the
research-profile CVE repro is a regression harness.

### Post-plan addendum (2026-04-25, Phase 1 closure)

The 2026-04-25 session closed the residual Phase 1 work that
the post-Q1 plan had flagged as "deferred for focused sessions":

- **dyn-loader green** on kvm row (task #270 + #272). The
  ld-linux RELR loop's RCX clobber via SYSRETQ-based bootstrap
  re-entry was the load-bearing latent bug; the IRETQ switch
  plus the host-CPUID passthrough (#273) clear it.
- **KVM/seccomp ratio at 1.15×** — closed at memo-12-vision
  parity-target via #238 STEP-2 + #242 + experiment #1, far
  below the original 1.2× exit goal. perf-fallback /
  perf-getpid both at 1.15-1.17×, validated 5/5 deterministic.
- **Audit round 7 closed** (task #248) — IDT[13] #GP handler
  + mm-pointer cache key.
- **Memo 12 / 13 / 14 written** — design specs for the Phase-3
  v2 forkserver, record/replay determinism, and syzkaller
  vm/uml backend respectively. Memo 12 has steps 1+2+bench
  shipped (kvm_snapshot_capture / restore_full / KUnit /
  selftest / cycle bench).

Three Phase-1 levers remain explicitly deferred (low-priority
since the perf goal is already met):

- **#243** per-mm cached shadow PGD — would shave another
  ~5% on multi-mm workloads; not blocking anything in tree.
- **#244** huge-page shadow PT — re-analysis showed the
  lever doesn't apply as framed; demoted, won't ship.
- Phase-5 (LKML series posting) — explicitly parked per
  user directive until "100% of all other tasks are
  completed."

See `04-risks/decisions-log.md` D100 for the full closure
record + commit references.

## Not in this plan

Explicitly deferred, logged here so they don't surface
as "forgotten":

- ARM64 / RISC-V host (Lift #9) — post-v1.
- Context Analysis / clang-22+ (Lift #10) — toolchain-
  gated.
- `time-travel` profile thickening (currently the
  thinnest of the nine shipped profiles).
- Future-phases parking lot items (05 umlctl, 06 MCP,
  07 bisect, 08 probe-web, 09 diff, 10 record-replay,
  11 API daemon, 12 Perfetto). These open only after
  v1 ships.

## Cross-references

- `00-vision.md` — success criteria this plan targets.
- `06-sequencing/critical-path.md` — structural
  blockers this plan respects.
- `06-sequencing/milestones.md` — M1–M12 scoring.
- `04-risks/decisions-log.md` D58 (KMSAN broken),
  D59 (`using_seccomp` residue), D45 (fork-first
  upstream policy), D34 addendum-4 (fgraph landing
  fix) — all referenced by phases above.
- `AGENT-PROMPT.md` — per-task workflow (research →
  design → code → test → document → commit) applies to
  every sub-lift in every phase.
