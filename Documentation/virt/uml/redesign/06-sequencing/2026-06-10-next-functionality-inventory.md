# UML v2 next Functionality Inventory

Date: 2026-06-10

Purpose: track every known UML v2 feature area that must either be present on
`next`, imported into `next`, completed on `next`, or explicitly retired before
the UML v2 effort can be called complete.

This file is the live execution tracker for
`2026-06-10-next-full-functionality-integration-plan.md`.

## Branch Baseline

| Ref | Commit | Date | Role |
| --- | ------ | ---- | ---- |
| `next` | `51a058582ccd` | 2026-06-10 | Authoritative integration target. |
| `origin/next` | `51a058582ccd` | 2026-06-10 | Remote tracking ref for `next`. |
| `kvm-v2-snapshot-elf64` | `fe9616e221c7` | 2026-05-21 | KVM v2 snapshot, ELF export, record, and state trace source branch. |
| `fork-server-phase1c` | `df19046e0b49` | 2026-05-21 | Historical fork-server phase branch. |
| `memo09-phase2` | `d933f95f06c2` | 2026-05-21 | Historical identity apply phase branch. |
| `memo09-phase3-pool-bench` | `64d5eef34ca1` | 2026-05-21 | Historical pool benchmark and acceptance branch. |
| `memo09-phase4` | `83ab00dc2f33` | 2026-05-21 | Historical pool/fork-server landed branch. |
| `experiment-path-c` | `73c98eabd31b` | 2026-05-21 | Historical path-C experiment branch. |
| `umlctl-deploy` | `ea849a35e20c` | 2026-05-25 | Historical launcher/deploy/gate branch. |

## Status Labels

- `Present`: code exists on `next` and has not been found broken by this
  inventory.
- `Present-needs-fix`: code exists on `next`, but a correctness, validation,
  documentation, or user-surface issue is known.
- `Historical-only`: functionality exists only on one or more historical
  branches.
- `Partial`: some of the feature exists on `next`, but required behavior is
  missing.
- `Stale-surface`: docs, tools, or tests on `next` refer to functionality that
  is not implemented on `next`.
- `Needs-decision`: the plan must decide whether to complete, keep
  experimental, or retire the feature.
- `Retired`: intentionally not part of the final branch. Retirement requires
  explicit user approval and a historical note.

## Feature Inventory

| Area | Feature | Current `next` status | Historical source | Required disposition | Validation gate |
| ---- | ------- | --------------------- | ----------------- | -------------------- | --------------- |
| KVM v2 core | Backend selection and VM/vCPU lifecycle | Present | `next` | Keep and harden. | KVM smoke, CPython parity. |
| KVM v2 core | Checked restore of saved XSAVE and VCPU events | Fixed-in-current-update | `next` | Keep checked fail-fast restore handling. | Build plus KVM v2 smoke. |
| KVM v2 core | LSTAR gadget and fallback syscall path | Present-needs-validation | `next`, `kvm-v2-snapshot-elf64` | Reconcile gadget default with current Tier 3 evidence. | Gadget on/off Tier 3 comparison. |
| KVM v2 core | XSAVE/YMM preservation after AVX exposure | Present | `next` | Keep; ensure no legacy FPU restore remains in active paths. | CPython/Tier 3 flake soak. |
| KVM v2 core | APERF/MPERF passthrough | Present | `next`, `umlctl-deploy` | Keep optional. | `aperf-mperf-smoke` if config enabled. |
| KVM v2 core | RDPMC userspace support | Present | `next`, `umlctl-deploy` | Keep optional and document sandbox tradeoff. | `rdpmc-smoke` if config enabled. |
| KVM v2 core | ITIMER_VIRTUAL accounting | Present | `next` | Keep; include in KVM smoke. | timer/CPython signal smoke. |
| KVM snapshot | Register-only snapshot | Present-needs-validation | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; add KUnit/smoke validation. | snapshot KUnit. |
| KVM snapshot | Full memslot snapshot capture | Present-needs-validation | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; validate metadata-only large-slot behavior and define SMP semantics. | snapshot KUnit and smoke. |
| KVM snapshot | Snapshot restore | Present-needs-validation | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; validate restore ordering and lazy-state reset. | snapshot restore smoke. |
| KVM snapshot | Snapshot ELF64 export | Present-needs-validation | `kvm-v2-snapshot-elf64` | Kernel exporter and debugfs trigger imported; validate via CLI/readelf/gdb. | `umlctl snapshot export`, `readelf`, `gdb`. |
| KVM snapshot | GDB snapshot helper | Present-needs-validation | `kvm-v2-snapshot-elf64`, `next` | Validate helper against a fresh exported core. | helper loads against exported core. |
| KVM snapshot | Snapshot selftests | Partial | `kvm-v2-snapshot-elf64`, `memo09-phase4` | Reconcile and import after KUnit priming helper is adapted to current vCPU code. | `snapshot-smoke`, `snapshot-kvm-smoke`, ELF roundtrip. |
| Record/replay | Record state machine | Historical-only | `kvm-v2-snapshot-elf64` | Complete or land behind explicit experimental Kconfig. | record KUnit. |
| Record/replay | Syscall observe path | Historical-only | `kvm-v2-snapshot-elf64` | Complete; no-op stubs are not completion. | record smoke. |
| Record/replay | Replay consume path | Historical-only/needs-decision | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Implement deterministic replay tier or keep experimental. | replay smoke. |
| Record/replay | Gadget bypass in record mode | Historical-only | `kvm-v2-snapshot-elf64` | Required if gadget remains enabled. | gadget-on record smoke. |
| Record/replay | Time, vvar, RDTSC, SIGALRM determinism | Historical-only/needs-decision | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Define supported tier and implement before declaring complete. | deterministic workload gate. |
| Diagnostics | KVM v2 state trace ring | Historical-only | `kvm-v2-snapshot-elf64` | Rework as clean optional debug infrastructure. | enable/capture/dump/clear smoke. |
| Diagnostics | State trace parser | Historical-only | `umlctl-deploy`, `kvm-v2-snapshot-elf64` | Import only if state trace lands. | parser smoke. |
| Template pause | Kernel template pause entry | Present-needs-comparison | `next`, `memo09-phase*`, `fork-server-phase1c` | Compare against phase branches and import missing semantics. | template-pause smoke/stress. |
| Template pause | Identity blob layout and apply path | Present-needs-comparison | `next`, `memo09-phase2`, `memo09-phase4` | Confirm current layout matches final tool contract. | identity KUnit/selftest. |
| Template pause | Template pause pivot mode | Partial/needs-decision | `memo09-phase4`, `next` | Decide whether pivot smoke remains required. | pivot smoke if kept. |
| Fork server | Fork-on-resume loop | Present-needs-comparison | `next`, `memo09-phase4` | Compare missing kernel/user semantics. | fork-server smoke. |
| Fork server | Child PID reporting through identity memfd | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate in pool take. | pool serve/take smoke. |
| Fork server | Snapshot-backed fork-server path | Historical-only/needs-decision | `memo09-phase4`, `kvm-v2-snapshot-elf64` | Complete after KVM snapshot import or retire with approval. | snapshot fork smoke. |
| Pool | Direct `umlctl pool spawn` | Present | `next`, `memo09-phase*` | Keep. | `pool-spawn-smoke`. |
| Pool | Daemon `pool serve` | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate. | `pool-serve-smoke`. |
| Pool | `pool take` | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate. | pool take/status smoke. |
| Pool | `pool list/status/destroy/shutdown` | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate. | pool lifecycle smoke. |
| Pool | Warm member pool | Partial/needs-decision | `memo09-phase3-pool-bench`, `memo09-phase4`, `next` | Implement or explicitly mark out of current completion. | pool benchmark. |
| Pool | Pool benchmark thresholds | Partial | `memo09-phase3-pool-bench`, `memo09-phase4` | Import final acceptance thresholds. | `pool-bench`. |
| Pool | mconsole path synthesis | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate against exec. | `pool-exec-smoke`. |
| Pool | `umlctl exec` via daemon | Present-needs-validation | `next`, `memo09-phase4` | Keep and test with mconsole availability detection. | exec smoke. |
| Pool | `umlctl port-forward` | Present-needs-validation | `next`, `memo09-phase4` | Keep and validate. | port-forward smoke. |
| Pool | TAP/fd handoff | Partial | `next`, `memo09-phase4`, `umlctl-deploy` | Complete with vector2 fd path. | fd handoff smoke. |
| Vector2 | Typed parser | Present | `next` | Keep. | vector2 parser KUnit. |
| Vector2 | Queue ownership | Present | `next` | Keep. | vector2 queue KUnit. |
| Vector2 | fd backend | Present-needs-validation | `next`, `umlctl-deploy` | Keep and test through launcher/pool. | fd handoff smoke. |
| Vector2 | tap backend | Present-needs-validation | `next`, `umlctl-deploy` | Keep and run networking gates. | tap smoke and Tier 3. |
| Vector2 | multiqueue | Partial | `next`, `umlctl-deploy` | Finish fairness/performance gates. | multiqueue perf/fairness. |
| Vector2 | raw/gre/l2tpv3/vde/bess/proxy/hybrid transports | Needs-decision | `next`, historical vector branches | Implement or remove parser/doc claims for unsupported modes. | transport-specific smoke. |
| Vector2 | sandbox mode | Present-needs-validation | `next` | Keep default-safe and audit. | vector2 sandbox audit. |
| Vector2 | in-process trusted mode | Present-needs-validation | `next` | Keep explicit and document authority. | trusted mode smoke. |
| Vector2 | replacement-readiness claim | Present-needs-fix | `next` docs/Kconfig | Align wording with validation. | docs/Kconfig audit. |
| Launcher | `umlctl up/down/ps/logs` | Present | `next`, `umlctl-deploy` | Keep. | `umlctl-smoke`. |
| Launcher | deploy configs | Present-needs-comparison | `next`, `umlctl-deploy` | Import missing useful configs only. | deploy smoke. |
| Launcher | gates and gate-loop | Present-needs-validation | `next`, `umlctl-deploy` | Keep and validate. | gate dry run. |
| Launcher | snapshot export CLI | Present-needs-validation | `next`, `kvm-v2-snapshot-elf64` | Kernel debugfs exporter is present; validate end-to-end from `umlctl`. | snapshot export smoke. |
| Launcher | transparency tooling | Present-needs-validation | `next`, `umlctl-deploy` | Keep if docs/tests match. | transparency smoke. |
| Launcher | `umlbuild` | Present-needs-validation | `next`, `umlctl-deploy` | Keep and run MVP smoke. | `umlbuild` smoke. |
| Syzkaller | UML VM shim | Present-needs-validation | `next`, `umlctl-deploy` | Validate against final pool/exec path. | syzkaller-style exec smoke. |
| Profiles | profile configs | Partial | `next`, historical docs | Build and test matrix required. | profile build matrix. |
| Instrumentation | kprobes | Present-needs-validation | `next` | Keep and test. | kprobes stress. |
| Instrumentation | ftrace | Present-needs-validation | `next` | Keep and test. | ftrace smoke. |
| Instrumentation | KMSAN | Present-needs-validation | `next` | Keep and test where config supports it. | KMSAN smoke. |
| Instrumentation | KASAN/KCSAN/KFENCE/KCOV | Partial/needs-validation | `next`, docs | Verify each profile and document gaps. | instrumentation matrix. |
| Instrumentation | BPF/JIT shims | Present-needs-validation | `next` | Keep if tests pass and docs are accurate. | BPF smoke if available. |
| Instrumentation | KGDB | Needs-decision | original plan/docs | Implement, document absent, or retire with approval. | KGDB smoke if kept. |
| Selftests | Upstreamable smoke/regression tests | Partial | all branches | Curate comments and prerequisites. | selftest target list. |
| Selftests | local stress/soak tests | Present | `next`, `umlctl-deploy` | Keep under clear local/soak docs. | selected soak gates. |
| Selftests | historical repros | Present/needs-cleanup | all branches | Move or mark archival. | grep for diary/internal labels. |
| Docs | `STATUS.md` | Present-needs-update | `next` | Update after every landed workstream. | status review. |
| Docs | old reports/presentations | Present-stale | `next`, historical docs | Refresh or mark historical. | docs audit. |
| Docs | snapshot ELF docs | Present-needs-validation | `next`, `kvm-v2-snapshot-elf64` | Re-audit against imported kernel code and fresh exported core. | trigger/symbol grep. |
| Docs | vector2 validation docs | Present-needs-update | `next` | Align with final gates. | docs audit. |
| Upstream | patch queue | Present-stale | `next` docs | Regenerate from final `next`. | checkpatch and cover letters. |

## Closed In Current Update

- KVM v2 no longer ignores failed architectural-state restore ioctls for
  `KVM_SET_MSRS`, `KVM_SET_XSAVE`, or `KVM_SET_VCPU_EVENTS`.
- KVM v2 snapshot capture/restore and snapshot ELF64 export source is present
  on `next` and builds with `make ARCH=um -j16`.

## Remaining Hard Blockers

These items must be closed before the final branch can be called complete:

1. Snapshot KUnit, snapshot smoke, `umlctl snapshot export`, `readelf`, `gdb`,
   and GDB helper validation must pass on the imported snapshot code.
2. Snapshot SMP constraints must be defined, gated, or validated.
3. Record/replay functionality must be imported or completed.
4. Vector2 replacement claims must match validation evidence.
5. Pool/fork-server behavior must be compared against all `memo09-*` branches.
6. Selftests and source comments must be cleaned of diary/history material on
   upstream-facing paths.
7. The final validation matrix from the integration plan must pass.

## Next Update Rules

When a workstream lands:

1. Change affected rows from `Open`/`Partial`/`Historical-only` to the new
   status.
2. Add the landed commit hash.
3. Add the validation command and result.
4. Update `STATUS.md` if the user-visible project status changed.
5. Leave historical branch references intact until the feature is either fully
   imported or explicitly retired.
