.. SPDX-License-Identifier: GPL-2.0

=============
UML profiles
=============

A **profile** is a curated UML Kconfig: a defconfig fragment
under ``arch/um/configs/profiles/`` plus the shared
``base_defconfig``, documenting a specific operational posture
(production-fast, debug-research, fuzzing, sandboxing, etc.). Every
profile ships from the same source tree and the same ``arch/um/``
subsystem; the differences live in Kconfig and in which Layer 2
gates run on or off at boot.

Building a profile
==================

::

   make ARCH=um uml/<profile>         # configures .config
   make ARCH=um -j$(nproc)            # builds

The ``uml/<profile>`` target merges ``arch/um/configs/base_defconfig``
with ``arch/um/configs/profiles/<profile>.config`` via
``scripts/kconfig/merge_config.sh``. To list profiles::

   make ARCH=um uml/list-profiles

Profiles
========

.. toctree::
   :maxdepth: 1

   prod-fast
   prod-with-hooks
   research
   fuzz
   fuzz-deep
   race
   sandbox
   embedded
   time-travel

Summary matrix
==============

.. list-table::
   :header-rows: 1
   :widths: 18 20 20 20 22

   * - Profile
     - Backend
     - debugfs
     - Hooks flippable
     - Intended user
   * - prod-fast
     - DYNAMIC (auto)
     - no
     - no (compiled in but no runtime control)
     - production workload
   * - prod-with-hooks
     - DYNAMIC (auto)
     - yes
     - yes
     - production with incident-capture capability
   * - research
     - SECCOMP_ONLY
     - yes
     - yes
     - developer debugging or profiling
   * - fuzz
     - SECCOMP_ONLY
     - yes (for KCOV)
     - yes
     - syzkaller-style coverage-guided fuzzing
   * - fuzz-deep
     - SECCOMP_ONLY
     - yes
     - yes
     - targeted fuzz session after a candidate bug (KASAN-focused)
   * - race
     - SECCOMP_ONLY
     - yes
     - yes
     - data-race detection (KCSAN; no KASAN — mutually exclusive)
   * - sandbox
     - SECCOMP_ONLY
     - no
     - no (not compiled; minimum TCB)
     - hostile-workload isolation
   * - embedded
     - PTRACE_ONLY
     - no
     - no
     - host without seccomp-filter support
   * - time-travel
     - SECCOMP_ONLY
     - yes
     - yes
     - deterministic replay / simulation

Related docs
============

- ``Documentation/virt/uml/backends.rst`` — backend mechanism
- ``Documentation/virt/uml/debugfs.rst`` — runtime hook control
- ``Documentation/virt/uml/section-split.rst`` — .text layout
- ``Documentation/virt/uml/redesign/03-profiles/`` — planning notes
  (what each profile aspires to; this directory documents what
  actually ships)
