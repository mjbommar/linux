.. SPDX-License-Identifier: GPL-2.0

============================
UML profile: research-kmsan
============================

:Intended user: developers hunting uninitialized-memory bugs under UML
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/research-kmsan.config``
:Toolchain: clang/LLVM required

What this profile is for
========================

``research-kmsan`` is the research profile variant for KMSAN. Use it
when you need Linux's uninitialized-memory detector in a UML guest and
can build the kernel with clang.

It is a separate profile instead of a flag on ``research`` because
KMSAN is mutually exclusive with KASAN and has a much larger memory
footprint. The normal ``research`` profile keeps KASAN/KFENCE/UBSAN;
``research-kmsan`` swaps in KMSAN for uninitialized-memory reports.

Build
=====

::

   make ARCH=um LLVM=1 uml/research-kmsan
   make ARCH=um LLVM=1 -j$(nproc)

Run
===

Boot with at least ``mem=512M``. KMSAN instruments touched memory and
uses significantly more guest memory than the normal research profile.

::

   ./linux mem=512M root=/dev/root rootfstype=hostfs rw

What's on
=========

- **Backend**: SECCOMP_ONLY.
- **KMSAN**: ``CONFIG_KMSAN=y`` and
  ``CONFIG_KMSAN_CHECK_PARAM_RETVAL=y``.
- **FORTIFY**: ``CONFIG_FORTIFY_SOURCE=y`` so explicit memory helpers in UML's
  ``-fno-builtin`` build can route through KMSAN-aware ``__msan_mem*`` helpers.
- **KASAN**: explicitly off, because KASAN and KMSAN cannot coexist
  in one kernel image.
- **Debug surface**: ``DEBUG_FS``, ``DEBUG_KERNEL``,
  ``DEBUG_INFO`` (DWARF5), and ``MAGIC_SYSRQ``.
- **Tracing**: ``FTRACE``, ``FTRACE_SYSCALLS``, ``USER_EVENTS``.
- **Modules**: enabled, matching the normal research workflow.

Regression test
===============

``tools/testing/selftests/um/kmsan-smoke/`` boots a KMSAN-enabled UML
binary and checks that the KMSAN runtime is present. The runner exits
with kselftest SKIP if the supplied binary was not built with KMSAN.

As of 2026-06-11, a clean LLVM ``uml/research-kmsan`` kernel build passes.
The runtime smoke now gets past the earlier KMSAN vmalloc shadow/origin
mapping failure, the UMID host-helper boundary report, the printk
``console_flush_type`` local-state report, and the raw ``memset()`` reports
caused by UML's normal ``-fno-builtin`` build flags. It still fails before the
``KMSAN_SMOKE`` marker with a repeated ``vsnprintf()`` report from the
non-instrumented ``os_add_epoll_fd()`` host-helper path during
``console_on_rootfs()``. Treat this profile as buildable but not yet
runtime-closed.

See also
========

- :doc:`research`
- :doc:`race`
- :doc:`../kmsan`
- ``Documentation/dev-tools/kmsan.rst``
