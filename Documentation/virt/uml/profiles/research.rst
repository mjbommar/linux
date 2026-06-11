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

Run
===

Invoke the ``./linux`` binary directly or via ``uml-launcher``.
Example config at
``tools/uml/uml-launcher/examples/research.toml``::

   uml-launcher -v run \\
       --config tools/uml/uml-launcher/examples/research.toml

See ``Documentation/virt/uml/launcher.rst``.

What's on
=========

- **Backend**: SECCOMP_ONLY. Faster than ptrace and better
  integration with sanitizers.
- **Full debug surface**: ``DEBUG_FS``, ``DEBUG_KERNEL``,
  ``DEBUG_INFO`` (DWARF5), ``MAGIC_SYSRQ``, ``IKCONFIG``,
  ``PROC_KCORE``, ``BSD_PROCESS_ACCT``.
- **Tracing**: ``FTRACE``, ``FTRACE_SYSCALLS``, ``USER_EVENTS``,
  ``DYNAMIC_EVENTS``, ``HIST_TRIGGERS``, ``FUNCTION_TRACER`` +
  ``DYNAMIC_FTRACE`` (workstream C-05). See :doc:`../ftrace`
  and :doc:`../kprobes` for mechanism and limitations. Function
  graph tracing is intentionally not enabled for UML; kretprobes
  via the rethook shadow stack remain the supported return-probe
  primitive.
- **Dynamic probes**: since workstream C-04 — ``KPROBES`` (entry
  and mid-function probes via ``int3`` + single-step) and
  ``KRETPROBES`` (return probes via the generic rethook shadow
  stack, auto-selected by ``HAVE_RETHOOK``). ``samples/kprobes/
  kprobe_example.ko`` and ``kretprobe_example.ko`` both load and
  fire; ``/sys/kernel/debug/kprobes/list`` and
  ``/sys/kernel/debug/kprobes/blacklist`` work as expected.
  ``CONFIG_KPROBES_SANITY_TEST`` is enabled; the kunit sanity
  suite runs at boot. ``CONFIG_SAMPLE_KPROBES=m`` +
  ``SAMPLE_KRETPROBES=m`` build the example modules as
  loadables; the ``tools/testing/selftests/um/kprobes-stress/``
  selftest uses ``kretprobe_example.ko`` on ``kernel_clone`` to
  regression-guard the port under a fork-heavy workload
  (``PASS iters=N fires=M errors=0``). ``KPROBE_EVENTS``
  (tracefs-based probe installation) depends on
  ``HAVE_REGS_AND_STACK_ACCESS_API`` which UML does not yet
  provide — tracked as a follow-up port.
- **BPF JIT**: since workstream C-06 v1 (2026-04-21) — UML x86_64
  consumes ``arch/x86/net/bpf_jit_comp.c`` via UML-local shim
  headers (option A per decisions-log D43 fifth-view).
  ``CONFIG_BPF_SYSCALL=y`` + ``CONFIG_BPF_JIT=y`` + ``JIT_ALWAYS_ON``
  are on by default in this profile, so ``bpftrace``'s
  ``kprobe:`` / ``kretprobe:`` matchers and any other BPF program
  type requiring the syscall + JIT are reachable here directly.
  ``/proc/sys/net/core/bpf_jit_enable`` reads ``1`` in a freshly
  booted guest. Upstream-bound hygiene commits (e2b686c962,
  5b95b1bb3e) stay staged under ``Documentation/virt/uml/
  redesign/02-workstreams/C-profiles-and-gaps/upstream-patches/
  bpf-hygiene-v1/`` for later LKML coordination.
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
  ``/sys/kernel/debug/kfence/objects``. ``CONFIG_KASAN_KUNIT_
  TEST=m`` builds ``mm/kasan/kasan_test.ko`` as a loadable
  module; the ``tools/testing/selftests/um/cve-repro/``
  selftest loads it on demand and asserts that the full KASAN
  KUnit suite reproduces CVE-class memory-safety bugs (heap
  OOB, UAF, double-free, stack OOB, etc.) in under 30 s of
  wall-clock — the verifiable form of M6
  ("research profile reproduces a syzbot CVE") from the
  redesign milestones.
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
