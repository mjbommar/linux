.. SPDX-License-Identifier: GPL-2.0

=======================
UML profile: prod-fast
=======================

:Intended user: production workload with no runtime observability
   requirement
:Backend: DYNAMIC — ``backend=auto`` picks seccomp where available,
   ptrace otherwise (KVM will be added in workstream D)
:Fragment: ``arch/um/configs/profiles/prod-fast.config``

What this profile is for
========================

Minimum-overhead UML for workloads that are already debugged and just
need to run fast. The smallest binary and the fastest syscall path
the current codebase offers. No runtime knobs — if an incident
requires observability, the profile to reach for is
``prod-with-hooks``, not this one.

Build
=====

::

   make ARCH=um uml/prod-fast
   make ARCH=um -j$(nproc)

What's on
=========

- **Backend**: DYNAMIC. On most modern hosts this selects seccomp at
  boot (faster than ptrace). On hosts without seccomp-filter support
  it falls back to ptrace. Boot param ``backend=ptrace`` or
  ``backend=seccomp`` forces the choice.
- **Size tuning**: ``CC_OPTIMIZE_FOR_SIZE=y``.
- **DEBUG_KERNEL=y** — WARN_ON() is still active; DEBUG sub-options
  with runtime cost stay off.

What's off
==========

- **debugfs** (``CONFIG_DEBUG_FS is not set``) — the Layer 2 gate
  toggle interface (``/sys/kernel/debug/um/hooks/*``) does not exist
  at runtime. Gates are compiled in but stay in their default (off)
  state for the boot's lifetime.
- **Tracing** — ``CONFIG_FTRACE`` is not set.
- **Modules** — ``CONFIG_MODULES`` is not set. Everything is statically
  linked.
- **SysRq, IKCONFIG, BSD_PROCESS_ACCT** — all off.

When to use it
==============

Steady-state production workloads that have already been profiled
and don't need runtime knobs. Or a latency-sensitive benchmark target
where you want to measure the floor of the stack's overhead.

When to use something else
==========================

- You might need to capture an incident → ``prod-with-hooks``
- You're debugging or profiling → ``research``
- You're running hostile workloads → ``sandbox``

See also
========

- :doc:`index`
- :doc:`prod-with-hooks`
