# UML v2 `next` Completion Execution Plan

Date: 2026-06-11

Branch target: `next`

Current committed baseline at plan write:

- `next`: `d562ad95451a`
- `origin/next`: `d562ad95451a`
- `torvalds/master`: `9716c086c8e8`
- `torvalds/master...next`: `0` commits behind, `116` commits ahead
- upstream ancestry: `torvalds/master` is an ancestor of `next`
- worktree state: clean at this plan refresh (`## next...origin/next`)

Current execution evidence added on 2026-06-11:

- rebuilt `./linux` from `next` at `59ad334001ea`;
- rebuilt kernel version: `7.1.0-rc7-00187-g59ad334001ea`;
- focused pool/fork/syzkaller regression pass recorded in
  `2026-06-11-pool-fork-regression-pass.md`;
- the pass covers template pause, fork smoke/stress, pool-member, replicated
  sustained pool, pivot, pool spawn/serve/exec/port-forward/mconsole, full
  pool benchmark, and syzkaller shim; and
- this is current-HEAD evidence, not a substitute for the final post-KVM/vector2
  validation matrix.

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

## Review Surface And Cleanup Calibration

The full UML v2 branch is too large for one undifferentiated cleanup pass. The
working review set should be split by whether a file is active implementation,
active user-facing documentation, validation infrastructure, current status, or
archival record.

Current surface relative to `torvalds/master` at this plan refresh:

| Area | Files in tree | Files changed vs. `torvalds/master` | Review posture |
| --- | ---: | ---: | --- |
| `arch/um/` | 314 | 206 | Full source review required for active KVM v2, vector2, snapshot, record/replay, profiles, and sanitizer paths. |
| `tools/testing/selftests/um/` | 289 | 289 | Full validation-surface review required; remove private labels and stale expectations from active tests. |
| `tools/uml/uml-launcher/` | 95 | 95 | Full CLI/help/config review required; match commands to live kernel surfaces. |
| `Documentation/virt/uml/` total | 452 | 451 | Split active user docs from redesign archive before editing. |
| Non-redesign UML docs | 36 | 35 | Active user-facing docs; must be current and upstream-style. |
| `Documentation/virt/uml/redesign/` | 416 | 416 | Mostly archive/status/planning; mark historical material instead of rewriting it all. |
| `redesign/06-sequencing/` | 43 | 43 | Current execution tracker; keep accurate and dated. |
| `redesign/08-future-phases/` | 59 | 59 | Mixed active trackers and historical notes; promote only live trackers to current truth. |
| `redesign/report-presentation/` | 42 | 42 | Historical May 2026 report/deck workspace unless regenerated from a new cutoff. |
| `redesign/upstream-patches/` | 26 | 26 | Regenerate after final branch shape; do not treat old cover letters as current. |

The changed active/review surface across source, selftests, launcher, and UML
docs is about 1,041 files in 158 directories. The practical cleanup target is
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
| Record/replay supported tier | The original vision names deterministic time-travel and record/replay as first-class functionality. Current `next` has an experimental core, live syscall hook, snapshot-backed start, and time-travel clock-event logging, but raw time/RDTSC/vvar, signals, device I/O, randomness, and deterministic workload policy are incomplete. | Define the first supported tier, implement missing policy, pass KUnit/live/deterministic workload gates, and document unsupported operations; or explicitly exclude record/replay from the completion claim as experimental. |
| KMSAN runtime | The original instrumentation goal includes KMSAN. Current `next` builds `uml/research-kmsan` with LLVM and fixes the vmalloc metadata layout, but `kmsan-smoke` still fails before the result marker. | Fix the UML/KMSAN runtime metadata/stack/context issue or document the exact kernel blocker and decide whether KMSAN can remain non-completion. |
| KGDB disposition | The original instrumentation list includes KGDB, but current UML does not select `HAVE_ARCH_KGDB` and no live profile fragment enables `CONFIG_KGDB`. | KGDB is deferred-not-present in the current completion tracker. Reintroduce it only with UML architecture support, backend register access, a transport decision, and a smoke test. |
| Vector2 publication readiness | Vector2 has strong focused and long seccomp evidence, but the replacement/publication claim still needs final Tier 3, KVM v2, and multiqueue/fairness coverage. | Finish the natural seccomp long run, run equivalent KVM v2 Tier 3 networking, add fairness/performance evidence, and keep parser-only transports out of runtime claims. |
| KVM v2 final workload breadth | KVM v2 is past architecture unknowns, but publication still needs broader dynamic-userspace and final-vector2 workload evidence. | Run Tier 3 and selected CPython/substrate gates on the final tree, including dynamic userspace beyond `/bin/true` and `dyn-loader`. |
| Pool/fork-server final regression pass | The rebuilt current-HEAD `59ad334001ea` binary passes the focused pool/fork/syzkaller regression set, including warm-pool, replicated sustained-pool, pool benchmark, and syzkaller shim. Final validation must still be rerun after later KVM/vector2 changes. Snapshot-backed fork-server remains a decision item. | Re-run the full pool/fork-server/syzkaller smoke set on the final KVM/vector2 stack and retire or complete snapshot-backed fork-server. |
| Active cleanup | The branch must read like normal kernel work. Active code cannot carry private issue numbers, phase diaries, or random branch history. | Review scans over active source, selftests, launcher, active UML docs, live status, and current vector2 trackers; archive or remove stale material. |
| Final validation matrix | Individual smokes do not prove the branch as a product. | Run the final integration gate and record exact pass/fail/skip, commit, upstream base, retired/deferred items, and push confirmation. |

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
- Raw time, vvar, RDTSC, signal, device, randomness, external I/O, and
  deterministic workload policy remain incomplete.

Decision required:

- Either complete a first supported replay tier and count it in the original
  mission, or keep record/replay experimental and exclude it from the 100%
  completion claim with an explicit rationale.

If completing:

- Define the supported workload tier.
- Define treatment of raw time, RDTSC, vvar, SIGALRM, randomness, network, and
  device I/O.
- Keep the hot path behind static keys.
- Preserve gadget bypass while recording.
- Add deterministic workload selftests.
- Document unsupported operations precisely.

Acceptance:

- `um_kvm_v2_record` KUnit passes.
- Live record smoke passes.
- Clock replay bench passes.
- Deterministic workload record/replay gate passes for the supported tier.
- User docs state exactly what is deterministic and what is not.

### 3. Snapshot, Fork-Server, And Pool Closure

Purpose: ensure startup acceleration and syzkaller-facing pool behavior are
complete and not just historically present.

Current state:

- Snapshot capture, restore, and ELF export are present and validated.
- SMP snapshot behavior is bounded to one online CPU.
- Template pause, fork-on-resume, pool spawn, pool serve, pool take, daemon
  exec, ready members, pool benchmark, port-forward, and syzkaller shim all
  have current smoke evidence.
- The 2026-06-11 current-HEAD rerun on rebuilt `59ad334001ea` also covers
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
  validation, sandbox audit, pool TAP reopen, and long seccomp Tier 3 evidence
  exist.
- Current-head rebuilt `98166580dc4f` validation adds fresh vector2 KUnit
  evidence, focused live smoke reruns, and a bounded KVM-v2/vector2 Tier 3
  path smoke for one Django-v2 and one FastAPI-v2 iteration.
- GRE/L2TPv3 helpers are parser/header coverage only.
- Raw, proxy, VDE, BESS, and hybrid transports are not current netdev runtime
  transports.

Remaining tasks:

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
- Seccomp Tier 3 long run.
- KVM v2 Tier 3 networking run.
- Multiqueue fairness/performance report.

### 5. Profiles And Instrumentation

Purpose: ensure profile claims are real builds and real runtime behavior.

Current state:

- Profile config matrix exists.
- KASAN, KFENCE, KCSAN, KCOV, kprobes, ftrace, and BPF/JIT have focused
  validation evidence.
- KMSAN profile configuration and a clean LLVM `uml/research-kmsan` build
  exist. The vmalloc metadata range alignment bug is fixed on `next`, but the
  runtime smoke still fails before its result marker with early KMSAN reports
  in kthread-name, scheduler, credential, and stack/string metadata paths.
- KGDB is deferred-not-present: the current tree does not select
  `HAVE_ARCH_KGDB`, no live UML profile fragment enables `CONFIG_KGDB`, and
  the active profile docs no longer claim KGDB as available.

Remaining tasks:

- Build each kernel profile from a clean worktree.
- Run each profile's runtime probe against a matching binary.
- Keep KGDB out of current feature claims unless a future slice implements
  UML `HAVE_ARCH_KGDB`, backend register access, transport support, and a
  smoke test.
- Finish KMSAN runtime validation or document the exact blocker.
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
| W5 | Vector2 publication readiness | Open | Yes | Seccomp and KVM v2 Tier 3 gates plus multiqueue/fairness evidence pass. |
| W6 | Profiles and instrumentation | Open | Yes | All profile builds and runtime probes pass, including KMSAN/KGDB disposition. |
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

1. Start from a clean, pushed `next` baseline or explicitly isolate any local
   experimental changes from the next commit.
2. Close the KMSAN/profile runtime question or document its exact blocker.
3. Decide the record/replay completion tier.
4. Run the final pool/syzkaller smoke set after any KVM/vector2 changes.
5. Finish vector2 KVM v2 Tier 3 and multiqueue/fairness validation.
6. Complete the active-source and selftest cleanup scan.
7. Update `STATUS.md` and the live inventory after each closed work package.
8. Push each validated slice to `origin/next`.
