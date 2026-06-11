# UML Redesign Status

Last updated: 2026-06-11 profiles, ftrace, launcher, selftest/doc curation, report/presentation archival marking, active source comment cleanup, umlbuild validation, experimental record/replay core, record/replay live syscall hook/gadget bypass/debugfs control/time-travel clock events, KVM v2 dynamic-loader TLS closure, snapshot ELF/debugfs documentation validation, vector2 validation documentation alignment, BPF/JIT runtime smoke validation, kprobes stress validation, KMSAN runtime-smoke closure, follow-up KVM v2 comment cleanup, x86 UML ptrace/TLS regset cleanup, substrate gate tightening, CPython tier-0 gate evidence, KGDB disposition cleanup, current-HEAD pool/fork/syzkaller regression evidence, current-HEAD vector2 validation evidence, record/replay task-owned session-start evidence, first record/replay syscall-payload evidence, strict replay fail-closed syscall policy, strict replay gate coverage, record/replay versioned event-format coverage, `getcwd(2)` payload coverage, fail-closed raw-time replay policy, vector2 TCP diagnostic capture, and vector2 TX/RX NAPI scheduling closure.

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
- snapshot capture/restore and snapshot ELF export;
- experimental record/replay container, syscall-log core, and live syscall
  hook; and
- the validation needed to decide which pieces are publishable upstream.

Private state-trace code remains historical-only at this point. Record/replay
now has an experimental Kconfig-gated core in `next`, including a debugfs
control/status surface and live syscall-record smoke coverage, but live
deterministic runtime replay is not complete. Generic UML snapshot and
fork-server work remains separate from the KVM backend core unless the
integration plan explicitly pulls it into `next`.

## Current Readiness

KVM v2 is functionally past the architecture-unknown stage. The current work
is full-functionality integration: importing or completing missing historical
features, normalizing comments and documentation, checking series shape, and
finishing validation.

Current source-tree direction:

- KVM v1 archive code has been removed from the active tree.
- KVM v2 snapshot capture/restore and snapshot ELF export source is present on
  `next` and builds.
- KVM v2 snapshot KUnit, live `umlctl snapshot export`, and snapshot restore
  smoke validation pass on `next`; SMP snapshot semantics are explicitly
  gated to one online CPU.
- Snapshot ELF/debugfs documentation now matches the current producer
  surfaces: boot-time `kvm_v2_snapshot_elf_export=`, mconsole/`umlctl
  snapshot export`, direct debugfs `kvm_v2_snapshot_elf_export_path`, debugfs
  `kvm_v2_snapshot_bench`, and the in-kernel ELF export helpers.
- KVM v2 record/replay has an experimental core on `next` behind
  `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`; its live syscall
  dispatcher hook can observe and replay syscall return values, and debugfs can
  start/stop/reset a singleton record container for validation. Debugfs record
  start also captures and attaches a KVM v2 task snapshot. The record log can
  also round-trip UML time-travel clock advances through the `record_replay`
  hook. Record status now reports first/last recorded syscall PID and
  same-task versus other-task syscall counters, and the task-owned smoke proves
  a single process can snapshot itself and record a focused scalar plus
  `uname(2)`/`getcwd(2)` payload workload without other-task contamination.
  The first payload replay primitives can restore `uname(2)`'s
  `struct new_utsname` and `getcwd(2)`'s returned path bytes when syscall
  number and arguments match. Strict replay now fails closed and records a
  failure counter for syscalls outside the initial R/R-1 subset:
  `getpid`, `getppid`, `gettid`, and payload-aware `uname(2)`/`getcwd(2)`.
  Raw time syscalls are rejected by that same strict policy, and replay mode
  sets CR4.TSD so user `RDTSC`/`RDTSCP` faults instead of observing host time
  outside the log. Replay mode also blocks `SIGALRM` in KVM's per-vCPU signal
  mask while inside `KVM_RUN`, preventing timer delivery from creating
  unrecorded in-guest `EINTR` points. Strict replay also rejects randomness
  and external I/O syscalls outside the supported subset, including
  `getrandom`, `openat`, `read`, `write`, and `ioctl`, instead of replaying
  them as scalar-only entries. Broader payload coverage, replayable
  signal-event ordering, device/network/hostfs event replay, and deterministic
  replay policy remain incomplete.
  Private trace-ring sources have not yet been reimported.
- KVM v2 keeps normal kernel tracepoints as its public observability surface.
- Runtime backend selection remains explicit; seccomp stays the fallback
  backend unless KVM v2 is selected.
- KVM v2 KUnit coverage remains for register marshaling, byte-shape
  invariants, snapshots, and the experimental record/replay core.
- Focused active-source and selftest comment cleanup has removed the remaining
  internal audit label and temporary-policy wording found in `arch/um`,
  `tools/testing/selftests/um`, and `tools/uml/uml-launcher`. KVM isolation
  reproducers are explicitly documented as diagnostic-only material.
- Follow-up cleanup removed phase, memo, date, and internal bug labels from the
  active x86 UML KVM v2 per-task state comments while keeping the state
  isolation invariants in the source.
- Follow-up x86 UML cleanup fixed `PTRACE_ARCH_PRCTL` to operate on the traced
  task's FS/GS base state instead of `current`, removed obsolete `XXX` include
  and ptrace comments, and replaced the 32-bit TLS-regset TODO with an
  `NT_386_TLS` regset backed by UML's existing TLS entry cache.
- KGDB is deferred, not implemented. Current UML does not select
  `HAVE_ARCH_KGDB`, the live profile fragments do not enable `CONFIG_KGDB`,
  and the profile docs no longer claim KGDB as an available research or
  time-travel feature. Re-enabling it requires UML architecture support,
  backend register read/write integration, a transport decision, and a smoke
  test.
- The old report/deck workspace under `report-presentation/` is marked as a
  historical May 2026 artifact. Its report, slides, comprehensive report,
  generated PDFs, and CSV data point readers back to this status file and the
  live sequencing inventory for current completion claims.

## Validation Snapshot

The strongest current KVM v2 evidence is:

- CPython parity: 21/21 curated standard-library modules match the seccomp
  backend under both UP and SMP configurations.
- CPython tier-0 gate: `umlctl gate run` passes under both seccomp and KVM v2
  on the current `./linux` binary. Each backend reports `pass=1 fail=0
  expected_fail=0`, and the extracted hashlib empty-string SHA-256 metric
  matches across backends.
- Substrate gate: seccomp reports 25 pass, 3 fail, and 3 expected-fail
  results; KVM v2 reports 27 pass, 3 fail, and 1 expected-fail result on the
  same repro set. The `regrtest-substrate` gate now uses 25 as the shared pass
  floor and 3 as the known-fail ceiling.
- SMP stress: the mt-mmap-stress family, threaded subprocess, and threaded
  fork/malloc checks pass on the post-fix KVM v2 builds recorded in the
  redesign archive.
- Python startup benchmark: current KVM v2 measurements remain materially
  faster than seccomp after the LSTAR fast path and FPU-state fixes.
- Cheap syscall benchmark: the in-guest fast path keeps getpid-family calls
  in the low-hundreds-cycle range on the measured host. On the current
  gadget-enabled validation build, freestanding KVM `perf-getpid` reports
  `cyc_per_call=89` and `perf-pidfam` reports `cyc_per_call=94`.
- Dynamic-loader/TLS smoke: forced-KVM `/bin/true` with `kunit.enable=0`
  now reaches the expected clean init-exit panic with `exitcode=0`, and
  `tools/testing/selftests/um/dyn-loader/run-dyn-loader.sh` reports
  `DYN_LOADER: backend=kvm PASS`. The fix keeps the user segment-selector
  refresh from zeroing the `arch_prctl()`-installed FS/GS bases before KVM
  entry.
- Snapshot KUnit: `um_kvm_v2_snapshot` passes 4/4 under
  `backend=force=kvm-v2` with `kunit_shutdown=halt`, covering register-only
  capture, one-page memslot capture/restore, task iotrap state restore, and
  ELF64 note export.
- Snapshot export: a disposable KVM v2 hostfs guest exports a core through
  `umlctl snapshot export`; `readelf -h/-l/-n`, `gdb -c`, and
  `tools/uml/uml-gdb/uml-snapshot.py` all parse the resulting ELF.
- Snapshot kselftests: `kvm-snapshot-bench`, `snapshot-kvm-smoke`,
  `snapshot-elf-roundtrip`, and `kvm-snapshot-restore-smoke` pass against
  the current `./linux` build.
- Snapshot restore smoke: `kvm-snapshot-restore-smoke` boots KVM v2 with
  `kvm_v2_snapshot_bench=1` and observes the kernel capture plus
  `restore_full` timing summary in full snapshot mode.
- Snapshot SMP policy: capture and restore reject guests with more than one
  online CPU with `-EOPNOTSUPP`, because all-vCPU quiescence is not implemented.
  The default validated tree is a UP build, so the selftest reports the negative
  SMP leg as not-built unless the tested UML binary has `CONFIG_SMP=y`.
- Experimental record/replay KUnit: `um_kvm_v2_record` passes 21/21 with
  `CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL=y`, covering lifecycle,
  invalid transitions, single-active ownership, syscall observe, FIFO replay,
  version/flags/header contract checks, bad-format replay rejection with cursor
  preservation, divergence cursor preservation, `uname(2)` and `getcwd(2)`
  payload restore, too-small payload buffer rejection, payload argument
  mismatch, payload overflow accounting, strict replay syscall-policy coverage,
  strict raw-time syscall rejection, strict external-I/O/randomness syscall
  rejection, strict-on/strict-off gate behavior, strict replay failure
  accounting, time-travel clock-event FIFO replay, the synthetic gadget-bypass
  page helper, and snapshot metadata cleanup. The live KVM syscall dispatcher
  now calls the observe/consume hooks, so this is no longer core-only syscall
  plumbing.
- Experimental live record smoke: `kvm-record-smoke` passes through the
  debugfs control surface, captures and attaches a KVM v2 task snapshot at
  record start, and records 3066 live KVM v2 syscall entries with 294336
  bytes used and 0 drops.
- Experimental task-owned record smoke: the static `kvm-record-task` helper
  runs as a single guest process, writes the debugfs `start` command itself,
  verifies `snapshot_source_pid == first_syscall_pid == last_syscall_pid`,
  requires `syscalls_from_other_tasks=0`, records two payload entries
  totaling 392 bytes, then replays the same 386-entry task-owned workload from
  the snapshot-backed log. This closes the first R/R-1 session-start,
  payload-model, deterministic workload replay, and strict
  unsupported-syscall fail-closed gates for the selected syscall subset.
- Experimental strict negative smoke: the static `kvm-record-negative` helper
  arms strict replay with an empty log and issues `getrandom(2)` as the first
  replay-mode syscall. The host runner treats the expected init-killing
  SIGSEGV as PASS only when the kernel logs the strict unsupported-syscall
  rejection (`nr=318` on the validated x86_64 run).
- Experimental record clock bench: `kvm-record-clock-bench` passes with
  `N=100`, `observed=100`, `replayed=100`, and `mismatches=0`, proving the
  KVM v2 record log can round-trip time-travel clock advances.
- Existing pure KVM v2 KUnit suites still pass on the same build:
  `kvm_v2_marshal` 9/9 and `kvm_v2_byteshape` 9/9.
- UML KMSAN now has a clean LLVM `uml/research-kmsan` build, page-aligned
  vmalloc metadata ranges, KMSAN-aware memory intrinsic lowering, UML-local
  host-boundary metadata cleanup, hostfs unpoisoning for host-filled data, and
  a stackless seccomp `rt_sigreturn` restorer for frame-pointer/KMSAN builds.
  `kmsan-smoke` reaches `KMSAN_SMOKE: PASS runtime=y reproducer=n`; the
  `reproducer=n` result is expected because the research profile does not
  enable the optional KUnit KMSAN test module.
- x86 UML ptrace/TLS cleanup validation: `git diff --check`, strict
  `scripts/checkpatch.pl --no-tree`, and a targeted TODO/XXX scan passed for
  the touched files. The x86_64 UML objects `syscalls_64.o`, `ptrace_64.o`,
  and `ptrace.o` compile in the current checkout. A clean detached i386 UML
  compile was attempted for the 32-bit TLS-regset path, but the host lacks the
  32-bit libc development headers required by UML's `user-offsets.c`
  (`bits/libc-header-start.h`), so the build stops before reaching the
  touched objects.

The most important correctness closure was the CPython cache-flake fix:
per-task FPU save/restore now uses KVM XSAVE state instead of the older FPU
ioctls, preserving YMM upper halves after AVX is exposed to the guest. Related
hardening also pins debug-register state, saves/restores pending vCPU events,
and keeps CPUID xstate leaves consistent with the exposed feature set.

Remaining validation before publication or completion:

- broaden the dynamic-userspace closure beyond `/bin/true` and the
  dyn-loader kselftest into Tier 3 KVM v2 workloads on the final vector2
  stack;
- complete a natural 24-hour KVM v2 soak on the final cleaned tree;
- finish record/replay time, signal, device, and deterministic replay policy
  before counting the original record/replay mission complete;
- rerun Tier 3 networking workloads on KVM v2 with the final vector2 stack;
- rerun `kmsan-smoke` in the final validation matrix to protect the KMSAN
  runtime-smoke closure;
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

Current bounded vector2 evidence adds:

- A bounded KVM-v2/vector2 Tier 3 current-head smoke on rebuilt
  `98166580dc4f` passed one Django-v2 and one FastAPI-v2 iteration. Both rows
  recorded vector2, TAP transport, KVM-v2 backend, in-process host mode, and a
  single queue, and both logs reached `SERVER_READY`,
  `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, and `REPRO_DONE rc=0`. This is a
  path smoke, not the full KVM-v2 Tier 3 publication gate.
- Current-head `um_vector2_*` KUnit passes on rebuilt `98166580dc4f`: 84 pass,
  0 fail, 2 trusted-TAP skips across config, queue, transport, fake-host,
  model, cmdline, netdev, ethtool, host-fd, and host-tap suites.
- A focused `vector2-fd-multiqueue-smoke` now validates launcher-owned fd
  multiqueue handoff: `umlctl up` creates a multiqueue TAP, opens four TAP
  fds, inherits fd range 200..203, the guest reports `UMLCTL_NETWORK_QUEUES=4`
  and `UMLCTL_NETWORK_FD_COUNT=4`, `vec2.0` reports four TX queues, and a
  one-packet host TAP ping succeeds.
- A focused `vector2-inproc-tap-smoke` now validates the explicit trusted
  in-process TAP path: `umlctl up` reports `transport=tap host_mode=inproc`,
  does not inherit launcher-owned TAP fds, the guest reports TAP/inproc
  metadata with no fd count, and a one-packet host TAP ping succeeds.
- The TCP performance benchmark harness is now wired into the UML selftest build
  surface without becoming a default runtime test: `net-bench` builds the
  `tcp-send` helper through kselftest, the wrappers use repo-relative defaults,
  and the privileged TAP benchmark remains an explicit operator-run gate.
- The first current-head guest-to-host TCP gate failed on performance, not
  function: legacy vector and vector2 both passed 3/3 `umlctl gate loop`
  iterations, but vector2 median throughput was 18949.8 Mbps versus legacy
  vector 39805.4 Mbps, a 0.476 ratio against the 0.85 acceptance bar.
- A first vector2 TX scatter-gather fix removed forced skb linearization from
  fd/TAP TX and added fragmented-skb KUnit coverage.  The follow-up TCP gate
  improved vector2 median throughput to 22953.7 Mbps versus legacy vector
  40041.7 Mbps, ratio 0.573, which still failed the 0.85 bar at that stage.
- A follow-up bounded `sendmmsg()` prototype was tested locally and rejected
  because it did not improve the TCP ratio.
- The net-bench template now logs guest `ip`/route/ethtool diagnostics around
  the sender, and vector2's `vnet_hdr_enabled` ethtool stat reports the
  runtime inherited-fd vnet-header state instead of treating all fd handoff as
  raw Ethernet.
- The diagnostic evidence pointed at TX-driven RX allocation churn.  The
  current TX/RX NAPI scheduling fix filters empty TX IRQ reschedules, honors
  `netdev_xmit_more()` while the TX ring has space, and gates RX buffer
  preparation on RX readiness.  The normal guest-to-host TCP gate now passes:
  legacy vector median 40080.5 Mbps, vector2 median 37116.0 Mbps, ratio 0.926
  against the 0.85 acceptance bar.  The same slice passed `um_vector2_*` KUnit
  with 87 pass and 2 trusted-TAP skips, plus fd handoff, in-process TAP, and
  fd multiqueue smokes.
- A current fixed-byte bidirectional TCP refresh with 1 MiB, 8 MiB, and
  32 MiB transfers confirms the guest-to-host improvement: vector2/legacy
  best observed host-side ratios were 0.966, 1.007, and 0.925 respectively.
  Host-to-guest is mixed: 8 MiB and 32 MiB clear at 2.002 and 0.901, but
  1 MiB remains below legacy at 0.483 in the bidirectional run and 0.579 in a
  focused four-repeat rerun.
- The vector2 runtime transport claim is bounded to TAP and inherited fd.
  GRE and L2TPv3 remain parser/header-helper coverage only; raw, proxy, VDE,
  BESS, and hybrid are unsupported by the current netdev datapath. KUnit now
  guards that parser-only transports fail explicitly with `-EOPNOTSUPP`
  instead of being counted as implemented runtime transports.
- Kconfig now keeps v2 opt-in without saying the existing vector driver is
  superseded or telling new deployments to prefer v2. The v2 help text names
  TAP and inherited fd as the current netdev runtime scope.
- `fail_open_after=N` remains available for the live vector2 open-unwind gate,
  but it is documented as validation-only in the kernel config structure,
  umlctl schema comments, and example README. Normal workloads should leave it
  unset.
- `vector2-failed-open` PASS on the current tree: `umlctl gate loop` ran the
  fd-handoff TAP path with `fail_open_after=2`, verified host gateway ping,
  drove a second `ndo_open()` failure, observed `open_delta=1`,
  `fail_delta=1`, `close_delta=1`, and left `vec2.0` closed/registered with
  the host TAP cleaned up.

Current vector2 validation gates are tracked in
`08-future-phases/49-uml-vector-driver-v2-validation-gates-2026-05-17.md`;
older post-May-19 default-flip summaries are historical branch snapshots.

Open vector2 publication work:

- finish a natural 7200-second seccomp/vector2 run;
- complete the same Tier 3 coverage on KVM v2 now that the current-head
  one-iteration Django-v2/FastAPI-v2 path smoke passes;
- extend performance coverage beyond the fixed guest-to-host TCP gate: follow
  up the host-to-guest 1 MiB regression, add UDP, syscall-rate, and
  CPU-utilisation data, and expand fairness/performance coverage for
  multiqueue operation;
- keep CI/preflight coverage aligned with the launcher-facing configuration.

## Fork-Server And Pool Work

Template pause, pool spawning, pool serving, identity application, and the
syzkaller shim are useful UML features, but they are not part of the KVM v2
backend core. Their current state should be documented separately from the
backend publication series.

Current boundary:

- the pool daemon and basic pool selftests exist;
- kernel identity parsing and application have KUnit coverage;
- the syzkaller-facing shim and JSON command paths exist, and
  `syzkaller-shim-smoke` validates the take/exec/port-forward/status/destroy
  wire path through `umlctl`;
- comparison against `fork-server-phase1c`, `memo09-phase2`,
  `memo09-phase3-pool-bench`, and `memo09-phase4` is documented in
  `06-sequencing/2026-06-10-pool-fork-historical-comparison-plan.md`;
- current `next` already contains the memo09 command surfaces, pivot mode,
  pool-member mode, per-member mconsole path handling, and related selftests;
- `template-pause-smoke` now completes with bounded teardown: cases 1-3 pass
  and the vector2 case skips when `vec0` is not visible in the guest;
- `template-pause-fork-smoke`, `template-pause-fork-stress`,
  `template-pause-pivot-smoke`, `template-pause-pool-member-smoke`,
  `template-pause-pool-sustained-smoke` with replication,
  `pool-spawn-smoke`, `pool-serve-smoke`, `pool-exec-smoke`,
  `pool-port-forward-smoke`, `pool-mconsole-path-probe`, `pool-bench`, and
  `syzkaller-shim-smoke` pass against the rebuilt current-HEAD `./linux`
  binary at `59ad334001ea`
  (`7.1.0-rc7-00187-g59ad334001ea`);
- `template-pause-pool-member-smoke` now tears down the full UML process group
  after the long-lived member reaches `MEMBER_DONE`, so the one-shot PASS does
  not leave an orphaned member process;
- `template-pause-fork-smoke` now drives two SIGSTOP/SIGCONT cycles and
  observes two distinct child PIDs plus two master resume cycles;
- `template-pause-fork-stress` passed its default gate with 540 kernel
  iterations in 10 seconds, median 18.5 ms iteration time, 540/540 clean
  identity round-trips, 425 distinct child PIDs, no kernel panics, no RSS
  drift, and no live orphans after teardown;
- `template-pause-pool-sustained-smoke` remains an expected failure after the
  first member in the default path because the current MAP_SHARED physmem model
  does not support repeated member lifetime; this default XFAIL remains useful
  as a sentinel for accidental shared-physmem regressions; the gated
  replication path now copies and remaps the full runtime
  `uml_reserved..high_physmem` kernel physmem window before the child mutates
  task, timer, or saved-register state, resets inherited timer and hrtimer
  queues, and passes the sustained smoke with `UML_POOL_REPLICATE=1`: three
  members reach `MEMBER_DONE`, `POOL_REPLICATE_OK` appears three times, and no
  kernel panic or v1 ceiling regression is observed;
  `template-pause-pool-member-smoke` also passes, including five timer ticks;
  the
  implementation path is tracked in
  `06-sequencing/2026-06-10-sustained-pool-physmem-isolation-plan.md`;
- `umlctl pool serve` now boots the master with replicated pool-member mode,
  maintains a separate `min_warm` ready queue, reports ready/taken/failed
  counts in status, exposes `pool take --ready` for consuming a pre-identified
  ready member, and SIGSTOPs returned members into a live quiesced state until
  daemon-routed `exec` resumes them; `pool-serve-smoke` proves `--min-warm=1`
  prefills one ready member, an anonymous ready take returns a live pid, the
  daemon replenishes the ready queue, request-specific lazy `take` still
  returns a live member, destroy makes members non-runnable, and shutdown kills
  the master cleanly;
- `pool-bench` now gates the 100-member memory target on proportional set size
  rather than summed RSS. Summed RSS is still printed as diagnostic context, but
  the pass/fail metric is PSS because live quiesced members intentionally share
  executable, libc, and tmpfs-backed physmem pages that RSS counts once per
  process;
- the full default-scale `pool-bench` passes all five gates on the current
  rebuilt binary: p50 1.8 ms, p99 2.4 ms, 100/100 live quiesced members at
  138.9 MiB PSS and 510.0 MiB summed RSS, 17.5 MiB private dirty, 0.05%
  lifecycle drift across 10,000 cycles, and 3000/3000 throughput takes in the
  60-second gate;
- `pool-mconsole-path-probe` now passes: with a non-empty `mconsole_path`, the
  replicated member reaches `PMCON_MEMBER_DONE` without panic, the requested
  per-member mconsole socket exists, and the socket answers `version`;
- `pool-exec-smoke` now validates successful daemon-routed guest exec through
  the member mconsole socket: `/bin/true` exits 0, a shell command returns
  captured stdout/stderr and guest exit code 7, a one-second timeout returns
  exit code 124 with `timed_out=true`, no late stdout, and no leaked guest
  `sleep` helper, and stale daemon error boundaries such as missing
  `uml_mconsole(1)` or kernel `Unknown command` are rejected; `exec/1` is the
  current public ABI, while the bounded shell-backed mconsole lowering and guest
  `timeout(1)` helper dependency are documented implementation details. A
  stricter kernel argv/env/cwd transport is future `exec/2` work, not a blocker
  for the current completion claim;
- request-specific warm scheduling remains intentionally lazy because the
  kernel applies identity before forking the member; pre-warmed members carry
  daemon-assigned identity and cannot safely be rebound to a later caller
  MAC/TAP/mconsole request. This is the final current contract: syzkaller uses
  request-specific lazy takes for deterministic TAP/IP identity, while
  `pool take --ready` is daemon-assigned identity only;
- `vector2-sandbox-audit` validates the untrusted vector2 fd boot audit:
  `umlctl gate loop --audit-vector-sandbox` ran a vector2 auto-queue fd boot
  and reported `PASS=1/1 FAIL=0 TIMEOUT=0` with no forbidden host operations;
- a bounded KVM-v2/vector2 Tier 3 smoke now validates the same network path
  under the KVM-v2 backend for one Django-v2 and one FastAPI-v2 iteration on
  rebuilt `98166580dc4f`; both logs reached `SERVER_READY`,
  `GUEST_CURL ok=100 fail=0`, `TIER3_OK`, and `REPRO_DONE rc=0`;
- vector2 parser-only transports are explicitly outside the current runtime
  transport claim: only TAP and inherited fd are netdev-backed; raw, GRE,
  L2TPv3, hybrid, BESS, VDE, and proxy return `-EOPNOTSUPP` from the netdev
  open path, with GRE/L2TPv3 header helpers retained under KUnit coverage;
- `vector2-inproc-tap-smoke` validates the explicit trusted vector2
  in-process TAP path: `umlctl up` reports `transport=tap host_mode=inproc`,
  no launcher-owned fd inheritance is reported, guest metadata reports
  TAP/inproc mode with fd count zero, and the guest reaches the host-side TAP
  with a one-packet ping;
- `vector2-fd-multiqueue-smoke` validates launcher-owned vector2 fd
  multiqueue handoff: `umlctl up` creates a multiqueue TAP, opens four TAP
  fds, inherits fd range 200..203, guest metadata reports four queues and four
  fds, `vec2.0` reports four TX queues, and the guest reaches the host-side
  TAP with a one-packet ping;
- `vector2-fd-handoff-smoke` validates launcher-owned vector2 fd handoff:
  `umlctl up` creates the TAP, opens the TAP queue in the launcher, reports
  fd 200 inheritance, the guest sees `UMLCTL_NETWORK_*` fd metadata, `vec2.0`
  has the assigned IPv4 address, and the guest reaches the host-side TAP with
  a one-packet ping; and
- `vector2-pool-tap-smoke` now validates live vector2 pool-member TAP
  handoff: `pool serve` boots a vector2 TAP-backed master, `pool take`
  assigns a different per-member TAP/MAC/IPv4/mconsole identity, daemon-routed
  `exec` observes the assigned `vec2.0` address, brings the link up, and
  reaches the host-side TAP with a one-packet ping.

The 2026-06-11 pool/fork/syzkaller rerun is a focused current-HEAD regression
pass. It does not replace the final integration matrix, and the same pool and
syzkaller gates must still be rerun after any later KVM v2 or vector2 changes.

Remaining pool/vector2 boundary:

- launcher-owned vector2 fd handoff is validated on the standalone `umlctl up`
  path. Per-take pool fd handoff is retired from the current completion claim:
  historical design notes deferred a future SCM_RIGHTS TAP-fd swap, but
  current `next` pool takes deliberately carry string identity through the
  identity memfd and use the vector2 TAP reopen path for per-member TAP
  isolation.

## Host Tooling

The active launcher and build tooling is validated on `next` for the core
developer-facing paths:

- `cargo fmt --check` and `cargo test` pass in `tools/uml/uml-launcher`;
- `umlctl-smoke` and `launcher-smoke` pass against the current build;
- `umlctl gate run --dry-run` passes against
  `tools/testing/selftests/um/gates/launcher-cargo.toml`;
- `run-bpftrace-validate.sh` attaches all five transparency scripts, with
  syscalls and sched producing idle UML data;
- historical `umlctl-deploy` comparison found no missing example or profile
  TOML files; all 18 example Umlfiles pass `umlctl up --dry-run` with the
  current `./linux` build, and all 5 built-in profiles resolve with
  `umlbuild profile show`; and
- `run-umlbuild-mvp.sh` passes when `UMLBUILD_SOURCE` points at a clean
  temporary source worktree and the already-built debug `umlbuild`/`umlctl`
  binaries are supplied. The selftest now accepts `UMLBUILD_SOURCE`,
  `UMLBUILD`, and `UMLCTL` overrides so validating `umlbuild` does not require
  destructive cleanup of a developer tree that contains in-tree kernel build
  products; and
- active launcher, selftest, and non-redesign UML documentation cleanup is
  closed for the targeted planning-label patterns: focused scans over
  `arch/um`, `tools/testing/selftests/um`, `tools/uml/uml-launcher`, and
  non-redesign `Documentation/virt/uml/*.rst` no longer find standalone
  workstream, decision-log, memo, phase-history, or future-phase labels, apart
  from a real OpenWrt sample kernel version line containing `#0`.

The active deployment path is `umlctl up/down` with Umlfile parsing, not a
separate `deploy` subcommand.

## Profiles And Instrumentation

The active profile surface has two layers:

- 10 kernel Kconfig profiles under `arch/um/configs/profiles/`;
- 5 `umlbuild` TOML profiles under `tools/uml/uml-launcher/profiles/`.

Current profile evidence:

- all 10 kernel profiles pass clean-worktree config generation through
  `make ARCH=um O=<out> uml/<profile>`;
- all 5 `umlbuild` profiles resolve through `umlbuild profile show`;
- `research-kmsan` is now documented, listed in `make ARCH=um help`, and
  covered by the profile runtime-probe harness when an LLVM-built
  `/tmp/uml-profile-research-kmsan/linux` binary is present; and
- full per-profile kernel builds and runtime feature probes remain open.

Current instrumentation evidence:

- normal dynamic ftrace is validated by a clean-worktree UML build that passes
  `ftrace-smoke` with `FUNCTION_TRACER=y` and no `FUNCTION_GRAPH_TRACER`;
- UML no longer advertises function graph tracing because the fgraph
  return-address rewriting path is not safe across UML task switching;
- `hooks-flip` passes against the current `./linux` build;
- kprobes/kretprobes are validated by a clean-worktree research-profile build
  plus `samples/kprobes/kretprobe_example.ko`; `kprobes-stress` passes 1000
  fork-heavy iterations with `fires=1004`, `errors=0`, and `graph=on`;
- BPF/JIT is runtime-validated for the research profile: clean-worktree
  `make ARCH=um O=<out> uml/research` enables `CONFIG_HAVE_EBPF_JIT=y`,
  `CONFIG_BPF_SYSCALL=y`, `CONFIG_BPF_JIT=y`, and
  `CONFIG_BPF_JIT_ALWAYS_ON=y`, `arch/x86/net/bpf_jit_comp.o` builds, and
  `bpf-jit-smoke` passes on a fresh research-profile UML binary with
  `bpf_jit_enable=1`, `xlated_len=16`, and `jited_len=16`;
- KASAN is runtime-validated for the research profile: a fresh
  research-profile UML binary plus `mm/kasan/kasan_test.ko` passes
  `cve-repro` with `ok=10`, `not_ok=0`, `kasan_bugs=13`,
  `guest_wall_s=0`, and `host_wall=2.71s`;
- KFENCE is runtime-validated for the research profile: a clean-worktree
  research build enables `CONFIG_KFENCE=y`,
  `CONFIG_KFENCE_KUNIT_TEST=m`, and `CONFIG_KFENCE_SAMPLE_INTERVAL=100`,
  `mm/kfence/kfence_test.ko` builds, the runtime profile probe reports
  `PASS research (8 features match)`, and `kfence-smoke` passes with
  `KFENCE_SMOKE: PASS bugs=1 stats_bugs=1 ok=1 not_ok=0`;
- KCSAN is runtime-validated for the race profile: a clean-worktree
  race build enables `CONFIG_HAVE_ARCH_KCSAN=y`, `CONFIG_KCSAN=y`,
  `CONFIG_KCSAN_SELFTEST=y`, `CONFIG_SMP=y`, `CONFIG_DEBUG_FS=y`,
  and `CONFIG_FTRACE=y`, the runtime profile probe reports
  `PASS race (5 features match)`, and `kcsan-smoke` passes with
  `KCSAN_SMOKE: PASS selftest=1 initial=0 enabled=1 disabled=0`,
  `microbench_begin=1`, `microbench_end=1`, and `unexpected=0`;
- KCOV is runtime-validated for the fuzz profile: a fresh fuzz-profile UML
  binary has `CONFIG_KCOV=y`, `CONFIG_KCOV_ENABLE_COMPARISONS=y`, and
  `CONFIG_KCOV_INSTRUMENT_ALL=y`, the runtime profile probe passes for
  `fuzz`, and `kcov-smoke` passes with `mode=pc`, `entries=4094`, and
  `first_pc=0x605daf39`; and
- KMSAN is runtime-validated for the research-kmsan profile:
  `KMSAN_SMOKE: PASS runtime=y reproducer=n`.
- KGDB is not part of any current UML profile. This is an explicit deferral,
  not a validated feature.

## Historical-Only Work

The following work is not yet present in the active `next` implementation:

- private state-trace ring and parser tooling.

KVM-specific record/replay is present as an experimental core with KUnit
coverage. Gadget-handled syscalls now have record/replay bypass plumbing: the
active record static key synchronizes a per-vCPU gadget-state byte, and the
LSTAR gadget falls back to the host dispatcher when that byte is set. Live
syscall recording and snapshot-backed record start are now covered by the
`kvm-record-smoke` debugfs test. UML time-travel clock events are covered by
`kvm-record-clock-bench`. Replay entries now carry explicit format version and
flags fields, and debugfs reports the format contract. The initial payload
copyout set now covers `uname(2)` and `getcwd(2)`. Strict replay rejects
syscalls outside the initial R/R-1 subset instead of replaying arbitrary
scalar-only entries. Raw time syscalls are fail-closed, and replay mode sets
CR4.TSD so user `RDTSC`/`RDTSCP` faults. Replay mode blocks `SIGALRM` at the
KVM vCPU signal mask while inside `KVM_RUN`, so timer delivery is deferred to
the post-exit UML signal path rather than becoming an unrecorded in-guest
interruption point. Randomness and external I/O syscalls outside the supported
subset are also fail-closed in strict replay. The task-owned smoke now proves
deterministic replay for the bounded 386-entry scalar plus
`uname(2)`/`getcwd(2)` payload workload. Replayable raw-time payloads, explicit
signal-event ordering, replayable device/network/hostfs events, and a live
supported-entry mismatch smoke remain open.

The private state-trace ring remains historical reference material. The
historical source is not a clean import target because it contains stale field
assumptions and investigation-specific auto-freeze logic. Current `next` uses
normal `TRACE_EVENT` coverage as the supported KVM v2 observability surface;
state trace should only return as a bounded optional debug facility with fresh
tests and parser coverage.

Snapshot capture/restore and snapshot ELF export have been restored as active
source. KUnit coverage, live `umlctl` ELF export validation, snapshot
benchmark, snapshot KUnit wrapper, snapshot ELF roundtrip, and restore smoke
now pass. SMP snapshot semantics are closed by an explicit
single-online-CPU gate; multi-vCPU snapshot support remains future work unless
all-vCPU quiescence is implemented.

## Publication Checklist

Before treating UML v2 as publishable, verify:

- `make ARCH=um O=<build-dir> -j$(nproc) vmlinux` passes on the cleaned tree;
- KVM v2 KUnit suites pass in the configured UML build;
- CPython parity remains 21/21 against seccomp;
- `cpython-tier0` continues to pass through `umlctl gate run` for both seccomp
  and KVM v2;
- KGDB remains absent unless a future slice adds `HAVE_ARCH_KGDB`, backend
  register access, transport support, and a smoke test;
- the substrate gate stays at or above the current shared floor: seccomp
  `PASS=25 FAIL=3 EXPECTED_FAIL=3`, KVM v2
  `PASS=27 FAIL=3 EXPECTED_FAIL=1`;
- the final KVM v2 24-hour soak completes naturally;
- vector2 Tier 3 runs complete on both seccomp and KVM v2 where applicable;
- checkpatch on changed KVM v2 patches has no unexplained warnings;
- public docs describe the design and validation state, not the development
  history;
- old generated reports and slide decks are either rebuilt from the final
  inventory or visibly archived as historical artifacts;
- historical-only experiments are either imported with tests or clearly marked
  as not part of the completed branch.

## Maintenance Rules

Keep this file current and short.

- Do not append dated session logs.
- Do not store task ledgers, issue queues, or old hypotheses here.
- Keep detailed root-cause writeups in the redesign archive.
- Keep this file as the top-level readiness view for reviewers and future
  maintainers.
