.. SPDX-License-Identifier: GPL-2.0

===============
UML ftrace port
===============

The UML function tracer landed in workstream C-05 of the redesign
(see ``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/05-port-ftrace.md``). It exposes the standard
Linux function-tracer surface — ``/sys/kernel/tracing/``,
``echo function > current_tracer``, ``set_ftrace_filter``,
tracepoints at function granularity — to UML guests.

Availability
============

The function tracer is compiled only when a profile explicitly
enables it. Today that is the ``research`` profile.
``prod-fast`` and ``sandbox`` intentionally leave ``CONFIG_FTRACE``
disabled (zero per-function overhead, zero patch surface).
``fuzz`` and ``fuzz-deep`` enable ``CONFIG_FTRACE`` only to unlock
``USER_EVENTS``; they do not select the function tracer (fast
reboots beat observability for a fuzz loop).

To build a tracer-capable UML::

   make ARCH=um uml/research
   make ARCH=um -j$(nproc)

To use it inside a booted guest::

   mount -t tracefs none /sys/kernel/tracing
   echo function > /sys/kernel/tracing/current_tracer
   # ... reproduce the scenario ...
   cat /sys/kernel/tracing/trace
   echo nop    > /sys/kernel/tracing/current_tracer

How it works
============

UML's port uses the compiler's ``-fpatchable-function-entry=5,0``
insertion rather than ``-pg -mfentry + scripts/recordmcount``
(D29 in the decisions log explains the toolchain choice —
``-mcmodel=large`` makes ``-mfentry`` emit 33-byte indirect call
sequences that ``recordmcount`` cannot recognize). The compiler
places a 5-byte ``nop`` at every traced function's entry and
records its address in ``__patchable_function_entries``. At
runtime the kernel patches the ``nop`` to ``call ftrace_caller``
to enable tracing, and back to ``nop`` to disable.

Runtime patching runs under ``stop_machine_cpuslocked()`` and
wraps the whole batch in a single ``mprotect(RW)`` /
``mprotect(RX)`` on ``[_text, _etext)`` (D28 + D30 explain why:
UML has exactly one ``struct mm_struct``, so the
``fixmap``-alias and ``text_poke_mm`` patterns used by
arm/arm64/riscv/x86 are not available; ``stop_machine`` gives the
closest equivalent safety property — no peer UML vCPU host
thread runs kernel code during the window).

Cost when tracing is off
========================

The compiler-emitted 5-byte NOP is paid at every function entry
regardless of whether tracing is on. Empirically on UML, the
research-profile boot of ``init=/bin/true`` medians:

.. list-table::
   :header-rows: 1

   * - Build
     - Median boot time (3 runs)
   * - ``research`` without ``CONFIG_FUNCTION_TRACER``
     - 3.37 s
   * - ``research`` with ``CONFIG_FUNCTION_TRACER``
     - 3.48 s

A ~3 % delta attributed to the NOP5 i-cache residency at every
call site. Well inside the research profile's design envelope
("every observability surface on; accept the cost"). The
``prod-fast`` and ``sandbox`` profiles do not pay this cost —
they leave ``CONFIG_FTRACE`` off.

What is not supported yet
=========================

- **Function graph** (``HAVE_FUNCTION_GRAPH_TRACER``) is
  deliberately deferred. D27 in the decisions log explains: UML's
  signal-based preemption can race with the ``return_to_handler``
  trampoline's in-flight state; before lighting graph up we want
  a signal-stress selftest. Arm (Thumb2), x86 (non-DYNAMIC), and
  RISC-V (non-WITH_ARGS) similarly gate graph on a subordinate
  capability, so UML deferring it is within existing kernel
  convention.
- ``HAVE_DYNAMIC_FTRACE_WITH_REGS`` / ``_WITH_ARGS`` /
  ``_WITH_DIRECT_CALLS``: not yet implemented. The minimal port
  in C-05 ships only the basic function tracer. REGS is expected
  to land alongside kprobes-on-ftrace in workstream C-04.

Interaction with other profile features
=======================================

See D31 in the decisions log: ``CONFIG_KCOV=y`` + ``CONFIG_FUNCTION_TRACER=y``
on UML-UP causes the first ``echo function > current_tracer`` to
hang for minutes. Root cause: KCOV instrumentation fires at every
basic block in ``kernel/trace/ftrace.c`` during the ~21000-record
batch patching loop, amplified by KASAN shadow-memory checks, with
no ``cond_resched`` because ``stop_machine`` blocks signals. The
resolution for now is profile-separation: ``research`` keeps the
tracer + sanitizers; ``fuzz`` / ``fuzz-deep`` keep KCOV. Three
follow-up engineering angles (upstream patch, narrow
``__no_sanitize_coverage``, per-CPU KCOV soft-off) are preserved
in D31 for a future session.

Selftest
========

``tools/testing/selftests/um/ftrace-smoke/`` exercises the guest
path end-to-end::

   cd tools/testing/selftests/um
   UML_BINARY=/path/to/research/linux \
     bash ftrace-smoke/run-ftrace-smoke.sh

Boots a UML guest built with ``CONFIG_FUNCTION_TRACER=y``, mounts
tracefs, enables the function tracer, runs one ``ls /``, reads
the trace buffer, confirms >100 trace lines captured, disables
the tracer, halts. Reports ``FTRACE_SMOKE: PASS`` / ``FAIL``.
Exits 4 (kselftest skip) if ``UML_BINARY`` is not set or does not
exist.

See also
========

- :doc:`profiles/research`
- :doc:`section-split`
- :doc:`backends`
- ``Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/05-port-ftrace.md``
  (design)
- ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
  §§ D27 (graph deferred), D28 (patch mechanism), D29 (toolchain),
  D30 (bulk mprotect), D31 (KCOV interaction)
