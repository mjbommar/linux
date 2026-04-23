.. SPDX-License-Identifier: GPL-2.0

==============
UML KMSAN port
==============

The UML port of KMSAN (Kernel Memory Sanitizer) landed in
workstream C-07 of the redesign (see
``Documentation/virt/uml/redesign/02-workstreams/
C-profiles-and-gaps/07-port-kmsan.md``). It exposes the
standard Linux uninitialized-memory detector surface —
``/sys/kernel/debug/kmsan/``, clang
``-fsanitize=kernel-memory`` instrumentation, the
``BUG: KMSAN: uninit-value`` report style — to UML guests,
running inside the host UML process under a dedicated-region
shadow + origin host-mmap scheme.

Availability
============

KMSAN on UML requires:

* ``ARCH=um LLVM=1`` — KMSAN is clang-only.
* ``CONFIG_X86_64`` — the only host architecture wired via
  ``select HAVE_ARCH_KMSAN if X86_64`` in ``arch/um/Kconfig``.
* A profile that enables ``CONFIG_KMSAN=y``. Shipped in
  ``arch/um/configs/profiles/research-kmsan.config`` as a
  sibling of the default research profile, because KMSAN
  triples RSS for touched pages and that trade-off is too
  invasive for the default ``research`` profile.

To build a KMSAN-capable UML::

   make ARCH=um LLVM=1 uml/research-kmsan
   make ARCH=um LLVM=1 -j$(nproc)

Memory layout
=============

On UML, KMSAN reserves two dedicated 16 TiB host-mmap regions
for shadow and origin, placed immediately above the existing
KASAN shadow::

   KASAN shadow    0x100000000000   (16 TiB, existing)
   KMSAN shadow    0x200000000000   (16 TiB, 1 byte / kernel byte)
   KMSAN origin    0x300000000000   (16 TiB, 1 u32 / kernel u32)

KASAN and KMSAN are Kconfig-mutually-exclusive (same constraint
as bare-metal x86 and s390), so the placement is a convention,
not a constraint the kernel will enforce.

Both regions are reserved via ``kasan_map_memory()`` — despite
its name, that helper is a generic "mmap a host VA range with
PROT_READ|PROT_WRITE + MADV_DONTDUMP" routine UML uses for
every arch-shadow bootstrap. The mmap runs once from
``kmsan_arch_init_early_shadow()``, which the mm/kmsan/ core
calls before its own reserved-range sweep.

Host RSS follows touched pages, not the 32 TiB reservation:
mmap with ``MAP_NORESERVE`` + demand-paging means the kernel
only spends real memory on shadow/origin bytes that
KMSAN-instrumented code actually touches.

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
