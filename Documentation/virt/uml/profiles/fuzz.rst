.. SPDX-License-Identifier: GPL-2.0

==================
UML profile: fuzz
==================

:Intended user: syzkaller-style coverage-guided fuzzing
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/fuzz.config``

What this profile is for
========================

A shape optimized for a coverage-guided fuzz loop that boots UML,
runs a generated program, captures coverage, and reboots many times
per second. KCOV is on; KASAN is on; most other heavy surfaces are
off to keep boot fast.

Build
=====

::

   make ARCH=um uml/fuzz
   make ARCH=um -j$(nproc)

Run
===

The resulting ``./linux`` binary can be invoked directly, or via
``uml-launcher``, which wraps the argv, signal forwarding, and
AFL-forkserver fd plumbing. The fuzz-profile example config ships at
``tools/uml/uml-launcher/examples/fuzz.toml``::

   exec 3<> /tmp/ctl   4<> /tmp/status
   uml-launcher run \\
       --config tools/uml/uml-launcher/examples/fuzz.toml \\
       --forkserver 3,4

See ``Documentation/virt/uml/launcher.rst`` and
``Documentation/virt/uml/snapshot.rst`` for the forkserver protocol
and launcher reference.

What's on
=========

- **Backend**: SECCOMP_ONLY. Fastest boot; cleanest behavior under
  future snapshot/restore.
- **KCOV**: ``CONFIG_KCOV=y``, ``KCOV_ENABLE_COMPARISONS=y``. Expose
  ``/sys/kernel/debug/kcov`` for syzkaller or a custom harness to
  mmap and read.
- **KASAN**: ``KASAN_GENERIC`` — catches the bugs we're fuzzing for.
- **debugfs**: on (needed for KCOV).
- **user_events**: on — a future syzkaller integration can emit
  structured events directly.

What's off
==========

- Modules (smaller attack surface, faster boot).
- SysRq, BSD_PROCESS_ACCT.
- Full FTRACE (adds boot time; fuzz loop reboots too often for
  tracing to be useful).

Using KCOV with this profile
============================

::

   # Inside a fuzz harness:
   fd = open("/sys/kernel/debug/kcov", O_RDWR)
   ioctl(fd, KCOV_INIT_TRACE, N)
   cover = mmap(..., fd, ...)
   ioctl(fd, KCOV_ENABLE, KCOV_TRACE_PC)
   // run target syscalls
   ioctl(fd, KCOV_DISABLE, 0)
   // cover[0] is count; cover[1..] are PCs

(Standard KCOV usage; see
``Documentation/dev-tools/kcov.rst``. The
``tools/testing/selftests/um/kcov-smoke/`` gate boots this profile,
opens ``/sys/kernel/debug/kcov``, initializes and mmaps the buffer,
enables ``KCOV_TRACE_PC``, runs syscalls, and requires nonzero
coverage entries.)

When to use something else
==========================

- You've found a candidate bug and want data-race / replay context
  → ``fuzz-deep``.
- You're debugging a single reproducer → ``research`` (more tools).
- You're running generated programs that might escape the kernel →
  ``sandbox`` — fuzz assumes the target is the kernel, not the
  environment.

See also
========

- :doc:`index`
- :doc:`fuzz-deep`
- ``Documentation/dev-tools/kcov.rst``
