.. SPDX-License-Identifier: GPL-2.0

================
UML kprobes port
================

The UML kprobes surface landed in workstream C-04 of the redesign
(see ``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/04-port-kprobes.md``). It exposes the standard
Linux kprobes API — ``register_kprobe()``, ``register_kretprobe()``,
``samples/kprobes/`` modules, and the ``/sys/kernel/debug/kprobes/``
tracefs surface — to UML x86_64 guests.

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

``bpftrace``'s ``kprobe:`` and ``kretprobe:`` matchers work once the
workstream C-06 BPF JIT port lands. ``register_kprobe()`` /
``register_kretprobe()`` from out-of-tree modules work today.

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

Text patching uses the page-scoped ``mprotect`` path that the B-04
section split set up. The single-byte install is atomic with
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

Limitations (2026-04-20)
========================

Three user-visible gaps remain after the C-04 landing:

``CONFIG_KPROBE_EVENTS`` (tracefs-based probe installation via
  ``/sys/kernel/tracing/kprobe_events``) requires
  ``HAVE_REGS_AND_STACK_ACCESS_API``, which UML does not yet
  provide. Installing probes from C is the workaround. A future
  port will add the regs-and-stack-access helpers.

``HAVE_KPROBES_ON_FTRACE`` (the ftrace-fast-path optimisation that
  routes function-entry probes through the ftrace call stub
  instead of through ``int3``) is not implemented. Function-entry
  probes work via ``int3`` and pay the trap cost (~400 ns per
  hit). Mid-function probes need ``int3`` regardless. Tracked as a
  Q4-phase follow-up; see D32 for the scope decision.

``HAVE_FUNCTION_GRAPH_TRACER`` is deferred per decisions-log D34.
  The generic fgraph trampoline assumes every traced function
  returns via a matching ``ret`` that pops the rewritten parent
  slot. UML violates that for a class of generic-kernel functions
  (``kthread()``, ``smpboot_thread_fn()``) that end in ``do_exit``
  and never return, plus UML-specific longjmp-entered paths. An
  arch-local workaround would need generic-kernel cooperation or
  a UML-specific graph shim. Until then, kretprobes cover the
  return-instrumentation use case.

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

Further reading
===============

- ``Documentation/trace/kprobes.rst`` — the generic kprobes
  user-facing guide.
- ``Documentation/virt/uml/redesign/02-workstreams/
  C-profiles-and-gaps/04-port-kprobes.md`` — the workstream plan,
  including scope decisions D32/D33/D34.
- ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
  — architectural decisions and their rationale.
- ``tools/testing/selftests/um/kprobes-stress/`` — the regression
  harness.
