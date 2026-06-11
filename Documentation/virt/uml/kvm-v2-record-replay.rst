.. SPDX-License-Identifier: GPL-2.0

====================
KVM v2 Record/Replay
====================

``CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL`` builds the
experimental KVM v2 record/replay core. It is disabled by default and depends
on ``CONFIG_UM_BACKEND_KVM_V2``. The current implementation is a validation
surface, not a stable workload replay ABI.

Scope
=====

The current record/replay core provides:

* one active in-memory record container per UML instance;
* explicit ``start``, ``stop``, ``replay``, ``strict``, and ``destroy``
  commands through debugfs;
* a bounded, versioned event stream for syscall entries and selected payloads;
* KVM v2 task snapshot capture at record start and restore before replay;
* LSTAR gadget bypass while record/replay is active, so gadget-handled
  syscalls reach the host dispatcher;
* strict replay failure accounting for unsupported syscalls or mismatched
  supported entries.

The feature is intentionally narrow. It proves the KVM v2 plumbing for
snapshot-backed syscall replay, but it is not yet a general deterministic
replay system for arbitrary UML guests.

Debugfs ABI
===========

Mount debugfs inside the UML guest::

  mount -t debugfs none /sys/kernel/debug

The record files live under ``/sys/kernel/debug/um/``:

``kvm_v2_record_ctl``
    Write-only command file. Supported commands are::

      start [bytes]
      stop
      replay
      strict 0|1
      destroy

    ``start`` allocates the singleton record container if needed, captures a
    KVM v2 task snapshot, attaches it to the session, and enables recording.
    ``bytes`` is optional; the default buffer is 64 KiB and the maximum is
    64 MiB. ``stop`` disables recording. ``replay`` restores the attached
    snapshot and enters replay mode. ``destroy`` stops any active session and
    resets counters and snapshot state.

``kvm_v2_record_status``
    Read-only status and counters. It reports the container state, strict
    replay flag, event-format version, buffer usage, syscall and payload
    counters, snapshot metadata, first/last recorded syscall pids, same-task
    versus other-task syscall counts, strict failure count, and the last
    strict failure syscall and return code.

Supported Experimental Tier
===========================

The supported experimental replay tier is task-owned and syscall-bounded:

* the task that writes ``start`` is snapshotted and later restored by
  ``replay``;
* validation requires the recorded workload to come from that same task;
* replay is strict by default and must not fall back to live execution after a
  supported-entry mismatch or an unsupported syscall.

The current replayable syscall set is:

============  ===============================================================
Syscall       Replay behavior
============  ===============================================================
``getpid``    Scalar return value is replayed from the log.
``getppid``   Scalar return value is replayed from the log.
``gettid``    Scalar return value is replayed from the log.
``uname``     Return value and returned ``struct new_utsname`` are replayed.
``getcwd``    Return value and returned path bytes are replayed when the
              replay buffer-size argument matches the recorded one.
``clock_gettime``
              Return value and returned ``struct __kernel_timespec`` bytes are
              replayed when the clock id argument matches the recorded one.
``gettimeofday``
              Return value and the optional returned
              ``struct __kernel_old_timeval`` and ``struct timezone`` bytes
              are replayed when the output-pointer shape matches the recorded
              call.
``time``      Scalar return value is replayed from the log; when the optional
              ``tloc`` pointer is non-NULL, the stored
              ``__kernel_old_time_t`` bytes are replayed too.
============  ===============================================================

The record log can also round-trip UML time-travel clock advances through the
``record_replay`` hook. That is separate from replayable raw-time syscall
payloads.

Strict Replay Policy
====================

Strict replay fail-closes instead of guessing:

* a supported syscall with a mismatched logged entry, such as ``getcwd`` with a
  different buffer-size argument, records a strict replay failure and kills the
  guest;
* unsupported syscalls record a strict replay failure and kill the guest;
* raw time interfaces outside the current replay set remain unsupported;
* replay mode sets CR4.TSD so user ``RDTSC`` and ``RDTSCP`` fault instead of
  observing host time outside the log;
* replay mode asks KVM to block ``SIGALRM`` while the vCPU is inside
  ``KVM_RUN`` so timer delivery cannot create an unrecorded in-guest
  ``EINTR`` point.

The current policy rejects randomness and external I/O syscalls outside the
supported subset, including representative ``getrandom(2)``, ``openat(2)``,
``read(2)``, ``write(2)``, and ``ioctl(2)`` paths. Device, network, and hostfs
event replay are not implemented in this tier.

Current Non-Goals
=================

The following remain outside the current completion claim:

* deterministic replay for arbitrary user workloads;
* replayable asynchronous signal ordering;
* replay for raw-time interfaces beyond direct syscall or current UML vDSO
  wrapper calls to ``clock_gettime(2)``, ``gettimeofday(2)``, and
  ``time(2)``, including native VVAR-style fast paths;
* replayable device, network, hostfs, and randomness events;
* a persistent on-disk record format or a stable user ABI.

Validation
==========

Build the record/replay smoke helpers::

  make -C tools/testing/selftests/um/kvm-record-smoke

Run the KUnit and live KVM v2 smoke gate against a UML binary built with
``CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL=y``::

  UML_BINARY=$PWD/linux \
    tools/testing/selftests/um/kvm-record-smoke/run-kvm-record-smoke.sh

The smoke gate currently validates:

* ``um_kvm_v2_record`` KUnit coverage for lifecycle, format, payload, strict
  policy, time-travel FIFO, buffer overflow, and snapshot cleanup behavior;
* live debugfs recording with a snapshot-backed session;
* task-owned replay of a bounded scalar plus ``uname(2)``/``getcwd(2)``
  payload workload;
* strict fail-closed behavior for a supported ``getcwd(2)`` argument mismatch;
* raw ``clock_gettime(2)``, ``gettimeofday(2)``, and ``time(2)`` payload
  replay from the recorded log through direct syscalls and direct calls to the
  UML vDSO symbols, which route back through syscalls so UML can trap them;
* CR4.TSD fault behavior for direct user ``RDTSC``/``RDTSCP`` under replay;
* tracepoint-visible KVM signal-mask policy that blocks ``SIGALRM`` during
  replay ``KVM_RUN`` and restores the normal mask afterward;
* strict fail-closed behavior for an unsupported ``getrandom(2)`` syscall.

The expected summary line is::

  KVM_RECORD_SMOKE: PASS (KUnit=24/24 live-debugfs=1 task-owned=1 live-mismatch=1 live-signal=1 live-time=1 live-rdtsc=1 live-rdtscp=1 live-negative=1)
