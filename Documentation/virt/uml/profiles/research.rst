.. SPDX-License-Identifier: GPL-2.0

======================
UML profile: research
======================

:Intended user: developer debugging or profiling a workload or the
   UML kernel itself
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/research.config``

What this profile is for
========================

"Every observability surface on; accept the cost." The first profile
to reach for when you're clone-and-run'ing this tree as a reviewer:
it has the most things turned on, the richest runtime introspection,
and the widest opportunity to find bugs.

Build
=====

::

   make ARCH=um uml/research
   make ARCH=um -j$(nproc)

What's on
=========

- **Backend**: SECCOMP_ONLY. Faster than ptrace and better
  integration with sanitizers.
- **Full debug surface**: ``DEBUG_FS``, ``DEBUG_KERNEL``,
  ``DEBUG_INFO`` (DWARF5), ``MAGIC_SYSRQ``, ``IKCONFIG``,
  ``PROC_KCORE``, ``BSD_PROCESS_ACCT``.
- **Tracing**: ``FTRACE``, ``FTRACE_SYSCALLS``, ``USER_EVENTS``,
  ``DYNAMIC_EVENTS``, ``HIST_TRIGGERS``, and — since workstream
  C-05 — ``FUNCTION_TRACER`` + ``DYNAMIC_FTRACE`` (see
  :doc:`../ftrace` for mechanism and limitations). Function graph
  is deferred per decisions-log D27.
- **Coverage**: *none*. ``CONFIG_KCOV`` is explicitly off in
  ``research``; coverage-guided fuzzing lives in the ``fuzz`` and
  ``fuzz-deep`` profiles (which do not enable the function
  tracer — fast reboots beat observability in a fuzz loop). See
  decisions-log D31 for why ``KCOV`` and ``FUNCTION_TRACER``
  cohabiting on UML-UP compounds under ``stop_machine``.
- **Sanitizers**: ``KASAN`` (generic), ``UBSAN`` with bounds
  checking, ``KFENCE`` (sampling heap-corruption detector,
  ``sample_interval=100`` ms, 255 guarded objects). KFENCE works
  end-to-end on UML (workstream C-02) — every OOB/UAF inside
  the pool is caught and reported via ``dmesg``; live stats at
  ``/sys/kernel/debug/kfence/stats``, object metadata at
  ``/sys/kernel/debug/kfence/objects``.
- **Lockdep** + **PROVE_LOCKING** + **DEBUG_MUTEXES** +
  **DEBUG_ATOMIC_SLEEP**.
- **Memory debug**: ``DEBUG_VM``, ``DEBUG_PAGEALLOC``,
  ``DEBUG_OBJECTS`` + ``DEBUG_OBJECTS_RCU_HEAD``.
- **Time-travel available** (``CONFIG_UML_TIME_TRAVEL_SUPPORT=y``)
  — opt-in at boot via ``time-travel=`` param.
- Optimization favors debugability over size.

Cost
====

Significantly slower and larger than production profiles. Syscall
paths hit full KASAN + UBSAN instrumentation plus any user-enabled
tracer. Boot is longer. That's the deal.

Incident-capture pattern
========================

::

   # /sys/kernel/debug/um/hooks/* still works as on prod-with-hooks,
   # but research has additional surfaces:
   mount -t tracefs none /sys/kernel/tracing
   echo 1 > /sys/kernel/tracing/events/syscalls/sys_enter_openat/enable
   cat /sys/kernel/tracing/trace_pipe &
   # ... reproduce ...

When to use something else
==========================

- You're trying to measure a performance baseline → research's
  overhead destroys the measurement; use ``prod-fast`` or
  ``prod-with-hooks``.
- You need determinism for replay → ``time-travel``.
- You're fuzzing → ``fuzz`` (a narrower, faster-booting shape).

See also
========

- :doc:`index`
- :doc:`fuzz`
- :doc:`time-travel`
- :doc:`../ftrace` — how the function tracer works on UML
