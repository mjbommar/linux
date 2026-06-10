# UML v2 Pool/Fork Historical Comparison And Completion Plan

Date: 2026-06-10

Purpose: compare the active `next` pool, fork-server, template-pause, snapshot
test, and syzkaller-adjacent surfaces against the historical UML v2 branches,
then define the remaining work required before the functionality can be called
complete.

This file is part of the live execution plan that feeds
`2026-06-10-next-full-functionality-integration-plan.md` and
`2026-06-10-next-functionality-inventory.md`.

## Baseline

| Ref | Commit | Role |
| --- | ------ | ---- |
| `next` | `d0fecac57c26` | Active integration branch and only branch to ship. |
| `origin/next` | `d0fecac57c26` | Pushed remote state for `next`. |
| `torvalds/master` | `acb7500801e9` | Current Linus baseline used for up-to-date comparison. |
| `fork-server-phase1c` | `df19046e0b49` | Early pool daemon and fork-server phase branch. |
| `memo09-phase2` | `d933f95f06c2` | Template-pause identity apply phase branch. |
| `memo09-phase3-pool-bench` | `64d5eef34ca1` | Pool benchmark and acceptance-gate branch. |
| `memo09-phase4` | `83ab00dc2f33` | Later pool/fork-server branch with exec, port-forward, syzkaller-facing surfaces, and snapshot test wrappers. |
| `kvm-v2-snapshot-elf64` | `fe9616e221c7` | Snapshot ELF export, record/replay, and diagnostic source branch. |
| `experiment-path-c` | `73c98eabd31b` | Older experimental branch for record/replay and path-C investigations. |
| `umlctl-deploy` | `ea849a35e20c` | Launcher, deploy, gates, soak, and vector2 tooling branch. |

## Comparison Method

The comparison was intentionally file- and surface-driven. Direct branch merges
are unsafe because the historical branches contain old comments, branch-local
assumptions, incomplete diagnostics, and code that has already been cleaned or
superseded on `next`.

Commands used for this pass:

```sh
git ls-tree -r --name-only <ref> -- tools/uml arch/um/kernel arch/um/include \
	tools/testing/selftests/um Documentation/virt/uml |
	rg 'pool|template_pause|template-pause|fork|identity|exec|port_forward|port-forward|tapfd|snapshot|syzkaller|uml-template'
```

```sh
git diff --name-status next..<ref> -- tools/uml arch/um/kernel arch/um/include \
	tools/testing/selftests/um Documentation/virt/uml |
	rg 'pool|template|fork|exec|port|tapfd|snapshot|syzkaller|identity'
```

```sh
rg -n 'CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG|mfc_diag|Memo|Phase|#[0-9]+|DIARY|WIP|TODO|FIXME|HACK' \
	arch/um/kernel/template_pause.c \
	arch/um/kernel/template_pause_identity.c \
	arch/um/include/asm/um-template-pause.h \
	tools/uml/uml-launcher/src/bin/umlctl/{pool.rs,pool_serve.rs,pool_client.rs,pool_take_status.rs,exec.rs,port_forward.rs,tapfd.rs} \
	tools/testing/selftests/um/{pool-bench,pool-exec-smoke,pool-port-forward-smoke,pool-serve-smoke,pool-spawn-smoke,template-pause-pool-member-smoke,template-pause-pool-sustained-smoke,template-pause-pivot-smoke}
```

The targeted source-hygiene grep on current `next` found no memo, phase,
internal issue number, diagnostic-mode, TODO, FIXME, or HACK marker in the
selected kernel/template-pause and launcher pool/fork files. The only match in
the selected pool selftests was the ordinary phrase "Memory-gate fork count" in
`pool-bench`, not a diary or issue marker.

## High-Level Result

Current `next` is ahead of the old memo09 pool/fork branches in two important
ways:

1. The active code has already been cleaned toward kernel style. Historical
   branches still contain many "Memo 09", "Phase", diagnostic, and diary
   comments in source and selftests.
2. Most command surfaces from `memo09-phase4` already exist on `next`: direct
   pool spawn, pool serve/take/status/destroy/shutdown, daemon-routed exec,
   port-forward result generation, identity blob parsing/application, per-member
   mconsole socket handling, TAP/fd handoff hooks, template-pause pivot mode,
   pool-member mode, and the related smoke tests.

The remaining work is therefore not a clean rewrite from the old branches. It
is a focused completion pass:

- import missing historical test wrappers where they still exercise active
  functionality;
- run and fix the current pool/fork tests;
- complete warm-pool semantics that are currently accepted by the CLI but
  served lazily;
- validate vector2 TAP/fd handoff through pool members;
- validate the syzkaller shim against the final pool/exec path;
- keep historical diagnostics and diary material out of upstream-facing source.

## Current `next` Surfaces

### Kernel Template-Pause And Identity

Current `next` contains:

- `arch/um/include/asm/um-template-pause.h`
- `arch/um/kernel/template_pause.c`
- `arch/um/kernel/template_pause_identity.c`
- `arch/um/kernel/template_pause_identity.h`
- `arch/um/kernel/template_pause_identity_test.c`

Functional surfaces present:

- `um_template_pause` single-pause mode.
- `um_template_pause=fork` fork-on-resume mode.
- `um_template_pause=early` and `um_template_pause=early-fork`.
- `um_template_pause_private_stack=` private-stack control.
- `um_template_pause_pivot_test=1` pivot-test mode.
- `um_template_pause_pool_member=1` long-lived pool-member mode.
- identity blob parsing and validation.
- instance name, MAC, IPv4 CIDR, gateway, TAP name, and mconsole path fields.
- netdev MAC/IP/default-route application.
- vector2 TAP reopen hook when vector2 support is enabled.
- mconsole reinitialization for forked pool members.
- KUnit coverage for identity parse helpers.

Comparison result:

- The mode set from `memo09-phase4` is present on `next`.
- The old `CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG` and `mfc_diag` debug path is not
  present on `next`. That is the right shape for a clean branch unless a future
  failing test proves a need for a redesigned diagnostic path.
- The current comments are substantially cleaner than `memo09-phase4`; the old
  branch's phase notes and diagnostic diary text should not be reimported.

Current validation result:

- `template-pause-smoke` passes cases 1-3 and skips the vector2 case when
  guest `vec0` is absent.
- `template-pause-pivot-smoke` passes.
- `template-pause-pool-member-smoke` passes.
- `template-pause-fork-smoke` passes after the harness drives two
  SIGSTOP/SIGCONT cycles; it reports two distinct child PIDs and observes two
  master resume cycles.
- `template-pause-fork-stress` passes its default gate with 548 kernel
  iterations, median 18.2 ms iteration time, 548/548 clean identity
  round-trips, no kernel panics, and no live orphans.
- `template-pause-pool-sustained-smoke` is still an expected failure after the
  first member because repeated members hit the MAP_SHARED physmem limit.  The
  harness now stops on the first post-member panic/segfault and records the
  current member identity for each take.

Remaining work:

- Fix sustained member lifetime with production code, likely by giving members
  independent physmem file descriptors or an equivalent ownership model.
- Validate the vector2 leg of `template-pause-smoke` when a guest-visible
  `vec0` device is available.

### Launcher Pool And Fork Commands

Current `next` contains:

- `tools/uml/uml-launcher/src/bin/umlctl/pool.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_client.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_serve.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/pool_take_status.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/exec.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/port_forward.rs`
- `tools/uml/uml-launcher/src/bin/umlctl/tapfd.rs`

Functional surfaces present:

- direct `umlctl pool spawn`;
- `umlctl pool list`;
- file-backed `umlctl pool destroy`;
- daemon `umlctl pool serve`;
- RPC `take`, `status`, `destroy`, `exec`, and `shutdown`;
- `umlctl pool take`;
- `umlctl pool status`;
- daemon-routed `umlctl exec`;
- `umlctl port-forward` result generation;
- TAP allocation and fd preparation helpers;
- identity memfd handling;
- per-member mconsole path synthesis.

Comparison result:

- The command surface from `memo09-phase4` is present on `next`.
- Current `next` is cleaner than `memo09-phase4` in user-facing comments.
- `pool_serve.rs` still documents that `min_warm` is accepted but currently
  serves takes on demand. That is a real functional gap if warm pool support is
  part of completion.

Current validation result:

- `pool-spawn-smoke` passes for spawn, list, and destroy lifecycle.
- `pool-serve-smoke` passes for daemon readiness, status, take, destroy,
  shutdown, and master cleanup.
- `pool-exec-smoke` passes for typed NDJSON failure envelopes when the member
  lacks the mconsole path needed for successful exec.
- `pool-port-forward-smoke` passes for typed result/error handling.
- A reduced `pool-bench` passes take-latency, lifecycle-drift, and throughput
  gates, but fails the RSS gate because no benchmark children remain live.

Remaining work:

- Run `cargo fmt --check` and `cargo test` in `tools/uml/uml-launcher`.
- Complete real `min_warm` behavior. Because the overall policy is to import
  or complete all promised functionality, lazy-only compatibility is not enough
  for final completion.
- Fix or prove the successful member mconsole exec path.
- Re-run full `pool-bench` after fork/member lifetime fixes.
- Keep validated failure reporting for kernels that lack the required mconsole
  `exec` primitive.

### Pool And Template-Pause Selftests

Current `next` contains:

- `tools/testing/selftests/um/pool-bench/`
- `tools/testing/selftests/um/pool-exec-smoke/`
- `tools/testing/selftests/um/pool-port-forward-smoke/`
- `tools/testing/selftests/um/pool-serve-smoke/`
- `tools/testing/selftests/um/pool-spawn-smoke/`
- `tools/testing/selftests/um/template-pause-fork-smoke/`
- `tools/testing/selftests/um/template-pause-fork-stress/`
- `tools/testing/selftests/um/template-pause-pivot-smoke/`
- `tools/testing/selftests/um/template-pause-pool-member-smoke/`
- `tools/testing/selftests/um/template-pause-pool-sustained-smoke/`
- `tools/testing/selftests/um/template-pause-smoke/`

Comparison result:

- `memo09-phase4` does not have additional pool/fork selftest directories that
  are absent from current `next`.
- Current `next` has additional pool-member, sustained, and pivot selftests
  that are absent from some older phase branches.
- The historical `pool-bench` script is present on `next` and appears cleaned
  of memo/phase comments except ordinary benchmark gate phrasing.

Remaining work:

- Run the current tests with `UML_BINARY=./linux` after a fresh build.
- Treat a failing test as an implementation or test-contract bug to resolve.
- Keep prerequisite failures as SKIP only when the test truly cannot run.
- Convert any accidental pass-on-missing-feature behavior into FAIL or SKIP
  with a clear reason.

### Snapshot Test Wrappers

Current snapshot functionality on `next` is active and validated:

- KVM v2 snapshot KUnit passes 4/4.
- snapshot restore smoke passes through `kvm-snapshot-restore-smoke`.
- live `umlctl snapshot export` passes with `readelf`, `gdb -c`, and
  `tools/uml/uml-gdb/uml-snapshot.py`.
- SMP snapshot capture/restore is explicitly gated to one online CPU.

Historical-only test wrappers on `memo09-phase4`:

- `tools/testing/selftests/um/kvm-snapshot-bench/`
- `tools/testing/selftests/um/snapshot-kvm-smoke/`
- `tools/testing/selftests/um/snapshot-elf-roundtrip/`

Comparison result:

- The kernel hooks these wrappers need already exist on current `next`:
  `kvm_v2_snapshot_bench=`, debugfs `kvm_v2_snapshot_bench`, debugfs
  `kvm_v2_snapshot_elf_export_path`, mconsole `snapshot_export`, and the
  `um_kvm_v2_snapshot` KUnit suite.
- The historical wrappers still contain memo and issue-number prose. They
  should be imported only after comment cleanup and adjusted to the current
  names and validation policy.

Disposition:

- Import `kvm-snapshot-bench` as a clean kselftest wrapper for
  `kvm_v2_snapshot_bench=N`.
- Import or replace `snapshot-kvm-smoke` with a clean kselftest wrapper that
  boots KUnit and requires the current four cases to pass:
  `test_kvm_v2_snapshot_regs_only`,
  `test_kvm_v2_snapshot_full_memslot`,
  `test_kvm_v2_snapshot_task_state`, and
  `test_kvm_v2_snapshot_elf_regs_only`.
- Import `snapshot-elf-roundtrip` after updating it for the current exporter.
  Prefer the current mconsole `snapshot_export` path for host-driven testing;
  keep the debugfs path only for inside-guest coverage.

### Syzkaller Shim

Current `next` contains:

- `tools/uml/syzkaller-vm-shim/README.md`
- `tools/uml/syzkaller-vm-shim/uml.go`

Comparison result:

- The shim exists on current `next` and differs from `memo09-phase4`.
- The relevant dependency chain is pool serve/take, daemon-routed exec,
  port-forward, member cleanup, and console/crash capture.
- The comparison did not find a missing syzkaller source file from the memo09
  branches.

Remaining work:

- Build the shim.
- Add or run a syzkaller-style smoke that performs take, exec, destroy, and
  timeout/error-path checks through the final launcher.
- Validate crash/console capture if practical.
- Align README claims with the final behavior.

## Functionality Matrix

| Area | Current status on `next` | Historical source | Completion action |
| ---- | ------------------------ | ----------------- | ----------------- |
| Template pause single-shot | Present, validated | `fork-server-phase1c`, `memo09-*` | Keep cleaned source; validate vector2 leg when guest `vec0` is visible. |
| Template pause fork-on-resume | Present, validated | `memo09-phase2`, `memo09-phase4` | Keep smoke and stress green. |
| Template pause pivot mode | Present, validated | `memo09-phase4`, current `next` | Keep. |
| Template pause pool-member mode | Present, partially validated | `memo09-phase4`, current `next` | One-shot member passes; sustained lifetime needs production fix. |
| Identity blob parse/apply | Present, validated | `memo09-phase2`, `memo09-phase4` | Keep KUnit plus live pool validation. |
| Per-member mconsole path | Present, needs fix/proof | `memo09-phase4` | Error envelope validated; successful exec still pending. |
| Vector2 TAP/fd handoff | Present hook, needs validation | `memo09-phase4`, `umlctl-deploy` | Validate through pool member and vector2 smoke. |
| Direct pool spawn | Present, validated | `memo09-*` | Keep `pool-spawn-smoke` green. |
| Pool daemon serve/take/status | Present, validated | `fork-server-phase1c`, `memo09-phase4` | Extend validation after warm-pool work. |
| Pool destroy/shutdown | Present, validated | `memo09-phase4` | Keep in lifecycle smoke. |
| Daemon-routed exec | Present, partially validated | `memo09-phase4` | Missing-feature failures are validated; successful exec still pending. |
| Port-forward result | Present, validated | `memo09-phase4` | Tie to final network validation. |
| Warm pool `min_warm` | Partial | `memo09-phase3-pool-bench`, `memo09-phase4` | Complete real pre-warm queue. |
| Pool benchmark | Present, needs fix | `memo09-phase3-pool-bench`, `memo09-phase4` | 4/5 reduced gates pass; fix live-child/RSS gate. |
| Syzkaller VM shim | Present, unvalidated | `memo09-phase4`, current `next` | Build and run syzkaller-style take/exec/destroy smoke. |
| Snapshot bench kselftest | Historical-only wrapper | `memo09-phase4` | Import clean wrapper around active kernel hook. |
| Snapshot KUnit kselftest wrapper | Historical-only wrapper | `memo09-phase4` | Import or replace for current 4-case KUnit suite. |
| Snapshot ELF roundtrip kselftest | Historical-only wrapper | `memo09-phase4` | Import with current mconsole/debugfs exporter contract. |
| Record/replay | Historical-only | `kvm-v2-snapshot-elf64`, `experiment-path-c` | Separate workstream: complete or land experimental. |
| State trace | Historical-only | `kvm-v2-snapshot-elf64` | Separate workstream: import only as clean optional diagnostics. |

## Detailed Execution Plan

### Step 1: Import Clean Snapshot Test Wrappers

Status: complete on 2026-06-10. The imported wrappers are:

- `tools/testing/selftests/um/kvm-snapshot-bench/`
- `tools/testing/selftests/um/snapshot-kvm-smoke/`
- `tools/testing/selftests/um/snapshot-elf-roundtrip/`

The ELF roundtrip wrapper uses `kvm_v2_snapshot_elf_export=<host-path>` so it
can validate the exported file before guest userspace runs.

Files to add or adapt:

- `tools/testing/selftests/um/kvm-snapshot-bench/Makefile`
- `tools/testing/selftests/um/kvm-snapshot-bench/run-kvm-snapshot-bench.sh`
- `tools/testing/selftests/um/snapshot-kvm-smoke/Makefile`
- `tools/testing/selftests/um/snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh`
- `tools/testing/selftests/um/snapshot-elf-roundtrip/Makefile`
- `tools/testing/selftests/um/snapshot-elf-roundtrip/run-snapshot-elf-roundtrip.sh`

Cleanup requirements:

- Remove internal issue numbers.
- Remove memo and phase references.
- Use current KUnit case names.
- Default `UML_BINARY` to `./linux`, consistent with current local smokes.
- Use kselftest exit convention: 0 PASS, 1 FAIL, 4 SKIP.
- Do not hide real feature absence behind PASS.

Validation:

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/kvm-snapshot-bench/run-kvm-snapshot-bench.sh
```

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/snapshot-kvm-smoke/run-snapshot-kvm-smoke.sh
```

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/snapshot-elf-roundtrip/run-snapshot-elf-roundtrip.sh
```

### Step 2: Re-run Current Pool/Fork Tests

Commands:

```sh
cd tools/uml/uml-launcher
cargo fmt --check
cargo test
```

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-smoke/run-template-pause-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-fork-smoke/run-template-pause-fork-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-fork-stress/run-template-pause-fork-stress.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-pivot-smoke/run-template-pause-pivot-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-pool-member-smoke/run-template-pause-pool-member-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/template-pause-pool-sustained-smoke/run-template-pause-pool-sustained-smoke.sh
```

```sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/pool-spawn-smoke/run-pool-spawn-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/pool-serve-smoke/run-pool-serve-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/pool-exec-smoke/run-pool-exec-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/pool-port-forward-smoke/run-pool-port-forward-smoke.sh
UML_BINARY=$PWD/linux tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Expected outcome:

- Passing tests stay as evidence in `STATUS.md` and the inventory.
- Failing tests become implementation tasks, not documentation-only tasks.
- Any SKIP must identify the missing prerequisite exactly.

### Step 3: Complete Warm Pool Semantics

Current state:

- `pool serve --min-warm` is accepted for compatibility.
- The daemon currently serves takes on demand.

Required final behavior:

- `min_warm=N` maintains up to `N` pre-taken, ready members when possible.
- A `take` consumes a ready member when available.
- The daemon replenishes asynchronously after a successful take.
- The status RPC reports ready, taken, and failed member counts.
- Shutdown and destroy clean up ready members as well as taken members.
- Failures to pre-warm members do not wedge the daemon; status exposes the
  failure count and the next replenishment attempt.

Implementation notes:

- Reuse the existing identity blob and mconsole path generation.
- Keep a single serialized take/replenish path first; add concurrency only if
  benchmark evidence requires it.
- Ensure ready members are not visible as taken until handed out.
- Preserve the direct `pool spawn` path as the simple one-shot primitive.

Validation:

- `cargo test` for request parsing, status shape, and warm-pool bookkeeping.
- `pool-serve-smoke` for take/status/destroy/shutdown.
- `pool-bench` for p50/p95 take latency and memory growth.
- A new or extended smoke that proves `min_warm=1` returns an already-ready
  member and replenishes after take.

### Step 4: Validate TAP/fd Handoff Through Pool Members

Current state:

- Identity apply can reopen a vector2 TAP for a pool member.
- Launcher TAP helpers exist.
- Port-forward currently returns the address a guest should dial; it is not a
  NAT engine by itself.

Required final behavior:

- A pool member taken with TAP/vector2 identity can communicate through the
  assigned TAP/fd path.
- The member's mconsole and network identity are distinct from the master.
- `port-forward` reports a correct guest-reachable address for the selected
  mode.

Validation:

- pool member network smoke using vector2 TAP.
- fd handoff smoke.
- port-forward smoke.
- selected Tier 3 vector2 workload after pool paths are stable.

### Step 5: Validate Syzkaller Shim

Current state:

- The shim exists, but completion requires end-to-end proof.

Required final behavior:

- Shim starts or connects to a pool.
- Shim takes an instance.
- Shim runs a command through the exec path and returns stdout, stderr, exit
  status, signal, and timeout state.
- Shim destroys or recycles the instance.
- Console and crash output are captured well enough for syzkaller triage.

Validation:

- Go build or package build for the shim.
- Synthetic syzkaller-style smoke: take, exec `/bin/true`, exec failure,
  timeout, destroy.
- Crash capture smoke if practical.

### Step 6: Source And Documentation Cleanup

Scope:

- active kernel source;
- active launcher source;
- active selftests;
- public UML docs;
- current status and sequencing docs.

Rules:

- No internal issue numbers in upstream-facing source comments.
- No memo, phase, diary, WIP, or branch-history prose in active source.
- Keep comments that describe durable invariants: ABI layout, locking,
  ownership, failure policy, and externally visible contracts.
- Historical reports can remain in the redesign archive, but status documents
  must clearly mark them as historical if they contain stale claims.

Suggested greps:

```sh
rg -n 'Memo|Phase|#[0-9]+|DIARY|WIP|TODO|FIXME|HACK|temporary|debug-only|investigation' \
	arch/um tools/uml tools/testing/selftests/um Documentation/virt/uml
```

```sh
rg -n 'not implemented|stub|future|experimental|prototype|historical-only' \
	arch/um tools/uml tools/testing/selftests/um Documentation/virt/uml
```

Disposition:

- Active source: fix comments or code.
- Active selftests: make TODOs executable work items or remove stale notes.
- Historical docs: keep only with clear historical context.
- Status docs: describe present truth and blockers, not session history.

## Commit And Push Policy

Work should continue in small pushed increments:

1. Import and validate snapshot test wrappers.
2. Validate current pool/fork tests and commit any fixes.
3. Complete warm-pool behavior and validate.
4. Validate vector2 TAP/fd handoff through pool.
5. Validate syzkaller shim.
6. Refresh docs, reports, and final completion dashboard.

Each increment should include:

- relevant source or doc updates;
- focused validation output recorded in the inventory or status file;
- `git diff --check`;
- checkpatch for touched kernel/selftest shell files where appropriate;
- `cargo fmt --check` and `cargo test` for launcher changes;
- commit and push to `origin/next`.

## Completion Criteria For This Area

Pool/fork-server functionality can be called complete only when:

- every current pool and template-pause selftest passes or has a justified SKIP;
- `pool serve --min-warm` works as documented;
- pool exec and port-forward work through the daemon path;
- vector2 TAP/fd handoff works for pool members;
- the syzkaller shim has an end-to-end take/exec/destroy smoke;
- snapshot benchmark, KUnit smoke, and ELF roundtrip kselftest wrappers are
  either imported and passing or explicitly replaced by equivalent current
  tests;
- upstream-facing source and selftests are free of branch diary, internal issue
  numbers, and stale phase notes;
- `STATUS.md`, the functionality inventory, and the full integration plan all
  agree on what is complete and what remains.
