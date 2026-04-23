.. SPDX-License-Identifier: GPL-2.0

==============
UML KMSAN port
==============

**STATUS: BROKEN.** The UML KMSAN port is currently gated on
``BROKEN`` in ``arch/um/Kconfig`` (``select HAVE_ARCH_KMSAN if
X86_64 && BROKEN``) and cannot be selected in a normal build.
This document describes the intended surface and the reason
for the gate; it is kept on-tree so the C-07 redesign follow-up
has a starting point, not because KMSAN works today. See
``Documentation/virt/uml/redesign/04-risks/decisions-log.md``
entry ``D58`` for the full story.

The original design (when unbroken) exposes the standard Linux
uninitialized-memory detector surface —
``/sys/kernel/debug/kmsan/``, clang
``-fsanitize=kernel-memory`` instrumentation, the
``BUG: KMSAN: uninit-value`` report style — to UML guests,
running inside the host UML process under a dedicated-region
shadow + origin host-mmap scheme.

Why it's broken
===============

The dedicated-region scheme worked for KASAN because KASAN
shadow is 1 byte per 8 bytes of kernel VA: 128 TiB of kernel
addresses compresses into 16 TiB of shadow, which fits
comfortably in a dedicated host-mmap region above the UML
binary. KMSAN's shadow is 1 byte per 1 kernel byte, so the
same kernel-VA range wants 128 TiB of shadow, plus another
128 TiB for origin (1 u32 per kernel u32 = same byte size).
256 TiB of reservation does not fit anywhere in the lower
canonical half on x86_64.

The current code (``arch/um/include/asm/kmsan.h``) sizes both
regions to the full 128 TiB, while the Kconfig defaults
(``arch/um/Kconfig``) place them only 16 TiB apart — the
SHADOW region overflows catastrophically into the ORIGIN
region and beyond. Runtime smoke fails at early shadow mmap
with ``Couldn't allocate shadow memory``.

Resolution paths (C-07 follow-up)
=================================

Two workable paths have been identified; neither has been
implemented yet:

1. **Adopt x86's VMALLOC quarter-split.** Put shadow and
   origin inside the VMALLOC range itself, sized as 1/4 each
   of VMALLOC. This matches ``mm/kmsan/shadow.c``'s existing
   arithmetic without any UML-side slab.
2. **Cap ``task_size`` under KMSAN.** Limit the UML kernel's
   addressable VA to a size where two 1:1 slabs fit in the
   lower canonical half. Preserves the dedicated-region
   scheme at the cost of a smaller kernel VA ceiling.

Either path requires careful measurement against real UML
workloads; the choice is the follow-up's job. Until then,
this port stays gated behind ``BROKEN``.

Intended (non-functional) memory layout
=======================================

On UML, KMSAN reserves two dedicated 128 TiB host-mmap regions
for shadow and origin, placed immediately above the existing
KASAN shadow::

   KASAN shadow    0x100000000000   (16 TiB, existing)
   KMSAN shadow    0x200000000000   (128 TiB, 1 byte / kernel byte)
   KMSAN origin    0x300000000000   (128 TiB, 1 u32 / kernel u32)

KASAN and KMSAN are Kconfig-mutually-exclusive. The two 128
TiB regions would need to live at non-overlapping offsets in
the lower canonical half — which they don't today, per the
"Why it's broken" section above.

Both regions would be reserved via ``kasan_map_memory()`` —
despite its name, that helper is a generic "mmap a host VA
range with PROT_READ|PROT_WRITE + MADV_DONTDUMP" routine UML
uses for every arch-shadow bootstrap. The mmap would run once
from ``kmsan_arch_init_early_shadow()``, which the
``mm/kmsan/`` core calls before its own reserved-range sweep.

Host RSS follows touched pages, not the reservation total:
``mmap`` with ``MAP_NORESERVE`` + demand-paging would mean
the kernel only spends real memory on shadow/origin bytes
that KMSAN-instrumented code actually touches — still not a
workable design because the reservation itself fails before
the first touch.

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

Relationship to KASAN and KCSAN
===============================

* **KASAN** detects out-of-bounds and use-after-free. Complementary.
  KASAN + KMSAN cannot co-exist in a single image (by upstream
  convention and Kconfig) — ship each in its own profile.

* **KCSAN** detects data races. Complementary. KMSAN + KCSAN share
  no shadow state and CAN co-exist; the research-kmsan profile
  can select both if desired.

* **KFENCE** detects sampling-based heap bugs. Orthogonal.
  Strictly additive with KMSAN.

Snapshot / forkserver integration
=================================

When ``CONFIG_UM_SNAPSHOT_FORKSERVER=y``, both the KMSAN shadow
and origin regions register with the C-09 ``um_register_mmap_
region()`` registry so snapshot/fork workers inherit the
mappings via COW. Shadow contents are not deduplicated into
snapshots (same policy as KASAN) — they reconstruct from
allocator state on restore and are too sparse to pay the
serialization cost.

Regression test
===============

``tools/testing/selftests/um/kmsan-smoke/`` boots a
CONFIG_KMSAN=y UML image and asserts that
``/sys/kernel/debug/kmsan/`` exists and, optionally, that
a planted uninit-read reproducer triggered the
``BUG: KMSAN:`` report. Drives via::

   make -C tools/testing/selftests/um/kmsan-smoke run_tests

Further reading
===============

* ``Documentation/dev-tools/kmsan.rst`` — generic KMSAN docs
  (bare-metal x86 + s390 + UML).
* ``Documentation/virt/uml/redesign/02-workstreams/
  C-profiles-and-gaps/07-port-kmsan.md`` — design rationale,
  decisions-log D44 + D51 for the shape.
* ``arch/um/include/asm/kmsan.h`` — VA layout + arch hooks.
* ``mm/kmsan/init.c`` — generic core; the
  ``kmsan_arch_init_early_shadow()`` hook UML overrides is
  a weak symbol introduced upstream by the C-07 series.
