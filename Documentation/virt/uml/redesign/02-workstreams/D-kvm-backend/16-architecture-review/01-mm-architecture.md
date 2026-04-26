# KVM Backend MM Architecture Review

Author perspective: Memory / MM architecture.
Date: 2026-04-26.
Scope: shadow PT machinery, mm_id flow, host-VA / shadow-PT consistency,
deferred-vs-direct sync interaction, mm lifecycle, TLB-flush integration.

---

## Executive Summary

The KVM backend has three independent representations of the user address
space — UML's logical pgd, the host process's mmap'd VA via `os_map_memory`,
and the per-mm shadow PT — and tries to keep them coherent through five
overlapping mechanisms (set_ptes hook, pte_clear hook, flush_tlb_* hook,
deferred `um_tlb_sync` chain, eager `kvm_shadow_fill_from_uml_pgd`). My top
three concerns:

1. **Two-source-of-truth host VA aliasing is the unreviewed structural
   risk.** `kvm_mm_map` calls `os_map_memory(virt, MAP_FIXED, …)` against
   the parent UML process for every user mapping. The parent process is the
   one that runs UML's kernel binary, libc, allocator, KVM ioctls, and the
   shadow PT itself. There is no per-mm host VA isolation; one mm's user
   VAs can collide with another mm's user VAs, with the kernel's own
   allocations, with bootstrap aliases, or — most dangerously — with
   `kvm_shadow_pgd` pages and other GFP_KERNEL allocations. Seccomp/ptrace
   never collide because each mm is its own host process. The wild-pointer
   crashes are the exact symptom of host-VA collision corrupting either
   shadow page tables or kernel data structures the shadow walker reads.

2. **The "direct sync" added by memo 15 is a partial graft on top of the
   deferred chain, not a replacement.** `set_ptes` writes the UML PTE,
   marks NEEDSYNC, calls `um_tlb_mark_sync`, AND calls
   `kvm_shadow_sync_pte`. Then later `um_tlb_sync` walks the same range and
   calls `kvm_mm_map` (which calls `os_map_memory` AND
   `kvm_shadow_invalidate_va_range`), then `kvm_enter_guest` runs the
   transactional `kvm_shadow_fill_from_uml_pgd` clear-and-refill which
   blows away whatever direct sync just wrote. The same PTE transition is
   seen 3-4 times by the shadow with no single owner; one of those passes
   wins and the others are wasted, but which one wins depends on
   scheduling, lock ordering, and which call site uses the right `mm`.

3. **Several PTE mutation paths bypass the direct sync entirely**, most
   importantly `pmd_clear` / `pud_clear` / `p4d_clear`, which are
   plain macros that just set NEEDSYNC. On execve teardown, fork mm
   walks, or large mmap teardown, generic mm code clears intermediate
   levels directly — those clears reach `um_tlb_sync` only if the
   later `flush_tlb_*` covers the same range (it does for some but
   not all paths). The shadow leaves below the cleared parent stay
   `_PAGE_PRESENT`; subsequent fills re-walk pgd, find the parent
   absent, do nothing — and the leaves remain forever as
   ABI-compliant shadow entries pointing at freed PFNs.

The redesign memo 15 nailed the diagnosis. The implementation grafted
direct-sync into a system that still treats the deferred chain as
authoritative. That is a worse state than either pure design.

---

## Question-by-Question Analysis

### 1. Is the shadow PT model fundamentally sound, or is it introducing a class of bug seccomp/ptrace avoid by design?

The shadow PT model is fundamentally **architecturally distinct** from
seccomp/ptrace. Seccomp/ptrace each have one address space per UML mm —
the stub child process. The host kernel (the actual Linux kernel UML is
running on) maintains those page tables. There is no shadow; UML's
logical PTE is a derivation that gets pushed into the stub child's mm via
`mmap`/`munmap` syscalls, and the host kernel's MMU is the source of
truth for what the guest CPU actually walks.

Under integrated KVM, UML's logical PTE is no longer the only thing the
backend maintains; it's also the only readable copy. The shadow PT is a
**redundant derived copy** that the guest CPU walks *instead of* the host
kernel's page table. So we now have:

- UML pgd: the authoritative software model (per mm).
- Host process VA via `os_map_memory`: a shadow of UML pgd, maintained by
  `um_tlb_sync`. Seccomp/ptrace also maintain something like this for
  their stub child; KVM also maintains it but the rationale is unclear
  (see Question 6).
- KVM shadow PT (per mm): a second shadow of UML pgd, walked by the guest
  CPU.

Three views, two of which are derivations, all required to agree. That is
a class of bug seccomp/ptrace cannot have because they only have one
derivation (host page table per stub child process).

Is this avoidable? Yes, in principle. KVM's `mmu_notifier` mechanism is
designed exactly for this: a guest can use the host process's page table
directly via `KVM_USER_MEMORY_REGION` plus `gfn_to_hva`, and KVM's
internal shadow walks the host page table. UML could in principle use the
mmu_notifier flow rather than maintaining its own shadow tree.

But UML deliberately chose Policy A (single static memslot covering the
whole UML process VA), which means we are NOT using KVM's normal
shadow-page-table mode; we're using KVM's "host CPU walks our PT
directly" mode. That puts the burden of shadow correctness entirely on
us. With Policy A + identity memslot, the only way to reduce the number
of derivations is to make UML's pgd *be* the guest CR3 directly — which
would require UML's pgd to use x86 hardware bit positions, which it
doesn't (UML PTE bits 0x020 RW, 0x040 USER, etc., are software-defined
and don't match x86's PRESENT/RW/US bit positions).

**Conclusion: the shadow PT model is sound but introduces a class of bug
seccomp/ptrace cannot have. It's the right choice given Policy A + UML's
non-x86 PTE encoding, but it requires a single-source-of-truth discipline
that the current code does not have.**

### 2. Is the dual-write architecture (deferred + direct) itself the bug?

Yes — at a minimum it is a major contributing factor. Current call
sequence for one set_pte_at on user mm:

```
set_ptes(mm, addr, ptep, pte, nr)                           [pgtable.h:313]
  ├── for each pte: set_pte() → marks PTE with NEEDSYNC      [pgtable.h:283]
  ├── um_tlb_mark_sync(mm, addr, addr+nr*PAGE_SIZE)          [pgtable.h:353]
  └── for each pte: kvm_shadow_sync_pte(mm, addr, *ptep)     [pgtable.h:364]
                       ↓
                       atomic write of single shadow leaf u64
                       (or sets shadow->needs_full_resync if path absent)
                       ↓
                       sets shadow->dirty
```

Then later, when the next syscall returns to userspace:

```
kvm_run_userspace                                            [thread.c:2922]
  ├── um_tlb_sync(current->mm)                               [thread.c:3007]
  │     drains NEEDSYNC range:
  │       ops.mmap = um_backend->mm_map                       [tlb.c:197]
  │         → kvm_mm_map(id, virt, len, …)
  │            ├── os_map_memory(virt, MAP_FIXED, …)         [mm.c:135]  <-- HOST VA
  │            └── kvm_shadow_invalidate_va_range(           [mm.c:165]
  │                  shadow, virt, len)
  │                    ↓
  │                    walks shadow tree under fill_lock
  │                    CLEARS each present leaf in [virt, virt+len)
  │                    sets shadow->dirty AND shadow->synced=false
  │
  └── kvm_enter_guest(regs)                                  [thread.c:3014]
        ├── kvm_shadow_map_page(shadow, bootstrap_va, …)      [thread.c:1939]
        │     installs the bootstrap aliases
        ├── if (!shadow->synced || needs_full_resync)
        │     kvm_shadow_fill_from_uml_pgd(shadow, mm->pgd)   [thread.c:2131]
        │       ├── transactional clear of user-half leaves
        │       │     (PRESERVES bootstrap_va range)
        │       └── re-walk pgd, install x86-encoded leaf for every
        │           present UML PTE
        └── if (shadow->dirty) KVM_SET_SREGS(cr3 = shadow->pgd_gpa)
              → cached_cr3_gpa = shadow->pgd_gpa
              → shadow->dirty = false
```

The same logical event (one PTE transition) is processed by **at least
five** writers of the shadow tree:

| When                     | Writer                                 | Effect             |
|--------------------------|----------------------------------------|--------------------|
| set_ptes inline          | `kvm_shadow_sync_pte`                  | install/clear leaf |
| flush_tlb_page/range     | `kvm_shadow_sync_va/range_atomic`      | re-derive leaf     |
| um_tlb_sync drain        | `kvm_shadow_invalidate_va_range`       | CLEAR leaf         |
| um_tlb_sync drain        | `os_map_memory` (no shadow effect, but mutates host VA) |  |
| kvm_enter_guest re-fill  | `kvm_shadow_fill_from_uml_pgd`         | clear + install    |

Failure modes the dual chain creates:

- **Drain-then-clear-the-direct-install.** Direct sync installs the
  correct shadow leaf via `set_ptes`. Then `um_tlb_sync` runs the same
  range through `kvm_shadow_invalidate_va_range`, which under the current
  code CLEARS the leaf (sets it to 0) and sets `shadow->synced=false`.
  Then `kvm_enter_guest` sees `!synced`, runs the full fill — which
  reinstalls based on the same pgd that direct-sync was already current
  with. Net result: redundant work, but still correct *if* the fill
  succeeds. If anything goes wrong in fill (allocation, locking, the
  cleared-then-not-reinstalled gap window) the leaf is lost.

- **Fill nukes the bootstrap aliases unless preserved.** The
  transactional clear in `kvm_shadow_fill_from_uml_pgd` walks the user
  half and clears every present leaf, then re-installs from pgd. The pgd
  doesn't know about bootstrap aliases (they're installed by
  `kvm_shadow_map_page`, not by `set_pte_at`). This is "solved" by
  comparing each VA against `kvm_bootstrap_va` range — but that depends
  on `kvm_bootstrap_va` being populated *before* the first fill. There's
  a one-time race during early init where fill could run before the
  bootstrap is initialized. More importantly, if any future kernel
  mapping is done via the same `alloc_page`-then-`__pa` path, it needs
  to be added to the preservation list. This is not extensible.

- **Direct sync into a path that doesn't exist defers to
  `needs_full_resync`.** The repair path runs in `kvm_enter_guest`'s full
  fill — but the full fill walks the *current* pgd. If between the
  direct-sync and the next fill the pgd has changed again (legitimately,
  via another set_pte_at), the deferred install is silently superseded.
  The intent ("install this specific leaf") is lost.

- **The flush-tlb path uses sync-on-flush, not invalidate.** Per the F1
  comment in shadow_sync.c, blind clearing here would erase the leaf
  direct-sync just installed. So the flush path re-derives the leaf from
  the current UML PTE. But this means **two writers can race at the
  flush boundary**: set_ptes installs leaf X; concurrent set_ptes on a
  different cpu installs leaf Y (same address); flush_tlb_page sees Y in
  pgd and re-installs Y in shadow. Single host thread plus ncpus=1
  prevents this in the steady state, but signal handlers and mid-syscall
  fault recovery are exactly where re-entry happens.

**So yes, the dual-write architecture is a primary contributor.** Even
if every individual writer is correct in isolation, having five of them
guarantees that there's an ordering or window where one path runs and
another path doesn't, and the shadow drifts.

### 3. Is `kvm_um_pte_to_x86`'s software A/D model correct?

Mostly correct, with one **suspected bug** and one **architectural
concern**.

**Suspected bug:** The translator returns 0 (absent) when
`UM_PTE_ACCESSED` is clear. This forces a guest #PF on first access,
which is then recovered by `handle_page_fault`, which sets
`_PAGE_ACCESSED` via `set_pte_at`, which triggers `kvm_shadow_sync_pte`,
which now translates with ACCESSED set and installs the leaf. So far so
good.

But: this depends on `handle_page_fault` actually being called during
the pf-recovery path for not-yet-accessed leaves. The recovery code at
`thread.c:3389` calls `handle_page_fault(cr2, fault_rip, ...)`. If
`cr2` is in a vma but the PTE is present-but-not-accessed in UML's pgd,
`handle_mm_fault` may take the "spurious fault" fast path (which on
x86 just does `flush_tlb_fix_spurious_fault`). On UML that calls
`flush_tlb_page` → `kvm_shadow_sync_va_atomic` → re-derives leaf →
translator sees ACCESSED still clear → returns 0 → leaf stays absent
→ guest re-enters and faults again immediately → infinite loop.

The mitigating factor is `pte_mkyoung()` being called on the PTE in
`handle_pte_fault`'s spurious-fault arm before the flush. So the racing
scenario is: pgd shows young between fault and recovery → pte_mkyoung
→ set_pte_at → direct-sync sees young → installs leaf. The flush
afterward re-derives and sees the same. OK, it recovers.

**But** the translator dropping `UM_PTE_RW` when `UM_PTE_DIRTY` is
clear (the dirty-emulation fix from the comment) means a clean writable
PTE has shadow leaf with W=0. Guest writes fault. Recovery calls
`handle_page_fault(..., is_write=1)` → handle_mm_fault → ptep_set_access_flags
→ pte_mkdirty → set_pte_at → direct-sync sees dirty → installs leaf
with W=1. OK.

Where this gets dangerous: what does **page reclaim** see for these
PTEs? UML's reclaim path doesn't know that the shadow PT exists. If
reclaim wants to clear `_PAGE_DIRTY` on a UML PTE that has been mapped
W=1 in the shadow, the shadow leaf still has W=1 — guest can write
without faulting → write reaches the page → reclaim treats the page as
not-dirty → page evicted with the writes lost.

The redesign memo 15 explicitly calls this out:

> If shadow let KVM bypass UML's emulated A/D faults … writes went to one
> physical page, later reads used a stale shadow leaf, or page
> reclaim/COW treated a page as clean or old because the shadow let KVM
> bypass UML's emulated A/D faults.

This is consistent with the symptom: a freshly-allocated PyObject's
`ob_type` field is non-NULL when written, but later reads see NULL —
because reclaim freed the page that holds the write while the shadow
still routes guest reads to a stale physical page (or the read goes to
the right page but the write went to a now-freed page).

**Architectural concern:** the translator preserves bit 63 (NX) directly
from the UML PTE. UML doesn't appear to set NX (`_PAGE_NX` is not in
pgtable.h). So bit 63 is always 0 in UML PTEs. That's fine in isolation
but means we have no way to deny execute on a guest leaf. EFER.NXE is
set in `kvm_setup_production_sregs`, which is correct. But every present
UML PTE becomes a present-RWX-or-RW shadow leaf. That's broader than
the UML pgd's intent (UML uses `_PAGE_USER` to gate read AND execute,
not separately).

For the bug we're chasing, this is probably not the primary contributor.
But it's a real correctness gap for any workload that relies on PROT_NONE
or PROT_NONE-then-PROT_READ transitions.

### 4. Is the transactional fill correct given concurrent set_pte_at via direct sync?

**Probably correct in the steady state, suspected to break under
preemption.** The fill takes `shadow->fill_lock` (mutex) before its
clear-then-refill. The direct sync **does NOT take fill_lock** — by
design, because direct sync runs from atomic context (page-table
spinlock). So:

```
CPU0 (direct-sync from set_ptes)             CPU0 (or CPU1 in SMP) running fill:
                                              guard(mutex)(&shadow->fill_lock)
                                              for each pgd_i: clear leaves
                                                ↓
   READ_ONCE old leaf (= cleared 0)
   WRITE_ONCE new leaf
                                              for each pgd_i: install from pgd
                                                ↓ (sees the just-installed leaf
                                                   from direct-sync? Or overwrites
                                                   it with whatever kvm_um_pte_to_x86
                                                   currently translates?)
```

In ncpus=1 cooperative mode the direct-sync writer and the fill walker
can't actually overlap — they'd have to be on the same host thread,
which can only be in one of the two functions at a time. So this is a
non-issue in current builds. **But** signal-driven preemption can
interleave: a SIGALRM during set_ptes can defer the loop, then the
signal handler runs, then a fault recovery runs another set_ptes that
contends with the first. UML disables signals around critical sections
(`block_signals_trace`) — but I don't see that wrapping `set_ptes`
explicitly. The pte spinlock taken by generic mm is the only thing
protecting the PTE-page itself.

The real concern: **fill's clear pass is unconditional within the user
half.** It walks PGD slots 0..255, finds every leaf with `KVM_X86_PTE_P`,
clears it (preserving only the bootstrap-VA range). If direct-sync just
installed leaf X via `kvm_shadow_sync_pte`, and that VA is in the user
half but not in the bootstrap range, fill happily clears it — then
re-installs based on `mm->pgd`'s view. If the pgd hasn't been updated
yet (because we're mid-set_pte loop?), the leaf is lost.

The set_ptes hook calls direct-sync **after** the entire pte_copy loop
(see pgtable.h:355-365), so by the time direct-sync runs, the pgd is
already fully updated. So the fill should re-install correctly. OK.

The **proven bug** this exposes: the bootstrap_va range check in fill is
`va >= alias_lo && va < alias_hi`, with `alias_lo = kvm_bootstrap_va_get()`.
At very first entry (before bootstrap is initialized) `alias_lo = 0`,
which means the bootstrap-preservation check `if (alias_lo && va >=
alias_lo …)` short-circuits — fill clears everything in user half. But
the bootstrap pages haven't been mapped yet, so there's nothing to
clear. OK, that's actually safe by accident.

But: `kvm_bootstrap_va` is the kernel VA returned by `get_zeroed_page()`
during the lazy bootstrap init. That's a host-process VA, which on a
typical x86_64 Linux host lands somewhere in the linear map at
~0xffff8800… or similar (kernel half) — but UML doesn't use that
layout; UML's `__pa()` and `__va()` are tied to `uml_physmem`, and the
allocated page lives in the UML kernel's notion of physical memory at
some kernel-VA above `uml_physmem`.

**Suspected critical bug: bootstrap_va lands in PGD slot 0, the user
half.** From `um_arch.c`: `uml_physmem = __binary_start & PAGE_MASK`,
which on a typical Linux host is around `0x60000000` (low gigabyte
region of the host process). So a `get_zeroed_page` allocation ends up
at a kernel VA in the same low-GB range — let's say `0x65000000`. That
VA, decomposed:
- `pgd_i = (0x65000000 >> 39) & 0x1ff = 0`
- `pud_i = (0x65000000 >> 30) & 0x1ff = 1`

So the bootstrap pages do live in PGD slot 0, the user half. The fill's
clear pass will see them (correctly preserved by the alias_lo/hi check).
But this also means **the bootstrap pages and user-VA pages share the
same intermediate PUD/PMD tables in the shadow**. If a user mapping
exists at any VA in `[0x40000000, 0x80000000)` (PUD slot 1 of PGD slot
0), that user mapping's PMD page is the same physical page as the
bootstrap PMD. Filling, clearing, or invalidating one walks the same
intermediate tables as the other.

Worse: legitimate user mappings around 0x40000000 - 0x80000000 are
exactly where dynamic loaders place ld-linux. A typical Python
process's ld-linux maps at `0x40002000` or similar. So bootstrap-VA
intermediate tables and Python's ld-linux intermediate tables collide.

This is **suspected** because I haven't traced an actual collision case
through the code, but the address ranges line up exactly with the
observed `0x4002625f` faulting RIP from STATUS.md.

### 5. mm lifecycle: execve replacing an mm — windows where freed shadow could be referenced?

`destroy_context` (mmu.c:96) calls `um_backend_dispatch(mm_detach, ...)`,
which under KVM is `kvm_mm_detach`. That calls `kvm_shadow_mm_free`
(mm.c:77) which:

1. If `shadow->pgd_gpa == ctx->cached_cr3_gpa`, sets `cached_cr3_gpa = 0`
   (lifecycle.c:708). This invalidates the SREGS-skip cache.
2. Walks the PGD recursively and `__free_page`s every present
   intermediate PUD/PMD/PT page.
3. Frees the PGD page itself.
4. Frees the `kvm_shadow_mm` struct.

**Suspected bug: race between `shadow_mm_free` and another task running
KVM_RUN.** Cached_cr3_gpa is only checked against `shadow->pgd_gpa` —
but the active mm of the running task could be a *different* mm whose
shadow happens to share the same intermediate tables (per the collision
above) or whose CR3 the freed pages were also cached for. The check is
exact-equality on pgd_gpa; under any aliasing it misses.

More importantly: **destroy_context runs with the mm refcount at 0**
(it's only called via `__mmput` after the last reference drops). The
running task's `current->active_mm` should NOT be this mm at that
point. But `prev->active_mm` in `kvm_context_switch` could be — if a
context switch is in flight while `__mmput` runs concurrently. Linux's
mm refcount discipline prevents this, but it relies on the architecture
holding `mmgrab()` during context switch, which UML may or may not do
correctly given its cooperative scheduling.

**Proven gap (file:line):** `mm.c:71-101` — `kvm_mm_detach` does NOT
take any lock when calling `kvm_shadow_mm_free`. If a concurrent (in
SMP, or signal-handled) `kvm_shadow_sync_pte` is running on the same
shadow because it raced with the last set_pte_at on the dying mm, it
would access freed memory. The single-host-thread model masks this in
ncpus=1, but the comment in `shadow_sync.c:30-35` admits this is an
assumption that doesn't hold under signal-driven preemption.

`init_new_context`'s `mm_unmap(new_id, 0, STUB_START)` was the
original killer (already fixed by the early-exit in `kvm_mm_unmap`).
But the existence of that bug is a smell: the contract that
`mm_attach` followed by `mm_unmap(0, STUB_START)` makes sense for a
stub-child model and makes no sense for KVM. The integrated KVM
backend has been bolted onto contracts designed for the stub model,
and every "doesn't apply, skip" is a place where the contract is
silently violated.

### 6. `kvm_mm_map` does `os_map_memory` MAP_FIXED into the parent process AND maintains a per-mm shadow. Two sources of truth — when does this matter?

This is the area I'm most concerned about. Per memo 15 §"Host VA Mapping
Policy" and the `#276 reverted` comment in mm.c:124-134, the user
attempted to remove `os_map_memory` from the KVM path and got
"crashes inside libc". The comment hypothesizes "copy_to_user fallback
or io_uring fixed-buffer setup" needs the host VA mapping live.

**Looking at uaccess.c:46-93**, `raw_copy_from_user` and
`raw_copy_to_user` go through `maybe_map` → `pte_offset_kernel(pmd,
addr)` → `pte_page(*pte)` → `page_address(page)`. So they do NOT use
the host VA mapping at all on 64-bit; they use the kernel's linear map
of the physical page. So uaccess shouldn't depend on `os_map_memory`.

**But the comment says removing it broke things.** That points to one of:
- A direct user-VA dereference somewhere (futex atomics? signal-frame
  setup?) that I haven't located.
- An io_uring fixed-buffer registration that does
  `get_user_pages_fast`-equivalent, which under UML would walk the host
  VA via the host kernel's page table, not via UML's pgd.
- Something pinning user pages via `mlock`-like mechanisms.

Whatever it is, **the fact that nobody knows what depends on it is the
real issue**. The `os_map_memory` call has unknown consumers, runs into
an unbounded shared address space, can destructively unmap the parent
process if `len` is wrong (which it WAS before the
`init_new_context` early-exit), and creates collisions between mms.

The "two sources of truth" matters every time you have:
- A user mapping at VA X in mm A: host VA X is mapped to physical page
  P_A.
- A user mapping at VA X in mm B: host VA X gets re-mapped to physical
  page P_B (`MAP_FIXED` overwrites).

If at any point UML's kernel code (running in the parent process, NOT
under KVM_RUN) dereferences VA X, it hits whichever mapping was most
recently `os_map_memory`'d. The shadow PT is per-mm and resolves
correctly for the guest CPU under KVM_RUN; but the host VA is global to
the parent process.

`mmu_notifier` does NOT save us here. mmu_notifier is for KVM to be
notified when the host kernel's page tables change, so KVM can
invalidate its SHADOW. We're not using KVM's shadow; we're maintaining
our own per-mm shadow whose only relationship with the host kernel's
page table is that the GPAs in the leaves are interpreted via Policy A
(gpa = host_va - uml_physmem). Whether the host VA mapping is even
present doesn't affect what the guest CPU sees for user pages — the
guest CPU walks shadow PT → gets gpa → KVM resolves via memslot → reads
host physical page directly.

**So `os_map_memory` is structurally unnecessary for guest-side
correctness.** Some host-side path apparently needs it; that path is
the bug, not the absence of `os_map_memory`. Removing it correctly
would require auditing every host-side direct user-VA dereference.

### 7. Per-mm shadow and bootstrap-aliases-in-PGD-slot-0

Already covered in question 4. To re-state cleanly:

- `kvm_bootstrap_page` is allocated via `get_zeroed_page` which lives in
  the UML kernel's "linear map" at a kernel VA derived from
  `uml_physmem`. On a typical host, `uml_physmem` is `~0x60000000`,
  putting the bootstrap pages around `~0x65000000`.
- That VA decomposes as PGD slot 0, PUD slot 1, PMD slot N for some N
  in 0..511.
- User-mode dynamic loaders map at low GBs too: ld-linux often loads
  near `0x40000000`, libc near `0x70000000`, etc. All of these land in
  PGD slot 0.
- The shadow PT is per-mm, but **the intermediate PUD/PMD pages within
  one mm's shadow are shared between bootstrap aliases and user
  pages**. When the fill's clear pass walks the user half and clears
  every leaf-except-bootstrap, it traverses the *same* PMD pages that
  user mappings are in. That's correct, but means:

  - PUD slot 1 (covering 0x40000000 - 0x80000000) holds the bootstrap
    PMD page in some PMD slot.
  - It also holds user PMDs for ld-linux, libc, Python interpreter
    text, etc.
  - All of these PMD entries point at PT pages that might, in turn,
    contain bootstrap PT entries side-by-side with user PT entries.

If a user mapping happens to land on the same 4-page (2MB / 512-PTE
PMD) range as the bootstrap, the bootstrap PT and a user PT share a PT
page. Then a user-mapping clear walks that page and accidentally
clears bootstrap entries — except the bootstrap-VA preservation check
covers exactly the 4 pages of bootstrap, so single-PT-entry collisions
are caught.

But the real risk: **bootstrap_va is determined by what
`get_zeroed_page` returns at boot**, which depends on host memory
layout, ASLR, kernel slab state, etc. There's no guarantee it lands
outside any user vma's eventual range. Different boots produce
different bootstrap_va, which is consistent with the
"failure correlates with workload size — more memory ops, more chance
to hit the corruption" observation in STATUS.md.

**Suspected: the user-half / kernel-half split assumption is wrong for
UML.** x86_64 generic mm assumes user VAs are in PGD slots 0..255 and
kernel VAs in slots 256..511 (canonical low half / high half). UML
co-locates them all in the low half because UML's TASK_SIZE is the host
process's task size minus stub size, which is the whole 128 TB user
half. There is no "kernel VA in PGD slot 256+" on UML — kernel
allocations live in the same PGD slots as user mappings.

The fill code at lifecycle.c:1216 says:
```
for (pgd_i = 0; pgd_i < 256; pgd_i++) {  /* user half */
```

This is wrong for UML. Slots 0..255 contain BOTH user mappings AND
kernel-VA aliases like the bootstrap. The "preserve only bootstrap_va
range" check is a band-aid for one specific kernel allocation; any
other kernel allocation that ends up needing a shadow entry will be
silently wiped on every fill.

---

## Concrete Bugs Found

### PROVEN

**B1.** `arch/um/include/asm/pgtable.h:158` — `pmd_clear`, `pud_clear`,
`p4d_clear` are macros that only set `_PAGE_NEEDSYNC` and do NOT call
the direct-sync hook. On any path that uses these (mm teardown, large
unmap, fork mm copy with subsequent clear), the shadow tree retains
present leaves below the cleared parent. `kvm_shadow_invalidate_va_range`
is only called from `kvm_mm_unmap`, which is only called via
`um_tlb_sync`, which only fires if `flush_tlb_*` covers the range.
Higher-level clears that span entire PMDs/PUDs may or may not be
followed by a covering flush; for the ones that aren't, shadow leaves
stay present forever pointing at freed PFNs.

**B2.** `arch/um/backend/kvm/lifecycle.c:1216` — fill loop's user-half
boundary at PGD slot 0..255 is meaningless for UML because UML kernel
VAs live in the same PGD slots as user VAs. The preserve-bootstrap-only
exception covers exactly 4 pages; any other kernel allocation that
needs to be shadow-mapped is wiped on every fill.

**B3.** `arch/um/backend/kvm/mm.c:135` — `os_map_memory` operates on the
parent UML process's address space with `MAP_FIXED`. Cross-mm collisions
with kernel allocations or with another mm's user mappings have
no protection. The comment at line 124-134 admits "the cross-mm
collision case stays open for a different fix."

**B4.** `arch/um/backend/kvm/lifecycle.c:1418` — `kvm_shadow_map_page`
takes a passed-in `leaf_flags` and writes it to the shadow leaf as-is.
But `kvm_shadow_table_step` (lifecycle.c:1043) installs intermediate
entries with `KVM_X86_PTE_P | KVM_X86_PTE_RW | KVM_X86_PTE_US |
KVM_X86_PTE_A` unconditionally. So if a leaf intends to be ring-0-only
(e.g. the bootstrap page, mapped without `US`), the intermediate PMD
entry is `US=1` — guest ring-3 code can walk down to the leaf and
the leaf's `US=0` denies access. That's correct behavior, but it
means the bootstrap pages' "US=0 for defense in depth" is partial:
the page-table walk itself is visible to ring-3.

### SUSPECTED

**B5.** `arch/um/include/asm/tlbflush.h:60-61` —
`flush_tlb_page → kvm_shadow_sync_va_atomic` re-derives the leaf from
the current UML PTE inside an atomic-context handler. If the PTE was
just transitioned to `_PAGE_PRESENT | _PAGE_ACCESSED | _PAGE_RW |
_PAGE_DIRTY` and the old shadow leaf was W=0 (clean-writable), the
sync writes the new leaf with W=1. But the *guest TLB* may have
cached the W=0 entry. There's no IPI to the vCPU forcing TLB flush
between the shadow write and the next KVM_RUN. The next KVM_RUN does
SREGS reload only if `shadow->dirty` is set — and `kvm_shadow_sync_pte`
DOES set `shadow->dirty = true` (line 268). So this should trigger
TLB flush. Suspected, not proven, that there's a path that mutates
shadow without setting dirty.

**B6.** `arch/um/backend/kvm/lifecycle.c:1063-1133` —
`kvm_um_pte_to_x86` returns 0 for `!UM_PTE_ACCESSED`, which causes the
shadow leaf to be cleared. But UML's `set_pte` always sets
`_PAGE_NEEDSYNC` (and via `pte_mkneedsync` doesn't touch ACCESSED).
A freshly-installed PTE without ACCESSED → translator returns 0 →
direct-sync clears any existing leaf → guest faults on access →
recovery sets ACCESSED → flush re-derives → leaf installed.

But this depends on the recovery actually completing. If the access
is a guest write to a page that was freshly mapped via `mmap(MAP_ANON
| MAP_PRIVATE)` for which `do_anonymous_page` populated the PTE with
ACCESSED already set (it does), this is fine. If the access is to a
file mapping where `filemap_map_pages` populates the PTE with
ACCESSED clear (which it might for prefault), the first guest read
faults, recovery happens, second access succeeds. That's two #VMEXITs
per page on first touch. Probably not corrupt but adds latency proportional
to working set.

**B7.** `arch/um/backend/kvm/thread.c:2160-2165` — SREGS-skip cache check:
```c
if (ctx->sregs_primed &&
    ctx->cached_cr3_gpa == cr3_gpa &&
    ctx->cached_fs_base == cur_fs &&
    ctx->cached_gs_base == cur_gs &&
    shadow && !shadow->dirty)
    goto sregs_done;
```
The `cached_cr3_gpa` is set when SREGS was last loaded. If the active
mm changes (different `shadow->pgd_gpa`) but the new mm's pgd_gpa
happens to match the OLD cached_cr3_gpa (page reuse after free),
AND `shadow->dirty` is false (because nothing has marked the new
shadow dirty yet — it was just allocated clean), the skip path
returns without loading the right CR3. Guest then walks the
PREVIOUS mm's shadow tree.

`destroy_context` does invalidate `cached_cr3_gpa` when the FREED
shadow's pgd_gpa matches. But it's a single u64 cache; if mm-A is
detached, cache cleared, mm-B is created with the same physical PGD
page (free + alloc returns same page), mm-B's shadow has the same
pgd_gpa as the old mm-A. The cache is now 0 (cleared), so the next
entry programs SREGS for the new mm. OK.

Worse case: mm-A active. mm-B created (different pgd_gpa). Context
switch to mm-B. SREGS loaded with mm-B's pgd_gpa. mm-B detached
(rare-but-possible if mm-B was a kernel-thread-borrow on a dying mm).
Cache is invalidated to 0. Switch back to mm-A. cached=0, mm-A's
pgd_gpa ≠ 0, so SREGS reloaded. OK.

This looks correct, but it's load-bearing on the destroy path
correctly invalidating, AND on no race window where the destroyed
shadow's CR3 lives in a vCPU TLB entry that wasn't flushed.

### SPECULATIVE

**B8.** Direct-sync from `set_ptes` (pgtable.h:364) runs from
arbitrary contexts, including kernel threads with no UML user mm
attached. `kvm_shadow_sync_pte` checks `mm->context.id.kvm_shadow`
and bails if NULL. But it does not check whether the pte's address
is even in the user range. A `set_ptes` on a kernel-mapped page
(say, vmalloc) for `init_mm` would have `mm = &init_mm`, which has
`init_mm.context.id.kvm_shadow == NULL` (init_new_context only runs
for user mms). So it bails. OK.

But what about mm-walking helpers that operate on a user mm from a
non-current task context (e.g. /proc walkers)? `kvm_shadow_sync_pte`
would mutate that other mm's shadow, which could be the active mm of
a different KVM_RUN happening in another thread. Single-host-thread
saves us in current builds.

**B9.** The mut_ring (`shadow_sync.c:62-77`) is per-mm and single-writer
under the cooperative model. But `kvm_shadow_mut_dump_for` reads it
from the panic handler at `thread.c:3537`. If the panic happens
mid-write to the ring (writer was preempted by the fault that caused
the panic), the ring entry being read is half-formed. Probably
benign (worst case is unreadable diagnostic output) but worth noting.

---

## Data Flow Diagrams

### Current Architecture (problematic)

```
                                        [Linux mm core]
                                              |
                                  set_pte_at  |  pte_clear    pmd_clear
                                              |              pud_clear
                                              v              p4d_clear    flush_tlb_*
                            +-----------------+---------+   (no hook!)        |
                            |                           |                     |
                            v                           v                     v
                     UML pgd update             UML pgd update           [needs flush]
                     [pgtable.h:set_ptes]       [pgtable.h:pte_clear]         |
                            |                           |                     |
              +-------------+--------+         +--------+-------+   +---------+--------+
              |                      |         |                |   |                  |
              v                      v         v                v   v                  v
   um_tlb_mark_sync          kvm_shadow_      same        kvm_shadow_  um_tlb_mark_   kvm_shadow_
   (deferred range           sync_pte         marker      sync_pte     sync           sync_va/
    coalescing in              |              |             |          (defer)         range_atomic
    sync_tlb_lock)             v              v             v             |               |
              |          [direct path]    [defer]     [direct path]       |               |
              |         clear/install      |          clear/install       |               v
              |         shadow leaf        |          shadow leaf         |          re-derive
              |         atomically         |          atomically          |          shadow leaf
              |         OR set             |          OR set              |          atomically
              |         needs_full_resync  |          needs_full_resync   |          (no clear-only
              |                            |                              |           because that
              v                            v                              v           would erase
   [pending range until       [pgd marked NEEDSYNC]            [pending range]        direct-sync's
    next um_tlb_sync]                                                                  install]
              |                                                                |
              +----------------+ next um_tlb_sync (run_userspace,              |
                               | context_switch, pf_recovery)                  |
                               v                                               |
                   walk pgd → for each NEEDSYNC pte:                           |
                     ops.mmap=kvm_mm_map(id, va, len, ...) ----------+         |
                              |                                      |         |
                              v                                      v         |
                     os_map_memory(va, MAP_FIXED, ...)         kvm_shadow_     |
                     into PARENT UML process VA                invalidate_va_  |
                       (collides with kernel,                   range          |
                        with other mms, with                    (CLEARS leaves |
                        shadow PT pages, etc.)                   in [va,va+len)|
                                                                 OVERWRITING   |
                                                                 direct-sync's |
                                                                 install)      |
                                                                               |
                              +----------------+----------------+              |
                                               |                               |
                                               v                               |
                                   shadow->synced = false                      |
                                   shadow->dirty = true                        |
                                               |                               |
                                               v                               |
                                       kvm_enter_guest                         |
                                       (run_userspace per                      |
                                        trap)                                  |
                                          |                                    |
                          +---------------+----------+                         |
                          |               |          |                         |
                          v               v          v                         |
              kvm_shadow_map_page    if (!synced)    if (dirty)                |
              of bootstrap pages     OR (needs_     KVM_SET_SREGS              |
              (preserved by fill's   full_resync)   with new CR3,              |
              alias range check)         |         cached_cr3_gpa = pgd_gpa,   |
                                         v         shadow->dirty = false      |
                              kvm_shadow_fill_                                 |
                              from_uml_pgd:                                    |
                                clear ALL leaves                               |
                                in PGD 0..255                                  |
                                  (preserving                                  |
                                  bootstrap range)                             |
                                walk UML pgd,                                  |
                                install x86 leaf                               |
                                for every present                              |
                                UML PTE                                        |
                                                                               |
              [shadow PT now agrees with pgd "for now"]                        |
              [bootstrap aliases preserved]                                    |
              [host VA may or may not agree depending on um_tlb_sync result]   |
                                                                               |
                                       KVM_RUN                                 |
                                          |                                    |
                                          v                                    |
                                   guest CPU walks shadow PT,                  |
                                   gets gpas, KVM resolves via                 |
                                   single static memslot                       |
                                   (gpa → host_va = uml_physmem + gpa)         |
                                          |                                    |
                                          v                                    |
                                   reads/writes host physical page             |
                                   directly via host kernel's page             |
                                   table for the parent UML process            |
                                   (which may have been MAP_FIXED'd            |
                                   to a DIFFERENT physical page by             |
                                   os_map_memory under another mm)             |
```

### Proposed Architecture (single-source-of-truth)

```
                                    [Linux mm core]
                                          |
              all PTE and parent-table mutations
                                          |
                                          v
                             arch/um/include/asm/pgtable.h
                             arch/um/include/asm/pgalloc.h
                          (UML overrides for ALL mutators:
                           ptep_get_and_clear,
                           ptep_set_access_flags,
                           ptep_set_wrprotect,
                           ptep_test_and_clear_young,
                           ptep_modify_prot_*,
                           pmd_clear / pud_clear / p4d_clear → range)
                                          |
                                          v
                       um_pte_mutate(mm, addr, old_pte, new_pte)
                       (single chokepoint; takes per-mm pte_lock+notifier)
                                          |
                            +-------------+-------------+
                            |                           |
                            v                           v
              UML pgd update (atomic)        kvm_shadow_sync_pte
                                             (per-mm shadow update,
                                              same atomic context,
                                              fail-safe to absent on alloc fail
                                              — never leaves stale leaf)
                            |                           |
                            +-------------+-------------+
                                          |
                                          v
                             [single transaction complete]
                             shadow now mirrors pgd
                             shadow->dirty = true
                             shadow->generation++ (monotonic)
                                          |
                          (deferred chain GONE for KVM)
                                          |
                                          v
                                  kvm_run_userspace
                                          |
                       NO um_tlb_sync, NO mm_map, NO mm_unmap,
                       NO os_map_memory into parent process
                                          |
                                          v
                                  kvm_enter_guest
                                          |
                          if (shadow->generation_at_last_sregs
                              != shadow->generation)
                            KVM_SET_SREGS, cached gen = shadow->gen
                                          |
                                          v
                                       KVM_RUN
                                          |
                                          v
                              guest walks shadow PT, KVM resolves
                              via static memslot, reads physical
                              pages directly. Host parent process VA
                              is irrelevant to guest correctness;
                              only host kernel's mm_struct (for the
                              shared memslot view) matters.

              [Optional: keep os_map_memory only if a host-side
               direct-deref path is found; that path is then a
               documented exception, not the primary mechanism.]
```

---

## Recommended Architectural Changes

In rough priority order:

### R1 — Replace the deferred chain with direct sync. Don't have both.

The `set_ptes` / `pte_clear` already direct-sync. Keep that, AND:

- Override `pmd_clear` / `pud_clear` / `p4d_clear` to also call
  `kvm_shadow_clear_range_atomic` for the covered VA range.
- Override the generic `ptep_*` family in UML so each one calls
  direct-sync after its PTE write.
- DELETE the `_PAGE_NEEDSYNC` / `um_tlb_sync` code path under
  `CONFIG_UM_BACKEND_KVM_INTEGRATED`. Keep it for ptrace/seccomp
  builds only.
- DELETE `kvm_mm_map` / `kvm_mm_unmap`'s shadow-invalidation calls.
  Make them no-ops for the shadow side (host-side `os_map_memory`
  decision is independent — see R3).
- DELETE the unconditional `kvm_shadow_fill_from_uml_pgd` from
  `kvm_enter_guest`'s steady state. Keep it as initial-attach-only,
  needs-full-resync repair, and explicit debug audit.

### R2 — Fix the user-half / kernel-half assumption.

UML has no separate kernel half. Bootstrap aliases must NOT live in
user-half PGD slots. Two options:

- **R2a (simple).** Reserve a dedicated PGD slot (e.g. slot 510, well
  above any user VA UML can produce since TASK_SIZE is bounded) for
  KVM bootstrap aliases. Allocate bootstrap pages via a custom
  vmalloc-like region rather than `get_zeroed_page`. The fill code
  becomes "clear PGD slots [0, TASK_SIZE>>PGDIR_SHIFT)" and never
  touches the kernel-bootstrap PGD slot.
- **R2b (better).** Don't fill from pgd at all in steady state; the
  shadow is maintained transactionally per R1. Fill is only needed
  for initial population, which can install the bootstrap alias
  first then walk pgd without any clear pass.

### R3 — Resolve the `os_map_memory` question definitively.

Pick one:

- **R3a.** Audit every host-side direct user-VA dereference. Confirm
  no path needs `os_map_memory` once direct-sync covers shadow
  correctness. Remove `os_map_memory` from `kvm_mm_map`. Verify
  `import unittest` no longer crashes in libc (the symptom that
  caused #276 to revert).
- **R3b.** Keep `os_map_memory` but isolate it in a per-mm host
  worker process (the seccomp stub-child model). Each UML mm's
  user VAs map into ITS own host worker, not the parent. Heavier
  to implement; higher confidence of no cross-mm collision.

R3a is preferred per memo 15.

### R4 — Per-shadow generation counter, not per-shadow flags.

Replace `shadow->dirty` (one bit) and `shadow->synced` (one bit, plus
a synced_pgd_va comparison) with:

```c
struct kvm_shadow_mm {
    atomic64_t generation;          /* incremented on any mutation */
    u64        sregs_loaded_gen;    /* generation at last KVM_SET_SREGS */
    u64        last_audit_gen;      /* generation at last full-fill audit */
    ...
};
```

Then:
- Every direct-sync writer atomic_inc the generation.
- `kvm_enter_guest` issues KVM_SET_SREGS iff `generation !=
  sregs_loaded_gen`, then sets `sregs_loaded_gen = generation`.
- Audit / verifier paths can check whether anything has changed
  since the last audit without comparing the entire tree.
- Diagnostics get a clean monotonic timeline of "this corruption
  happened between gen X and gen Y."

### R5 — Make `kvm_shadow_mm` self-describing.

Add to the struct (keeping the existing direct-sync counters):
- A pointer back to `mm_struct` so the shadow can self-validate at
  any callback ("am I being called for the right mm?").
- A unique 64-bit ID set at alloc time (atomic counter) so logs can
  unambiguously identify "this is shadow #42, allocated at boot+5s,
  for mm-pointer 0xffff...."
- A teardown-in-progress flag set under the fill_lock at the start
  of `kvm_shadow_mm_free`. Direct-sync checks this; if set,
  fast-bail. Ensures no late writer touches a freed shadow.

### R6 — Lift the contract assumption that mm_unmap(0, STUB_START) is meaningful.

`init_new_context` (mmu.c:86) calls
`um_backend_dispatch(mm_unmap, new_id, 0, STUB_START)` for every fresh
mm. This is meaningful for a stub-child model (clear the new child's
address space). Under integrated KVM there is no child; the per-mm
shadow was just allocated empty by `kvm_shadow_mm_alloc`. The current
code papers over this with an early-return in `kvm_mm_unmap` (mm.c:211).

Better: split the `init_new_context` contract. Define a separate
backend op `mm_init` that means "the mm is freshly created; do
whatever per-mm setup is needed". For stub-child backends, that
includes the bulk-clear. For integrated KVM, that's a no-op (the
allocation already gave us a clean shadow). Then no integrated-KVM
backend op ever receives a "len=128TB, virt=0" call.

---

## What I Would Do First If I Could Pick ONE Thing

**Implement R1 — collapse to a single source of truth for the shadow tree.**

Specifically: under `CONFIG_UM_BACKEND_KVM_INTEGRATED`, gut the deferred
`um_tlb_sync` path entirely for KVM, and gut the `kvm_shadow_fill_from_uml_pgd`
on every entry. Make the per-PTE direct sync the sole shadow writer.
Make `kvm_enter_guest` a verifier (KVM_SET_SREGS only when generation
advanced) and TLB flush point, never a refiller in the steady state.

Why this and not, say, fixing the `os_map_memory` collision risk first:
- The `os_map_memory` collision is theoretically scary but the symptoms
  (NULL+small-offset crashes, random 45-bit addresses) are also
  consistent with shadow-leaf-pointing-at-wrong-physical-page, which
  R1 fixes by removing the racy multi-writer model.
- The "many local fixes haven't moved parity" pattern is exactly what
  the dual-write architecture predicts: every fix to one writer is
  shadowed by a different writer's stale state. R1 collapses the
  writers.
- R1 is structurally smaller than R3 (which requires a host-side
  audit) and a strict prerequisite for R3 (you can't safely remove
  `os_map_memory` until you trust the shadow alone).
- It directly tests memo 15's hypothesis that the bug is structural,
  not a missing bookkeeping update.

Concrete first step: under `CONFIG_UM_BACKEND_KVM_INTEGRATED`, change
`kvm_run_userspace` to NOT call `um_tlb_sync(current->mm)` (already
suspect — see thread.c:2997-3012, which calls it directly, panicking on
failure). Change `kvm_enter_guest` to NOT call
`kvm_shadow_fill_from_uml_pgd` unless `shadow->needs_full_resync` is
set. Add an assertion that direct-sync coverage is complete (every
NEEDSYNC PTE we'd see has already had `kvm_shadow_sync_pte` called).
Boot, see what happens.

If parity moves: hypothesis confirmed; complete R1 and proceed to R2/R3.
If parity gets worse: the deferred chain is currently masking
direct-sync gaps; the gaps need to be found (which will be visible in
the assertion failures from the previous step).

If parity is unchanged: the bug is elsewhere — most likely R3 (host VA
collision corrupting kernel data structures). Move to R3 next.

Either way the experiment narrows the search. The current state — both
chains running, neither trusted, no monotonic "this is the source of
truth" invariant — admits no productive bisection.

---

## Honesty: What I Don't Know

- Whether `os_map_memory`'s removal actually breaks libc as the comment
  in mm.c:124-134 claims, or whether some other change in the same
  commit caused the crash and `os_map_memory` got blamed by association.
  The commit message (#276) and reproducer should be re-examined.
- Whether `kvm_bootstrap_va` actually ends up colliding with user vmas
  on real boots. I argued it's plausible from address arithmetic; I did
  not run the binary and confirm. The diagnostic to add is one
  `pr_info` at bootstrap init logging the VA + decomposition; one
  `pr_info` at the first user mmap logging its VA + decomposition;
  compare.
- Whether the direct-sync fast-path (atomic single-u64 PTE write) is
  actually atomic on all the architectures UML targets. I assumed
  x86_64 only; UML on aarch64 may need different barriers.
- Whether the ncpus=1 cooperative model genuinely prevents all
  multi-writer races, or whether signal-driven preemption inside
  set_ptes can interleave. The set_ptes inline runs without
  `block_signals_trace` wrapping it; signals could fire mid-loop.
- Whether the vCPU TLB is correctly flushed on every shadow change.
  The KVM_SET_SREGS-on-CR3-write contract is documented to invalidate
  PCID, but whether that fully flushes the vCPU's TLB caches is
  KVM-version-dependent. I trust the comment in lifecycle.c:1539-1541
  ("CR3 reload always falls through to kvm_invalidate_pcid") but didn't
  verify against current upstream KVM.
- Whether removing the entire deferred chain (R1) causes problems for
  the host-process-side path. Specifically: `flush_tlb_kernel_range`
  is documented to need immediate sync (tlbflush.h:71-77) and bypasses
  the shadow-sync hook (kernel mappings don't have a kvm_shadow). If
  any kernel-VA mapping is needed for KVM correctness — beyond
  bootstrap aliases — removing the deferred chain could break it.

The user's directive — "the design is brittle and doesn't provide
foundation for observability/reliability/safe implementation" — is
correct as observed. The current code can't even tell us with
confidence which of the five writers wrote the leaf at the time of
crash, because the mut_ring (the only mutation-tracking we have) only
captures direct-sync events; full-fill installs and `kvm_mm_map`-driven
invalidates aren't recorded. Adding observability to all five writers
is possible but treats the symptom; the cure is to have one writer.
