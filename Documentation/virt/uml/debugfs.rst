.. SPDX-License-Identifier: GPL-2.0

========================
UML debugfs runtime API
========================

The UML kernel exposes runtime-flippable observability hooks under
``/sys/kernel/debug/um/`` when built with ``CONFIG_DEBUG_FS=y``. The
interface is the user-facing side of UML's static-key hook
infrastructure.

Requires root (files are mode ``0400``/``0600``).

Layout
======

::

  /sys/kernel/debug/um/
  ├── backend            (ro, 0400)  current backend name
  ├── hooks/
  │   ├── trace_syscalls        (rw, 0600)  0|1
  │   ├── kcov_enabled          (rw, 0600)  0|1
  │   ├── time_travel_active    (rw, 0600)  0|1
  │   ├── kfence_sample         (rw, 0600)  0|1
  │   ├── record_replay         (rw, 0600)  0|1
  │   └── perf_dispatch         (rw, 0600)  0|1
  ├── kvm_v2_snapshot_bench (wo, 0200)  KVM v2 snapshot benchmark trigger
  ├── kvm_v2_snapshot_elf_export_path (wo, 0200)  KVM v2 snapshot ELF export
  ├── kvm_v2_record_ctl     (wo, 0200)  experimental KVM v2 record control
  ├── kvm_v2_record_status  (ro, 0400)  experimental KVM v2 record counters
  └── stats              (ro, 0400)  per-hook on-state + hit counter

``backend``
-----------

Plain text, one line, the current backend's name: ``ptrace``,
``seccomp``, or a KVM backend name when KVM is enabled. Empty backend
shows ``(uninitialized)`` — should never be observed on a booted
kernel.

``hooks/<gate>``
-----------------

Each file is a single bit: ``0`` when the gate is off (default),
``1`` when on. Reading returns the current value; writing flips
the underlying ``static_branch``::

  # cat /sys/kernel/debug/um/hooks/trace_syscalls
  0
  # echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls
  # cat /sys/kernel/debug/um/hooks/trace_syscalls
  1

Writing a value other than ``0`` or ``1`` returns ``-EINVAL``.
Toggling is effective immediately: the next syscall / page fault /
context switch / IRQ / clock read that crosses the relevant hook
site will fire the slow path.

Gate semantics:

``trace_syscalls``
    Fires the trace slow-path on every syscall entry/exit, page
    fault, context switch, and IRQ. Destined for the kernel's
    ftrace/trace-events subsystem.

``kcov_enabled``
    Fires on syscall entry. Builds without a live KCOV consumer use
    it as a counter-only hook.

``time_travel_active``
    Fires on clock reads. Engaged by the time-travel profile to
    swap in deterministic timekeeping.

``kfence_sample``
    Fires on clock reads. Reserved for KFENCE sampling control.

``record_replay``
    Fires on time-travel clock advances. When a KVM v2 record container
    is active, the hook records the advanced nanosecond value and the
    current syscall-count anchor. It does not allocate a KVM v2 record
    container by itself.

``perf_dispatch``
    Fires on syscall entry and context switch. Feeds the kernel's
    perf subsystem.

``stats``
---------

One line per gate, YAML-ish. Three columns: gate name, on/off state,
cumulative hit count since boot. Example on an idle system with
``trace_syscalls`` flipped on briefly::

  # cat /sys/kernel/debug/um/stats
  trace_syscalls       on=0 hits=147
  kcov_enabled         on=0 hits=0
  time_travel_active   on=0 hits=0
  kfence_sample        on=0 hits=0
  record_replay        on=0 hits=0
  perf_dispatch        on=0 hits=0

KVM v2 snapshot controls
========================

When ``CONFIG_UM_BACKEND_KVM_V2=y`` and ``CONFIG_DEBUG_FS=y``, the KVM v2
backend also exposes validation-oriented snapshot controls:

``kvm_v2_snapshot_bench``
    Write-only benchmark trigger. Write a positive integer iteration count to
    run repeated KVM v2 capture/restore cycles against the current task state,
    using the same benchmark engine as the ``kvm_v2_snapshot_bench=<n>``
    command-line option. The value is bounded by the kernel implementation and
    returns ``-EINVAL`` for malformed or out-of-range input. This file is for
    validation and development, not a stable workload-management interface.

``kvm_v2_snapshot_elf_export_path``
    Write-only ELF64-core export trigger. Write an inside-guest path to capture
    a fresh KVM v2 snapshot and synchronously write the snapshot ELF file. This
    is useful for guest scripts and self-contained validation. For host-driven
    live exports, prefer ``umlctl snapshot export <instance> --output <path>``,
    which sends mconsole ``snapshot_export <host-path>`` and avoids guest-path
    ambiguity.

KVM v2 record controls
======================

When ``CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL=y`` and
``CONFIG_DEBUG_FS=y``, the KVM v2 backend also exposes an experimental
record container control surface:

``kvm_v2_record_ctl``
    Write-only command file. Supported commands are::

      start [bytes]
      stop
      replay
      strict 0|1
      destroy

    ``start`` allocates the singleton debugfs record container on first
    use, captures a KVM v2 task snapshot, attaches it to the record
    session, and enables the KVM v2 record static key. ``bytes`` is
    optional; the default buffer is 64 KiB and the maximum is 64 MiB.
    Once the singleton exists, later ``start`` commands may only request
    a size no larger than the existing buffer. ``stop`` disables the
    static key. ``destroy`` stops an active session if needed and resets
    the singleton to ``init`` state; the storage stays allocated for the
    lifetime of the UML instance so no live dispatcher can observe freed
    record memory. ``replay`` restores the attached snapshot before
    entering the experimental syscall replay core, but deterministic
    workload replay is not yet a supported user workflow.

``kvm_v2_record_status``
    Read-only status and counters. Example after stopping a live record
    run::

      state: stopped
      enabled: 0
      strict: 1
      format_version: 1
      entry_header_size: 24
      entry_size: 96
      max_payload: 4096
      snapshot_attempted: 1
      snapshot_valid: 1
      snapshot_rc: 0
      snapshot_memslots: 2
      snapshot_task_state: 1
      snapshot_source_pid: 1
      buffer_size: 1048576
      buffer_used: 38608
      buffer_replayed: 0
      sequence: 398
      entries_recorded: 398
      entries_replayed: 0
      entries_dropped: 0
      syscall_count: 398
      first_syscall_pid: 1
      last_syscall_pid: 1
      syscalls_from_snapshot_task: 398
      syscalls_from_other_tasks: 0
      payload_entries_recorded: 2
      payload_entries_replayed: 0
      payload_bytes_recorded: 392
      payload_bytes_replayed: 0
      strict_replay_failures: 0
      last_replay_failure_syscall: -1
      last_replay_failure_rc: 0

The record path is still explicitly experimental. It can record live
KVM v2 syscall returns through the host dispatcher and the LSTAR gadget
bypass path, and it can pair a record session with a KVM v2 task snapshot.
It can also round-trip UML time-travel clock advances through the record
log. The status counters identify whether recorded syscalls came from the
snapshot owner or from other tasks, which lets selftests distinguish a
task-owned record run from a control-file writer that merely enabled
recording for later work. The first payload-aware syscalls are ``uname(2)``,
which records the returned ``struct new_utsname``, and ``getcwd(2)``, which
records the returned path bytes. Replay mode can restore those payloads when
the syscall number and arguments match. Strict replay currently allows the
R/R-1 scalar task-owned subset (``getpid``, ``getppid``, ``gettid``) plus
payload-aware ``uname(2)`` and ``getcwd(2)``; other syscalls fail closed and
update the strict replay failure counters instead of falling back to live
execution. Full deterministic replay still needs broader payload coverage plus
raw time/RDTSC, signal, and device policy described in the redesign plan. The
current in-memory event format is versioned and
debugfs reports its header size, total fixed entry size, and maximum
variable payload length so validation tools can reject stale logs instead of
guessing their shape.

Cost impact
===========

With every gate off (default), the steady-state cost is ~1 ns per
gate per hook pass.
Flipping a gate on adds the cost of the slow-path implementation
on *every* pass through that hook site; see ``stats`` to monitor
hit rate.

When ``CONFIG_HAVE_ARCH_JUMP_LABEL=y`` is enabled, the off-state
cost drops further to a 5-byte NOP at each gate. Until then, the C
fallback form is used.

Absent ``CONFIG_DEBUG_FS=y``
============================

The ``sandbox`` profile sets ``CONFIG_DEBUG_FS=n`` and therefore
has no ``/sys/kernel/debug/um/`` tree. The gates themselves are
still compiled in (or can be compiled out via per-gate Kconfig
symbols). Without the debugfs interface, gates remain in their
Kconfig-chosen default state for the lifetime of the boot — nothing
flips them.
