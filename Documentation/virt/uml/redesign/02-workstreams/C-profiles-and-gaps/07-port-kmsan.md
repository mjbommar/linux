# C-07: Port KMSAN to UML

**Status:** design-locked (2026-04-22); four empirical probes in
D44 established that options A (VMALLOC quarter-split) and B
(dedicated host-mmap past KASAN) both fight KMSAN's per-page-
struct metadata model, and the fourth probe's boot hang
confirmed phase-1 mmap is the wrong contract for KMSAN's
runtime (unlike KASAN, whose stateless zero-filled shadow
tolerates it). Resolution in the D44 "Resolution" addendum
and D51: path forward is a new weak arch callback
(`kmsan_arch_init_early_shadow`) on `mm/kmsan/init.c`, called
from `kmsan_init_shadow()` before the existing reserved-range
sweep. UML overrides it; x86 and s390 keep the default empty
no-op and behave byte-identically to today. The core upstream
patch is ~20 LOC and introduces a new entrypoint rather than
changing any existing function — deliberately the least
invasive shape to pitch to KMSAN maintainer Alexander
Potapenko. See D51 for the upstream framing.

Implementation-queued behind the upstream RFC. Effort estimate
holds at 1 week of focused work once the generic callback
merges (5-6 bisectable UML-side commits per the plan in D44's
second-probe addendum). Still the heaviest remaining C-port,
but the blocker has narrowed from "design question" to
"upstream coordination".
**Effort:** 6 weeks (budget). Optimistic-case scope — KASAN's
UML port paved the mmap pattern we reuse — is closer to **2-3
weeks of disciplined work** if U1/U2/U3 hold. Kept at 6 weeks
in `00-vision.md` because KMSAN is historically the highest-
risk sanitizer port (see "Risk summary" below).
**Dependencies:** stable A (backend ops), stable B (section
                  split + static-key gates), **KASAN port
                  working on UML** (all landed; KASAN is the
                  template). `HAVE_ARCH_KASAN if X86_64` is
                  currently in `arch/um/Kconfig:20`.
**Blocks:** research profile having uninit-memory detection;
            closing out the "sanitizer trio" (KASAN + KCSAN +
            KMSAN) for UML-on-x86_64.

## Goal

`select HAVE_ARCH_KMSAN if X86_64` for UML. Detect uninitialized-
memory reads (roughly a 10-15 percent slice of recent
kernel CVEs per syzkaller telemetry) via the in-tree KMSAN
instrumentation, running under the research profile with the
clang toolchain and the existing host-mmap-backed shadow-region
pattern we already proved in KASAN.

## Prior-art survey

### What we reuse unchanged

- **`mm/kmsan/`** (the KMSAN core): init, shadow lookup, hook
  machinery, report generation. Arch-generic.
- **Clang's `-fsanitize=kernel-memory`**. Already how KMSAN
  lands on bare-metal x86; UML just needs to turn it on via
  `HAVE_ARCH_KMSAN + LLVM=1`. UML's LLVM build support already
  landed via the A/B workstreams.
- **UML's own `kasan_map_memory()` + `.kasan_init` section
  pattern** (`arch/um/kernel/mem.c:48-72`, `arch/um/include/asm/
  kasan.h`). This is the shadow-mmap bootstrap template; KMSAN
  extends it with one additional 16 TB mmap for the origin
  region and mirrors the `.kmsan_init` pre-`main()` constructor
  trick.
- **`um_register_mmap_region()`** (`arch/um/kernel/physmem.c`,
  landed as part of C-09 commit 3b's D37 pull-forward #1). KMSAN
  shadow + origin both register through this so snapshot/fork-
  server inherits them correctly.

### What's different from the bare-metal x86 KMSAN port

- **No CPU entry area, no lowcore, no fixmap-level per-CPU
  blocks.** x86's `arch/x86/mm/kmsan_shadow.c` has per-CPU
  metadata for the CPU entry area (`cpu_entry_area_{shadow,
  origin}`) so exception and IDT handling can read metadata
  without page-table walks. UML has no IDT, no real exception
  entry, and no CPU entry area; the metadata hook
  (`arch_kmsan_get_meta_or_null`) returns `NULL` for everything
  on UML and defers to the generic page-struct lookup. Much
  simpler than s390 or x86_64.
- **No `KMSAN_INIT_RUNTIME` dance.** x86 special-cases the
  bootstrap path for early-boot code that runs before shadow is
  up. UML's boot is later-stage-by-construction — by the time
  kernel code runs, the `.kasan_init` constructor has already
  mmap'd the shadow; KMSAN piggybacks on the same seam via a
  `.kmsan_init` constructor that mmaps shadow + origin in the
  same pre-`main()` window.
- **mmap'd, not direct-mapped.** KMSAN on bare metal uses a
  VMALLOC-region quarter-split (1/4 vmalloc, 1/4 shadow, 1/4
  origin, 1/4 module shadow+origin). UML's shadow lives in a
  host `mmap()` at `KASAN_SHADOW_OFFSET`; KMSAN's shadow and
  origin sit in two more `mmap()`s at fixed UML-specific
  offsets. Same lookup math, different backing store.
- **Host-memory amplification: 3x kernel-memory at runtime.**
  Every byte of kernel memory needs 1 byte of shadow and 4
  bytes of origin metadata (depth-compressed stack depot IDs).
  Under the fuzz profile's snapshot/fork loops this triples peak
  host RSS — documented as U1 below.

## Proposed shadow + origin VA layout

UML on x86_64 already uses a 16 TB shadow at
`KASAN_SHADOW_OFFSET` (`arch/um/include/asm/kasan.h:8-23`). Add
two parallel 16 TB regions for KMSAN:

```
 ┌─────────────────────┬────────────────────────────────────────┐
 │ region              │ offset                                 │
 ├─────────────────────┼────────────────────────────────────────┤
 │ KASAN shadow        │ CONFIG_KASAN_SHADOW_OFFSET              │
 │                     │   16 TB @ shadow-scale-3               │
 │ KMSAN shadow        │ CONFIG_KMSAN_SHADOW_OFFSET              │
 │                     │   16 TB, 1 shadow-byte per kernel-byte │
 │ KMSAN origin        │ CONFIG_KMSAN_ORIGIN_OFFSET              │
 │                     │   16 TB, 1 u32 per kernel-u32 (4-byte) │
 └─────────────────────┴────────────────────────────────────────┘
```

Kconfig gets three new `*_SHADOW_OFFSET` / `*_ORIGIN_OFFSET`
values. Exact numeric choice lives in `arch/um/Kconfig` alongside
the existing `KASAN_SHADOW_OFFSET` (see D15 for how that offset
was chosen; KMSAN picks the next free aligned 16 TB window per
the host x86_64 canonical-hole layout).

`arch_kmsan_get_meta_or_null(addr, is_origin)` returns NULL for
every caller on UML v1 — no special-region metadata pools to
route around — and all lookups walk the standard shadow/origin
mmap ranges. If future UML work adds a CPU entry area analogue
(say, for seccomp backend entry trampolines), the hook grows a
branch; v1 doesn't need it.

## Arch contract (what UML must implement)

Minimum surface. Per file with a one-line rationale:

- `arch/um/include/asm/kmsan.h` (new). Declares
  `KMSAN_SHADOW_START`, `KMSAN_SHADOW_END`, `KMSAN_ORIGIN_START`,
  `KMSAN_ORIGIN_END`, `kmsan_init()`,
  `arch_kmsan_get_meta_or_null()`, `kmsan_virt_addr_valid()`,
  `kmsan_phys_addr_valid()`. Mirrors `asm/kasan.h`.
- `arch/um/kernel/mem.c` (modified). Add `kmsan_init()`
  calling `kasan_map_memory(KMSAN_SHADOW_START,
  KMSAN_SHADOW_SIZE)` + same for origin; register both as mmap
  regions via `um_register_mmap_region()`; wire a
  `.kmsan_init`-section function pointer matching the
  `.kasan_init` trick at `mem.c:70-72`.
- `arch/um/Kconfig` (modified). `select HAVE_ARCH_KMSAN if
  X86_64` + `select HAVE_ARCH_KMSAN_VMALLOC if HAVE_ARCH_KMSAN`
  alongside existing KASAN selects. Kconfig help text should
  call out U1 (memory overhead) + U2 (clang-only) so config
  users aren't surprised.
- `arch/um/Makefile` (modified, likely one or two lines).
  `KMSAN_SANITIZE := n` for `arch/um/os-Linux/` (host-
  interfacing code that shouldn't be instrumented; same posture
  as existing KASAN overrides for `os-Linux/` paths).
- `Documentation/virt/uml/kmsan.rst` (new, ~150 LOC). User-
  facing doc: which profile enables it, how to reproduce a
  canonical uninit finding, memory-overhead expectations,
  selftest pointer, Invariant-I5 caveat about snapshot/fork
  interaction (U3).

What UML explicitly does NOT need:

- `arch/um/mm/kmsan_shadow.c` (per-CPU entry-area metadata).
  UML has no CPU entry area; the arch_kmsan_get_meta_or_null
  default (returns NULL) handles every path. Defer to v2 if a
  future UML feature ever needs its own special region.
- ORIGIN_SIZE special-casing. Generic KMSAN already handles 1:1
  shadow and 4:1 origin ratios.
- Any touches to `mm/kmsan/` core. Arch-generic by design.

## Commit plan (bisectable)

All commits build clean on `ARCH=um LLVM=1` (KMSAN is clang-
only) and boot the research profile with `CONFIG_KMSAN=y`. Plain
`ARCH=um` (gcc) builds must also remain clean — with
`CONFIG_KMSAN=n` the code compiles to nothing.

1. **commit 1:** Kconfig + header + Makefile scaffold.
   - `select HAVE_ARCH_KMSAN if X86_64` and
     `HAVE_ARCH_KMSAN_VMALLOC if HAVE_ARCH_KMSAN` in
     `arch/um/Kconfig`.
   - `arch/um/include/asm/kmsan.h` with VA-layout macros +
     function prototypes (extern).
   - `arch/um/Makefile` `KMSAN_SANITIZE := n` for
     `arch/um/os-Linux/`.
   - No `kmsan_init` body yet; KMSAN is selectable but the
     shadow isn't populated. Build verifies Kconfig deps are
     right; runtime is KMSAN-off-by-default.

2. **commit 2:** Shadow + origin mmap + init.
   - `kmsan_init()` in `arch/um/kernel/mem.c`:
     `kasan_map_memory(KMSAN_SHADOW_START, KMSAN_SHADOW_SIZE)`
     and same for origin; `um_register_mmap_region()` both.
   - `.kmsan_init` section function pointer matching
     `.kasan_init`.
   - Verifiable: `make ARCH=um LLVM=1 uml/research` with
     `CONFIG_KMSAN=y` boots, `/proc/self/maps` shows the two new
     regions, no panic at first allocation.

3. **commit 3:** arch_kmsan_get_meta_or_null stub + UML-
   specific arch hook bodies.
   - `arch_kmsan_get_meta_or_null` returning NULL (all paths
     take the generic shadow-lookup).
   - `kmsan_virt_addr_valid` / `kmsan_phys_addr_valid` tied to
     UML's existing `virt_addr_valid` where appropriate.
   - Verifiable: mm/kmsan/kmsan_test.c kunit passes under UML.

4. **commit 4:** selftest + user doc + research defconfig flip.
   - `tools/testing/selftests/um/kmsan-smoke/` host-driven
     selftest modeled on snapshot-smoke / kprobes-stress.
     Loads a guest init that deliberately reads uninitialized
     memory; asserts the KMSAN report appears in dmesg.
   - `Documentation/virt/uml/kmsan.rst` + toctree entry.
   - `arch/um/configs/profiles/research.config` —
     `CONFIG_KMSAN=y` (or `research-kmsan.config` as a sibling
     profile if memory overhead disqualifies it from default
     research per U1).
   - `07-port-kmsan.md` Status → `landed (YYYY-MM-DD)` with
     kmsan_test + selftest evidence.

## Validation

Per AGENT-PROMPT §3 Q1 bar on each commit:

- `uml-quality-q1.sh research` clean vs committed baseline.
- `scripts/checkpatch.pl --strict -g HEAD` on each commit.
- `uml-boot-matrix.sh` clean across PTRACE_ONLY / SECCOMP_ONLY /
  DYNAMIC.

C-07-specific validation:

- **Compile symbol visibility:**
  `nm vmlinux | grep -E "kmsan_init|__msan_"` must show defined
  (T) symbols. Clang instrumentation inserts `__msan_*` stubs
  KMSAN core resolves; UML v1 doesn't override any of them.
- **Boot-time plumbing:** `/proc/self/maps` for a booted UML
  shows KMSAN_SHADOW_START and KMSAN_ORIGIN_START as PROT_READ|
  PROT_WRITE mappings the size they should be (16 TB each).
- **Functional proof:** mm/kmsan/kmsan_test.c kunit (in-tree,
  `tools/testing/selftests/lib/` or similar — see
  `Documentation/dev-tools/kmsan.rst`). Run under UML research
  profile with `CONFIG_KMSAN=y`. Expect the same report counts
  the x86 host kernel produces — Invariant I5.
- **Targeted reproducer:** Inject a known uninit read
  (`u8 buf[16]; WRITE_ONCE(*(u8 *)0x0, buf[3]);` sort of thing)
  in a module; verify KMSAN prints a report naming the uninit
  origin stack. Compare byte-for-byte to the same reproducer
  on a bare-metal x86_64 build. Invariant I5 says they must
  match.
- **Fuzz-profile interaction (per U3):** Build a fuzz-kmsan
  combined profile (or override fuzz with
  `CONFIG_KMSAN=y` for the test), run the C-09
  `snapshot-smoke-driver.py` for a handful of iterations, and
  confirm no KMSAN-report corruption across worker fork.

## Unknowns

**U1: 3x host-memory overhead under fuzz workloads.**

- Question: KMSAN adds 1x shadow + 4:1 origin to every kernel
  allocation. On a 4 GB UML guest that's ~12 GB effective host
  RSS. Fuzz profile forks workers via `um_snapshot_ready()`;
  each fork CoW's the kernel but grows metadata as the worker
  dirties pages. Does a sustained fuzz run OOM the host?
- Probe: commit 2 boot with `mem=4G` fuzz profile, count peak
  RSS via `/proc/<uml_pid>/status` across 100 forkserver
  iterations. If peak > 16 GB we flag v1 as "research profile
  only; fuzz+KMSAN deferred until an opt-in sampling mode".
- Mitigation if over: defer KMSAN-in-fuzz to v2 and add a
  `CONFIG_KMSAN_SAMPLING` path (upstream has discussed it;
  upstream work, not UML-local). Research profile keeps full
  KMSAN; fuzz stays KMSAN-off. **Medium risk; measurable pre-
  land.**

**U2: Clang version gate + LLVM=1 build wrinkles across
backends.**

- Question: KMSAN requires clang ≥ 14 with
  `-fsanitize=kernel-memory`. UML's LLVM=1 builds work (A/B
  workstreams) but is the clang instrumentation stable across
  UML's three backends (ptrace / seccomp / KVM)? Different
  backends have different entry paths; clang's inline instru-
  mentation might emit different metadata writes depending on
  -mcmodel or -fno-common flags UML sets.
- Probe: commit 1 adds a Kconfig `depends on
  CC_IS_CLANG_VERSION >= 14` or similar. Build each backend
  via `uml-boot-matrix.sh` with KMSAN=y; verify no link
  errors. Run kmsan_test under each; expect identical report
  shape.
- Mitigation: if a backend diverges (e.g., seccomp entry
  triggers a false positive clang doesn't know to skip),
  narrow the Kconfig to that backend subset and document in
  the user doc. Clang-version bumps of the host distro aren't
  our problem. **Low-to-medium risk.**

**U3: Origin-chain coherence across snapshot/fork worker
reinit.**

- Question: C-09's worker reinit path calls
  `sched_worker_detach_other_tasks` (D42) and drops various
  host-state handles. KMSAN's origin stack depot is global
  state with a spinlock; after a fork, both parent and worker
  have the same depot but the worker's origin-chain entries
  may index stack-frame addresses that no longer exist (the
  parent's unmapped vmas). Does a KMSAN report in the worker
  dereference a stale origin stack pointer and panic?
- Probe: alloc an uninit value before the ready-point, snapshot,
  worker reads the uninit. Generate a KMSAN report. Check that
  the origin trace points at valid pages in the worker's
  address space.
- Mitigation: if origin-chain dereferencing crashes the worker,
  add an origin-chain re-anchoring step to
  `um_snapshot_worker_init()` (D41-style signal-gated,
  mirroring the sigio/timer rebuild). In the worst case,
  document a v1 limitation: "KMSAN reports across snapshot
  may point at stale origin frames; source frame info is
  best-effort." **Medium risk.**

## Risk summary

- **v1 implementation risk: low-to-medium.** KASAN's mmap
  pattern is the template. The arch contract is small; every
  item has a concrete prior-art reference (KASAN, x86 KMSAN).
- **Memory risk: the dominant concern.** Triples kernel RSS.
  Research profile is fine; fuzz profile will be measured
  pre-land (U1).
- **Toolchain risk: accepted.** Clang-only. LLVM=1 UML builds
  already work; adding `-fsanitize=kernel-memory` is the
  standard recipe.
- **Maintenance risk: low.** No UML-specific KMSAN engine; we
  piggyback on `mm/kmsan/` core.

## Resolution path (2026-04-22)

Four probes in D44 closed the design question. The accepted
path is:

1. **Upstream first:** one-patch RFC to `mm/kmsan/` adding a
   weak `kmsan_arch_init_early_shadow()` hook called from
   `kmsan_init_shadow()`, plus un-staticing
   `kmsan_record_future_shadow_range()` so arch overrides can
   reach it. Existing architectures (x86, s390) keep the
   default empty no-op and are byte-identical to today —
   **no visible impact, zero behavior change.** Framing + LKML
   routing captured in D51.
2. **UML side, gated on the generic patch landing:** 5-6
   bisectable commits per D44 second-probe addendum — bounded
   `VMALLOC_END` under `CONFIG_KMSAN=y`, `asm/kmsan.h`,
   Kconfig `select HAVE_ARCH_KMSAN`, `KMSAN_SANITIZE := n`
   where needed, and the `kmsan_arch_init_early_shadow()`
   override that mmaps shadow+origin VAs and registers them
   via the (now-public) `kmsan_record_future_shadow_range()`.
3. **Selftest + docs + research-kmsan profile:** KMSAN KUnit
   passes on UML, reproducer module catches a planted
   uninit-read, user doc at `Documentation/virt/uml/kmsan.rst`.

Why "new entrypoint, not behavior change" is the right
upstream shape: it's the minimum semantic surface to add, keeps
maintenance burden for KMSAN core at zero, and sells to the
maintainer as "enable KMSAN on an additional architecture" —
not as "rework KMSAN init." See D51 for the full cover-letter
frame.

## Historical: what the empirical probes found

Commit-1 scratch (Kconfig + s390-style header + Makefile) built
clean under gcc (KMSAN=n default) and failed under clang with
`CONFIG_KMSAN=y` at `mm/kmsan/shadow.c:62-68`:

```
error: use of undeclared identifier 'KMSAN_VMALLOC_ORIGIN_START'
error: use of undeclared identifier 'KMSAN_VMALLOC_SHADOW_START'
error: use of undeclared identifier 'KMSAN_MODULES_ORIGIN_START'
error: use of undeclared identifier 'KMSAN_MODULES_SHADOW_START'
warning: 'KMSAN_ORIGIN_SIZE' macro redefined (scratch collided
         with mm/kmsan/kmsan.h:30's existing 4-byte value)
```

These are not "arch hooks" — they're virtual-address constants
that `mm/kmsan/shadow.c`'s `vmalloc_meta()` uses to compute
shadow/origin addresses for vmalloc'd and module-area pointers.
Every arch with KMSAN support must define them; the values
encode a geometric assumption about how VMALLOC is carved.

UML's VMALLOC layout (`arch/um/include/asm/pgtable.h:49-53`):
```
VMALLOC_END   = TASK_SIZE - 2 * PAGE_SIZE
MODULES_VADDR = VMALLOC_START  (modules overlap vmalloc)
MODULES_END   = VMALLOC_END
```

Straight x86 quarter-split (`arch/x86/include/asm/pgtable_64_
types.h:132-168`) doesn't map cleanly because UML's VMALLOC is
bounded by TASK_SIZE (not a fixed canonical-hole range) and
modules live INSIDE vmalloc rather than above it.

Three options in D44: (A) shrink UML's VMALLOC_END to 1/4
under CONFIG_KMSAN=y and x86-style subdivide; (B) dedicated
host-mmap regions outside VMALLOC with offsets that make the
shadow.c math work (UML-KASAN pattern extended); (C) defer.
Implementation held pending decision.

The scratch probe also confirmed **U2 on the positive side**:
clang 21.1.8 + `-fsanitize=kernel-memory` works
(`HAVE_KMSAN_COMPILER=y` auto-selected), and UML's LLVM=1 build
reaches the KMSAN compile stage without toolchain friction.
U1 (memory pressure) remains unmeasured because the build
didn't complete. U3 (origin chain across snapshot/fork) is
still purely speculative.

## Sequencing note

This doc is the design; no implementation commits here. v1
implementation starts only once:

- C-06 BPF JIT is either landed (option B sign-off + upstream
  merge) or explicitly deferred via D43's option C. Mixing a
  blocked workstream with a new one muddles the branch state.
- A 30-minute throwaway build probes U1 (memory overhead
  measurement) so we're not committing to a path that OOMs the
  fuzz profile.

If both are satisfied, the four commits above land in roughly 2
weeks with the usual "build + Q1 + boot-matrix + checkpatch"
per AGENT-PROMPT §§3-4 on each.

## Cross-references

- `01-architecture/three-layers.md` — KMSAN is Layer 2
  instrumentation gated by `CONFIG_KMSAN`; no static-key gate
  because the compile-time instrumentation is on-or-off at
  build time (same as KASAN).
- `04-risks/decisions-log.md` D15 — how
  `KASAN_SHADOW_OFFSET` was chosen; KMSAN offsets follow the
  same process.
- `04-risks/decisions-log.md` D37 — v1 pull-forwards for C-09;
  the `um_register_mmap_region()` seam KMSAN shadow + origin
  both hook into.
- `05-validation/a-plus-quality-plan.md` — Q1 bar KMSAN commits
  must pass.
- `arch/um/include/asm/kasan.h` — KASAN arch header, the
  template `arch/um/include/asm/kmsan.h` mirrors.
- `arch/um/kernel/mem.c:48-72` — existing `kasan_init` +
  `.kasan_init` section pattern KMSAN extends.
- `arch/x86/include/asm/kmsan.h`, `arch/x86/mm/kmsan_shadow.c`
  — bare-metal x86 KMSAN arch hooks. UML's arch_kmsan_get_meta_
  or_null simplifies compared to x86 because we have no CPU
  entry area.
- `Documentation/dev-tools/kmsan.rst` — upstream KMSAN user
  doc; the UML-specific `Documentation/virt/uml/kmsan.rst`
  points back to this for the generic semantics.
