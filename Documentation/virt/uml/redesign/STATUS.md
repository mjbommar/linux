# UML Redesign Status

Last updated: publishability cleanup pass.

This file records the current state of the UML v2 work. It is not a running
chronicle. Prior investigations, retired designs, and detailed validation
logs belong in the redesign archive; this file should stay focused on what is
active, what is proven, and what still blocks publication.

## Active Scope

The active KVM v2 series is the backend core:

- VM and vCPU lifecycle;
- syscall dispatch and the optional in-guest LSTAR fast path;
- exception delivery through backend-owned descriptor and handler pages;
- signal, FPU/XSAVE, timer, and SMP state handling;
- memory-slot and region management;
- the validation needed to send the backend upstream.

KVM-specific snapshot, ELF export, record/replay, and private state-trace
experiments answered useful architecture questions, but they are no longer
part of the publishable KVM v2 series. Generic UML snapshot and fork-server
work remains separate from the KVM backend core.

## Current Readiness

KVM v2 is functionally past the architecture-unknown stage. The current work
is publication hardening: removing retired code, normalizing comments and
documentation, checking series shape, and finishing long-duration validation.

Current source-tree direction:

- KVM v1 archive code has been removed from the active tree.
- KVM v2 snapshot, record/replay, ELF export, and private trace-ring sources
  have been removed from the backend core.
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

The most important correctness closure was the CPython cache-flake fix:
per-task FPU save/restore now uses KVM XSAVE state instead of the older FPU
ioctls, preserving YMM upper halves after AVX is exposed to the guest. Related
hardening also pins debug-register state, saves/restores pending vCPU events,
and keeps CPUID xstate leaves consistent with the exposed feature set.

Remaining validation before publication:

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

## Retired Work

The following work is intentionally absent from the publishable KVM backend
series:

- KVM v1 backend archive sources;
- KVM-specific snapshot capture/restore sources and tests;
- KVM-specific record/replay sources and tests;
- snapshot ELF export sources and tests;
- private state-trace ring and parser tooling;
- benchmark and smoke tests whose only purpose was to exercise those retired
  KVM-specific experiments.

If a future series revives any of this, it should do so as a new design with a
small public interface, not by reintroducing the retired private scaffolding.

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
- retired experiments are absent from source, Kconfig, selftests, and
  publication-facing docs.

## Maintenance Rules

Keep this file current and short.

- Do not append dated session logs.
- Do not store task ledgers, issue queues, or old hypotheses here.
- Keep detailed root-cause writeups in the redesign archive.
- Keep this file as the top-level readiness view for reviewers and future
  maintainers.
