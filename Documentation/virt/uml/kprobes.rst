.. SPDX-License-Identifier: GPL-2.0

================
UML kprobes port
================

UML exposes the standard Linux kprobes API:
``register_kprobe()``, ``register_kretprobe()``, the
``samples/kprobes/`` modules, and the
``/sys/kernel/debug/kprobes/`` debugfs surface.

Availability
============

Kprobes and kretprobes are compiled only when a profile explicitly
enables ``CONFIG_KPROBES``. Today that is the ``research`` profile.
``prod-fast`` and ``sandbox`` intentionally leave it off (no ``int3``
insertion machinery in those profiles' TCBs). ``fuzz`` and
``fuzz-deep`` can turn it on as a build-time override when the fuzz
workload itself cares about probe coverage; the profile defaults
don't select it.

To build a kprobes-capable UML::

   make ARCH=um uml/research
   make ARCH=um -j$(nproc)

Usage
=====

From inside a booted guest the standard in-tree samples work::

   insmod samples/kprobes/kprobe_example.ko symbol=do_sys_openat2
   insmod samples/kprobes/kretprobe_example.ko         # default: kernel_clone

   cat /sys/kernel/debug/kprobes/list
   cat /sys/kernel/debug/kprobes/blacklist

   # Each open() triggers the kprobe; each fork triggers the
   # kretprobe. See dmesg.

   rmmod kprobe_example
   rmmod kretprobe_example

The ``research`` profile also enables ``CONFIG_BPF_SYSCALL`` and
``CONFIG_BPF_JIT`` so that kprobe-backed ``bpftrace`` workflows can be
validated with a matching research-profile runtime. The current BPF/JIT
closure is config/build-proven and still needs a guest runtime smoke with
``bpftool`` or ``bpftrace`` before it should be treated as fully closed.
``register_kprobe()`` / ``register_kretprobe()`` from out-of-tree modules work
today.

How it works
============

Entry and mid-function probes
-----------------------------

Kprobes are installed by writing the single-byte ``int3`` (``0xCC``)
opcode over the probed instruction. The UML kernel runs as a
host-userspace ELF, so executing ``int3`` delivers ``SIGTRAP`` to
the UML process. ``arch/um/kernel/trap.c``'s ``relay_signal()``
dispatches kernel-mode ``SIGTRAP`` to
``arch/um/kernel/kprobes/core.c``'s ``kprobe_int3_handler()``,
which runs the registered pre-handler, points ``%rip`` at an
out-of-line copy of the original instruction, sets ``X86_EFLAGS_TF``
to single-step it, and returns. The subsequent ``SIGTRAP`` (from
the TF step) is dispatched to ``kprobe_debug_handler()`` for the
post-handler and IP fix-up.

Text patching uses the page-scoped ``mprotect`` path documented in
:doc:`section-split`. The single-byte install is atomic with
respect to the host's load/store ordering; no ``stop_machine`` is
needed for a 1-byte poke.

Return probes (kretprobes)
--------------------------

Kretprobes use the generic ``rethook`` framework — UML selects
``HAVE_RETHOOK`` in ``arch/um/Kconfig``, which ``arch/Kconfig``
auto-promotes to ``CONFIG_KRETPROBES=y`` +
``CONFIG_KRETPROBE_ON_RETHOOK=y``. The arch-specific code lives in
``arch/um/kernel/rethook.c`` and consists of four ops:

- ``arch_rethook_trampoline`` (inline asm) — the target every
  probed function's return address is rewritten to.
- ``arch_rethook_trampoline_callback`` — glue that runs
  ``rethook_trampoline_handler()``.
- ``arch_rethook_prepare`` — called from ``pre_handler_kretprobe``
  to rewrite the return slot.
- ``arch_rethook_fixup_return`` — restores the real return address
  after the handler runs.

Current limitations
===================

Three user-visible gaps remain:

``CONFIG_KPROBE_EVENTS`` (tracefs-based probe installation via
  ``/sys/kernel/tracing/kprobe_events``) requires
  ``HAVE_REGS_AND_STACK_ACCESS_API``, which UML does not yet
  provide. Installing probes from C is the workaround. A future
  port will add the regs-and-stack-access helpers.

``HAVE_KPROBES_ON_FTRACE`` (the ftrace-fast-path optimisation that
  routes function-entry probes through the ftrace call stub
  instead of through ``int3``) is not implemented. Function-entry
  probes work via ``int3`` and pay the trap cost (~400 ns per
  hit). Mid-function probes need ``int3`` regardless.

``HAVE_FUNCTION_GRAPH_TRACER`` is intentionally not selected on UML.
  Kretprobes use the generic rethook shadow stack and remain the
  supported return-probe primitive. Function graph tracing needs a
  separate fix for UML's host-task stack switching before it can be
  advertised.

Blacklist and ``NOKPROBE_SYMBOL``
=================================

Certain functions cannot be probed safely and are excluded from
probe installation via ``kprobe_blacklist`` or ``NOKPROBE_SYMBOL``:

- The entire ``arch/um/kernel/kprobes/core.c``, ``arch/um/kernel/
  rethook.c``, and ``kernel/trace/`` instrumentation surface.
- ``arch/um/kernel/ftrace.c`` (ftrace's own machinery).
- ``arch/um/kernel/mcount.S`` (the ftrace_caller trampoline).
- Per-CPU access helpers used during probe dispatch.

The blacklist is enforced by the generic kprobes core at
``register_kprobe()`` time. Attempting to probe a blacklisted
address returns ``-EINVAL``; use ``cat /sys/kernel/debug/kprobes/
blacklist`` to inspect the live list.

Preemption and interrupts
=========================

Pre- and post-handlers run with preemption disabled — the arch code
does this explicitly because a UML ``SIGTRAP`` is just a signal,
not a trap gate. Handlers may therefore call anything safe in
non-preemptible context (e.g. ``printk``) but must not sleep or
call ``might_sleep`` primitives. The generic kprobes sanity test
under ``CONFIG_KPROBES_SANITY_TEST`` (on in the research profile)
enforces this at boot.

Regression harness
==================

``tools/testing/selftests/um/kprobes-stress/`` boots a UML binary
built from the research profile, loads ``kretprobe_example.ko``
against ``kernel_clone``, runs a fork-heavy workload, and asserts
that no ``BUG``/``Oops``/``WARNING``/``panic``/``Segfault-with-no-mm``
appears in ``dmesg`` across N iterations. Host-side driver
``run-kprobes-stress.sh`` passes module path + iteration count
through the UML kernel command line
(``kretprobe_module=/path``, ``kretprobe_iters=N``) because env
vars don't propagate through UML's kernel-start → init exec
path.

Typical run::

   KPROBES_STRESS_ITERS=1000 \\
      UML_BINARY=/tmp/uml-research/linux \\
      UML_KRETPROBE_MODULE=/tmp/uml-research/samples/kprobes/kretprobe_example.ko \\
      tools/testing/selftests/um/kprobes-stress/run-kprobes-stress.sh

A clean run reports
``KPROBES_STRESS: PASS iters=N fires=M errors=0 graph=<on|deferred>``.
Function graph tracing is not advertised on UML, so the current
expected graph token is ``deferred``.

Further reading
===============

- ``Documentation/trace/kprobes.rst`` — the generic kprobes
  user-facing guide.
- ``tools/testing/selftests/um/kprobes-stress/`` — the regression
  harness described above.
