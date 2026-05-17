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
    ├── 13-uml-observability-spine.md run_id + schema + bundle format
    ├── 14-uml-vector-driver-v2.md    safe high-performance vector net rewrite
    ├── 15-uml-vector-driver-v2-buildout.md
                                        concrete runtime-driver buildout plan
    ├── 16-uml-vector-legacy-tap-crash-r0.md
                                        legacy TAP crash R0 fix note
    ├── 17-uml-vector-driver-v2-r1-runtime-skeleton.md
                                        vector v2 R1 skeleton evidence
    ├── 18-uml-vector-driver-v2-r2-netdev-skeleton.md
                                        vector v2 R2 netdev evidence
    └── 19-uml-vector-driver-v2-r3-fd-backend.md
                                        vector v2 R3 fd backend evidence
```

## Live status

For the up-to-date "what works, what doesn't, what's blocked, and
what's next" view, see [`STATUS.md`](STATUS.md). That file is the
single source of truth — this README and the deeper memos may lag
when priorities shift.

## Status of this plan

- [x] Architecture layers committed (`01-architecture/`)
- [x] Workstream decomposition (`02-workstreams/`)
- [x] Profile matrix (`03-profiles/`)
- [x] First-pass sequencing (`06-sequencing/`) + post-Q1 push
      execution plan (`06-sequencing/post-q1-push.md`, Phases
      I-VI complete as of 2026-04-23)
- [x] Decisions log open and ready to grow (`04-risks/decisions-log.md`
      now runs from D1 through D94)
- [ ] Open question resolution (per-workstream; mostly closed via
      decisions-log entries; remaining open Qs flagged in each
      workstream doc)
- [ ] Upstream strategy validation (ordered queue at
      `upstream-patches/SUBMISSION-QUEUE.md` per D63;
      bpf-hygiene-v1 + kmsan-arch-callback-rfc ready for
      `git send-email`, remaining five series staged to write)

## Landed in tree (as of 2026-04-25)

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
  - C-04 kprobes + function_graph: **fully landed.** Commits
    1+2 (2026-04-20), commit 3a strip infrastructure
    (2026-04-21), commit 3b HAVE_FUNCTION_GRAPH_TRACER flip
    + trampolines (landed on-branch subsequently per D34
    addendum-3's local fix path; `select HAVE_FUNCTION_GRAPH_TRACER
    if HAVE_FUNCTION_TRACER` in `arch/um/Kconfig`).
    `Documentation/virt/uml/ftrace.rst` + ftrace-smoke
    selftest regression-test the result (Finding #5, task
    #145).
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

**Workstream D (KVM backend):** **Phase 1 closed 2026-04-25
at 1.15× kvm/seccomp.** Phase III of the post-Q1 push
landed D-01 through D-06 as a working in-kernel harness; the
2026-04-24 push extended that to a sustained per-trap
dispatcher (audit A1) plus the systrap gadget ladder G1-G8
plus full closure of audit rounds 4 / 5 / 6. The 2026-04-25
session shipped Phase 1 closure: dyn-loader kselftest PASSES,
audit round 7 closed, perf-fallback ratio 2.92× → 1.15× via
the #238 / #242 / experiment-#1 / experiment-#2 ladder. See
`02-workstreams/D-kvm-backend/measurements.md` "2026-04-25 —
Phase 1 closure" entry for the per-stage table.

  - **Production dispatcher** (audit A1, D70): per-trap
    `interrupt_end()` contract matching ptrace + seccomp;
    one KVM_RUN, one decode, one return per call.
  - **CPL-derived `is_user`** (audit A2, D70): sampled
    via `KVM_GET_SREGS` post-VMEXIT.
  - **Loud failure on writeback errors** (audit A4, D70):
    `KVM_SET_REGS` + `KVM_SET_MSRS` failures are fatal,
    not silently swallowed.
  - **Per-mm shadow PT** (D82 F6, D90 G2): `kvm_shadow_
    invalidate_va_range` covers same-mm map/unmap;
    `kvm_shadow_pgd_clear_user` covers cross-mm context
    switches. PGD slots 256+ (kernel half) preserved.
  - **Bootstrap page locked down** (D81 F5): `KVM_X86_PTE_
    US` dropped; ring-3 cannot read or write the LSTAR
    code path.
  - **#PF recovery** (D86 F7/1, D92 G3): real x86 error
    code propagated to UML faultinfo; gadget-mid-store
    faults divert to handle_syscall (-EFAULT semantics);
    write-to-RO no longer loops.
  - **User RFLAGS preserved across SYSCALL + #PF**
    (D75 F2, D88 F3-followon, D93 G1): `kvm_build_
    sysret_r11` helper merges saved flags with REQ_ON;
    KUnit + dedicated DF-preservation selftest cover
    the round-trip; clock/time/getcpu gadget bodies
    keep RAX = NR across fallback (F7/2 pattern).
  - **Systrap gadget G1-G8** (D69 / D71 / D73 / D74 /
    D78 / D79): in-guest LSTAR fast-path for 10
    syscalls (7 pid-family + clock_gettime + time +
    getcpu — sched_yield demoted by G5 / D94). Class
    E in memo 10. Per-vCPU state page + shared vvar
    clock seqlock + call-budget refresh cap (F8 /
    D83). Bounds check vs `task_size_cap` per audit
    G1 / D93 — no supervisor-VA writes, no
    non-canonical #GP. Validated across the s0-s7
    fleet (D79, post-round-6 numbers in
    `02-workstreams/D-kvm-backend/measurements.md`):
    23-32 ns / 87-115 cyc gadget cost, 2.9-4.3×
    margin under memo-07's <100 ns target. **D70 = GO.**
  - **Upstream Series 7** (D87): SUBMISSION-NOTES +
    cover-letter draft in `upstream-patches/
    kvm-backend-series/`. Patch emission still waits
    on Series 4 (backend-ops-abstraction-rfc) landing
    + a focused squash pass against the 80+ branch
    commits.

  All P0/P1 audit findings through **round 7** are closed
  (round 7 ran 2026-04-25; see "Phase 1 closure (2026-04-25)"
  below).

  Phase-1 follow-ons that landed 2026-04-24/25:
  - **#230** F5-followon — bootstrap split into RO
    code+tables + RW NX IST stack pages.
  - **#238** STEP-2 — `kvm_touch_all_user_vmas` removed;
    lazy-fault recovery via direct `handle_page_fault`
    call is now the only path.
  - **#272** — IRETQ-based bootstrap re-entry replaces
    SYSRETQ; preserves user RCX/R11 across recoverable
    #PF (uncovered by ld-linux's RELR loop using RCX
    as the relocation cursor).
  - **#273** — `KVM_GET_SUPPORTED_CPUID` +
    `KVM_SET_CPUID2` passthrough; guest sees host
    x86-64-v3 features, modern dynamically-linked
    binaries load cleanly.
  - **#242** — skip `kvm_shadow_fill_from_uml_pgd`
    when the shadow PT already mirrors `current->
    active_mm->pgd` (cache key: synced flag + mm
    pointer + pgd-VA).
  - **Audit round 7 P1** — IDT[13] (#GP) handler at
    bootstrap +0x4d8 / port 0xf9; non-canonical
    iretq targets surface as SIGSEGV instead of
    cascading to #DF.

  Phase-1 follow-ons deferred (perf, non-blocking):
  - **#243** per-mm cached shadow PGD (lever #6).
  - **#244** huge-page (2 MiB) shadow PT (re-analysis
    showed the lever doesn't apply as framed).

  D failure remains explicitly acceptable as a backend
  policy: prod-fast falls back to seccomp if the host
  doesn't support /dev/kvm or KVM_INTEGRATED is
  disabled.

**Tooling (umlctl + observability spine, 2026-04-23):**

  - `tools/uml/uml-launcher/src/bin/umlctl/` — podman-shaped
    multi-instance lifecycle CLI. Verbs: `create` / `start` /
    `stop` / `rm` / `ps` / `logs` / `schema` / `events` /
    `assert` / `export`. Spec: `08-future-phases/05-umlctl.md`.
  - `08-future-phases/13-uml-observability-spine.md` — the
    unifying telemetry architecture. **Phase O1 is fully
    landed as of 2026-04-24** (all six sub-lifts: run_id
    (ULID) + bundle directory, kernel-console split +
    `dmesg` verb, schema registry + structured
    `events.jsonl`, `.umlbundle.tar.zst` export, `events` tail
    verb, `assert` CI-predicate verb). O1.2 ships a
    post-hoc derivation — `umlctl stop` filters the merged
    `init.log` for printk-shape lines into a `kernel.log`
    sidecar; the `dmesg` verb reads that (or filters live
    init.log on the fly for running runs). **Phase O2.1
    landed 2026-04-24** — `umlctl metrics` one-shot
    `/proc/<pid>/*` + cgroup v2 scraper (human or `--json`).
    **Phase O3.1 landed 2026-04-24** — dmesg parser at
    `umlctl stop` time converts sanitizer (KASAN / KFENCE /
    KCSAN / KMSAN / UBSAN), panic, OOM-kill, RCU-stall,
    lockdep, and watchdog splats in `kernel.log` into
    structured events.jsonl records (10 schemas now
    "emitted"). O2.2 (eBPF) + O2.3 (OpenMetrics HTTP) +
    O3.2 (upstream tracepoints) + O4-O6 parking-lot.
    O3.3 is "pattern documented, per-test migrations
    opt-in" — memo 13 carries the canonical shell
    snippet that new host-side kselftests can copy.
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
Plus the post-Q1 / Phase-1 additions: cve-repro,
df-preserve, **dyn-loader** (task #270 — ld-linux + libc
end-to-end repro, deterministic kvm row), kvm-bounds,
kvm-smoke, perf-fallback, perf-getpid, **snapshot-kvm-smoke**
(task #251 — KUnit-shape gate against the v2 snapshot API
surface), umlctl-smoke. Direct-invocation regression coverage
for every landed sub-system. As of 2026-04-25 all current-
profile selftests pass on the kvmint and seccomp configs.

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
  3. `upstream-patches/ftrace-notrace-generic-v1/` —
     cover letter + 1 patch (`kthread()` +
     `smpboot_thread_fn()` notrace), prepared 2026-04-24.
  4-6. Three further series staged to write
     (backend-ops-abstraction-rfc, static-key-hot-paths,
     per-profile C-series).
  7. `upstream-patches/kvm-backend-series/` — scoped
     post-G8/D70=GO (D87, 2026-04-24): SUBMISSION-NOTES
     + cover-letter draft in tree, 15-patch ordering
     planned. Patch emission waits on Series 4 landing +
     focused squash against the 80+ commits in
     `arch/um/backend/kvm/`.

**Cross-subsystem work carried on the fork (per D45):**

  D45 (2026-04-21) clarifies this plan's posture: the fork
  demonstrates the working end state first; LKML adoption
  follows, motivated by the running artifact. Items previously
  framed as "blocked on upstream review" are in fact just
  "work to do on the fork." As of 2026-04-23 two of D45's
  three items are landed and only one remains:

  - ~~C-04 commit 3 HAVE_FUNCTION_GRAPH_TRACER~~ — **landed.**
    The local `kernel/trace/` fix anticipated by D45 proved
    unnecessary once the strip-infrastructure (commit 3a)
    was in place; `HAVE_FUNCTION_GRAPH_TRACER` is now
    selected unconditionally under `HAVE_FUNCTION_TRACER`
    in `arch/um/Kconfig`.
  - C-06 full port — D43 option B2 is a
    `arch/x86/net/bpf_jit_comp.c` portable-emitter refactor.
    Under D45, land the split on the fork. **Still
    outstanding** as task #68 (deferred post-real-
    deliverable). The two hygiene commits (e2b686c962 /
    5b95b1bb3e) stay as-is in `upstream-patches/
    bpf-hygiene-v1/` for when the upstream conversation
    opens; submission order now tracked in
    `upstream-patches/SUBMISSION-QUEUE.md` (D63).
  - ~~C-07 KMSAN~~ — **landed.** D44's fourth-probe
    map-on-demand arch callback turned out not to be
    needed: D62 (Phase V Lift #3c of the post-Q1 push,
    2026-04-23) redesigned the UML-side shadow/origin
    layout as a VMALLOC quarter-split that works with
    stock `mm/kmsan/init.c`, unblocking
    `select HAVE_ARCH_KMSAN if X86_64` in
    `arch/um/Kconfig` with no kernel-core patch required.
    `kmsan-arch-callback-rfc/` remains staged in
    `upstream-patches/` as a "no visible impact, just
    benefits" offering for the KMSAN maintainer, but UML
    is no longer blocked on it.

So D45's three-item "carried on the fork" list collapses to
one item (C-06 B2) as of 2026-04-23; the other two items
proved to be solvable on the fork-side alone without ever
touching kernel-core / mm/kmsan infrastructure.

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
