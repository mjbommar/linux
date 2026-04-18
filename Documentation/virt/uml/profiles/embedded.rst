.. SPDX-License-Identifier: GPL-2.0

=======================
UML profile: embedded
=======================

:Intended user: CI environments, hosts without seccomp-filter
   support, or legacy systems
:Backend: PTRACE_ONLY
:Fragment: ``arch/um/configs/profiles/embedded.config``

What this profile is for
========================

Not everyone runs on a host that supports ``seccomp-filter``. Many
CI environments, shared-kernel containers without ``CAP_SYS_ADMIN``,
and older systems only offer ``ptrace``. This profile targets those
hosts exclusively, pinning the backend to ``PTRACE_ONLY`` so no
seccomp probe runs at boot.

Build
=====

::

   make ARCH=um uml/embedded
   make ARCH=um -j$(nproc)

What's on
=========

- **Backend**: ``PTRACE_ONLY``. Works on any Linux host with
  ptrace support, which is essentially all of them.
- **MCONSOLE**: on — ``mconsole`` is the traditional UML diagnostic
  channel. In an embedded deployment where ``/sys/kernel/debug``
  may not be mountable, mconsole gives a developer a control channel
  into the running kernel.
- **Modules**: on — embedded users often load custom drivers.
- **DEBUG_KERNEL**: on — WARN_ON() + similar remain active.

What's off
==========

- ``DEBUG_FS``, ``FTRACE``, ``MAGIC_SYSRQ`` — no runtime gate
  toggling; the profile is static.

Cost
====

Ptrace is slower than seccomp per trap (see
``Documentation/virt/uml/backends.rst`` for the cost table).
For steady-state workloads on an embedded-class host the trade-off
is usually acceptable. For benchmarks or high-syscall-rate loads,
see whether a host upgrade to a seccomp-capable kernel is feasible.

When to use something else
==========================

- Your host supports seccomp-filter → ``prod-fast`` or
  ``prod-with-hooks`` (faster).
- You're fuzzing → ``fuzz`` (requires seccomp today).
- You're sandboxing → ``sandbox`` (requires seccomp today).

See also
========

- :doc:`index`
- ``Documentation/virt/uml/backends.rst``
