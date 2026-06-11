.. SPDX-License-Identifier: GPL-2.0

==========================
UML profile: time-travel
==========================

:Intended user: deterministic replay, simulation, deterministic
   test runs
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/time-travel.config``

What this profile is for
========================

UML's time-travel mode provides deterministic clock and scheduling:
two runs of the same workload in the same state produce the same
outputs, and the "clock" can be driven by logical steps rather than
wall time. Useful for deterministic test suites, network simulation
harnesses, and debugging workflows that depend on reproducible
ordering.

This profile turns on ``CONFIG_UML_TIME_TRAVEL_SUPPORT`` and pins
the kernel to single-CPU (``!SMP``) — time travel is defined for UP
guests. It also wires enough debug machinery that a developer using
the profile for a tricky race can reach the usual tools.

Build
=====

::

   make ARCH=um uml/time-travel
   make ARCH=um -j$(nproc)

Run
===

Time travel is selected at boot::

   ./linux time-travel= mem=128M rootfstype=hostfs root=/dev/root

See
``Documentation/virt/user-mode-linux-howto-v2.rst`` for the boot
parameters the time-travel machinery accepts.

What's on
=========

- **Backend**: SECCOMP_ONLY.
- ``CONFIG_UML_TIME_TRAVEL_SUPPORT=y``.
- ``!SMP`` — time travel is a UP guarantee.
- Full debug surfaces: ``DEBUG_FS``, ``DEBUG_KERNEL``,
  ``MAGIC_SYSRQ``, tracing (``FTRACE``, ``FTRACE_SYSCALLS``,
  ``USER_EVENTS``), sanitizers (``KASAN``).
- Modules on (time-travel rigs commonly load custom virtio devices).

What's off
==========

- ``CONFIG_SMP`` — forced off; time-travel mode doesn't compose
  with SMP scheduling.

Layer 2 gate note
=================

The ``um_hook_time_travel_active`` gate (see
``Documentation/virt/uml/debugfs.rst``) is present and runtime-
flippable in this profile. Today its slow path is a counter stub;
real deterministic-clock hooks are not wired to it yet.

When to use something else
==========================

- You want SMP → any non-time-travel profile.
- You're doing generic debugging → ``research`` (richer toolset).

See also
========

- :doc:`index`
- ``Documentation/virt/user-mode-linux-howto-v2.rst`` — time-travel
  boot params
