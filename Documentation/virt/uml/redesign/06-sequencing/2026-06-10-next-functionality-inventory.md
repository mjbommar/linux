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
| `next` | `d0fecac57c26` | 2026-06-10 | Authoritative integration target. |
| `origin/next` | `d0fecac57c26` | 2026-06-10 | Remote tracking ref for `next`. |
| `kvm-v2-snapshot-elf64` | `fe9616e221c7` | 2026-05-21 | KVM v2 snapshot, ELF export, record, and state trace source branch. |
| `fork-server-phase1c` | `df19046e0b49` | 2026-05-21 | Historical fork-server phase branch. |
| `memo09-phase2` | `d933f95f06c2` | 2026-05-21 | Historical identity apply phase branch. |
| `memo09-phase3-pool-bench` | `64d5eef34ca1` | 2026-05-21 | Historical pool benchmark and acceptance branch. |
| `memo09-phase4` | `83ab00dc2f33` | 2026-05-21 | Historical pool/fork-server landed branch. |
| `experiment-path-c` | `73c98eabd31b` | 2026-05-21 | Historical path-C experiment branch. |
| `umlctl-deploy` | `ea849a35e20c` | 2026-05-25 | Historical launcher/deploy/gate branch. |
| `torvalds/master` | `acb7500801e9` | 2026-06-10 | Linus baseline used for current up-to-date check. |

## Status Labels

- `Present`: code exists on `next` and has not been found broken by this
  inventory.
- `Present-KUnit-pass`: code exists on `next` and the named KUnit gate has
  passed, but broader runtime/tool validation may still be open.
- `Present-validated`: code exists on `next` and the named runtime/tool gate
  has passed.
- `Present-validated-needs-decision`: code exists on `next` and the named
  runtime/tool gate has passed, but a final ABI, support, or publication
  decision remains open.
- `Present-needs-fix`: code exists on `next`, but a correctness, validation,
  documentation, or user-surface issue is known.
- `Historical-only`: functionality exists only on one or more historical
  branches.
- `Partial`: some of the feature exists on `next`, but required behavior is
  missing.
- `Partial-needs-fix`: some of the feature exists on `next`, and a known
  implementation fix is required before it can count as complete.
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
| KVM snapshot | Register-only snapshot | Present-KUnit-pass | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; runtime smoke still pending. | `um_kvm_v2_snapshot` KUnit PASS 4/4, 2026-06-10. |
| KVM snapshot | Full memslot snapshot capture | Present-validated | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; live export smoke covers metadata-only large-slot behavior; SMP is gated to one online CPU. | `um_kvm_v2_snapshot` KUnit PASS 4/4 plus live export smoke PASS, 2026-06-10. |
| KVM snapshot | Snapshot restore | Present-validated | `kvm-v2-snapshot-elf64` | Kernel code imported and cleaned; SMP is gated to one online CPU. | `um_kvm_v2_snapshot` KUnit PASS 4/4 plus `kvm-snapshot-restore-smoke` PASS, 2026-06-10. |
| KVM snapshot | Snapshot ELF64 export | Present-validated | `kvm-v2-snapshot-elf64` | Kernel exporter, debugfs trigger, mconsole trigger, boot-time export trigger, and `umlctl` export path are present; SMP is gated to one online CPU. | `umlctl snapshot export` PASS with `readelf -h/-l/-n`, `gdb -c`, and helper load; `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| KVM snapshot | GDB snapshot helper | Present-validated | `kvm-v2-snapshot-elf64`, `next` | Keep helper matched to UML private note layout. | helper loads against fresh exported core, 2026-06-10. |
| KVM snapshot | Snapshot selftests | Present-validated | `kvm-v2-snapshot-elf64`, `memo09-phase4` | Clean KUnit suite, restore smoke, benchmark wrapper, KUnit wrapper, and ELF roundtrip wrapper are present. | `um_kvm_v2_snapshot` KUnit PASS 4/4, `kvm-snapshot-restore-smoke` PASS, `kvm-snapshot-bench` PASS, `snapshot-kvm-smoke` PASS, and `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| KVM snapshot | Snapshot benchmark kselftest | Present-validated | `memo09-phase4` | Clean wrapper around current `kvm_v2_snapshot_bench=` imported. | `kvm-snapshot-bench` PASS, 2026-06-10. |
| KVM snapshot | Snapshot KUnit kselftest wrapper | Present-validated | `memo09-phase4` | Clean wrapper for the current four-case `um_kvm_v2_snapshot` suite imported. | `snapshot-kvm-smoke` PASS, 2026-06-10. |
| KVM snapshot | Snapshot ELF roundtrip kselftest | Present-validated | `memo09-phase4` | Clean wrapper imported using `kvm_v2_snapshot_elf_export=<host-path>` and host `readelf`/`gdb` validation. | `snapshot-elf-roundtrip` PASS, 2026-06-10. |
| Record/replay | Record state machine | Historical-only | `kvm-v2-snapshot-elf64` | Complete or land behind explicit experimental Kconfig. | record KUnit. |
| Record/replay | Syscall observe path | Historical-only | `kvm-v2-snapshot-elf64` | Complete; no-op stubs are not completion. | record smoke. |
| Record/replay | Replay consume path | Historical-only/needs-decision | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Implement deterministic replay tier or keep experimental. | replay smoke. |
| Record/replay | Gadget bypass in record mode | Historical-only | `kvm-v2-snapshot-elf64` | Required if gadget remains enabled. | gadget-on record smoke. |
| Record/replay | Time, vvar, RDTSC, SIGALRM determinism | Historical-only/needs-decision | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Define supported tier and implement before declaring complete. | deterministic workload gate. |
| Diagnostics | KVM v2 state trace ring | Historical-only | `kvm-v2-snapshot-elf64` | Rework as clean optional debug infrastructure. | enable/capture/dump/clear smoke. |
| Diagnostics | State trace parser | Historical-only | `umlctl-deploy`, `kvm-v2-snapshot-elf64` | Import only if state trace lands. | parser smoke. |
| Template pause | Kernel template pause entry | Present-validated | `next`, `memo09-phase*`, `fork-server-phase1c` | Historical comparison complete; smoke now finishes with bounded teardown. Vector2 leg skips when guest `vec0` is absent. | `template-pause-smoke` PASS cases 1-3, SKIP case 4, 2026-06-10. |
| Template pause | Identity blob layout and apply path | Present-validated | `next`, `memo09-phase2`, `memo09-phase4` | Current layout matches the final launcher contract by source comparison and live pool-member validation. | `template-pause-pool-member-smoke` PASS, 2026-06-10. |
| Template pause | Template pause pivot mode | Present-validated | `memo09-phase4`, `next` | Keep; pivot smoke passed on current `next`. | `template-pause-pivot-smoke` PASS, 2026-06-10. |
| Fork server | Fork-on-resume loop | Present-validated | `next`, `memo09-phase4` | Keep the current production path; the corrected smoke drives the second take and default stress now passes. | `template-pause-fork-smoke` PASS, child PIDs distinct and master resume cycles=2; `template-pause-fork-stress` PASS, 548 iterations, median 18.2 ms, 548/548 clean identities, 2026-06-10. |
| Fork server | Child PID reporting through identity memfd | Present-validated | `next`, `memo09-phase4` | Keep; validate in pool take and pool-member smoke. | `pool-serve-smoke` PASS and `template-pause-pool-member-smoke` PASS, 2026-06-10. |
| Fork server | Snapshot-backed fork-server path | Historical-only/needs-decision | `memo09-phase4`, `kvm-v2-snapshot-elf64` | Complete after KVM snapshot import or retire with approval. | snapshot fork smoke. |
| Pool | Direct `umlctl pool spawn` | Present-validated | `next`, `memo09-phase*` | Keep. | `pool-spawn-smoke` PASS, 2026-06-10. |
| Pool | Daemon `pool serve` | Present-validated | `next`, `memo09-phase4` | Keep and continue through warm-pool completion. | `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | `pool take` | Present-validated | `next`, `memo09-phase4` | Keep and extend for warm ready members. | `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | `pool list/status/destroy/shutdown` | Present-validated | `next`, `memo09-phase4` | Keep. | `pool-spawn-smoke` PASS and `pool-serve-smoke` PASS, 2026-06-10. |
| Pool | Warm member pool | Partial-needs-fix | `memo09-phase3-pool-bench`, `memo09-phase4`, `next` | Daemon-assigned `min_warm` ready members are implemented and validated; request-specific warm identity scheduling still needs a final API/contract decision. | `pool-serve-smoke` PASS with `--min-warm=1`, ready take, replenish, destroy, and shutdown, 2026-06-10. |
| Pool | Pool benchmark thresholds | Present-needs-fix | `memo09-phase3-pool-bench`, `memo09-phase4` | Reduced live-child benchmark passes; full default benchmark now closes the throughput gate and still needs RSS closure or a documented target revision. | Reduced raw `pool-bench` PASS 5/5 with p50 0.5 ms, p99 0.9 ms, and 148.6 MiB RSS; full `pool-bench` PASS 4/5, FAIL RSS 5,486.9 MiB/200 MiB with 3,964.5 MiB private dirty, and throughput PASS 3000/3000, 2026-06-10. |
| Pool | mconsole path synthesis | Present-validated | `next`, `memo09-phase4` | Keep the master-side bind plus child-side SIGIO rearm model; avoid the rejected child-side rebind experiment that panicked before `MEMBER_DONE`. | `pool-mconsole-path-probe` PASS with member alive, per-member socket present, and `version` reply; focused investigation in `2026-06-10-pool-mconsole-exec-investigation.md`, 2026-06-10. |
| Pool | `umlctl exec` via daemon | Present-validated-needs-decision | `next`, `memo09-phase4` | Keep the current bounded exec path for now; decide whether shell-backed command strings and the guest `timeout(1)` helper dependency are the final ABI before the completion claim. | `pool-exec-smoke` PASS: `/bin/true` exits 0, stdout/stderr capture round-trips, guest exit 7 is preserved without a daemon error, timeout returns code 124 with `timed_out=true`, late stdout is suppressed, no extra guest `sleep` helper leaks, and stale `Unknown command`/missing-host-tool boundaries are rejected, 2026-06-10. |
| Pool | `umlctl port-forward` | Present-validated | `next`, `memo09-phase4` | Keep and later validate against final network mode. | `pool-port-forward-smoke` PASS, 2026-06-10. |
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
| Launcher | snapshot export CLI | Present-validated | `next`, `kvm-v2-snapshot-elf64` | Keep mconsole-driven host export path. | live `umlctl snapshot export` smoke PASS, 2026-06-10. |
| Launcher | transparency tooling | Present-needs-validation | `next`, `umlctl-deploy` | Keep if docs/tests match. | transparency smoke. |
| Launcher | `umlbuild` | Present-needs-validation | `next`, `umlctl-deploy` | Keep and run MVP smoke. | `umlbuild` smoke. |
| Syzkaller | UML VM shim | Present-validated-needs-ABI-decision | `next`, `umlctl-deploy` | Keep the shim aligned with the final exec ABI; current take/exec/port-forward/status/destroy wire path is validated. | `syzkaller-shim-smoke` PASS: source contract check plus syzkaller-style take, exec output merge, port-forward, status, destroy, 2026-06-10. |
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
- KVM v2 snapshot KUnit coverage is present on `next` and passes under
  `backend=force=kvm-v2` with
  `kunit.filter_glob=um_kvm_v2_snapshot kunit_shutdown=halt`:
  4 pass, 0 fail, 0 skip.
- Live `umlctl snapshot export` works against a disposable KVM v2 hostfs
  guest and the resulting ELF parses with `readelf -h/-l/-n`, `gdb -c`, and
  `tools/uml/uml-gdb/uml-snapshot.py`.
- Snapshot restore smoke works through `kvm_v2_snapshot_bench=1` and the
  `kvm-snapshot-restore-smoke` selftest, which observes a capture plus
  `restore_full` timing summary in full snapshot mode.
- Snapshot SMP semantics are explicitly gated: capture and restore return
  `-EOPNOTSUPP` when more than one CPU is online.
- Daemon-routed pool exec now validates success, stdout/stderr capture, guest
  exit status preservation, timeout reporting, and guest helper cleanup through
  `pool-exec-smoke`; the remaining exec work is the final ABI/helper
  dependency decision.

## Remaining Hard Blockers

These items must be closed before the final branch can be called complete:

1. Record/replay functionality must be imported or completed.
2. Vector2 replacement claims must match validation evidence.
3. Pool/fork-server current tests must pass, including warm-pool, pool-member,
   and vector2 TAP/fd paths; the syzkaller-facing take/exec/destroy path now
   has a dedicated smoke gate.
4. Selftests and source comments must be cleaned of diary/history material on
   upstream-facing paths.
5. The final validation matrix from the integration plan must pass.

## Next Update Rules

When a workstream lands:

1. Change affected rows from `Open`/`Partial`/`Historical-only` to the new
   status.
2. Add the landed commit hash.
3. Add the validation command and result.
4. Update `STATUS.md` if the user-visible project status changed.
5. Leave historical branch references intact until the feature is either fully
   imported or explicitly retired.
