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
| Thinking beyond A-D | [08-future-phases/](08-future-phases/) |

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
│   ├── a-plus-quality-plan.md        all-tools quality/safety/perf playbook
│   └── upstream-strategy.md          how patches land in mainline without a 7-year RFC chain
│
├── 06-sequencing/                    when and how
│   ├── README.md
│   ├── 24-month-plan.md              quarter-by-quarter
│   ├── milestones.md                 verifiable checkpoints
│   ├── critical-path.md              what blocks what
│   └── parallelism-map.md            what can run concurrently
│
├── 07-references/
│   ├── README.md
│   ├── prior-art.md                  gVisor, LKL, Firecracker, crosvm — what we steal
│   └── sources.md                    URLs from the original research agents
│
└── 08-future-phases/                parking lot + tooling memos
    ├── README.md                     index + dependency graph
    ├── 01-end-user-ideal-world.md    ideal end-user UX and roadmap gaps
    ├── 02-snapshot-to-disk.md        v2 ELF64-core format beyond C-09
    ├── 03-uml-fuzz-rust-companion.md Rust in-tree fuzzer (post C-08)
    ├── 04-uml-launcher-tui.md        ratatui dashboard
    ├── 05-umlctl.md                  multi-instance lifecycle CLI (LANDED v1)
    ├── 06-uml-mcp.md                 MCP server for AI-agent consumers
    ├── 07-uml-bisect.md              bisect wrapper around boot-matrix
    ├── 08-uml-probe-web.md           browser UI over tracefs/debugfs
    ├── 09-uml-diff.md                semantic diff between two UML boots
    ├── 10-uml-record-replay.md       deterministic capture + replay
    ├── 11-uml-api-daemon.md          rootless REST API surface
    ├── 12-uml-perfetto-trace.md      tracefs → Perfetto converter
    └── 13-uml-observability-spine.md run_id + schema + bundle format
                                      (O1 ~83% LANDED 2026-04-23)
```

## Status of this plan

- [x] Architecture layers committed (`01-architecture/`)
- [x] Workstream decomposition (`02-workstreams/`)
- [x] Profile matrix (`03-profiles/`)
- [x] First-pass sequencing (`06-sequencing/`) + post-Q1 push
      execution plan (`06-sequencing/post-q1-push.md`, Phases
      I-VI complete as of 2026-04-23)
- [x] Decisions log open and ready to grow (`04-risks/decisions-log.md`
      now runs from D1 through D63)
- [ ] Open question resolution (per-workstream; mostly closed via
      decisions-log entries; remaining open Qs flagged in each
      workstream doc)
- [ ] Upstream strategy validation (ordered queue at
      `upstream-patches/SUBMISSION-QUEUE.md` per D63;
      bpf-hygiene-v1 + kmsan-arch-callback-rfc ready for
      `git send-email`, remaining five series staged to write)

## Landed in tree (as of 2026-04-23)

This plan has outgrown "paper design" — much of it is now code
on `uml-redesign-plan` branch, tracked in the per-workstream
status headers but rolled up here:

**Workstream A (backend ops):** complete. All 7 sub-tasks landed
2026-04-17/18. `struct um_backend_ops` + ptrace + seccomp + KVM
stub + Kconfig + KUnit contract + perf-CI all in tree.
**Phase 2 extraction** (Phase II of the post-Q1 push, Lifts
#4a-#4d+ landed 2026-04-23) moved the last Layer-1 residues —
`start_userspace` clone/wait split, `do_syscall_stub` +
`syscall_stub_dump_error` per-backend splits, `SIGCHLD`
registration behind a capability flag, removal of
`extern int using_seccomp` from public headers. Task #144
carries the remaining residuals.

**Workstream B (static-key hot paths):** complete. All 6
sub-tasks landed 2026-04-18. Hot paths gated, debugfs controls
live (`/sys/kernel/debug/um/{backend,hooks/*,stats}`), first
runtime flip demonstrated via selftest.

**Workstream C (profiles + gap-fill):** mostly landed. See
`02-workstreams/C-profiles-and-gaps/README.md` for the cell-
level table. Summary:

  - C-01 defconfig: landed 2026-04-18
  - C-02 KFENCE: landed 2026-04-18
  - C-03 KCSAN: landed 2026-04-18
  - C-04 kprobes: partially landed 2026-04-20 (commits 1+2
    + commit 3a strip infrastructure 2026-04-21; commit 3b —
    the actual HAVE_FUNCTION_GRAPH_TRACER flip + trampolines —
    deferred one session pending an atomic-context fix in
    prepare_ftrace_return, see D34 addendum-3)
  - C-05 ftrace: landed 2026-04-19
  - C-06 BPF JIT: landed v1 2026-04-21 (option A per D43
    fifth-view — UML consumes arch/x86/net/bpf_jit_comp.c with
    UML-local shim headers + reg2pt_regs[] CONFIG_UM branch;
    `uml/research` profile now enables CONFIG_BPF_SYSCALL +
    CONFIG_BPF_JIT out of the box). Option B2 portable-emitter
    refactor tracked separately as task #68. Two upstream-bound
    hygiene commits (e2b686c962, 5b95b1bb3e) remain staged in
    `upstream-patches/bpf-hygiene-v1/`.
  - C-07 KMSAN: redesigned 2026-04-23 per Phase V of the
    post-Q1 push. D62 adopts a VMALLOC quarter-split
    shadow/origin layout that works with stock KMSAN, unblocking
    `HAVE_ARCH_KMSAN=y` in tree without waiting for upstream.
    D58's dedicated-slab design superseded.
    `kmsan-arch-callback-rfc/` remains staged upstream as the
    "no visible impact, just benefits" offering, but UML no
    longer blocks on it.
  - C-08 syzkaller `vm/uml` backend: external to linux.git;
    Phase VI Lift #6 (2026-04-23) pinned the kernel-side
    interface fingerprint (`/sys/kernel/um/state_version`,
    forkserver wire protocol, launcher `--forkserver` CLI,
    zombie-drain contract) so the off-tree Go driver has a
    stable surface to bind to.
  - C-09 snapshot/forkserver: landed v1 2026-04-20. Finding #1
    forensic memo (Phase I Lift #5) documents the
    SIGALRM→switch_threads→stale jmp_buf hazard that caps v1
    at status=0; v2 exit-status reporting tracked as future
    work, not Q1.
  - C-10 crosvm-style launcher: **v2 landed 2026-04-20**
    (vhost-user per-device + seccomp + AppArmor). v2's
    [[bin]] crate at `tools/uml/uml-launcher/` now hosts the
    `umlctl` lifecycle CLI + observability-spine v1 — see
    "Tooling (umlctl + observability spine)" below.

**Workstream D (KVM backend):** **spikes + harness complete as
of 2026-04-23.** Phase III of the post-Q1 push landed D-01
through D-06 as a working in-kernel harness: per-mm
`kvm_um` with `mm_attach` / `mm_detach` (D-03b), real memslot
wiring + `mm_map`/`mm_unmap` (D-03c, D-03d), long-mode SREGS
+ CR3/GDT/IDT/EFER (D-04b), LSTAR-routed SYSCALL dispatch
through real `sys_call_table` (D-04c / Phase III Lift #1c),
KVM_EXIT_MMIO decode into UML's fault path (Lift #1d),
KVM_INTERRUPT + IDT delivery (Lift #1e), and the A-05
contract KUnit suite passing 20/20 against
`CONFIG_UM_BACKEND_KVM_ONLY` (Lift #1f). Real
`run_userspace` integration — replacing the one-shot harness
with a sustained trap loop — remains as task #162 for a
follow-on session. D failure remains explicitly acceptable:
prod-fast still falls back to seccomp.

**Tooling (umlctl + observability spine, 2026-04-23):**

  - `tools/uml/uml-launcher/src/bin/umlctl/` — podman-shaped
    multi-instance lifecycle CLI. Verbs: `create` / `start` /
    `stop` / `rm` / `ps` / `logs` / `schema` / `events` /
    `assert` / `export`. Spec: `08-future-phases/05-umlctl.md`.
  - `08-future-phases/13-uml-observability-spine.md` — the
    unifying telemetry architecture. Phase O1 is ~83% landed
    (sub-lifts O1.1 + O1.3 + O1.4 + O1.5 + O1.6): run_id
    (ULID) + bundle directory, schema registry + structured
    `events.jsonl`, `.umlbundle.tar.zst` export, `events` tail
    verb, `assert` CI-predicate verb. O1.2 (split kernel
    console + `dmesg` verb) remains; needs real UML
    cooperation testing. O2-O6 parking-lot.
  - Selftest: `tools/testing/selftests/um/umlctl-smoke/`
    regression-locks the no-orphan guarantee (the motivation)
    plus the full lifecycle + spine event + assert + export
    round-trip.

**Post-Q1 push (`06-sequencing/post-q1-push.md`):** all six
phases landed 2026-04-23.

  - Phase I (quality gates): KFENCE + KCSAN profiles (C-02,
    C-03), CVE-repro selftest, ftrace.rst landing, clang-22+
    blocker documented, forensic memo D59.
  - Phase II (A-workstream Phase 2 extraction): see Workstream
    A paragraph above.
  - Phase III (KVM backend spikes through D-06): see
    Workstream D paragraph above.
  - Phase IV (systrap gadget feasibility): D61 decides
    go-for-a-post-v1 gadget workstream; no code change this
    phase.
  - Phase V (KMSAN redesign): D62 supersedes D58; VMALLOC
    quarter-split landed.
  - Phase VI (organizational): C-08 interface fingerprint
    (Lift #6) + LKML submission queue (Lift #7) + D63.

**Selftests under `tools/testing/selftests/um/`:**
ftrace-smoke, hooks-flip, kprobes-stress, launcher-smoke
(Parts A/B/C), profiles, snapshot-smoke, userspace-smoke.
Direct-invocation regression coverage for every landed
sub-system. As of 2026-04-21 all seven pass against the
current HEAD on the research profile; snapshot-smoke
additionally passes on fuzz.

**Review-fix series (continuing D46 discipline):** a fourth
external review on 2026-04-21 caught five items the prior
sessions' validation gates had missed. All fixed and landed in
commits `e27b853264cc..257b8cf61b84` per D47 — modpost
section mismatches in `init_backend()`, a Q1 warning-grep case
bug that had masked those same mismatches (now fixed
gate-level), the `backend=auto` policy drift between
prod-fast.config and the actual probe gate (superseding D15),
the `backend->probe()`/`init()` lifecycle ops that
`init_backend()` never dispatched, and a non-blocking zombie
drain in the snapshot forkserver so worker exits don't leak
zombies across iterations. Q1 baseline + boot matrix + full
selftest sweep all green after the series.

**Upstream patches staged for LKML:** ordered queue at
`upstream-patches/SUBMISSION-QUEUE.md` (Phase VI Lift #7,
2026-04-23, D63):

  1. `upstream-patches/bpf-hygiene-v1/` — two arch-generic
     fixes in `arch/x86/net/bpf_jit_comp.c` (cpufeature.h
     include + instruction_pointer helpers), cover letter +
     notes ready for `git send-email` to BPF + netdev lists.
  2. `upstream-patches/kmsan-arch-callback-rfc/` — RFC
     framing for the map-on-demand arch extension point
     (notes only; UML-side no longer blocks on it per D62).
  3-7. Five further series staged to write, ordered for
     reviewer-credibility + dependency cleanliness. See the
     queue doc for routing + pitches per series.

**Cross-subsystem work carried on the fork (per D45):**

  D45 (2026-04-21) clarifies this plan's posture: the fork
  demonstrates the working end state first; LKML adoption
  follows, motivated by the running artifact. Items previously
  framed as "blocked on upstream review" are in fact just
  "work to do on the fork."

  - C-04 commit 3 HAVE_FUNCTION_GRAPH_TRACER — D34 identifies a
    `notrace` + `-fpatchable-function-entry` interaction.
    Under D45, we write the `kernel/trace/` fix ourselves,
    carry it on the fork, unblock commit 3 locally, defer the
    upstream submission to later.
  - C-06 full port — D43 option B2 is a
    `arch/x86/net/bpf_jit_comp.c` portable-emitter refactor.
    Under D45, land the split on the fork; the two hygiene
    commits (already on-branch as e2b686c962 / 5b95b1bb3e)
    stay as-is in `upstream-patches/bpf-hygiene-v1/` for when
    the upstream conversation opens.
  - C-07 KMSAN — D44 fourth probe calls for a map-on-demand
    arch-extension point in `mm/kmsan/init.c`. Under D45,
    add it on the fork; implement UML's callback; demonstrate
    the full sanitizer trio (KASAN + KCSAN + KMSAN) working
    before engaging KMSAN maintainer upstream.

Each item still has a concrete next-step recommendation in the
linked decisions-log entry; D45 removes the LKML-timing gate.

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
