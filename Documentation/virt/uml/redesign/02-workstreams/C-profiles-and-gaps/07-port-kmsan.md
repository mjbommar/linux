# C-07: Port KMSAN to UML

**Status:** planned
**Effort:** 6 weeks (heaviest port)
**Dependencies:** stable A, stable B, KASAN port working
**Blocks:** research profile having uninit-memory detection

## Goal

`select HAVE_ARCH_KMSAN` for UML. Detect uninitialized-memory
reads — a 10-15% slice of recent kernel CVEs.

## Approach

1. KMSAN needs a 16 TB shadow region (like KASAN) plus a 16 TB
   origin region (where the uninit was created). UML's KASAN port
   already has the shadow `mmap` model; KMSAN extends it.
2. KMSAN's compile-time wraps add origin tracking to every
   allocation/store.
3. Boot-time init: pre-`main()` constructor mmaps both shadow
   and origin regions.
4. `select HAVE_ARCH_KMSAN` and dependencies in `arch/um/Kconfig`.
5. Run KMSAN kunit tests.

## Deliverable

- `arch/um/include/asm/kmsan.h`
- `arch/um/mm/kmsan.c`
- KMSAN-instrumented build works; tests pass
- Documentation in `arch/um/Documentation/kmsan.rst`

## Validation

- `kunit_test_kmsan` passes
- Inject a known uninit read; KMSAN catches it
- Reproduce a known kernel CVE found by KMSAN under QEMU; verify
  UML reproduces it identically (invariant I5)

## Open questions

- **Q1**: Origin tracking is expensive — does UML's mmap mechanism
  scale to 32 TB total shadow + origin? (Plan: yes; UML routinely
  mmaps 16 TB for KASAN. Doubling is fine on x86_64.)
- **Q2**: KMSAN's boot is delicate (constructor order matters,
  things get instrumented before shadow is ready). Validate
  early.

## Risk

Highest of the C ports. KASAN took 2.5 years on UML; KMSAN
likely shorter (KASAN paved the way) but still hard.

**Mitigation:**
- Do this last (after KFENCE, KCSAN, kprobes, ftrace already
  working — those are easier and demonstrate workstream
  capability)
- Engage Patricia Alfonso / Vincent Whitchurch (KASAN-on-UML
  authors) early
- If it stalls, ship without; KMSAN is the most-skippable port
