# D-04b.2 design note: CR3 over UML's address space

**Status:** design (2026-04-23) — pre-code
**Follows:** D-04b.1a ✓ 06d1cb4063c2, D-04b.1b ✓ ebdfcb5d0fcc,
           D-04b.1c ✓ c461e178da68.
**Supersedes:** The "CR3 = init_mm.pgd - kvm_guest_mem_base"
           formula from 04b-long-mode-sregs.md. That formula
           assumed UML's `init_mm.pgd` was a hardware-valid
           page-table tree — it isn't. Details below.

D-04b.1c proved that the backend's SREGS / GDT / paging port
sits at the spike floor on both Skylake-W and Alder Lake. The
next step, per 04b-long-mode-sregs.md, was to swap the
handcrafted 2 MiB identity mapping for UML's own
`init_mm.pgd` so the vCPU runs real UML kernel code. On
re-examination of UML's mm layer that plan needs revision.

## Why `init_mm.pgd` can't be used directly

UML's `swapper_pg_dir[PTRS_PER_PGD]` is declared statically in
`arch/um/kernel/mem.c:140`. It's what `init_mm.pgd` points
at. At compile time it's zeroed; at run time it's populated
by UML's own bookkeeping as it tracks kernel virtual → kernel
physical mappings.

But UML doesn't use its own PGD for hardware paging. UML is a
userspace program on the host; the host's kernel page tables
(under UML's own pid) are what the CPU actually walks. UML's
`map_memory()` (arch/um/kernel/physmem.c:47) calls
`os_map_memory()` which is a thin wrapper around `mmap()` on
the host. So a new mapping flows through:

  UML kernel `map_memory()` →
  `os_map_memory()` →
  host `mmap(virt, phys_fd, ...)` →
  host kernel populates ITS page tables for UML's process →
  bookkeeping entry in `swapper_pg_dir`

The PGD entries UML writes are UML-internal metadata (for its
vm_area_struct, pte_offset, etc.). They are **not** the
binary PTE bits that x86_64 hardware expects for a real page
walk — UML's pte_t is a compatible-enough struct for UML's
arch code to diff, not a CR3-walk-ready encoding of "frame
number + present bit + write bit + ..." into a 64-bit entry.

Pointing KVM's CR3 at `init_mm.pgd` would either (a) fault on
the first page-walk when the guest MMU can't parse the
entries, or (b) silently walk into random memory.

## Policy for D-04b.2: a dedicated kvm-owned PGD

Build a small, hardware-valid page-table tree owned by the
KVM backend. Identity-map UML's physmem range via 2 MiB huge
pages — same pattern as D-04b.1b's handcrafted 2 MiB slot,
scaled up.

At a minimal UML `mem=128M`:

  128 MiB / 2 MiB = **64 PD entries** (one PD, one 4 KiB page)
  64 entries × 2 MiB each = one PDPT entry
  1 PDPT entry / 512 per PML4 = one PML4 entry

So the whole tree is 3 × 4 KiB = 12 KiB. Comfortably fits at
the end of the existing 2 MiB harness slot (after 0x4000
code sled, with 0x5000+ free).

At full default UML `mem=~64M` or the 128 MiB we boot with,
the cost is the same three pages.

For larger UML physmem (say, 1 GiB), we'd need:

  1024 MiB / 2 MiB = 512 PD entries × 1 PD page each,
                    = 1 PD at the low end + 1 PDPT pointing
                      at potentially 1 or 2 PDs

Still trivial. The upper limit is when we'd want 1 GiB pages
(PDPT.PS bit) — tracked as a future optimization, not D-04b.2
blocker.

## What this buys

  - CR3 points at the kvm-owned tree; walks succeed.
  - Guest sees identity mapping over UML's full physmem
    (via 2 MiB huge pages + EPT via memslot → host VA).
  - RIP can point at any address inside UML's kernel text
    (e.g. a tiny `kvm_harness_hlt_target` function that's
    nothing but `hlt; ret`), and the guest executes it.
  - When D-04c lands the LSTAR trampoline, RIP at
    LSTAR points at bytes inside UML kernel text — those
    are the instructions the guest will execute.

## Not using UML's map_memory to build the tree

Tempting to call UML's own `map_memory()` to build the
kvm-owned tree, but that would double-map (both host mmap
and our own PGD entry). The kvm-owned tree is a synthetic
construct for KVM-backend-internal consumption; UML's own
mm state doesn't need to know about it. Build it with raw
64-bit PTE writes to the harness slot, not via UML's
arch-mm helpers.

## Commit plan

Two landable steps:

**D-04b.2a** — extend the handcrafted tree to cover
physmem_size. Still uses the harness slot for the PGD
itself, but the identity map now covers UML's actual
memory range (not just 2 MiB). Add `kvm_setup_harness_
paging_range()` to sregs.c. Keep harness.c's RIP pointing
at the in-slot code sled so the harness still exits
KVM_EXIT_IO — this commit is pure page-table-coverage
expansion, no new execution path.

**D-04b.2b** — swap harness's RIP to point at a UML-kernel
function. Add a `kvm_harness_hlt_target` function in
harness.c (or a new tu) that's just `asm("hlt")` — address
known to linker. Harness `regs.rip = (u64)&
kvm_harness_hlt_target`, KVM_RUN, expect KVM_EXIT_HLT.
This proves the vCPU can execute real UML kernel text.

After .2a + .2b, D-04b is structurally complete: SREGS +
CR3 + GDT/IDT + memslot + real UML-text execution, no
LSTAR bypass. D-04c adds LSTAR.

## Open questions

  1. **Page-table entry layout.** PML4/PDPT/PD entries are
     standard 4-level x86_64. Hardware-ready. Spike 04
     validated the exact bit layout (PTE_P|PTE_RW|PTE_US|
     PTE_PS). No unknowns.
  2. **Huge-page alignment.** The 2 MiB huge-page entries
     in the PD need their physical address field to be
     2 MiB-aligned. Our slot at 0 is aligned. UML's
     memslot (via kvm_ensure_memslot) covers physical
     address 0 onwards, so guest_phys 0 + 2 MiB offset
     hits real memory.
  3. **Kernel-text executable bit.** Under SMEP / NX, the
     PD entries we set don't have NX (bit 63). That's
     fine; the "NX" in paging terminology is an explicit
     *disallow* bit; not setting it = executable.

## Not in D-04b.2 scope

  - Supporting multiple UML mm's via CR3 switching. D-04c
    or later.
  - LSTAR / SYSCALL dispatch. D-04c.
  - Swapper-level mapping of KASAN shadow / KMSAN shadow
    / vmalloc regions. Later — D-04b.2 covers
    `[0, physmem_size)`, which is where kernel text and
    ordinary heap live.

## Cross-references

  - `04b-long-mode-sregs.md` — overall D-04b plan.
  - `03b-memslot-policy.md` — Policy A memslot.
  - decisions-log D57 — per-process VM fd model.
  - spike 04 (`spikes/04-longmode-lstar-vmcall/spike.c`) —
    the reference encoding for PD / PDPT / PML4 entries
    this tree mirrors.
