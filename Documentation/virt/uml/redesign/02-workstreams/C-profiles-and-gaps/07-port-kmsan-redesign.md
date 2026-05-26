# C-07 redesign: KMSAN-on-UML paths (post-D58)

**Status:** feasibility memo + recommended path (2026-04-23).
**Companion to:** `07-port-kmsan.md` (the broken first-pass
design), `04-risks/decisions-log.md` D58 (the "this is
broken, here are two paths" entry), `06-sequencing/
post-q1-push.md` §"Phase V".

This memo synthesizes the two resolution paths D58 sketched
for the KMSAN-on-UML port. The intent is to turn "here are
two paths, pick one later" into "here's the recommended path
with enough detail to land it." Phase V Lifts #3a + #3b are
the paper probes; Lift #3c is the decisions-log entry that
records the pick.

## Recap of the D58 breakage

The first-pass KMSAN port
(`arch/um/include/asm/kmsan.h` on-tree, Kconfig gated on
BROKEN) reserves two dedicated host-mmap slabs for shadow
and origin:

- `KMSAN_SHADOW_START = 0x200000000000`, 128 TiB
- `KMSAN_ORIGIN_START = 0x300000000000`, 128 TiB

The scheme fails because the two 128 TiB slabs overlap —
SHADOW extends from `0x200000000000` through
`0xa00000000000`, well past ORIGIN's start at
`0x300000000000`. Also, 256 TiB of shadow + origin doesn't
fit anywhere in the lower canonical half (128 TiB total).

Root cause: KASAN's 1-byte-per-8-bytes shadow shift makes
its shadow 1/8 of kernel VA (16 TiB for 128 TiB of VA).
KMSAN's shadow is 1:1, so 128 TiB of VA needs 128 TiB of
shadow. Doubling for origin makes it 256 TiB. No dedicated-
slab scheme works in that space budget.

## Path (a) — VMALLOC quarter-split

Mirror the x86_64 convention in `arch/x86/include/asm/
pgtable_64_types.h:124-169`:

```
ifndef CONFIG_KMSAN
    VMALLOC_END = VMEMORY_END          # full 1× vmalloc
else
    VMALLOC_END = VMALLOC_START + VMALLOC_SIZE/4
                                        # 1/4 vmalloc
    KMSAN_VMALLOC_SHADOW_START = VMALLOC_START + VMALLOC_SIZE/4
    KMSAN_VMALLOC_ORIGIN_START = VMALLOC_START + VMALLOC_SIZE/2
    KMSAN_MODULES_*_START      = VMALLOC_START + 3*VMALLOC_SIZE/4
endif
```

So under CONFIG_KMSAN, the existing VMALLOC range is split
into 4 quarters: new vmalloc (1), vmalloc shadow (2),
vmalloc origin (3), modules shadow+origin (4).

The generic KMSAN code in `mm/kmsan/shadow.c::vmalloc_meta`
is already designed around this layout — `addr + OFFSET`
arithmetic to compute shadow/origin addresses. No generic
change needed; arch/um just plugs the same macros.

**Pros:**

- **Idiomatic upstream.** The x86_64 reference lives a few
  directories over; reviewers recognize the pattern instantly.
  Submission to LKML is "copy x86's approach to UML" rather
  than "here's a novel scheme."
- **No arch-specific `task_size` changes.** UML's existing
  boot-time `task_size = host_task_size & PGDIR_MASK`
  logic stays unchanged under CONFIG_KMSAN. User-visible
  guest VA ceiling remains the same as non-KMSAN builds.
- **`arch/um/include/asm/kmsan.h` shrinks.** Drop the
  dedicated-slab SHADOW_START/END/SIZE + the
  `kasan_map_memory()` of two 128 TiB regions. Replace
  with `#include` of the VMALLOC-quarter macros.
- **`kasan_map_memory()` only ever allocates the KASAN
  shadow slab.** Simpler invariant: dedicated mmap reserves
  are KASAN-only; KMSAN lives in the page-table layout
  directly.
- **Demand-paged shadow.** KMSAN metadata pages are
  allocated on first access via the generic
  `kmsan_alloc_meta` path. Under quarter-split the shadow
  is inside vmalloc, so allocation goes through the
  standard vmalloc → `alloc_pages` path UML already
  supports.

**Cons:**

- **Effective VMALLOC is 1/4 its normal size under
  CONFIG_KMSAN.** For a typical UML with ~10 GiB of
  VMALLOC, KMSAN builds see ~2.5 GiB. Research workloads
  (the only builds likely to enable KMSAN) don't approach
  this limit.
- **Module loading space also shrinks** (the 4th quarter
  holds both the 1/4 effective modules area plus modules
  shadow + origin in that quarter). For UML's "modules are
  uncommon in research profile" reality, negligible.

**Blockers found during the probe:**

- None structural. The implementation is ~20 lines of
  Kconfig + ~40 lines of pgtable.h / kmsan.h edits + a
  decisions-log entry.

**Minimum implementation footprint:**

1. `arch/um/Kconfig`: drop the `BROKEN` gate on
   `HAVE_ARCH_KMSAN`. Keep `X86_64` dep.
2. `arch/um/include/asm/pgtable.h`: redefine VMALLOC_END
   under CONFIG_KMSAN to `VMALLOC_START + quarter_size`.
   Define `KMSAN_VMALLOC_SHADOW/ORIGIN_START` and
   `KMSAN_MODULES_SHADOW/ORIGIN_START` in the remaining
   quarters. Mirror x86's macro shape.
3. `arch/um/include/asm/kmsan.h`: drop the dedicated-slab
   SHADOW_START/SIZE/END. Replace with the VMALLOC-quarter
   variants.
4. `arch/um/Kconfig` KMSAN_SHADOW_OFFSET / KMSAN_ORIGIN_OFFSET
   config entries become moot — the macros derive from
   VMALLOC_START. Drop or deprecate.
5. `Documentation/virt/uml/kmsan.rst`: flip the "BROKEN"
   banner to a "landed (2026-04-23)" status with VMALLOC-
   split explanation.
6. `tools/testing/selftests/um/kmsan-smoke/`: existing
   smoke harness works as-is; it just needs a
   `research-kmsan`-profile binary that doesn't panic at
   early shadow mmap.

**Estimated LOC: ~150 lines, one commit.**

## Path (b) — `task_size` cap

Preserve the dedicated-slab scheme by shrinking UML kernel's
addressable VA so two 1:1 slabs fit in the lower canonical
half:

```
ifdef CONFIG_KMSAN
    task_size = min(host_task_size, 32 TiB) & PGDIR_MASK
    KMSAN_SHADOW_START = 0x400000000000  (above 32 TiB)
    KMSAN_SHADOW_SIZE  = 32 TiB
    KMSAN_ORIGIN_START = KMSAN_SHADOW_START + 32 TiB
    KMSAN_ORIGIN_SIZE  = 32 TiB
endif
```

Under this scheme, guest userspace VA is capped at 32 TiB
(vs. ~128 TiB otherwise). Shadow + origin total 64 TiB,
which fits in the lower canonical half alongside the
32 TiB task + ~30 TiB residue.

**Pros:**

- **Preserves the dedicated-slab design.** Already-written
  code in `arch/um/include/asm/kmsan.h` mostly applies;
  just the SIZE constants shrink.
- **Per-task state structures** (task_struct, mm_struct,
  etc.) never touch the VMALLOC range, so no potential
  interference with other vmalloc consumers.

**Cons:**

- **User-visible `task_size` change.** UML guests under
  CONFIG_KMSAN see a 32 TiB VA ceiling instead of 128 TiB.
  Anything that depends on the full task_size range
  (ELF mapping, mmap layout, thread-stack placement)
  changes at runtime based on a config flag. Surprising.
- **`task_size` is currently computed from
  `host_task_size`** — a host-derived runtime value.
  Capping it requires a second clamp step under
  CONFIG_KMSAN, cascading into any code that caches
  task_size or uses it for address-space computations
  (uaccess, stack placement, vsyscall).
- **Not idiomatic.** x86 uses VMALLOC-quarter. s390
  uses VMALLOC-quarter. Landing a different scheme for
  UML is a reviewer speedbump.
- **More code.** The `task_size` cap path involves ~300+
  lines across pgtable, uaccess, elf, stack, vsyscall
  because task_size cascades. The VMALLOC-quarter path
  is localized to pgtable/kmsan.

**Blockers found during the probe:**

- **Cascade breadth.** Every reference to `TASK_SIZE` /
  `task_size` in UML needs to confirm it handles the
  CONFIG_KMSAN-dependent value correctly. ~30 sites in
  arch/um/ alone.
- **Reviewer pushback likely.** "Why not just do what
  x86 does?" is the inevitable question, and the answer
  ("preserves dedicated-slab design") isn't strong
  enough to justify a different convention.

**Estimated LOC: ~400-500 lines plus validation of every
TASK_SIZE consumer, ~3-5 commits.**

## Recommendation

**Path (a) — VMALLOC quarter-split.**

1. **Idiomatic.** Matches x86 and s390 verbatim. LKML
   reviewers see a pattern they know.
2. **Localized blast radius.** ~150 lines in pgtable +
   kmsan headers. No cascade into uaccess / elf / stack /
   vsyscall / task_size consumers.
3. **Cheaper cost.** Effective VMALLOC of ~2.5 GiB under
   KMSAN is still plenty for the research profile (the
   only profile that enables KMSAN). User-visible
   `task_size` unchanged.
4. **Exists and is maintained.** `mm/kmsan/shadow.c::
   vmalloc_meta` is already wired for this layout; UML
   just consumes it.

**What Phase V Lift #3c should land:**

- `decisions-log.md` D62: supersede D58 with the VMALLOC-
  quarter-split pick + the cost/tradeoff reasoning above.
- `arch/um/Kconfig`: remove the `BROKEN` gate on
  HAVE_ARCH_KMSAN. Keep X86_64 dep.
- `arch/um/include/asm/pgtable.h`: split-aware VMALLOC_END
  + KMSAN_VMALLOC_*_START macros.
- `arch/um/include/asm/kmsan.h`: rewrite to the VMALLOC-
  quarter layout. Drop dedicated-slab SHADOW/ORIGIN
  size/start defines.
- `Documentation/virt/uml/kmsan.rst`: flip BROKEN banner
  to landed-with-VMALLOC-split status.
- Boot-test with `CONFIG_KMSAN=y` research profile;
  smoke-test via `tools/testing/selftests/um/
  kmsan-smoke/`.

**What Phase V Lift #3c should NOT do:**

- Full research-profile boot bring-up with KMSAN catching
  real bugs. That's C-07 completion (a separate workstream
  item); the redesign just removes the architectural
  blocker.
- Performance measurements. KMSAN has known 10-50× slowdown
  on bare metal; under UML the numbers will be similarly
  bounded. Not the scope of "unbreak the dedicated-slab
  scheme."
- ARM64 / i386 variants. x86_64 only for the redesign,
  matching the existing X86_64 dep.

## What this memo is NOT

- Not an implementation commit. Lift #3c's decisions-log
  entry + Kconfig/header edits are where the actual code
  change lands.
- Not a promise that path (a) is painless — only that it's
  lower-TCO than path (b) for UML's specific constraints.
- Not a replacement for real-workload KMSAN validation.
  That's a follow-up that runs after path (a) lands.

## Cross-references

- D58 (2026-04-23) — the entry this memo synthesizes the
  resolution paths from.
- D51 (2026-04-22) — C-07 KMSAN upstream strategy; still
  relevant once the redesign lands.
- `arch/x86/include/asm/pgtable_64_types.h:124-169` — the
  reference VMALLOC-quarter-split layout.
- `mm/kmsan/shadow.c::vmalloc_meta` — the generic code
  consumer of the quarter-split layout.
- `06-sequencing/post-q1-push.md` §"Phase V" — parent.
