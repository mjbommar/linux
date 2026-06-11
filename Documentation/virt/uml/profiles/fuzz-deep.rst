.. SPDX-License-Identifier: GPL-2.0

=======================
UML profile: fuzz-deep
=======================

:Intended user: targeted debugging after ``fuzz`` identifies a
   candidate bug; collects more detail per iteration even at the cost
   of iteration rate
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/fuzz-deep.config``

What this profile is for
========================

Everything ``fuzz`` has, plus tighter KASAN mode and full lockdep.
KCSAN is not available in this profile because upstream Kconfig keeps
it mutually exclusive with KASAN.

Use ``fuzz-deep`` after a candidate bug falls out of the ``fuzz``
corpus and you want a denser observation window before handing off
to ``research``.

Build
=====

::

   make ARCH=um uml/fuzz-deep
   make ARCH=um -j$(nproc)

What's on (in addition to ``fuzz``)
====================================

- ``KASAN_INLINE`` - tighter poison catch.
- ``KFENCE`` - sampling OOB/UAF detector with guard pages.
  Complements KASAN by catching bugs in paths that KASAN's shadow
  cost would cover up. Stats at
  ``/sys/kernel/debug/kfence/stats``.
- ``PROVE_LOCKING``, ``DEBUG_ATOMIC_SLEEP`` - lockdep catches some
  of the races KCSAN would eventually catch.

What's still off
================

- ``KCSAN`` - unavailable together with KASAN in a stock kernel.

KCSAN availability
==================

``KASAN + KCSAN`` is **not achievable on a stock kernel**:
``lib/Kconfig.kcsan`` declares ``depends on DEBUG_KERNEL && !KASAN``,
so a single build can have one or the other, not both.

Rather than modify the upstream constraint or fork fuzz-deep into
two variants, this release ships:

- **``fuzz-deep``** - KASAN-focused (the current profile).
- **``race``** - KCSAN-focused. See :doc:`race`.

Pick the detector matching the bug class.

Gap note
========

Beyond the KCSAN constraint above, fuzz-deep's distinctive features
vs fuzz are ``KASAN_INLINE`` + lockdep + KFENCE.

When to use something else
==========================

- Iteration rate matters more than depth: ``fuzz``.
- You want every tool on, not just the deep-catch ones: ``research``.

See also
========

- :doc:`index`
- :doc:`fuzz`
- :doc:`research`
