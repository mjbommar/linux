.. SPDX-License-Identifier: GPL-2.0

==============================
UML profile: prod-with-hooks
==============================

:Intended user: production workload that needs optional
   incident-capture capability
:Backend: DYNAMIC — ``backend=auto``
:Fragment: ``arch/um/configs/profiles/prod-with-hooks.config``

What this profile is for
========================

The production-fast profile, plus runtime-flippable Layer 2 gates
reachable through ``/sys/kernel/debug/um/hooks/*``. All gates ship
compiled-in but off at boot; an operator enables narrow observability
on demand during an incident, then flips back off.

This is the intended default for production workloads where incidents
happen and need to be diagnosed without a reboot.

Build
=====

::

   make ARCH=um uml/prod-with-hooks
   make ARCH=um -j$(nproc)

What's on
=========

- **Backend**: DYNAMIC (auto-selects seccomp → ptrace).
- **debugfs**: ``CONFIG_DEBUG_FS=y`` — exposes
  ``/sys/kernel/debug/um/`` with the ``backend``, ``hooks/<gate>``,
  and ``stats`` files. See
  ``Documentation/virt/uml/debugfs.rst``.
- **Modules**: operators may need to load device modules on demand.
- **MAGIC_SYSRQ**: for emergency debug access.
- **FTRACE** core (no ``FTRACE_SYSCALLS``): enables
  ``/sys/kernel/tracing/`` for ad-hoc use.

What's off by default
=====================

- Every Layer 2 gate is OFF at boot. Flipping one on adds the per-hook
  slow-path cost documented in the redesign cost model; flipping it
  off restores the baseline.
- No sanitizers. No heavy tracing consumers.

Incident-capture pattern
========================

::

   # identify a slow syscall
   mount -t debugfs none /sys/kernel/debug
   echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls
   # reproduce the problem for a few seconds
   cat /sys/kernel/debug/um/stats           # inspect hit counts
   echo 0 > /sys/kernel/debug/um/hooks/trace_syscalls

When to use something else
==========================

- The workload never needs any observability → ``prod-fast`` (smaller
  binary).
- You're already debugging → ``research`` (all tools on).
- You're running hostile workloads → ``sandbox`` (no debug surface
  at all; the hooks themselves are a marginal attack surface).

See also
========

- :doc:`index`
- :doc:`prod-fast`
- :doc:`research`
