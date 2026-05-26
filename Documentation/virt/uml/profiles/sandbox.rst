.. SPDX-License-Identifier: GPL-2.0

======================
UML profile: sandbox
======================

:Intended user: running untrusted or hostile workloads under UML
   with a minimum trusted computing base
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/sandbox.config``

What this profile is for
========================

The smallest, most feature-stripped UML the tree ships. No debugfs,
no tracing, no modules, no magic SysRq, no proc-kcore. The intended
operator is running workloads they don't trust and wants no
additional attack surface from UML's own introspection machinery.

Build
=====

::

   make ARCH=um uml/sandbox
   make ARCH=um -j$(nproc)

Run
===

Invoke via ``uml-launcher`` (workstream C-10). v1 runs the
sandbox-profile kernel under a single launcher process; v2 adds
per-device vhost-user helpers with seccomp filters for real
host-side isolation. Example config at
``tools/uml/uml-launcher/examples/sandbox.toml``::

   uml-launcher run \\
       --config tools/uml/uml-launcher/examples/sandbox.toml

See ``Documentation/virt/uml/launcher.rst`` for the launcher
surface and the v2 roadmap.

What's on
=========

- **Backend**: SECCOMP_ONLY. Seccomp-filter isolation is part of
  the sandbox story; ptrace fallback would weaken it.
- Base networking (TCP/IP stack, UNIX sockets) — the workload
  typically needs them.
- Size optimization.

What's off
==========

- ``DEBUG_FS``, ``DEBUG_KERNEL``, ``MAGIC_SYSRQ`` — no runtime
  introspection surface accessible from inside the guest.
- ``IKCONFIG``, ``PROC_KCORE``, ``BSD_PROCESS_ACCT``.
- ``FTRACE``, all tracing — no syscall tracer, no user_events.
- ``MODULES`` — static kernel only.
- Optional network drivers (``TUN``, ``PPP``, ``SLIP``, ``DUMMY``,
  ``BINFMT_MISC``) — not useful for the sandbox story, and each
  line is attack surface.

What this profile is NOT
========================

- **Not a container runtime.** See the discussion in
  ``Documentation/virt/uml/redesign/08-future-phases/01-end-user-ideal-world.md``
  §"Tightened container-runtime boundary". A launcher around
  sandbox can provide operational containment (process limits,
  cgroups, host-side seccomp); UML itself is the guest kernel.
- **Not a hardened kernel.** Sandbox is about minimal TCB, not
  about kernel self-hardening. Grsecurity/KSPP features are
  orthogonal and not bundled here.
- **Not immune to guest-kernel bugs.** A sandbox-profile UML is as
  susceptible to a kernel-side vulnerability as any other profile.
  The sandbox story is "the guest environment is small"; it is
  not "the kernel is unbreakable".

When to use something else
==========================

- You need any observability → ``prod-with-hooks`` or ``research``.
- The host doesn't support seccomp-filter → ``embedded``.

See also
========

- :doc:`index`
- ``Documentation/virt/uml/backends.rst``
- ``Documentation/virt/uml/redesign/03-profiles/sandbox.md``
  (design rationale)
