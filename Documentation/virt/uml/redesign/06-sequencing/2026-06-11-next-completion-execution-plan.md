# UML v2 `next` Completion Execution Plan

Date: 2026-06-11

Branch target: `next`

Functional code baseline used for this plan refresh:

- `next`: `bb092119158b`
- `origin/next`: `bb092119158b`
- `torvalds/master`: `9716c086c8e8`
- latest fetched Linus ref for this refresh: `FETCH_HEAD=9716c086c8e8`
- `FETCH_HEAD...next`: `0` commits behind, `196` commits ahead
- upstream ancestry: the fetched Linus ref is an ancestor of `next`
- branch state before this plan/tooling refresh: `next` matched `origin/next`
- note: this is the functional code baseline; planning/tooling refresh
  commits may sit above it.

Current execution evidence added on 2026-06-11:

- refreshed this plan after the branch reached `bb092119158b` and a fresh
  fetch of Linus' `master` confirmed `9716c086c8e8` as the upstream base used
  for the comparison above;
- cleaned KVM v2 state comments and x86 UML ptrace TLS register handling in
  active source;
- tightened the substrate gate and recorded CPython tier-0 evidence through
  `umlctl gate run` for both seccomp and KVM v2;
- documented KGDB as deferred-not-present rather than a current UML v2
  instrumentation feature;
- rebuilt `./linux` from `next` at `bb092119158b`;
- rebuilt kernel version: `7.1.0-rc7-00261-gbb092119158b`;
- focused pool/fork/syzkaller regression pass recorded in
  `2026-06-11-pool-fork-regression-pass.md`;
- the pass covers template pause, fork smoke/stress, pool-member, replicated
  sustained pool, pivot, pool spawn/serve/exec/port-forward/mconsole, full
  pool benchmark, and syzkaller shim;
- the post-vector2 rerun also passed `cargo test --locked` for
  `tools/uml/uml-launcher`, `umlctl-smoke`, `pool-bench`,
  `pool-mconsole-path-probe`, `syzkaller-shim-smoke`, and
  `vector2-pool-tap-smoke` against `bb092119158b`;
- rebuilt and validated vector2 on `98166580dc4f`, with `um_vector2_*` KUnit
  reporting 84 pass, 0 fail, and 2 trusted-TAP skips, plus focused fd,
  multiqueue, in-process TAP, sandbox, and pool TAP smokes;
- recorded a bounded KVM-v2/vector2 Tier 3 path smoke for one Django-v2 and
  one FastAPI-v2 iteration;
- closed the UML KMSAN runtime-smoke blocker. The landed fix series clears
  KMSAN host-boundary call metadata in UML host helpers, unpoisons hostfs data
  filled by host syscalls, rejects host-side mappings in
  `kmsan_virt_addr_valid()`, restores Clang KMSAN memory intrinsic lowering,
  and makes the seccomp stub signal restorer stackless under
  frame-pointer/KMSAN builds. Validation: clean LLVM `uml/research-kmsan`
  build plus `kmsan-smoke` reporting
  `KMSAN_SMOKE: PASS runtime=y reproducer=n`;
- reran the current record/replay focused gates against the current `./linux`:
  `kvm-record-smoke` reported 3066 live syscall entries, 294336 bytes used,
  and 0 drops, and `kvm-record-clock-bench` reported `N=100`,
  `observed=100`, `replayed=100`, and `mismatches=0`; and
- added the first R/R-1 task-owned session-start gate: record status now
  reports first/last syscall PID and same-task versus other-task syscall
  counters, and `kvm-record-smoke` includes a static helper that snapshots
  itself, records a scalar workload, and requires `same=syscall_count` with
  `other=0`;
- added the first R/R-1 syscall-payload model gate: `uname(2)` records and can
  restore `struct new_utsname` payloads when syscall number and arguments
  match. The follow-up `getcwd(2)` slice records and restores returned path
  bytes. Follow-up raw-time payload slices added `clock_gettime(2)`,
  `gettimeofday(2)`, and `time(2)` to the supported direct-syscall payload
  set, and the task-owned helper now exercises multiple raw-time argument
  shapes. `kvm-record-smoke` reports `KUnit=24/24`, and the task-owned leg
  reports `payload_entries=8`, `payload_bytes=528`, and `replayed=393`; and
- added the first R/R-1 strict replay policy gate: strict replay accepts only
  the current bounded syscall subset (`getpid`, `getppid`, `gettid`,
  payload-aware `uname(2)`/`getcwd(2)`, and payload-aware
  `clock_gettime(2)`/`gettimeofday(2)`/`time(2)`) and fails closed with
  recorded failure counters for other syscalls instead of serving arbitrary
  scalar-only log entries.
  KUnit now covers both the accepted subset and unsupported `getuid`,
  `clock_gettime`, `gettimeofday`, `time`, and `getrandom` policy checks where
  those syscall numbers are available, plus external-I/O rejection for
  `openat`, `read`, `write`, and `ioctl`, strict-on rejection, and strict-off
  permissive behavior; and
- added the first raw-time fail-closed replay policy: UML vDSO clock helpers
  already route through syscalls, strict replay rejects raw time syscalls, and
  KVM v2 sets CR4.TSD during replay so user `RDTSC`/`RDTSCP` faults instead of
  observing host time outside the log; and
- added the first R/R-1 signal policy: normal KVM execution keeps `SIGALRM`
  unblocked for timer preemption, while replay mode blocks `SIGALRM` in KVM's
  per-vCPU signal mask so timer delivery cannot create unrecorded in-guest
  `EINTR` points. Replayable signal-event ordering remains future work; and
- added the first versioned event-format gate: each replay entry now records
  format version and flags, debugfs reports `format_version=1`,
  `entry_header_size=24`, `entry_size=96`, and `max_payload=4096`, and KUnit
  rejects bad version/flags without advancing the replay cursor; and
- the pool/fork, CPython, vector2, KMSAN, and record/replay evidence is
  current focused evidence, not a substitute for the final post-record/replay/
  vector2 validation matrix;
- wired the vector2 TCP throughput harness into kselftest as `net-bench`:
  `tcp-send` now builds through the kselftest Makefile, wrapper scripts use
  repo-relative defaults instead of developer-local paths, and syntax/preflight
  checks cover the scripts and helper;
- ran the first current-tree TCP net-bench gate through `umlctl`: legacy
  vector and vector2 both completed 3/3 functional iterations, but vector2
  reported median 18949.8 Mbps versus legacy vector's 39805.4 Mbps, a 0.476
  ratio against the 0.85 gate;
- replaced vector2 fd/TAP TX skb linearization with a shared scatter-gather
  `writev` path that sends `virtio_net_hdr`, skb head data, and skb frags
  without copying the skb into a linear buffer;
- validated that scatter-gather TX with `um_vector2_*` KUnit, fd handoff,
  in-process TAP, and multiqueue smokes, then reran TCP net-bench. The median
  ratio improved to 0.573, but the gate still failed: legacy vector median
  40041.7 Mbps versus vector2 median 22953.7 Mbps;
- tested a bounded `sendmmsg()` TX batching prototype locally and rejected it
  because it added complexity without improving the TCP gate. The prototype was
  not committed;
- added the first vector2 TCP diagnostic slice: the `net-bench` template now
  logs guest link, route, feature, and ethtool-counter state around each TCP
  sender, and vector2's `vnet_hdr_enabled` ethtool stat reports the runtime
  inherited-fd vnet-header state; and
- closed the measured vector2 guest-to-host TCP blocker with a TX/RX NAPI
  scheduling fix: empty TX IRQs no longer force NAPI, TX enqueue honors
  `netdev_xmit_more()` while the ring has space, and RX polling prepares
  receive buffers only after RX readiness.  Validation reported
  `um_vector2_*` KUnit 87 pass, 0 fail, and 2 trusted-TAP skips; fd handoff,
  in-process TAP, and fd multiqueue smokes PASS; and normal TCP net-bench PASS
  with legacy vector median 40080.5 Mbps, vector2 median 37116.0 Mbps, and
  ratio 0.926 against the 0.85 gate; and
- refreshed the fixed-byte bidirectional TCP harness on current `next`:
  guest-to-host clears 1 MiB, 8 MiB, and 32 MiB with best observed host-side
  ratios 0.966, 1.007, and 0.925; host-to-guest clears 8 MiB and 32 MiB with
  ratios 2.002 and 0.901, but 1 MiB remains below legacy at 0.483 and 0.579
  on a focused rerun; and
- reran vector2 1 MiB host-to-guest as a single-queue fd configuration,
  improving the best observed ratio to 0.663 but not closing the gap.  The
  fixed-byte harness now records before/after link, route, feature, and
  ethtool-counter diagnostics so the next pass can inspect RX IRQ, NAPI, and
  batch behavior.
- added a lazy-RX batch cleanup: fd and TAP receive paths now prepare one RX
  slot per nonblocking read attempt instead of preparing the whole NAPI budget.
  Validation reported `um_vector2_*` KUnit 88 pass and 2 trusted-TAP skips,
  fd handoff, fd multiqueue, in-process TAP, and pool TAP smokes PASS, and the
  normal guest-to-host TCP gate remains green with legacy vector median
  39215.2 Mbps, vector2 median 38960.1 Mbps, and ratio 0.993.  The cleanup
  reduces representative 1 MiB host-to-guest RX-slot churn from 32448 prepared
  / 906 received / 31542 released slots to about 1410 prepared / 905 received
  / 505 released slots, but the focused host-to-guest 1 MiB best ratio is only
  0.594, so the small-transfer blocker remains open; and
- extended the fixed-byte TCP diagnostic harness with knobs for vector2 host
  mode, active-sender `TCP_NODELAY`, and host/guest sender chunk size so the
  remaining 1 MiB host-to-guest blocker can be reproduced without ad hoc
  script edits; and
- aligned vector2 RX checksum feature reporting with the vnet-header receive
  path: when `csum=1`, vector2 now exposes fixed `NETIF_F_RXCSUM` alongside
  TX checksum offload.  Validation reported `um_vector2_*` KUnit 89 pass,
  0 fail, 2 trusted-TAP skips; fd handoff, fd multiqueue, and in-process TAP
  smokes PASS; fixed-byte diagnostics show `rx-checksumming: on [fixed]` with
  `vnet_hdr_enabled: 1`; and the normal TCP net-bench gate remains green with
  legacy vector median 39854.4 Mbps, vector2 median 39441.7 Mbps, and ratio
  0.990.  This closes a feature-advertisement mismatch, not the 1 MiB
  host-to-guest throughput blocker.
- aligned the vector2 fake-host RX path with production fd/TAP lazy batching:
  the fake backend now prepares one slot per attempted read and releases one
  speculative probe slot on empty or limited reads instead of preparing and
  releasing the whole NAPI budget.  Validation reported
  `make ARCH=um -j$(nproc)`, `um_vector2_fake_host` KUnit 10/10 PASS, and
  `um_vector2_*` KUnit 94 pass, 0 fail, 2 trusted-TAP skips.  This tightens
  KUnit coverage for the RX path used by the host-to-guest investigation, but
  it does not close the throughput blocker; and
- ran a bounded 1 MiB host-to-guest TCP parameter/topology sweep with
  transfer-window perf collection enabled.  Default fd/multiqueue vector2
  remained below legacy vector with a host-side median ratio of 0.650602.
  `TCP_NODELAY` and in-process TAP also remained below the bar at 0.560706 and
  0.639896.  The 16 KiB sender chunk cell had a median ratio above 1.0 only
  because legacy vector produced two slow outliers; its best ratio was
  0.530945.  Single-queue fd mode was the best hint, with a median ratio of
  0.810510 and best ratio of 0.906732, but it still did not close the median
  no-regression gate.  The next vector2 pass should target queue selection,
  wakeup, and receive scheduling policy rather than fd handoff alone.

This file is now the plan of record for completing, importing, or explicitly
retiring all original UML v2 functionality on `next`.

This is the operating plan for turning `next` into the single complete UML v2
branch. It complements the feature inventory in
`2026-06-10-next-functionality-inventory.md` and the earlier integration plan in
`2026-06-10-next-full-functionality-integration-plan.md`.

The plan is deliberately execution-oriented. It is not another mission
statement, and it is not a research journal. Each remaining feature must leave
behind one of three outcomes: landed and validated code, an explicit retirement
record, or a bounded experimental label that is excluded from the final
completion claim.

## Objective

Declare UML v2 complete only after every intended feature from the original
mission, the current `next` branch, and the relevant historical UML branches is
either:

- present on `next`;
- completed on `next`;
- intentionally retired with a dated rationale; or
- moved to clearly marked archival documentation because it is no longer part
  of the completion claim.

The completion branch must read like normal kernel work. Active source,
selftests, tools, and user-facing docs should not contain internal issue
numbers, phase diaries, branch archaeology, investigation logs, or stale claims.
Historical context can remain in redesign archives when it is useful, but it
must not leak into upstream-facing code or current user documentation.

The plan is intentionally conservative: do not reimplement already-sound code
for aesthetic reasons, and do not direct-merge historical branches. Mine old
branches for missing behavior and tests, then port or reimplement only the
pieces that still belong in the final `next` branch.

## Completion Standard

`next` is complete when all of the following are true:

- No intended UML v2 functionality exists only on a historical branch.
- Every active CLI command, selftest, Kconfig option, debugfs file, mconsole
  command, and documentation page maps to functionality that exists on `next`.
- KVM v2 core behavior is validated across syscall dispatch, signal/FPU/XSAVE
  state, dynamic userspace, SMP boundaries, snapshot, and the selected
  record/replay tier.
- Snapshot capture, restore, snapshot ELF export, fork-server, pool, vector2,
  syzkaller shim, launcher workflows, and profile builds have matching code,
  tests, and documentation.
- Record/replay is either completed to an explicitly defined first tier or
  clearly marked experimental and excluded from the original completion claim
  with approval.
- Instrumentation profile claims match real builds and runtime probes.
- Vector2 replacement/publication claims match Tier 3, multiqueue/fairness,
  seccomp, KVM v2, and sandbox evidence.
- Active source and tests are cleaned of random history, old branch labels,
  issue-number breadcrumbs, and note-style comments.
- The final validation matrix passes, or any skips are bounded and documented.
- The branch is pushed to GitHub after the final validated slice.

## Source Material

Treat these as input, not automatically-current truth:

- `00-vision.md`: mission, success criteria, profiles, instrumentation,
  snapshot/forkserver, record/replay, KVM backend, and syzkaller goals.
- `02-workstreams/`: original workstream design and historical investigations.
- `03-profiles/`: target profile behavior and profile-specific expectations.
- `05-validation/a-plus-quality-plan.md`: quality bar and validation ladder.
- `06-sequencing/2026-06-10-next-functionality-inventory.md`: current live
  feature tracker.
- `06-sequencing/2026-06-10-next-full-functionality-integration-plan.md`:
  earlier detailed integration plan and historical branch disposition.
- `STATUS.md`: current user-facing readiness view.
- `report-presentation/`: archived report/deck material; useful for original
  intent, not current completion truth.
- Historical branches: `kvm-v2-snapshot-elf64`, `fork-server-phase1c`,
  `memo09-phase2`, `memo09-phase3-pool-bench`, `memo09-phase4`,
  `experiment-path-c`, and `umlctl-deploy`.

The conflict rule is simple: current implementation and fresh validation on
`next` override old plans, reports, presentations, and branch notes.

## Original Mission Coverage Map

This table ties the original `00-vision.md` goals and the archived
report/presentation claims to the remaining completion work. It is the guard
against declaring success because a feature appears in old notes while the
current `next` branch still lacks code, tests, or accurate documentation.

| Original mission item | Current `next` posture | What must happen before 100% completion |
| --- | --- | --- |
| KVM backend with low syscall overhead | KVM v2 core, syscall dispatch, LSTAR gadget, fallback path, XSAVE/FPU hardening, dynamic-loader/TLS fix, and focused CPython/substrate evidence are present. | Broaden dynamic-userspace and Tier 3 workload evidence on the final tree, keep KUnit and failure-policy gates green, and record the final upstream-ready performance envelope. |
| Full instrumentation: KASAN, KMSAN, KCSAN, KFENCE, KCOV, kprobes, ftrace, BPF JIT, KGDB | KASAN, KMSAN, KCSAN, KFENCE, KCOV, kprobes, ftrace, and BPF/JIT have focused build/runtime evidence. KGDB is explicitly deferred-not-present. | Rerun the full profile/instrumentation matrix after final KVM/vector2 changes. Keep KGDB out of completion unless it is implemented with UML architecture support, backend register access, transport support, and a smoke test. |
| Deterministic time-travel and record/replay as first-class | Experimental Kconfig-gated R/R-1 exists with task-owned snapshot start, scalar replay, `uname(2)`/`getcwd(2)` payload replay, task-owned direct raw-time argument-shape payload replay, time-travel clock events, strict fail-closed unsupported syscall policy, supported-entry mismatch rejection, timestamp-instruction blocking, SIGALRM blocking during replay, and device/randomness rejection. | Either finish and document R/R-1 as the supported first tier, or explicitly exclude record/replay from the final completion claim with approval. Broader workload coverage, signal-event ordering, and device/network/hostfs policy remain the main gaps. |
| Snapshot/forkserver startup below the intended fuzzing threshold | Snapshot capture/restore/ELF export, template pause, fork-on-resume, pool serve/take, daemon exec, warm members, pool benchmark, and syzkaller shim have current focused evidence. | Rerun the full pool/fork/syzkaller gate on the final KVM/vector2 stack and decide whether snapshot-backed fork-server is completed, deferred, or retired. |
| Explicit research/fuzz/sandbox/production/library/embedded/time-travel profiles | Kernel profile fragments and `umlbuild` profiles exist; several profile-specific runtime probes have passed. | Build every kernel profile from a clean tree, run matching runtime probes, rerun KMSAN, and ensure launcher profiles do not drift from Kconfig profiles. |
| Runtime-flippable hooks and observability | KVM tracepoints, debugfs status/control surfaces, profile hooks, launcher gates, and transparency tooling exist. Private KVM state trace remains historical-only. | Keep public observability on normal kernel trace/debugfs surfaces. Reintroduce private state trace only if redesigned as optional diagnostics with tests. |
| Backend abstraction: ptrace, seccomp, and KVM as swappable trap mechanisms | Seccomp remains the fallback backend and KVM v2 is selectable; ptrace-era code has been cleaned or bounded where touched. | Keep backend selection explicit, preserve seccomp comparison gates, and avoid stale docs implying unsupported automatic backend behavior. |
| `vm/uml` syzkaller backend | The launcher-side shim and syzkaller-style pool take/exec/port-forward/status/destroy path have focused smoke evidence. | Rerun after final pool/vector2/KVM changes and keep the shim tied to the supported `exec/1` JSON/NDJSON ABI unless an `exec/2` ABI is deliberately implemented. |
| ARM64 and RISC-V host buildability | Original mission goal remains outside the current x86_64-centered validation evidence. | Do not claim this as complete without real host-build evidence or an explicit deferral note. |

## All-Functionality Closure Model

Every original or historical feature must move through this closure model
before the final completion claim:

1. **Inventory.** Name the feature, the live `next` surface, the historical
   branch or document where it came from, and the current status label in
   `2026-06-10-next-functionality-inventory.md`.
2. **Disposition.** Choose exactly one outcome: keep as already complete, clean
   in place, import from history, reimplement against current architecture,
   retire with rationale, or keep experimental and excluded from the completion
   claim.
3. **Implementation.** Make the smallest coherent code change on `next`; avoid
   importing diary comments, local issue numbers, stale phase labels, or branch
   archaeology.
4. **Validation.** Run the smallest meaningful focused gate for that feature
   and record the exact command shape, commit, result, and remaining limits.
5. **Documentation.** Update the live inventory, this execution plan if the
   blocker state changed, `STATUS.md` for user-visible readiness changes, and
   active user docs for any CLI/Kconfig/debugfs/mconsole behavior changes.
6. **Commit and push.** Commit one coherent slice with a `Signed-off-by` trailer
   and push `next` before starting the next unrelated slice.

The closure model prevents two common failure modes: claiming completion while
a feature still exists only on an old branch, and weakening the branch by
copying old research-code history into active upstream-facing files.

## Review Surface And Cleanup Calibration

The full UML v2 branch is too large for one undifferentiated cleanup pass. The
working review set should be split by whether a file is active implementation,
active user-facing documentation, validation infrastructure, current status, or
archival record.

Current surface relative to `torvalds/master` at this plan refresh:

| Area | Files in tree | Files changed vs. `torvalds/master` | Review posture |
| --- | ---: | ---: | --- |
| `arch/um/` | 315 | 207 | Full source review required for active KVM v2, vector2, snapshot, record/replay, profiles, and sanitizer paths. |
| `tools/testing/selftests/um/` | 293 | 293 | Full validation-surface review required; remove private labels and stale expectations from active tests. |
| `tools/uml/uml-launcher/` | 95 | 95 | Full CLI/help/config review required; match commands to live kernel surfaces. |
| `Documentation/virt/uml/` total | 460 | 459 | Split active user docs from redesign archive before editing. |
| Non-redesign UML docs | 36 | 35 | Active user-facing docs; must be current and upstream-style. |
| `Documentation/virt/uml/redesign/` | 424 | 424 | Mostly archive/status/planning; mark historical material instead of rewriting it all. |
| `redesign/06-sequencing/` | 51 | 51 | Current execution tracker; keep accurate and dated. |
| `redesign/08-future-phases/` | 59 | 59 | Mixed active trackers and historical notes; promote only live trackers to current truth. |
| `redesign/report-presentation/` | 42 | 42 | Historical May 2026 report/deck workspace unless regenerated from a new cutoff. |
| `redesign/upstream-patches/` | 26 | 26 | Regenerate after final branch shape; do not treat old cover letters as current. |

The changed active/review surface across source, selftests, launcher, and UML
docs is about 1,054 files. The practical cleanup target is
not "rewrite every historical markdown file." The target is:

- every active source file and selftest touched by UML v2;
- every launcher command, schema, help string, example, and gate used by the
  current workflow;
- every active UML RST page outside the redesign archive;
- current status and sequencing docs that make completion claims;
- future-phase/vector2 tracker docs that still serve as live publication
  gates; and
- upstream patch queue material after the implementation stabilizes.

Archive material may keep dated history, but it must be visibly archival and
must not be cited as current completion evidence.

## Branch And Commit Discipline

Use `next` as the authority.

For each work slice:

1. Start from `next` matching `origin/next`, with a known upstream comparison.
2. Avoid direct merges from historical branches.
3. Port or reimplement only the needed behavior.
4. Rewrite comments into normal kernel style while porting.
5. Add or fix the smallest meaningful tests with the feature.
6. Run targeted validation before commit.
7. Update the inventory, status, and any affected user docs.
8. Commit a coherent slice with `Signed-off-by`.
9. Push `next`.
10. Record the new commit and validation result in the documentation.

Historical branches are source material and regression references. They are not
integration bases.

## Import Versus Reimplementation Rule

Do not reimplement the entire branch just because the architecture is now
understood. Keep already-landed code when it is structurally sound, validated,
and documented in normal kernel style. Reimplement or port narrowly when the
historical source has one of the following problems:

- it exists only on a historical branch;
- it carries stale architecture assumptions that conflict with current `next`;
- it is coupled to removed KVM v1/path-C code;
- it exposes a user surface that no longer exists;
- it contains diary prose, internal bug IDs, phase labels, or investigation
  breadcrumbs in active source or tests;
- it lacks a focused validation gate; or
- it would make final upstream patch splitting harder than a clean rewrite.

The default decision for each feature is:

| Situation | Action |
| --- | --- |
| Present on `next`, validated, clean comments | Keep and protect with regression tests. |
| Present on `next`, correct but comment/test/docs are messy | Clean in place with the smallest semantic delta. |
| Present on `next`, user surface stale or behavior broken | Fix before importing more adjacent functionality. |
| Historical-only and small/self-contained | Port with normal kernel comments and a focused test. |
| Historical-only and tied to discarded architecture | Reimplement from the current design or retire. |
| Historical-only and not required for completion | Retire explicitly with rationale and archive pointer. |
| Experimental but useful | Keep behind Kconfig/debugfs/static-key boundaries and exclude from completion unless the selected tier passes. |

## Required Historical Branch Disposition

| Branch | Use | Required disposition |
| --- | --- | --- |
| `kvm-v2-snapshot-elf64` | Snapshot, ELF export, record core, state trace, KUnit history. | Snapshot and ELF export are already present on `next`; finish record/replay decisions and import no state trace code unless it is redesigned as clean optional diagnostics. |
| `fork-server-phase1c` | Early template-pause and fork-server behavior. | Use only to confirm that current template-pause and fork-on-resume behavior did not regress. |
| `memo09-phase2` | Identity apply and pool-member identity semantics. | Compare identity layout and application semantics; port only missing behavior. |
| `memo09-phase3-pool-bench` | Pool benchmark thresholds and acceptance logic. | Preserve benchmark value, but keep the current `next` pool architecture as authority. |
| `memo09-phase4` | Later fork-server, pool, snapshot, and smoke surfaces. | Use for missing smokes and behavioral parity checks; do not import stale assumptions. |
| `experiment-path-c` | KVM v1 archive, record/replay experiments, diagnostic repros. | Mine tests and lessons; keep KVM v1 archive out of active `next` unless explicitly archived. |
| `umlctl-deploy` | Launcher, deploy, gates, vector2, and soak tooling. | Diff module-by-module; keep current launcher structure and import only missing commands, examples, or gates. |

## Current Blocker Ledger

The following items block a "100% complete from the original plan" claim unless
they are implemented and validated, or explicitly retired with approval.

| Blocker | Why it blocks | Required closeout |
| --- | --- | --- |
| Record/replay supported tier | The original vision names deterministic time-travel and record/replay as first-class functionality. Current `next` has an experimental core, live syscall hook, snapshot-backed start, time-travel clock-event logging, direct-syscall payload models, fail-closed raw-time/signal/device/randomness policy, a task-owned 393-entry deterministic replay smoke, and live strict mismatch/unsupported syscall rejection across the current supported subset. Broader workload coverage, explicit signal-event ordering, and arbitrary device/network/hostfs replay remain incomplete. | Finish the remaining broader-policy gates, keep unsupported operations documented, and preserve the experimental label until those gates pass. |
| KMSAN regression protection | The original instrumentation goal includes KMSAN, and the runtime-smoke blocker is now closed on `next`. It remains in the blocker ledger only because final completion must prove the closure did not regress after record/replay, vector2, profile, or cleanup changes. | Rerun a clean LLVM `uml/research-kmsan` build and `kmsan-smoke` in the final validation matrix. Do not reopen KMSAN as an implementation gap unless that rerun regresses. |
| KGDB disposition | The original instrumentation list includes KGDB, but current UML does not select `HAVE_ARCH_KGDB` and no live profile fragment enables `CONFIG_KGDB`. | KGDB is deferred-not-present in the current completion tracker. Reintroduce it only with UML architecture support, backend register access, a transport decision, and a smoke test. |
| Vector2 publication readiness | Vector2 has strong focused smoke evidence, and the measured guest-to-host TCP regression now passes the 0.85 gate after the TX/RX NAPI scheduling fix: vector2 median 37116.0 Mbps versus legacy 40080.5 Mbps, ratio 0.926. Lazy RX preserves that gate at ratio 0.993 and removes the worst RX-slot over-preparation. The fake-host KUnit backend mirrors production fd/TAP lazy RX batching, and the latest no-TX-write-IRQ slice removes permanent TAP/fd `IRQ_WRITE` registration in favor of a bounded retry timer for no-progress TX polls. Its focused 1 MiB host-to-guest fd/multiqueue rerun improved the host-side median vector2/vector ratio from the prior 0.650602 sweep result to 0.899069, while transfer-window perf ratios showed vector2 at 0.315536x legacy vector syscalls, 0.362255x task-clock, and 0.205458x context switches. This is bounded progress, not final publication closure: vector2 still trails on best host throughput at 0.682038x and remains about 1.916x legacy vector on scheduler pcount/voluntary context-switch deltas. Current-head repeat-2 UDP matrices pass for buffered unpaced 1 MiB UDP and paced 8 MiB UDP in both directions. Unpaced larger host-to-guest UDP remains bounded by earlier exact-byte loss. Replacement/publication still needs the rest of P4.3 plus long-run and fairness evidence. | Finish the natural seccomp long run, equivalent KVM v2 Tier 3 networking, broader host-to-guest repeat/fairness coverage using the fixed-byte diagnostics, final UDP acceptance notes, steady-state syscall-rate, full CPU-utilisation, and multiqueue fairness/performance evidence. Keep parser-only transports out of runtime claims and keep vector2 opt-in until the evidence justifies stronger language. |
| KVM v2 final workload breadth | KVM v2 is past architecture unknowns, but publication still needs broader dynamic-userspace and final-vector2 workload evidence. | Run Tier 3 and selected CPython/substrate gates on the final tree, including dynamic userspace beyond `/bin/true` and `dyn-loader`. |
| Pool/fork-server final regression pass | The rebuilt current-HEAD `bb092119158b` binary passes the focused pool/fork/syzkaller regression set, including warm-pool, replicated sustained-pool, pool benchmark, syzkaller shim, and vector2 pool TAP. Final validation must still be rerun after later KVM/vector2 changes. Snapshot-backed fork-server remains a decision item. | Re-run the full pool/fork-server/syzkaller smoke set on the final KVM/vector2 stack and retire or complete snapshot-backed fork-server. |
| Active cleanup | The branch must read like normal kernel work. Active code cannot carry private issue numbers, phase diaries, or random branch history. | Review scans over active source, selftests, launcher, active UML docs, live status, and current vector2 trackers; archive or remove stale material. |
| Final validation matrix | Individual smokes do not prove the branch as a product. | Run the final integration gate and record exact pass/fail/skip, commit, upstream base, retired/deferred items, and push confirmation. |

## Execution Slice Plan From Current Head

The plan from `bb092119158b` is to finish one high-risk surface at a time and
push after each validated slice. The ordering is intentional: record/replay
changes can affect KVM syscall dispatch and time/signal handling, while vector2
publication still depends on long-run and performance/fairness evidence. The
measured guest-to-host TCP blocker is now closed and protected by the lazy-RX
cleanup. The no-TX-write-IRQ slice materially improves the 1 MiB
host-to-guest fd/multiqueue median, so remaining vector2 work should confirm
that improvement under broader repeats, fairness, CPU/syscall, seccomp/KVM
Tier 3, and long-run gates before treating the publication matrix as closed.

| Slice | Target | Implementation outcome | Required validation before commit |
| --- | --- | --- | --- |
| S0 | Plan and baseline refresh | This file records the current branch, closed KMSAN blocker, live record/replay evidence, and remaining order of work. | `git diff --check`; pushed docs-only commit. |
| S1 | Record/replay tier definition and ABI | First task-owned session-start gate is implemented: the record status reports syscall ownership counters, and the task helper proves a single process can snapshot itself, keep the focused workload on that task, and replay that log. Keep the public support label experimental until the remaining broader-policy gates pass. | Current status: `kvm-record-smoke` PASS with `KVM_RECORD_TASK: PASS pid=1 entries=393 syscalls=393 same=393 other=0 payload_entries=8 payload_bytes=528 replayed=393`; clock bench PASS. |
| S2 | Record/replay syscall payload and event format model | First payload models are implemented for `uname(2)`, `getcwd(2)`, `clock_gettime(2)`, `gettimeofday(2)`, and `time(2)`: the record log stores variable-sized payload entries, replay validates syscall number plus syscall-specific input arguments before restoring payload bytes, and the task-owned smoke records the full currently supported direct-syscall payload set with multiple raw-time argument shapes. Replay entries also carry explicit format version and flags, with debugfs exposing the current header/entry/payload sizes. Broader copyout coverage remains open. | Current status: `kvm-record-smoke` PASS with `KUnit=24/24`, `payload_entries=8`, `payload_bytes=528`, and `replayed=393`; `kvm-record-clock-bench` PASS. |
| S3 | Record/replay time, signal, and device policy | First fail-closed strict policy is implemented for the initial R/R-1 syscall subset: `getpid`, `getppid`, `gettid`, and payload-aware `uname(2)`, `getcwd(2)`, `clock_gettime(2)`, `gettimeofday(2)`, and `time(2)`. Unsupported syscalls now record a strict replay failure and receive SIGSEGV instead of silently falling back to live execution or scalar-only replay. Direct raw-time syscalls are replayed from payload bytes, and replay mode sets CR4.TSD so direct user timestamp reads fault. R/R-1 signal handling uses a blocked-delivery tier: replay mode blocks `SIGALRM` during `KVM_RUN`, leaving replayable signal-event ordering for later. Randomness and representative external-I/O syscalls (`getrandom`, `openat`, `read`, `write`, `ioctl`) fail closed; replayable device/network/hostfs payloads remain open. | Current status: `um_kvm_v2_record` KUnit covers the accepted subset, unsupported `getuid`, raw-time syscall checks, strict external-I/O/randomness policy, strict-on rejection, strict-off permissive behavior, strict replay failure accounting, and the strict raw-time policy. `kvm-record-smoke` reports `KUnit=24/24`, task-owned replay 393/393, `live-time=1`, `live-rdtsc=1`, `live-rdtscp=1`, `live-negative=1` for `getrandom`, and `live-external-io=4/4`; `kvm-record-clock-bench` passes. |
| S4 | Record/replay user-facing documentation | Update debugfs, Kconfig help, selftest README, and live inventory so users know the exact supported tier, limitations, and experimental status. | Documentation grep for stale stronger claims; `git diff --check`. |
| S5 | Vector2 publication gates | The net-bench harness, scatter-gather TX cleanup, diagnostics, TX/RX NAPI scheduling fix, lazy-RX cleanup, fake-host lazy RX fidelity gate, and no-TX-write-IRQ retry-timer slice are landed. The normal guest-to-host TCP gate passes at ratio 0.993 against the 0.85 bar, and `um_vector2_*` KUnit covers the production-style one-slot speculative RX read/release shape plus RX-only IRQ and TX-stall retry-timer behavior. The focused no-TX-write-IRQ 1 MiB host-to-guest fd/multiqueue rerun improves the host-side median vector2/vector ratio to 0.899069 from the prior 0.650602 sweep result, with transfer-window perf ratios lower than legacy vector for syscalls, task-clock, and context switches. UDP harness support, current-head repeat-2 buffered unpaced 1 MiB UDP, current-head repeat-2 paced 8 MiB UDP, an initial privileged perf-stat smoke, and bounded `perf-window.tsv`/aggregate/comparison support are landed; unpaced larger host-to-guest UDP remains bounded by byte-loss evidence. Next work is the rest of the publication matrix: natural seccomp long gate, full KVM-v2 Tier 3 networking, broader host-to-guest repeats/fairness, steady-state syscall-rate, full CPU-utilisation, and multiqueue fairness/performance tests. | Vector2 KUnit, fd/multiqueue/inproc/pool/sandbox smokes, TCP net-bench ratio >= 0.85, Tier 3 reports, and fairness/performance report. |
| S6 | Pool, fork-server, and syzkaller final rerun | Revalidate the pool/fork/syzkaller surfaces after record/replay and vector2 are stable. Decide snapshot-backed fork-server disposition. | Full pool/fork/syzkaller smoke set listed below. |
| S7 | Profile and instrumentation final matrix | Rebuild every UML profile and run matching runtime probes. KMSAN is a regression-protection gate here, not an open implementation gap. KGDB remains deferred unless implemented in a separate slice. | Profile config/build matrix; sanitizer/instrumentation smokes, including KMSAN. |
| S8 | Active-source cleanup pass | Review active source, selftests, launcher, non-redesign docs, live status docs, and current trackers for private history, stale phase labels, and unsupported claims. | Focused grep scans reviewed; checkpatch for touched source; launcher Rust and script syntax gates as needed. |
| S9 | Upstream queue and final integration gate | Regenerate patch-series planning from the final branch, run the full integration gate, update status/inventory/final note, commit, and push. | Final validation matrix; `next == origin/next`; upstream ancestry check. |

## Workstream Order

### 0. Entry Hygiene

Purpose: make every subsequent claim reproducible.

Tasks:

- Confirm `next` and `origin/next` match before each slice.
- Confirm `torvalds/master` is an ancestor of `next`.
- Keep unrelated local experiments out of commits.
- Remove temporary worktrees, scratch logs, and generated root directories after
  each validation slice.
- Run `git diff --check` before every commit.

Acceptance:

- `git status --short --branch`
- `git rev-list --left-right --count torvalds/master...next`
- `git merge-base --is-ancestor torvalds/master next`
- Clean or intentionally scoped worktree before staging.
- `git push` confirms `next` and `origin/next` match after each completed
  slice.

### 1. Current Correctness And User-Surface Mismatches

Purpose: fix any current `next` behavior where the branch advertises something
that does not work, or where kernel state handling is unsafe.

Tasks:

- Finish the KVM v2 ioctl/failure-policy audit.
- Keep dynamic userspace validation beyond `/bin/true` and `dyn-loader`.
- Keep snapshot and snapshot ELF docs matched to current mconsole, debugfs,
  boot-time, and `umlctl` trigger paths.
- Audit active CLI help for commands that depend on absent kernel surfaces.
- Resolve any profile smoke that silently skips because the required profile
  binary was not actually built.

Acceptance:

- `make ARCH=um -j$(nproc)`
- KVM v2 KUnit suites.
- Focused selftests for changed surfaces.
- `umlctl --help` and subcommand help match implemented behavior.

### 2. Record/Replay Completion Decision And Implementation

Purpose: close the largest remaining original-plan gap.

Current state:

- Experimental Kconfig-gated core exists on `next`.
- Debugfs can start, stop, reset, and report a singleton record container.
- Live KVM syscall dispatcher can observe and replay syscall return values.
- Snapshot-backed record start exists.
- UML time-travel clock events can round-trip through the record log.
- Current live validation proves syscall recording and time-travel event replay:
  `kvm-record-smoke` records 3066 live syscalls with 0 drops, and
  `kvm-record-clock-bench` replays 100/100 observed clock events with 0
  mismatches.
- The task-owned helper now suppresses the debugfs control write from the
  record log, records the bounded scalar plus `uname(2)`, `getcwd(2)`, and
  direct raw-time argument-shape payload workload, replays 393/393 entries
  from that log, compares replayed raw-time bytes with the recorded values,
  and verifies no other-task contamination.
- The negative helper arms strict replay and proves an unsupported live
  `getrandom(2)` syscall is rejected instead of falling back.
- Raw-time interfaces beyond direct syscall/current UML vDSO wrappers,
  explicit signal-event ordering, and replayable device/network/hostfs events
  remain incomplete.

Decision required:

- Either complete a first supported replay tier and count it in the original
  mission, or keep record/replay experimental and exclude it from the 100%
  completion claim with an explicit rationale.

Default decision for this plan:

- Complete a bounded first tier, called **R/R-1**, on `next`.
- Keep `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL` and debugfs
  labels until the remaining broader-policy gates pass.
- Do not promise whole-system replay, arbitrary networking, arbitrary block
  I/O, or cross-kernel-version replay in R/R-1.
- Do promise deterministic replay for a single KVM v2 userspace task executing
  a bounded syscall/time workload from a task-owned snapshot, with strict
  mismatch detection and explicit fail-closed handling for unsupported events.

If completing:

- Define the supported workload tier.
- Define treatment of raw time, RDTSC, vvar, SIGALRM, randomness, network, and
  device I/O.
- Keep the hot path behind static keys.
- Preserve gadget bypass while recording.
- Keep deterministic workload selftests in the focused smoke suite.
- Document unsupported operations precisely.

Acceptance:

- `um_kvm_v2_record` KUnit passes.
- Live record smoke passes.
- Clock replay bench passes.
- Deterministic workload record/replay gate passes for the supported tier.
- User docs state exactly what is deterministic and what is not.

Concrete R/R-1 implementation slices:

1. **Task-owned session start.**
   Add a task-scoped record/replay runner or kselftest harness so the task
   whose KVM v2 state is snapshotted is also the task that executes the
   deterministic workload. The debugfs singleton can remain for inspection,
   but it cannot be the only proof because its current writer-task semantics
   do not represent a workload replay ABI.

2. **Versioned event format.**
   Keep the existing syscall and time-travel records, but make the supported
   R/R-1 log shape explicit enough to survive code review: event type, syscall
   number, selected arguments, return value, payload length, payload bytes
   where needed, clock anchor, flags, and overflow state. A replay mismatch
   must preserve enough cursor/status information for the selftest to explain
   what diverged. Status: the current in-memory log now records
   `format_version=1` and per-entry flags, exposes header/entry/payload sizes
   through debugfs, and has KUnit coverage for bad-version and bad-flags
   rejection with cursor preservation.

3. **Payload copyout support.**
   Syscalls that only return scalars are not enough for a credible replay
   claim. R/R-1 needs payload handling for the bounded workload's selected
   copyout syscalls, or the workload must explicitly avoid such syscalls and
   the docs must say so. The preferred path is payload support for a small
   reviewed set rather than a fake no-copyout demo. Status: `uname(2)` and
   `getcwd(2)` are the current reviewed copyout set, with core KUnit coverage
   and task-owned live record evidence.

4. **Time policy.**
   UML time-travel clock events already round-trip through the record log.
   R/R-1 must also define raw host time, vvar, and timestamp-instruction
   behavior. The safe
   first-tier answer is to route supported time observations through recorded
   syscall/event payloads and fail closed or disable unsupported vvar/timestamp
   paths in strict replay until a stronger implementation lands. Status:
   direct syscall and current UML vDSO wrapper calls for `clock_gettime(2)`,
   `gettimeofday(2)`, and `time(2)` replay from logged payload bytes, including
   multiple task-owned raw-time argument shapes. Replay mode sets CR4.TSD so
   direct user timestamp reads fault instead of observing host time outside the
   log. Live smoke coverage now proves replay-mode `RDTSC` and `RDTSCP` fault;
   broader raw-time interfaces remain future work.

5. **Signal policy.**
   R/R-1 must define whether asynchronous signal delivery is unsupported,
   blocked during the deterministic gate, or recorded with explicit siginfo and
   delivery-order events. If the first tier excludes SIGALRM and host-injected
   signals, strict replay must detect and report that exclusion rather than
   silently falling back to live behavior. Status: R/R-1 now uses the blocked
   delivery tier for UML timer preemption. Replay mode reprograms KVM's
   per-vCPU signal mask to block `SIGALRM` while inside `KVM_RUN`, while normal
   KVM execution keeps `SIGALRM` unblocked. Pending UML timer work is handled
   after VM exit; precise asynchronous signal-event ordering is outside the
   current R/R-1 contract.

6. **Device and randomness policy.**
   Network, block, hostfs mutation, and randomness are not allowed to be
   implicit best-effort behavior. Either record the selected payloads for the
   deterministic workload, or reject those operations in strict replay. Keep
   vector2/Tier 3 workloads outside the R/R-1 completion claim unless their
   device payload policy is implemented. Status: R/R-1 rejects randomness and
   representative external-I/O syscalls (`getrandom`, `openat`, `read`,
   `write`, `ioctl`) in strict replay. Replayable device/network/hostfs events
   remain future work.

7. **Deterministic workload selftest.**
   Add a selftest that records a bounded workload, restores the task snapshot,
   replays the log, verifies the same user-visible result, and also exercises
   at least one strict divergence path. The test should fail if replay merely
   re-executes live syscalls or if it snapshots the wrong task. Status:
  `kvm-record-smoke` now includes the task-owned 393-entry replay leg, live
  raw-time and RDTSC/RDTSCP rejection legs, plus live-negative
  `getrandom(2)` and external-I/O strict rejection legs.

8. **Documentation and status update.**
   Update debugfs docs, Kconfig help, the inventory, and `STATUS.md` with the
   exact R/R-1 contract. If R/R-1 remains experimental after implementation,
   the final completion note must say record/replay is present as an
   experimental tier and list the unsupported operations.

### 3. Snapshot, Fork-Server, And Pool Closure

Purpose: ensure startup acceleration and syzkaller-facing pool behavior are
complete and not just historically present.

Current state:

- Snapshot capture, restore, and ELF export are present and validated.
- SMP snapshot behavior is bounded to one online CPU.
- Template pause, fork-on-resume, pool spawn, pool serve, pool take, daemon
  exec, ready members, pool benchmark, port-forward, and syzkaller shim all
  have current smoke evidence.
- The 2026-06-11 current-HEAD rerun on rebuilt `bb092119158b` also covers
  replicated sustained-pool mode, pivot mode, per-member mconsole path
  synthesis, full pool-bench, and syzkaller-style take/exec/port-forward/
  status/destroy.
- Per-take pool fd handoff is retired from the current claim in favor of the
  validated TAP reopen path.

Remaining tasks:

- Keep the final `exec/1` ABI explicit.
- Decide whether any stricter `exec/2` kernel argv transport is required now
  or is future work.
- Keep request-specific lazy takes separate from daemon-assigned ready-member
  mode.
- Re-run pool and syzkaller smokes after vector2 and KVM v2 changes.
- Keep snapshot-backed fork-server as either completed, explicitly deferred, or
  retired.

Acceptance:

- `template-pause-smoke`
- `template-pause-fork-smoke`
- `template-pause-fork-stress`
- `template-pause-pool-member-smoke`
- `template-pause-pool-sustained-smoke` with `UML_POOL_REPLICATE=1`
- `template-pause-pivot-smoke`
- `pool-spawn-smoke`
- `pool-serve-smoke`
- `pool-exec-smoke`
- `pool-port-forward-smoke`
- `pool-mconsole-path-probe`
- `pool-bench`
- `syzkaller-shim-smoke`

### 4. Vector2 Publication Readiness

Purpose: make networking claims match runtime behavior.

Current state:

- TAP and inherited-fd netdev datapaths are implemented.
- Launcher-owned fd handoff, multiqueue fd handoff, in-process TAP, failed-open
  validation, sandbox audit, pool TAP reopen, and stopped-clean long seccomp
  Tier 3 evidence exist.
- Current-head rebuilt `98166580dc4f` validation adds fresh vector2 KUnit
  evidence, focused live smoke reruns, and a bounded KVM-v2/vector2 Tier 3
  path smoke for one Django-v2 and one FastAPI-v2 iteration.
- The current `cb7ee7d69471` tree adds the kselftest-integrated TCP net-bench
  harness and removes the forced vector2 TX linearization path for fd and TAP
  backends.
- TCP guest-to-host throughput was the current measured publication blocker.
  The first integrated run completed all iterations but failed the 0.85
  replacement-readiness gate at 0.476. Scatter-gather TX improved the ratio to
  0.573, and a local bounded `sendmmsg()` prototype did not improve the ratio.
  The follow-up TX/RX NAPI scheduling fix passes the normal gate at 0.926:
  legacy vector median 40080.5 Mbps versus vector2 median 37116.0 Mbps.
- Lazy RX keeps that normal gate green at 0.993 and collapses representative
  1 MiB host-to-guest RX-slot over-preparation from 32448 prepared / 906
  received / 31542 released slots to about 1410 prepared / 905 received / 505
  released slots.  The focused 1 MiB host-to-guest ratio remains only 0.594,
  so this is a churn cleanup rather than the final small-transfer fix.
- RX checksum feature reporting now matches the vnet-header receive path:
  vector2 reports `rx-checksumming: on [fixed]` when `csum=1`, while the
  normal guest-to-host TCP gate remains green at ratio 0.990.  This removes a
  legacy-vector parity mismatch but does not close the 1 MiB host-to-guest
  cell.
- fd/vnet RX allocation now follows the channel runtime `vnet_hdr` state
  instead of the configured transport enum.  This closes an inherited-fd TAP
  framing correctness bug and is covered by `um_vector2_*` KUnit 90 pass, 0
  fail, 2 trusted-TAP skips plus fd handoff, fd multiqueue, and in-process TAP
  smokes.  The focused 1 MiB host-to-guest rerun reported vector2 median
  0.895 MiB/s versus legacy vector median 0.976 MiB/s, so the publication
  blocker remains open.
- UDP fixed-byte coverage now exists in the same helper through
  `UML_VECTOR_PERF_PROTOCOL=udp`.  TCP remains the default.  Current-head
  validation covers a TCP compatibility smoke, a vector2 UDP bidirectional
  64 KiB smoke, a repeat-2 buffered unpaced 1 MiB UDP matrix, and a repeat-2
  paced 8 MiB UDP matrix.  In the repeat-2 buffered unpaced 1 MiB run,
  vector2/vector host-side ratios were 0.998392 guest-to-host and 1.125627
  host-to-guest.  In the repeat-2 paced 8 MiB run with
  `UML_VECTOR_PERF_UDP_PACE_USEC=20`, vector2/vector host-side ratios were
  0.992588 guest-to-host and 1.003083 host-to-guest.  Earlier unpaced 8 MiB
  host-to-guest evidence still lost bytes before exact-byte completion for
  both legacy vector and vector2, so larger unpaced host-to-guest UDP remains
  bounded rather than accepted.
- The fixed-byte helper now appends endpoint process CPU timing to the same
  result stream.  Guest `VECTOR_NET_PERF` and host `HOST_SINK`/`HOST_SEND`
  lines include `cpu_seconds=...`, and `summary.tsv` appends
  `guest_cpu_seconds` plus `host_cpu_seconds`.  Validation covers `bash -n`,
  `git diff --check`, vector2 TCP 64 KiB guest-to-host smoke, paced vector2
  UDP 64 KiB bidirectional smoke, and cleanup checks for stale `vperf-*` TAPs
  and live `umlctl` instances.  This is per-process endpoint timing only; the
  stronger P4.3 syscall-rate and full CPU-utilisation gate remains open.
- The fixed-byte helper now samples `umlctl metrics --json` around
  host-to-guest transfers and appends UML host-process user/system CPU seconds,
  scheduler run/wait seconds, scheduler pcount, context-switch deltas, and the
  before/after metrics JSON paths.  Validation covers `bash -n`,
  `git diff --check`, vector2 TCP 64 KiB host-to-guest smoke with real metric
  deltas, side-by-side legacy vector/vector2 TCP 64 KiB host-to-guest smoke
  with 22-field summary rows for both drivers, vector2 TCP 64 KiB
  guest-to-host shape smoke with `NA` metric fields, and cleanup checks for
  stale `vperf-*` TAPs and live `umlctl` instances.  Unprivileged
  `perf stat -e syscalls:sys_enter_*` remains blocked by
  `perf_event_paranoid=4`, but privileged `sudo -n perf stat` now works on
  this host.
- A first privileged per-driver perf-stat smoke wrapped one 64 KiB
  host-to-guest TCP fixed-byte run for legacy vector and vector2.  The
  subtree-wide perf CSVs recorded 367 syscall tracepoint events per run:
  legacy vector reported 4,294,127 total syscalls, 111,886.52 ms task-clock,
  and 152,987 context switches; vector2 reported 510,383 total syscalls,
  8,725.39 ms task-clock, and 71,373 context switches.  This proves privileged
  syscall/CPU accounting is available and gives the next pass a concrete
  output shape, but it includes setup, boot, transfer, and teardown and does
  not close the steady-state P4.3 syscall-rate/full-CPU gate.
- The fixed-byte helper now supports an opt-in host-to-guest transfer-window
  perf mode.  `UML_VECTOR_PERF_PERF_STAT=1` resolves the live UML PID after
  guest sink readiness, runs bounded `sudo -n perf stat -p <pid>`, and writes
  parsed counters to `perf-window.tsv`, `perf-window-aggregate.tsv`, and
  `perf-window-comparison.tsv`.  A two-driver 64 KiB TCP smoke completed with
  non-empty raw perf CSVs: legacy vector recorded 85,743 total syscalls,
  1,195.24 ms task-clock, and 9,895 context switches; vector2 recorded 65,186
  total syscalls, 984.71 ms task-clock, and 6,582 context switches.  A
  follow-up two-repeat 1 MiB TCP smoke validated median aggregate rows and
  vector2/vector comparison ratios.  This is closer to the final measurement
  shape because it excludes most boot/setup cost, but it remains a short smoke
  rather than a same-throughput steady-state comparison.
- The first 1 MiB host-to-guest run with those metric columns keeps the
  small-transfer blocker open while narrowing the next investigation target.
  Four repeats each for legacy vector and vector2 passed. Host-side MiB/s
  medians were legacy vector 0.9845 and vector2 0.9005, ratio 0.9147; best
  observed host-side rates were legacy vector 1.630 and vector2 1.085, ratio
  0.6656. Median UML process scheduler pcount was 26547.5 for vector2 versus
  2213.0 for legacy vector, and median voluntary context switches were 26505.5
  versus 2186.5.
- A follow-up parameter/topology sweep ran five 1 MiB host-to-guest TCP cells,
  each with three repeats per driver and transfer-window perf collection:
  default fd/multiqueue, 16 KiB host sender chunk, `TCP_NODELAY`, single-queue
  fd, and in-process TAP.  Default fd/multiqueue vector2 still trailed at a
  host-side median ratio of 0.650602.  `TCP_NODELAY` and in-process TAP were
  also below the bar at 0.560706 and 0.639896.  The 16 KiB chunk cell was
  noisy and not a closure: its median ratio was above 1.0 only because legacy
  vector produced two slow outliers, and its best-run ratio was 0.530945.
  Single-queue fd mode was the strongest hint, with median ratio 0.810510 and
  best ratio 0.906732, but still does not close the median no-regression bar.
  The next implementation pass should focus on vector2 queue selection,
  wakeup, and receive scheduling policy rather than fd handoff alone.
- Vector2 now suppresses permanent TAP/fd `IRQ_WRITE` registration and uses a
  bounded retry timer only when a TX poll has queued descriptors but makes no
  progress.  This removes always-write-ready host-fd wakeups from receive-only
  workloads while keeping transient TX backpressure from stalling.  Validation
  reports a clean UML rebuild; `um_vector2_*` KUnit 96 pass, 0 fail, 2
  trusted-TAP skips with RX-only IRQ and TX-stall retry-timer coverage; fd
  handoff, fd multiqueue, in-process TAP, pool TAP, and sandbox audit smokes
  PASS; and a vector2 64 KiB guest-to-host TCP sanity run PASS.  The focused
  1 MiB host-to-guest default fd/multiqueue rerun improved the host-side
  median vector2/vector ratio to 0.899069 from the prior 0.650602 sweep
  result.  Transfer-window perf ratios also improved in this bounded run:
  vector2 used 0.315536x legacy vector syscalls, 0.362255x task-clock, and
  0.205458x context switches.  This is not final publication closure because
  vector2 still trails on best host throughput at 0.682038x and remains about
  1.916x legacy vector on scheduler pcount/voluntary context-switch deltas.
- The benchmark now captures guest link/route/feature/counter diagnostics
  around the sender, and vector2's ethtool stats now expose whether an
  inherited fd channel is actually using vnet-header framing.
- GRE/L2TPv3 helpers are parser/header coverage only.
- Raw, proxy, VDE, BESS, and hybrid transports are not current netdev runtime
  transports.

Remaining tasks:

- Follow up the host-to-guest 1 MiB fixed-byte cell with larger repeat counts
  and the full publication matrix while keeping the guest-to-host ratio at or
  above the 0.85 gate. Lazy RX removed RX-slot over-preparation, fd/vnet
  allocation now follows the live channel framing state, RX-reschedule probes
  reduced churn without closing throughput, and suppressing idle TX write IRQs
  moved the default fd/multiqueue host-side median ratio to 0.899069. The
  remaining work is to make that improvement hold under broader repeats,
  fairness, Tier 3, and steady-state CPU/syscall gates; the latest bounded run
  still shows vector2 below legacy on best throughput and about 1.916x legacy
  scheduler pcount/voluntary context-switch deltas.
- Expand the remaining P4.3 measurements: steady-state syscall rate and full CPU utilisation against legacy
  vector.  The new fixed-byte endpoint CPU columns, host-to-guest UML process
  deltas, `perf-window.tsv` rows, and perf-window aggregate/comparison TSVs
  are diagnostic aids and measurement scaffolding, not substitutes for that
  acceptance gate.
- Finish the natural seccomp/vector2 long run.
- Run the same full Tier 3 workload family under KVM v2; the one-iteration
  Django-v2/FastAPI-v2 path smoke is not the full gate.
- Add or refresh multiqueue fairness and performance gates.
- Keep Kconfig/help text from saying v2 supersedes legacy vector until the
  publication gates justify that claim.
- Keep parser-only transports explicitly unsupported by runtime code.
- Re-run pool/syzkaller smokes on the final vector2 stack.

Acceptance:

- Vector2 KUnit suites.
- `vector2-fd-handoff-smoke`
- `vector2-fd-multiqueue-smoke`
- `vector2-inproc-tap-smoke`
- `vector2-pool-tap-smoke`
- `vector2-sandbox-audit`
- `vector2-failed-open`
- `net-bench` TCP guest-to-host throughput ratio at or above the selected
  publication gate, currently 0.85.
- Seccomp Tier 3 long run.
- KVM v2 Tier 3 networking run.
- Multiqueue fairness/performance report.

### 5. Profiles And Instrumentation

Purpose: ensure profile claims are real builds and real runtime behavior.

Current state:

- Profile config matrix exists.
- KASAN, KFENCE, KCSAN, KCOV, kprobes, ftrace, and BPF/JIT have focused
  validation evidence.
- KMSAN profile configuration, a clean LLVM `uml/research-kmsan` build, and
  runtime smoke evidence are present. The current KMSAN gate reaches
  `KMSAN_SMOKE: PASS runtime=y reproducer=n`; rerun it in the final matrix to
  protect the closure after later KVM/vector2/profile changes.
- KGDB is deferred-not-present: the current tree does not select
  `HAVE_ARCH_KGDB`, no live UML profile fragment enables `CONFIG_KGDB`, and
  the active profile docs no longer claim KGDB as available.

Remaining tasks:

- Build each kernel profile from a clean worktree.
- Run each profile's runtime probe against a matching binary.
- Keep KGDB out of current feature claims unless a future slice implements
  UML `HAVE_ARCH_KGDB`, backend register access, transport support, and a
  smoke test.
- Rerun KMSAN runtime validation as a regression gate.
- Keep ftrace claims bounded to supported tracing modes.
- Ensure `umlbuild` profiles and kernel Kconfig profiles do not drift.

Acceptance:

- `make ARCH=um O=<out> uml/<profile>` for every kernel profile.
- Runtime profile harness for every profile with a matching binary.
- Focused sanitizer/instrumentation smokes:
  - `cve-repro`
  - `kfence-smoke`
  - `kcsan-smoke`
  - `kcov-smoke`
  - `kmsan-smoke`
  - `kprobes-stress`
  - `ftrace-smoke`
  - `bpf-jit-smoke`
- `umlbuild profile show` for every launcher profile.

### 6. Source, Selftest, And Documentation Cleanup

Purpose: make the work look like maintainable kernel code rather than a
research notebook.

Review scope:

- `arch/um/`
- `tools/testing/selftests/um/`
- `tools/uml/uml-launcher/`
- `Documentation/virt/uml/*.rst`
- `Documentation/virt/uml/redesign/STATUS.md`
- `Documentation/virt/uml/redesign/06-sequencing/`
- Any active files under `Documentation/virt/uml/redesign/08-future-phases/`
  that still serve as current status trackers.

Cleanup rules:

- Remove internal bug IDs from active source and tests.
- Remove diary comments and investigation timelines from active code.
- Replace branch labels, phase labels, and memo references with durable feature
  names.
- Keep comments that explain invariants, locking, ABI, memory ownership,
  failure policy, or security boundaries.
- Move useful history into clearly marked archival redesign docs.
- Mark old reports, slides, and generated PDFs as historical unless they are
  regenerated from a fresh cutoff.
- Remove stale user-facing claims instead of weakening them with caveats in
  multiple places.

Useful scans:

- `rg -n "TODO|FIXME|HACK|XXX|temporary|workaround|memo|phase|diary|journal|investigation|internal|#[0-9]+" arch/um tools/testing/selftests/um tools/uml/uml-launcher Documentation/virt/uml`
- `rg -n "not implemented|future work|stale|historical-only|experimental" Documentation/virt/uml tools/testing/selftests/um tools/uml/uml-launcher`
- `rg -n "workstream|decision log|branch|commit [0-9a-f]{7,40}" arch/um tools/testing/selftests/um tools/uml/uml-launcher Documentation/virt/uml`

Acceptance:

- Each scan is reviewed, not blindly zeroed.
- Remaining matches are either normal kernel terms, test fixtures, explicit
  archival material, or tracked active blockers.
- No active source comment reads like a private note to ourselves.
- `STATUS.md` remains a status page, not a journal.

### 7. Upstream Series Shape

Purpose: make the final branch splittable into reviewable upstream patches.

Tasks:

- Rebuild the upstream submission queue from final `next`.
- Regenerate cover letters after final validation.
- Split mechanical cleanup, Kconfig/profile work, KVM v2 core, snapshot,
  vector2, selftests, launcher tooling, and docs into reviewable groups.
- Run `checkpatch.pl` on staged patch groups.
- Confirm new files have SPDX identifiers where required.
- Ensure generated reports are not mixed into upstream-facing patch series
  unless intentionally submitted.

Acceptance:

- Updated `upstream-patches/SUBMISSION-QUEUE.md`.
- Patch groups apply cleanly to the intended base.
- `scripts/checkpatch.pl --strict` output is reviewed for each staged group.

### 8. Final Integration Gate

Purpose: prove the branch as a product, not a collection of individual smokes.

Minimum final gate:

- Full UML build with the default integration config.
- KVM v2 KUnit suites.
- Snapshot KUnit and runtime smokes.
- Record/replay selected-tier smokes, or explicit experimental exclusion.
- Pool and fork-server smokes.
- Syzkaller shim smoke.
- Vector2 smokes and Tier 3 networking.
- Profile build matrix and runtime probes.
- Launcher `cargo fmt --check` and `cargo test`.
- Python/shell syntax checks for touched selftest scripts.
- `git diff --check`.
- Focused source/comment cleanup scans.
- Branch state check against `origin/next` and `torvalds/master`.

The final completion note must include:

- exact `next` commit;
- upstream base commit;
- pass/fail/skip summary;
- explicit retired/deferred items;
- known non-blocking limitations;
- push confirmation.

## Work Package Checklist

| ID | Work package | Status | Blocking completion? | Exit condition |
| --- | --- | --- | --- | --- |
| W0 | Branch hygiene and current baseline tracking | Open continuous | Yes | Every slice starts clean, ends pushed, and records base/validation. |
| W1 | KVM v2 core hardening and dynamic-userspace expansion | Open | Yes | KVM v2 smoke, KUnit, CPython/Tier 3 subset, and failure-policy audit pass. |
| W2 | Snapshot capture/restore/ELF export | Substantially closed | Yes | Keep current KUnit/live export/restore smokes green after later changes. |
| W3 | Record/replay completion or explicit experimental exclusion | Open | Yes | Supported replay tier passes, or exclusion is approved and documented. |
| W4 | Fork-server, pool, daemon exec, and syzkaller path | Mostly closed | Yes | Full pool/syzkaller smoke set passes on final KVM/vector2 stack. |
| W5 | Vector2 publication readiness | Open, host-to-guest small-transfer blocker | Yes | Normal TCP net-bench stays above the selected publication gate, the 1 MiB host-to-guest gap is fixed or explicitly bounded, and seccomp/KVM v2 Tier 3 plus multiqueue/fairness evidence pass. |
| W6 | Profiles and instrumentation | Open | Yes | All profile builds and runtime probes pass, including KMSAN rerun and KGDB disposition. |
| W7 | Active code/comment/doc cleanup | Open continuous | Yes | Focused scans reviewed and active surfaces are free of random history. |
| W8 | Upstream queue refresh | Open | No for local completion, yes for publication | Submission queue and patch boundaries regenerated from final `next`. |
| W9 | Final integration gate | Open | Yes | Full gate passes and final status/inventory are updated. |

## Retirement Policy

A feature may be retired only if the final docs name it explicitly and explain:

- what was originally planned;
- where the historical implementation or discussion lives;
- why it is not part of the current completion claim;
- what user-facing surface, if any, remains; and
- what future work would be required to revive it.

Retirement is not the same as silently leaving stale code, stale tests, or stale
documentation in place.

## Immediate Next Actions

Execute the remaining work in this order unless a blocker forces a narrower
detour:

1. Start every slice from clean, pushed `next` at `origin/next`, and record the
   `torvalds/master...next` count before making claims about upstream currency.
2. Preserve the fixed vector2 TCP guest-to-host gate on the final stack. The
   harness, scatter-gather TX fix, diagnostics, and TX/RX NAPI scheduling fix
   are landed; lazy RX keeps the latest normal gate green at 0.993 against the
   0.85 bar. Rerun it after later vector2/KVM changes and extend the matrix to
   host-to-guest small-transfer, steady-state syscall-rate, and full
   CPU-utilisation evidence. The helper-level endpoint CPU timing is now
   available for focused runs, and host-to-guest runs now add UML process
   metric deltas, but the full acceptance gate remains open.
3. Finish vector2 publication evidence after the 1 MiB host-to-guest
   fixed-byte gap and the explicitly bounded larger unpaced UDP caveat:
   natural 7200-second seccomp Tier 3, full KVM-v2 Tier 3 networking coverage,
   and multiqueue fairness/performance. Keep v2 opt-in until those gates
   justify stronger language.
4. Finish the remaining R/R-1 record/replay closure. Task-owned session start,
   `uname(2)`/`getcwd(2)` payload models, direct raw-time payload replay,
   strict unsupported syscall rejection, versioned in-memory event format,
   signal blocking, device/randomness fail-closed policy, and deterministic
   task-owned replay are landed. Continue with broader workload policy,
   broader divergence coverage as new syscalls are added, selftest hardening,
   and documentation slices in order.
5. Re-run the pool, fork-server, daemon exec, and syzkaller smoke set on the
   final KVM/vector2 stack. Keep `exec/1` as the supported ABI unless a separate
   `exec/2` kernel argv transport is deliberately implemented and tested.
6. Complete profile and instrumentation closure: all kernel profile builds,
   runtime profile probes, focused sanitizer/instrumentation smokes, the KMSAN
   regression rerun, and KGDB exclusion or implementation.
7. Run the active-source cleanup scans over `arch/um`, UML selftests,
   `uml-launcher`, non-redesign UML docs, live status docs, and current
   sequencing/future-phase trackers. Rewrite active comments into normal kernel
   style and move useful history to clearly archival docs.
8. Regenerate upstream patch-series planning from the final branch shape,
   after implementation and cleanup stabilize.
9. Run the final integration gate, update `STATUS.md`, update the inventory,
   write the final completion note with pass/fail/skip and deferred/retired
   items, commit, push, and verify `next == origin/next`.
