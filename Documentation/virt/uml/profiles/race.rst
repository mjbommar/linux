.. SPDX-License-Identifier: GPL-2.0

===================
UML profile: race
===================

:Intended user: researchers hunting data-race bugs
:Backend: SECCOMP_ONLY
:Fragment: ``arch/um/configs/profiles/race.config``

What this profile is for
========================

Data-race detection under UML via KCSAN (Kernel Concurrency
Sanitizer). Compile-time TSAN-style instrumentation inserts
watchpoints at memory accesses and artificial delays that expose
races that would be hard to hit in normal execution. Pair with
lockdep (``PROVE_LOCKING``) for deadlock / lock-ordering catches,
and with ftrace for correlating a reported race back to its call
context.

Build
=====

::

   make ARCH=um uml/race
   make ARCH=um -j$(nproc)

Why a separate profile, not an option in ``fuzz-deep``
=======================================================

Upstream Linux (``lib/Kconfig.kcsan``) mutually excludes
``CONFIG_KASAN`` and ``CONFIG_KCSAN`` — you can enable one, not
both, per kernel build. The plan's original fuzz-deep matrix row
called for **both** detectors, which isn't achievable on a stock
kernel. Rather than change the upstream constraint (a large,
orthogonal patch series) or fork fuzz-deep into two variants, this
release splits concern:

- ``fuzz-deep`` stays KASAN-focused (heap corruption, use-after-free).
- ``race`` is KCSAN-focused (data races, concurrency bugs).

Each is a full profile a user can build and ship; picking "which
class of bugs am I hunting today?" is then a build-time choice.
See ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
D26 for the decision and
``Documentation/virt/uml/redesign/03-profiles/fuzz-deep.md`` for
the reconciliation note.

What's on
=========

- **Backend**: SECCOMP_ONLY.
- **SMP**: forced on. KCSAN without SMP has no races to catch.
- ``CONFIG_KCSAN=y`` with ``KCSAN_EARLY_ENABLE=y`` — active from
  boot. Runtime toggle via ``/sys/kernel/debug/kcsan`` (standard
  KCSAN debugfs knobs).
- ``CONFIG_KCSAN_KUNIT_TEST=y`` + ``CONFIG_KUNIT=y`` — the in-tree
  KCSAN test suite runs at boot.
- **Lockdep**: ``PROVE_LOCKING``, ``DEBUG_SPINLOCK``,
  ``DEBUG_MUTEXES``, ``DEBUG_ATOMIC_SLEEP``, ``DEBUG_RT_MUTEXES``.
- **debugfs + ftrace** surfaces for correlation.

What's off (explicitly)
=======================

- ``CONFIG_KASAN`` — mutually exclusive with KCSAN upstream.
- ``CONFIG_KFENCE`` — not explicitly disabled, but KCSAN + KFENCE
  is an unusual combination; default build has KFENCE off.

Reading a KCSAN report
======================

When KCSAN detects a race, ``dmesg`` prints a ``BUG: KCSAN:``
report similar to::

   BUG: KCSAN: data-race in some_function / other_function

   read to 0xffff... of N bytes by task K on cpu M:
    some_function+0xNN
    ...
   write to 0xffff... of N bytes by task L on cpu N:
    other_function+0xNN
    ...

UML's stack walker has a known limitation (same as it does for
KFENCE reports, see the KFENCE profile notes under ``research``):
symbol names for the innermost test-function frames may be absent,
leaving addresses instead. The race itself is detected correctly;
the report's readability is reduced.

When to use something else
==========================

- You're hunting heap corruption → ``fuzz-deep`` (KASAN).
- You're fuzzing for broad coverage → ``fuzz``.
- You're debugging a single race you've already localized →
  ``research`` (more tools, but slower per iteration).

See also
========

- :doc:`index`
- :doc:`fuzz-deep`
- ``Documentation/dev-tools/kcsan.rst`` — upstream KCSAN guide
- ``Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/03-port-kcsan.md``
