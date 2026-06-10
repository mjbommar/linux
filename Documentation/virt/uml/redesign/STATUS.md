# UML Redesign Status

Last updated: 2026-06-10 snapshot export pass.

This file records the current state of the UML v2 work. It is not a running
chronicle. Prior investigations, retired designs, and detailed validation
logs belong in the redesign archive; this file should stay focused on what is
active, what is proven, and what still blocks publication.

## Active Scope

The active KVM v2 series is the backend core plus the functionality being
restored for the full UML v2 completion branch:

- VM and vCPU lifecycle;
- syscall dispatch and the optional in-guest LSTAR fast path;
- exception delivery through backend-owned descriptor and handler pages;
- signal, FPU/XSAVE, timer, and SMP state handling;
- memory-slot and region management;
- snapshot capture/restore and snapshot ELF export; and
- the validation needed to decide which pieces are publishable upstream.

Record/replay and private state-trace code remain historical-only at this
point. Generic UML snapshot and fork-server work remains separate from the KVM
backend core unless the integration plan explicitly pulls it into `next`.

## Current Readiness

KVM v2 is functionally past the architecture-unknown stage. The current work
is full-functionality integration: importing or completing missing historical
features, normalizing comments and documentation, checking series shape, and
finishing validation.

Current source-tree direction:

- KVM v1 archive code has been removed from the active tree.
- KVM v2 snapshot capture/restore and snapshot ELF export source is present on
  `next` and builds.
- KVM v2 snapshot KUnit and live `umlctl snapshot export` validation pass on
  `next`; restore smoke and SMP semantics remain open.
- KVM v2 record/replay and private trace-ring sources have not yet been
  reimported into `next`.
- KVM v2 keeps normal kernel tracepoints as its public observability surface.
- Runtime backend selection remains explicit; seccomp stays the fallback
  backend unless KVM v2 is selected.
- KVM v2 KUnit coverage remains for register marshaling and byte-shape
  invariants.

## Validation Snapshot

The strongest current KVM v2 evidence is:

- CPython parity: 21/21 curated standard-library modules match the seccomp
  backend under both UP and SMP configurations.
- Substrate gate: KVM v2 matches the seccomp baseline at 25 pass, 3 fail,
  and 3 expected-fail results.
- SMP stress: the mt-mmap-stress family, threaded subprocess, and threaded
  fork/malloc checks pass on the post-fix KVM v2 builds recorded in the
  redesign archive.
- Python startup benchmark: current KVM v2 measurements remain materially
  faster than seccomp after the LSTAR fast path and FPU-state fixes.
- Cheap syscall benchmark: the in-guest fast path keeps getpid-family calls
  in the low-hundreds-cycle range on the measured host.
- Snapshot KUnit: `um_kvm_v2_snapshot` passes 4/4 under
  `backend=force=kvm-v2` with `kunit_shutdown=halt`, covering register-only
  capture, one-page memslot capture/restore, task iotrap state restore, and
  ELF64 note export.
- Snapshot export: a disposable KVM v2 hostfs guest exports a core through
  `umlctl snapshot export`; `readelf -h/-l/-n`, `gdb -c`, and
  `tools/uml/uml-gdb/uml-snapshot.py` all parse the resulting ELF.

The most important correctness closure was the CPython cache-flake fix:
per-task FPU save/restore now uses KVM XSAVE state instead of the older FPU
ioctls, preserving YMM upper halves after AVX is exposed to the guest. Related
hardening also pins debug-register state, saves/restores pending vCPU events,
and keeps CPUID xstate leaves consistent with the exposed feature set.

Remaining validation before publication or completion:

- run snapshot restore smoke and define, gate, or validate SMP snapshot
  semantics;
- complete a natural 24-hour KVM v2 soak on the final cleaned tree;
- rerun Tier 3 networking workloads on KVM v2 with the final vector2 stack;
- keep the seccomp comparison path green while the KVM v2 series is split;
- refresh the upstream cover letter and patch boundaries after the cleanup.

## Vector2 Networking

Vector2 is no longer scaffolding-only. The active stack includes netdev
registration, trusted TAP/fd datapaths, launcher selection, fd multiqueue,
queue-to-CPU policy, KUnit coverage, KCSAN workload evidence, sandbox checks
evidence, and substantial seccomp Tier 3 workload evidence.

The strongest long-run vector2 evidence is the stopped-clean seccomp Tier 3
run:

- 6142 seconds of a planned 7200-second window;
- 970/970 passes, with no failures or timeouts;
- Django vector2/seccomp: 490/490 passes;
- FastAPI vector2/seccomp: 480/480 passes;
- all scoreboard rows recorded vector2, TAP transport, in-process host mode,
  and a single queue;
- logs reached the expected server-ready and guest-request success markers;
- teardown left no matching soak process or stray TAP device.

Open vector2 publication work:

- finish a natural 7200-second seccomp/vector2 run;
- rerun the same Tier 3 coverage on KVM v2 now that the backend flake is
  closed;
- expand fairness and performance coverage for multiqueue operation;
- keep CI/preflight coverage aligned with the launcher-facing configuration.

## Fork-Server And Pool Work

Template pause, pool spawning, pool serving, identity application, and the
syzkaller shim are useful UML features, but they are not part of the KVM v2
backend core. Their current state should be documented separately from the
backend publication series.

Current boundary:

- the pool daemon and basic pool selftests exist;
- kernel identity parsing and application have KUnit coverage;
- the syzkaller-facing shim and JSON command paths exist;
- tap-fd handoff and per-pool-member mconsole plumbing remain open for the
  full end-to-end pool networking and exec path.

## Historical-Only Work

The following work is not yet present in the active `next` implementation:

- KVM-specific record/replay sources and tests;
- private state-trace ring and parser tooling;
- benchmark and smoke tests whose only purpose was to exercise code that is
  still historical-only.

Snapshot capture/restore and snapshot ELF export have been restored as active
source. KUnit coverage and live `umlctl` ELF export validation now pass.
Restore smoke and SMP constraints still need to close before the snapshot
workstream can be called complete.

## Publication Checklist

Before treating UML v2 as publishable, verify:

- `make ARCH=um O=<build-dir> -j$(nproc) vmlinux` passes on the cleaned tree;
- KVM v2 KUnit suites pass in the configured UML build;
- CPython parity remains 21/21 against seccomp;
- the substrate gate still matches seccomp;
- the final KVM v2 24-hour soak completes naturally;
- vector2 Tier 3 runs complete on both seccomp and KVM v2 where applicable;
- checkpatch on changed KVM v2 patches has no unexplained warnings;
- public docs describe the design and validation state, not the development
  history;
- historical-only experiments are either imported with tests or clearly marked
  as not part of the completed branch.

## Maintenance Rules

Keep this file current and short.

- Do not append dated session logs.
- Do not store task ledgers, issue queues, or old hypotheses here.
- Keep detailed root-cause writeups in the redesign archive.
- Keep this file as the top-level readiness view for reviewers and future
  maintainers.
