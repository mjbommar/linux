.. SPDX-License-Identifier: GPL-2.0

=======================
UML profile: fuzz-deep
=======================

:Intended user: targeted investigation after ``fuzz`` identifies a
   candidate bug; needs more detail per iteration even at the cost
   of iteration rate
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/fuzz-deep.config``

What this profile is for
========================

Everything ``fuzz`` has, plus tighter KASAN mode and full lockdep.
The long-term plan has ``CONFIG_KCSAN=y`` here too, but the UML
KCSAN port lands in workstream **C-03**; until then ``fuzz-deep``
ships without KCSAN (gap explicitly noted in the fragment).

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

- ``KASAN_INLINE`` — tighter poison catch.
- ``PROVE_LOCKING``, ``DEBUG_ATOMIC_SLEEP`` — lockdep catches some
  of the races KCSAN would eventually catch.

What's still off
================

- ``KCSAN`` — pending C-03.
- Record-replay's full machinery — the Layer 2 ``record_replay``
  gate ships, but its slow path is still the stub counter that B-02
  landed. A real record/replay consumer is phase-F work.

Gap note
========

This profile name promises more than it currently delivers:
fuzz-deep's distinctive features vs fuzz are today only
``KASAN_INLINE`` + lockdep. When C-03 lands ``KCSAN`` the diff
widens; when phase F lands real record/replay the diff widens
again. The fragment comment tracks the gap so the profile's name
doesn't quietly drift.

When to use something else
==========================

- Iteration rate matters more than depth → ``fuzz``.
- You want every tool on, not just the deep-catch ones → ``research``.

See also
========

- :doc:`index`
- :doc:`fuzz`
- :doc:`research`
- ``Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/03-port-kcsan.md``
