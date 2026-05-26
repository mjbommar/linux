.. SPDX-License-Identifier: GPL-2.0

==============
UML KMSAN port
==============

The UML KMSAN port uses the **VMALLOC quarter-split** layout
matching ``arch/x86/include/asm/pgtable_64_types.h:124-169``.
Under ``CONFIG_KMSAN=y``, the VMALLOC range is split into
four equal quarters: new vmalloc, vmalloc shadow, vmalloc
origin, and modules shadow+origin. See
``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/07-port-kmsan-redesign.md`` for the
feasibility comparison against the alternative (``task_size``
cap) and ``Documentation/virt/uml/redesign/04-risks/
decisions-log.md`` entries D58 + D62 for the decision
history.

The port exposes the standard Linux uninitialized-memory
detector surface — ``/sys/kernel/debug/kmsan/``, clang
``-fsanitize=kernel-memory`` instrumentation, the
``BUG: KMSAN: uninit-value`` report style — to UML guests.

Memory layout
=============

Under ``CONFIG_KMSAN=y`` on UML/x86_64, the original VMALLOC
range ``[VMALLOC_START, TASK_SIZE - 2 * PAGE_SIZE)`` is
divided into four equal quarters::

   quarter 1  VMALLOC_START            .. VMALLOC_END
              — effective vmalloc area (1/4 original size)
   quarter 2  KMSAN_VMALLOC_SHADOW_START ..
              — shadow for quarter 1 (1 byte per byte)
   quarter 3  KMSAN_VMALLOC_ORIGIN_START ..
              — origin for quarter 1 (1 u32 per kernel u32,
                same byte total as shadow)
   quarter 4  unused

**Modules-vs-vmalloc note.** On UML, ``MODULES_VADDR ==
VMALLOC_START``: modules live inside the vmalloc range.
The generic ``mm/kmsan/shadow.c::vmalloc_meta()`` checks
the vmalloc predicate before the module predicate, so
every module address is classified as vmalloc and routed
through quarter 2 (shadow) / quarter 3 (origin). The
``KMSAN_MODULES_SHADOW_START`` and
``KMSAN_MODULES_ORIGIN_START`` macros alias to their
``KMSAN_VMALLOC_*_START`` equivalents so any caller that
does reach the module branch gets a consistent address
in quarters 2 / 3. Quarter 4 is therefore unreserved
under UML and available for a future subsystem that
needs a fixed VA slot (e.g. a dedicated per-CPU shadow
bank, if one ever materializes).

The generic KMSAN core
(``mm/kmsan/shadow.c::vmalloc_meta``) computes each address
as ``VMALLOC_START + offset + KMSAN_VMALLOC_*_OFFSET``; the
arch/um macros in ``arch/um/include/asm/pgtable.h`` plug
that arithmetic directly.

Under ``CONFIG_KMSAN=n``, VMALLOC extends to
``TASK_SIZE - 2 * PAGE_SIZE`` as before — no impact on
non-KMSAN builds.

Why not a dedicated shadow slab?
================================

The original (D44) design reserved two 128 TiB host-mmap
slabs above the KASAN shadow. That scheme worked for KASAN
because KASAN's shadow is 1 byte per 8 kernel bytes —
16 TiB of shadow for 128 TiB of VA. KMSAN's shadow is 1:1,
so the same VA range wants 128 TiB of shadow plus another
128 TiB of origin. 256 TiB of reservation doesn't fit in
the lower canonical half on x86_64 (128 TiB total). D58
records the breakage; D62 records the VMALLOC-quarter-split
pick.

Comparison with x86:

* x86's ``VMALLOC_SIZE_TB`` is 32 TiB, so each quarter is
  8 TiB.
* UML's effective VMALLOC range is ``TASK_SIZE -
  VMALLOC_START`` which varies by build. Typical UML with
  a few GiB of ``mem=`` argument gives ~10 GiB of VMALLOC,
  so each quarter is ~2.5 GiB. Still ample for research-
  profile workloads (the only builds likely to enable
  KMSAN).

Cost: VMALLOC-quarter-split shrinks effective VMALLOC to
1/4 its normal size under ``CONFIG_KMSAN``. For research
profile this is acceptable; for profiles that rely on
large vmalloc reservations, don't enable KMSAN there.

Relationship to KASAN and KCSAN
===============================

* **KASAN** detects out-of-bounds and use-after-free.
  Complementary. KASAN + KMSAN cannot co-exist in a single
  image (by upstream convention and Kconfig) — ship each
  in its own profile.

* **KCSAN** detects data races. Complementary. KMSAN + KCSAN
  share no shadow state and CAN co-exist; the research-
  kmsan profile can select both if desired.

* **KFENCE** detects sampling-based heap bugs. Orthogonal.
  Strictly additive with KMSAN.

Snapshot / forkserver integration
=================================

When ``CONFIG_UM_SNAPSHOT_FORKSERVER=y``, the KMSAN shadow
and origin regions live inside VMALLOC — already part of
the mm_map-registered snapshot scope the C-09 forkserver
exposes. No additional ``um_register_mmap_region()``
plumbing beyond what VMALLOC itself registers.

Usage
=====

To trigger KMSAN from inside a booted guest::

   mount -t debugfs none /sys/kernel/debug
   ls /sys/kernel/debug/kmsan/
   # Planted uninit-read reproducer (via the existing
   # KMSAN test module when CONFIG_KMSAN_KUNIT_TEST=y):
   modprobe kmsan-kunit-test

KMSAN reports land in dmesg as::

   BUG: KMSAN: uninit-value in <function>+<offset>/<section>
   ...
   Local variable <name> created at ...
   the first 8 bytes of this origin chain are ...

Regression test
===============

``tools/testing/selftests/um/kmsan-smoke/`` boots a
``CONFIG_KMSAN=y`` UML image and asserts that
``/sys/kernel/debug/kmsan/`` exists and, optionally, that
a planted uninit-read reproducer triggered the
``BUG: KMSAN:`` report. Drives via::

   make -C tools/testing/selftests/um/kmsan-smoke run_tests

Further reading
===============

* ``Documentation/dev-tools/kmsan.rst`` — generic KMSAN docs
  (bare-metal x86 + s390 + UML).
* ``Documentation/virt/uml/redesign/02-workstreams/
  C-profiles-and-gaps/07-port-kmsan-redesign.md`` — the D62
  redesign's feasibility comparison.
* ``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
  D58 (broken original) + D62 (VMALLOC-split pick).
* ``arch/um/include/asm/pgtable.h`` — ``VMALLOC_END`` /
  ``KMSAN_VMALLOC_*_START`` / ``KMSAN_MODULES_*_START``
  macros (the quarter-split layout).
* ``arch/um/include/asm/kmsan.h`` — arch-hook inlines.
* ``mm/kmsan/shadow.c::vmalloc_meta`` — the generic
  consumer of the quarter-split layout.
