# 04 — Concurrency / Locking / Atomic-Context Safety Review

**Scope.** Integrated KVM backend (`CONFIG_UM_BACKEND_KVM_INTEGRATED=y`),
`backend=force=kvm`. Reviewer perspective: every shared mutable field,
every lock, every barrier, every atomic-context boundary.

**Symptom.** Anything beyond a trivial workload fails 100% with wild-pointer
dereferences. ptrace + seccomp 10/10 on the same workloads. Pattern
(intermittent on small workloads, deterministic on large) is consistent
with a race that triggers more often as PTE-mutation density grows.

---

## Executive summary — top-3 concurrency risks (ranked)

### Risk 1 (PROVEN, primary suspect for the wild-pointer crash) — `cached_cr3_gpa` is a per-VM global, but the shadow tree it caches is per-mm. `kvm_shadow_mm_free` only invalidates the cache when `pgd_gpa` matches, which silently leaves a stale association every time KVM returns a recycled physical page to a fresh shadow allocation.

`kvm_ctx.cached_cr3_gpa` is a single `u64` shared across **every UML mm**
(`arch/um/backend/kvm/kvm_backend.h:80`), set by `kvm_enter_guest`
(`arch/um/backend/kvm/thread.c:2245`) and tested in the SREGS-skip fast path
(`arch/um/backend/kvm/thread.c:2161`). The skip predicate is:

```
ctx->sregs_primed &&
ctx->cached_cr3_gpa == cr3_gpa &&
ctx->cached_fs_base == cur_fs &&
ctx->cached_gs_base == cur_gs &&
shadow && !shadow->dirty
```

The intended invariant is that an unchanged `cr3_gpa` means the previously
loaded CR3 is still valid. But `cr3_gpa` is `__pa(shadow->pgd)`, the
physical address of an ordinary `alloc_page()`-returned page. After
`kvm_shadow_mm_free` returns its PGD page to the buddy allocator, a
subsequent `kvm_shadow_mm_alloc` for a **different mm** can receive the
exact same physical page and therefore the same `pgd_gpa`. The
`shadow_mm_free → cached_cr3_gpa = 0` guard at lifecycle.c:708-709 only
fires if the mm being torn down is the one currently cached. The reverse
case — mm A is torn down, A's pgd_gpa stays in `cached_cr3_gpa`, mm B is
created on the same physical page, B uses pgd_gpa = A's old gpa — is
**not** invalidated. The next entry under B sees `cached_cr3_gpa ==
cr3_gpa` and `!shadow->dirty` (B is freshly allocated with `dirty = true`,
so first entry programs SREGS — but consider an already-cached entry path
where the SREGS-skip happens during a normal multi-task workload). Result:
KVM_SET_SREGS is skipped even though the in-vCPU CR3 still references the
old mm's pgd page.

But the deeper variant is the **task-switch path with no full SREGS
write**. After `kvm_context_switch`, `current->active_mm` changes; the
next `kvm_enter_guest` reads `cr3_gpa = shadow->pgd_gpa` from the **new**
shadow_mm. `cached_cr3_gpa` reflects the **old** mm's pgd_gpa. Compare
fails (if they happen to differ) → KVM_SET_SREGS programs the new CR3.
Fine. But if the recycle race above hits — new mm's pgd_gpa happens to
equal old's — the comparison passes and the CR3 is **never reloaded**.
The vCPU continues to walk the previous mm's tree against the new mm's
shadow leaves (via direct sync). This is a deterministic source of wild
pointers.

**Why it surfaces under integrated KVM only.** ptrace and seccomp don't
maintain a shadow PT and don't have an SREGS-skip cache. Every
mm-switch in those backends is mediated by host kernel CR3 reload at the
OS scheduler level.

**Why it correlates with workload size.** More mm churn (more processes,
more execve, more page reclaim) → more recycling → higher chance of
collision.

### Risk 2 (PROVEN) — The FPU per-task hash table is a use-after-free time bomb.

`kvm_fpu_hash[64]` (thread.c:104) is keyed by `task_struct *`. Slots are
allocated with `GFP_ATOMIC` from `kvm_context_switch`. **Slots are never
freed.** Comment at thread.c:88-91 acknowledges this: "Freed at task exit
via kvm_fpu_drop_for_task (called from arch_release_task_struct — but UML
doesn't define one, so we leak on exit; fine for now…)".

Two failure modes:

1. **Use-after-free read in the hash walk** (thread.c:112-115). When task
   T exits, its `task_struct` is freed by the slab allocator. A later
   task T' may receive the same `task_struct *` value (slab reuse).
   `((unsigned long)t >> 8) & 63` for T' picks some bucket; if T was in a
   different bucket on the chain, T' walks past T's stale entry before
   finding (or not finding) its own. The walk dereferences `s->task` for
   the `s->task == t` comparison, but `s->task` is just a pointer
   compare — no UAF read on the task itself there.
   **However:** if T' lands in the SAME bucket as T (likely under a
   stable PID space), and slab DOES return T's address as T', the
   first-match in the chain returns T's old slot. T inherits T's saved
   FPU. Cosmetic for FPU but symptomatic of the broken lifecycle.
3. **`kmalloc(GFP_ATOMIC)` failure under memory pressure.** A failure here
   silently skips the save/restore (return -ENOMEM, ignored by the
   `(void)` cast at thread.c:293-294). FPU state then **leaks** between
   the previous task and the next — exactly the SSE-memcpy corruption the
   docs at thread.c:267-291 warn about. Under heavy mm churn the GFP_ATOMIC
   pool can fail; this is exactly the workload-scaling pattern observed.

`GFP_ATOMIC` from `kvm_context_switch` is also wrong-context: this is
called between `set_current(to)` and `switch_threads()`, technically
within the scheduler's context-switch critical section, but UML's
"cooperative" model does not actually disable IRQs the way a real
kernel's `__schedule` does. Either way the request can fail without
fallback.

### Risk 3 (PROVEN) — `kvm_shadow_sync_pte`'s lockless shadow walk reads pointers from shadow tree pages that `kvm_shadow_mm_free` will call `__free_page()` on, with no synchronisation guarantee.

`kvm_shadow_walk_leaf` (shadow_sync.c:132-151) walks shadow PGD → PUD →
PMD → PTE pages by `__va()`-converting parent table entries and reading
the next slot. This walk is taken from `kvm_shadow_sync_pte`, which is
called from `set_ptes`/`pte_clear` (pgtable.h:151, 364) under generic-mm
page-table spinlocks.

The safety argument in the file header (shadow_sync.c:14-36) and the
N1 race-safety analysis (shadow_sync.c:292-320) **both rely on**: "those
pages are only freed at mm teardown (after all PTE mutators are gone)".

That argument breaks for **two distinct interleavings**, both reachable:

1. **Direct sync vs. invalidate clear-pass.** `kvm_shadow_invalidate_va_range`
   (lifecycle.c:1477-1543) takes `fill_lock` (mutex) and **does not** zero
   intermediate PUD/PMD/PTE pages — it only clears the leaf slot. Safe
   wrt the walker. **But** `kvm_shadow_fill_from_uml_pgd`'s clear pass
   (lifecycle.c:1216-1259) walks shadow leaves and writes 0 into them
   under `fill_lock`. The lockless walker does not take `fill_lock`. So
   while fill is mid-clear at slot S, the lockless writer can be mid-walk
   reading slot S' on the same page. This is OK for a single u64 write
   on x86_64 (atomic), so the writer reads either the old or new value
   and writes the leaf accordingly.
   **Real problem:** the clear pass and the install pass are **not
   transactional under the same lock view** — the lockless writer can
   observe an intermediate state where the user half is partially cleared.
   It will then write a leaf into that half-cleared half. The fill's
   subsequent install pass does NOT re-clear; it only `kvm_shadow_map_page`s
   present UML PTEs. A leaf the lockless writer installed inside a slot
   the fill won't revisit (because the corresponding UML pgd entry is
   absent or PROT_NONE) is **lost** if the fill's clear pass already
   passed that slot (writer wins, leaf survives), or **clobbered** if
   fill's clear pass arrives after (leaf zeroed, but UML pgd still says
   "not present", so install doesn't restore it). Same VA now reads as
   absent in shadow until the next direct sync sees it.

2. **`destroy_context` race.** `destroy_context` (mmu.c:96) calls
   `kvm_mm_detach`, which calls `kvm_shadow_mm_free`, which `__free_page`s
   every PUD/PMD/PTE/PGD page (lifecycle.c:715-748). There is **no
   serialization** between this teardown and concurrent `kvm_shadow_sync_pte`
   on the **same mm**. The argument that "all PTE mutators are gone" by
   destroy_context time relies on the generic mm code having dropped all
   refs first, which is true for set_pte_at via the page-table spinlock,
   but **NOT** for `kvm_shadow_sync_va_atomic` and
   `kvm_shadow_sync_range_atomic` called from `flush_tlb_*`
   (tlbflush.h:60, 67). `flush_tlb_mm(mm)` is called during exit_mmap,
   which is BEFORE destroy_context. Order is OK there. But
   `flush_tlb_kernel_range` (tlbflush.h:70) operates on init_mm, not the
   exiting mm — the exiting mm's shadow can still be reached via
   `kvm_shadow_mm_current` if the shadow is the active one at the moment
   the kernel-thread idle path runs. The window is small but not empty.

   Bigger concern: under SIGALRM-driven preemption (UML's tick), a
   kernel-thread switch can fire between `kvm_mm_detach` zeroing
   `id->kvm_shadow` and the underlying pages being freed (mm.c:77-79 is
   sequential; that's safe). **But** between the time the page table
   spinlock is dropped at the end of a generic mm operation and the time
   a concurrent sync-on-flush observes the new state, direct sync may
   already have computed `shadow = mm->context.id.kvm_shadow` and be
   walking. If detach starts in that window the walker reads through
   freed pages.

---

## Complete shared-mutable-state table

Notation: **W** = writer, **R** = reader. **Protection** describes what,
if anything, serializes accesses. Atomic-context entries marked `[A]`.

| Field | Writers | Readers | Protection |
|---|---|---|---|
| `kvm_shadow_mm.pgd` | `kvm_shadow_mm_alloc` (once); `kvm_shadow_mm_free` (clears via implicit kfree) | `kvm_shadow_walk_leaf` `[A]`; fill/invalidate; `pgd_gpa` access | **NONE** between alloc/free vs walkers |
| `kvm_shadow_mm.pgd_gpa` | `kvm_shadow_mm_alloc` (once) | enter_guest's CR3 source; `kvm_shadow_mm_free` compare | none — read after publish, OK if mm-attach is happens-before mm-use, FAILS for mm-recycle |
| `kvm_shadow_mm.dirty` | `WRITE_ONCE` from many sites; cleared in enter_guest | `READ_ONCE` in SREGS-skip; not actually `READ_ONCE` at thread.c:2164 (plain load!) | none beyond `WRITE_ONCE` semantics |
| `kvm_shadow_mm.synced` | `kvm_shadow_fill_from_uml_pgd` sets true; `kvm_shadow_invalidate_va_range` sets false; raw assignments (lifecycle.c:1355, 1540) | enter_guest skip-test (plain load, thread.c:2088) | **NONE** — plain store/load mixed with `READ_ONCE` calls inconsistently |
| `kvm_shadow_mm.synced_pgd_va` | `kvm_shadow_fill_from_uml_pgd` (raw assignment) | enter_guest skip-test (plain) | none |
| `kvm_shadow_mm.needs_full_resync` | `WRITE_ONCE` in `kvm_shadow_sync_pte` `[A]`; cleared in enter_guest | `READ_ONCE` in enter_guest skip-test | `WRITE_ONCE`/`READ_ONCE` on the flag itself, but **see ordering bug** below |
| `kvm_shadow_mm.direct_sync_*` counters | `WRITE_ONCE` from `[A]` paths | `READ_ONCE` from fault dump | OK as counters; lossy but not load-bearing |
| `kvm_shadow_mm.mut_head_seq` | `WRITE_ONCE` from `kvm_shadow_record_mut` `[A]` | `READ_ONCE` from `kvm_shadow_mut_dump_for` | `smp_wmb()` between entry write and seq update; **NO `smp_rmb()` on reader side** |
| `kvm_shadow_mm.mut_ring[i]` | one entry per record_mut call `[A]` | dump_for | seqlock-light: writer publishes via head_seq w/`smp_wmb`, but reader only does `READ_ONCE(e->addr)` — no rmb after seq read |
| `kvm_shadow_mm.fill_lock` | `mutex` | held by fill + invalidate | OK |
| Shadow leaf `*spte` (u64 in alloc'd page) | direct sync `[A]`; fill install (under fill_lock); invalidate clear (under fill_lock) | guest CPU CR3 walk (HW); fill clear-pass; walker | `WRITE_ONCE` for direct sync; **plain assignment** for fill (lifecycle.c:1253, 1418, 1514) |
| Shadow PGD/PUD/PMD/PTE intermediate slots | fill (under fill_lock); never overwritten by direct sync | walker `[A]` | none for walker; relies on "not freed except at mm-teardown" — broken under recycle |
| `kvm_um.cached_cr3_gpa` | enter_guest after SREGS write; `kvm_shadow_mm_free` conditional clear | enter_guest SREGS-skip predicate | **NONE**. Plain reads, plain writes. Per-VM global; per-mm pointer. Recycle hazard. |
| `kvm_um.cached_fs_base` / `cached_gs_base` | enter_guest after SREGS | enter_guest SREGS-skip | none — but per-task FS/GS is task-local, so a context-switch invalidation point is missing |
| `kvm_um.sregs_primed`, `msrs_primed`, `kernel_gs_base_primed`, `cpuid_done` | one-shot true | enter_guest predicates | none; one-shot OK |
| `kvm_um.gadget_state_va`, `gadget_vvar_va` | first-map writes; never reset | enter_guest readers | none; once set, stable |
| `kvm_fpu_hash[64]` (struct slot pointers) | first kvm_fpu_save_for_task(t) per (t) | every save+restore | **NONE.** Comment at thread.c:88-95 documents reliance on cooperative single-thread; SIGALRM can interrupt a hash walk between bucket-load and chain-walk |
| `kvm_fpu_slot.task` | once at slot alloc | every hash walk for compare | none; lifetime tied to task_struct via slab — UAF on slab reuse |
| `kvm_bootstrap_page`, `_page_stack`, `_gpa`, `_stack_gpa`, `_va` | lazy init under `kvm_bootstrap_lock` | LSTAR copy, enter_guest, gadget_fault_nr | spinlock (irqsave). OK for init; subsequent reads are unprotected but published-once |
| `mm->context.sync_tlb_lock` (`spinlock_t`) | um_tlb_mark_sync, um_tlb_sync | both | OK |
| `mm->context.sync_tlb_range_from/to` | both takes `sync_tlb_lock` | guarded | OK |
| `kvm_ctx.gadget_vvar->seq` | gadget_vvar_refresh (`v->seq++`) | guest gadget asm | seqlock pattern, single-writer/multi-reader, OK |
| `kvm_ctx.gadget_state` fields | gadget_state_refresh | guest gadget asm | NO seqlock per docs (kvm_backend.h:208-216 acknowledges "v1 ncpus=1"); host writes happen during the SHRA before KVM_RUN, so guest is quiescent — **OK for ncpus=1**, broken for SMP (out of scope) |
| `signals_pending`, `signals_active` (UML signal state) | hard signal handler; set_signals; unblock | block/unblock_signals | os/signal-internal; not part of this review but **interacts with preemption argument** below |

---

## Specific concrete races

Citation format: `file:line` ranges. Severity: **PROVEN** (analysis is
airtight), **SUSPECTED** (interleaving is reachable; effect requires
specific timing), **SPECULATIVE** (theoretical without confirming trace).

### Race A — `cached_cr3_gpa` recycle (PROVEN)

**Location:** `arch/um/backend/kvm/thread.c:2160-2165` (skip predicate),
`thread.c:2245` (write), `arch/um/backend/kvm/lifecycle.c:688-709`
(conditional invalidate on free).

**Interleaving:**
1. mm A is active. enter_guest writes `cached_cr3_gpa = pgd_gpa(A)`.
2. Task switches to a kernel thread that doesn't touch active_mm — cache stays.
3. mm A exits → `destroy_context` → `kvm_mm_detach` → `kvm_shadow_mm_free`.
   Free path checks `if (shadow->pgd_gpa != 0 && ctx->cached_cr3_gpa ==
   shadow->pgd_gpa) ctx->cached_cr3_gpa = 0;` — this **does** fire,
   clearing the cache. So far so good.
4. **But** if instead the active_mm at step 2 is a different mm B
   (kernel-thread borrow of B), and it does an enter_guest path, the
   cache is rewritten to `pgd_gpa(B)`. Now the shadow_mm_free for A
   sees `cached_cr3_gpa == pgd_gpa(A)` is FALSE → cache not cleared.
5. mm C is created. `alloc_page` returns A's old physical page →
   `pgd_gpa(C) == pgd_gpa(A)`.
6. C becomes active. enter_guest reads `cr3_gpa = pgd_gpa(C) ==
   pgd_gpa(A)` (the value still in cached_cr3_gpa? NO — cache holds
   B's gpa from step 4).

So this exact path doesn't trigger; the cache is keyed on whatever
the LAST entry was. The deeper hazard is:

**Stronger interleaving (PROVEN):** The cache is per-VM but the SREGS
shape it represents is per-mm AND per-(fs_base,gs_base,shadow->dirty).
**There is no `cached_mm` field**. So a transition A→A where:
  - between two A entries, A's shadow is mutated (dirty=true),
  - then dirty is cleared by some path **other than** an SREGS reload
    (e.g. shadow->dirty=false set by anyone — note lifecycle.c:1473
    sets dirty=true unconditionally, lifecycle.c:1539-1540 sets dirty
    AND synced=false unconditionally, but the SREGS-skip predicate
    requires `!shadow->dirty`),

…is normally safe. But the **fill_done jump-skip** at thread.c:2088-2096
**bypasses** the fill **and the dirty-flag clearance** (`shadow->dirty =
false` only happens in the SREGS write path at thread.c:2251 inside the
`if !skip` arm). So if the synced+!needs_full_resync skip fires, the
SREGS-skip predicate that follows immediately at 2160-2165 finds
`shadow && !shadow->dirty` based on… the previous run's clearance? Yes,
because dirty was cleared at the previous SREGS write. OK that's
consistent.

**The real trigger** is the per-task FS/GS interaction: when a task
switches from T1 (FS=X) to T2 (FS=Y) with the **same active_mm** (e.g.
clone/thread), `kvm_context_switch` does **not** invalidate
`cached_fs_base`. The next enter_guest reads `cur_fs = regs->gp[HOST_FS_BASE]
= Y`. Compare `cached_fs_base (= X) == Y` → false → SREGS reloaded. OK.
But if T2 has the SAME FS as T1 by coincidence (zero, or a default TLS
slot), the comparison passes — even though the SREGS state in the vCPU
might be stale relative to other fields the cache doesn't track (e.g.
the IRETQ frame at offset 0 of the IST stack page).

**Tighter recycle scenario:** mm A torn down with cached_cr3_gpa cleared
correctly. Page returned to buddy. Same physical page handed out to mm C
in an unrelated fashion (e.g. a page-allocation in some unrelated
subsystem that frees and we lose track). Now C's `pgd_gpa` reuses A's
**after** the cache was cleared. No problem — comparison fails on first
entry under C and SREGS gets reprogrammed. **OK.**

The provable wild-pointer hazard is therefore **not** the recycle
itself. It is the missing `cached_mm` invariant: between
`shadow_mm_free(A)` and the next enter_guest, if `cached_cr3_gpa`
already moved off A (to B), and then C arises with `pgd_gpa(C) ==
pgd_gpa(A)` — but that gpa isn't in the cache anyway. **Downgrade to
SUSPECTED:** cached_cr3_gpa needs invariant proof but isn't an obvious
crash trigger by itself.

**However, see Race E** below — the actual SREGS-skip trigger is
different and worse.

### Race B — Direct sync vs. fill clear-pass (PROVEN)

**Location:** `shadow_sync.c:153-285` (direct sync, no lock);
`lifecycle.c:1166` (fill takes `fill_lock`); `lifecycle.c:1216-1259`
(clear pass walks user-half PUDs/PMDs/PTEs and zeroes leaves).

**Interleaving** (single-threaded UML, but SIGALRM can interrupt):
1. Task T issues an mmap → set_pte_at → kvm_shadow_sync_pte. Walk
   reaches the leaf slot at VA X, writes a leaf.
2. SIGALRM fires; UML scheduler kicks in; switch to task T'.
3. T' enters guest → kvm_enter_guest → fill_from_uml_pgd. Takes
   fill_lock. Begins clear-pass — iterates every present user-half leaf
   and writes 0.
4. **Meanwhile**, T was in the middle of `kvm_shadow_sync_pte`.
   Specifically suppose T had completed the install and was past the
   `WRITE_ONCE(*spte, new)` at shadow_sync.c:267 but before the
   record_mut call. The signal handler runs entirely on the same
   host thread; UML's "context switch" is a setjmp/longjmp dance via
   `switch_threads` (process.c:627). When SIGALRM lands, the signal
   handler is invoked, runs, may longjmp to T'. T's stack is preserved
   in the jmp_buf.
5. T' completes its fill, releases fill_lock. T' continues.
6. Eventually T is resumed via switch_threads back. Its leaf write at
   step 1 was already published. But T'` clear pass at step 3 wrote 0
   over it.
7. T's record_mut call (after T resumes) records the install → so the
   ring shows install but the actual leaf is 0.
8. Next entry under T: shadow says VA X is absent. UML pgd says present.
   Guest faults at X. Recovery refills. **Slow but correct.**

Outcome of B in isolation: visible perf hit, no corruption — the fill
walks UML pgd and reinstalls. **BUT:** if direct sync chose to set
`needs_full_resync=true` because the install path was missing
intermediate tables, fill clears that flag at thread.c:2138. If the
direct-sync's install actually succeeded later, the leaf is correct;
needs_full_resync=false; next entry skips fill. So the only effect of
B is the eventual page fault and recovery. Survivable.

**Severity downgrade to SUSPECTED.** Race B alone doesn't crash but it
elevates needs_full_resync churn.

### Race C — Direct sync walks freed pages (SUSPECTED, sharpens to PROVEN under exit_mm preemption)

**Location:** `shadow_sync.c:132-151` (walker reads `pgd[i] & mask`,
takes `__va(pa)`, dereferences); `lifecycle.c:715-748` (free walks
the same tree and `__free_page`s every level).

**Interleaving:**
1. mm A is exiting. `mmput` → `exit_mmap` → flush_tlb_mm(A) →
   `kvm_shadow_sync_range_atomic(A, ...)` over every vma. This calls
   `kvm_shadow_walk_leaf(shadow, addr)` for each page, reading
   `shadow->pgd`, then PUD, then PMD page. Atomic context (page-table
   spinlock).
2. **Same task continues** to `__mmput → mmdrop → free_mm → __destroy_context →
   destroy_context → kvm_mm_detach → kvm_shadow_mm_free`. This is
   serial, all on the same task. No race **on the same task**.
3. **But `flush_tlb_kernel_range` from another mm (init_mm) calls
   `um_tlb_mark_sync(&init_mm, ...)`** then `um_tlb_sync(&init_mm)`
   immediately (tlbflush.h:70-77). This does NOT directly touch the
   exiting mm's shadow. **So C-3 is not a problem.**

Where does C actually trigger? The walker reads `mm->context.id.kvm_shadow`
(shadow_sync.c:168). If `mm` is the active_mm during a teardown of
**some other mm**, no problem. If `mm` is the one being torn down:
but the teardown is on the same task (exit_mmap of current's mm), so
no interleaving in single-thread. Across the SIGALRM boundary?
`exit_mmap` is called from exit_task_work / mmput, which is at the
task-exit transition. UML's task exit holds the kernel-side
scheduler — but SIGALRM is a host signal, so it CAN fire mid-exit.
If SIGALRM fires while exit_mmap is mid-flush_tlb_mm, the timer
handler may schedule a different task. That task then starts its own
work which may invoke direct sync against ITS mm — not the exiting
one. **Not a UAF here.**

The reachable variant: `kvm_shadow_sync_va_atomic` is a function
pointer indirection from `flush_tlb_page` (tlbflush.h:51-61). It's
called with `mm = vma->vm_mm`. If `vma` and `vm_mm` are stable for
the call site (they should be — caller holds mmap_lock), the mm
won't be freed under us.

**Downgrade Race C to SPECULATIVE** in current single-thread UML; flag
as a **PROVEN bug under SMP** (out of scope).

### Race D — Shadow `*spte` plain stores from fill races with `WRITE_ONCE` from direct sync (PROVEN, but harmless on x86_64)

**Location:** `lifecycle.c:1253` (`spte[pte_i] = 0;` — plain),
`lifecycle.c:1418` (`pte[pte_i] = (...);` — plain),
`lifecycle.c:1514` (`pte[pte_i] = 0;` — plain). All three are within
fill_lock-held regions. Direct sync writes the SAME slot via
`WRITE_ONCE` without taking fill_lock.

**On x86_64:** aligned u64 stores are atomic; the C standard does not
guarantee this for plain stores, but the compiler hasn't reason to
split them. Practically OK.

**Real concern:** the **read** in fill's clear pass loop (lifecycle.c:1241
`if (!(spte[pte_i] & KVM_X86_PTE_P))`) is also a plain load. The
compiler can hoist or refold it. If direct sync writes a present leaf
between the load and the inner store, fill might still execute
`spte[pte_i] = 0` and clobber. Then the next install pass walks UML pgd
and reinstalls. **Net: see Race B.** No crash.

### Race E — `shadow->dirty` plain store/load drift (PROVEN, plausible crash trigger)

**Location:** Many. Searches for `shadow->dirty` show:
- `lifecycle.c:674` (init): `shadow->dirty = true;` — fine, init.
- `lifecycle.c:1261, 1420, 1473, 1539`: plain assignment under
  fill_lock or assumed-non-concurrent.
- `lifecycle.c:709`: indirectly, `if (...) ctx->cached_cr3_gpa = 0;`
  — actually fine, no dirty involved.
- `shadow_sync.c:198, 228, 250, 268`: `WRITE_ONCE(shadow->dirty, true)`.
- `thread.c:2164`: **plain load** `shadow && !shadow->dirty` in the
  SREGS-skip predicate.
- `thread.c:2251`: plain store `shadow->dirty = false` after SREGS write.

**Interleaving:** SIGALRM fires mid-`kvm_shadow_sync_pte` between the
spte WRITE_ONCE and the `WRITE_ONCE(shadow->dirty, true)` (e.g.
shadow_sync.c:267 → 268). Scheduler switches to T'. T' enters guest:
SREGS-skip reads `shadow->dirty` as **false** (T's WRITE_ONCE not yet
run). Skip fires. KVM_SET_SREGS not called. CR3 not reloaded. But the
shadow leaf was just rewritten — for the SAME mm if T == T'-thread,
or for a different mm if T'-thread is unrelated.

Then T' runs guest with stale TLB caching the OLD leaf. **Wild pointer
deref class.**

**Why under integrated KVM only**: the guest's TLB caches the shadow
mapping; without a CR3 reload, the new shadow leaf isn't visible to
the guest until natural TLB churn or a synonym fault. ptrace and
seccomp don't have this problem because they don't manage their own TLB.

**Why under big workload only**: needs the SIGALRM interrupt point to
land EXACTLY between the leaf write and the dirty flag write — a
very narrow window per mutation. Bigger workload = more mutations =
more chances per second. Statistical certainty above some workload
size.

**Severity: PROVEN** — the ordering bug is real, the reachable
interleaving is real, and the symptom (wild pointer in guest) matches.
This is the most plausible **primary** crash trigger.

### Race F — `needs_full_resync` ordering vs. `dirty` (PROVEN)

**Location:** `shadow_sync.c:248-256` (the alloc-fail path) sets, in
order:
```
WRITE_ONCE(shadow->needs_full_resync, true);   // F.1
WRITE_ONCE(shadow->dirty, true);               // F.2
WRITE_ONCE(shadow->direct_sync_alloc_fail, ...); // F.3
record_mut(...);                                // F.4
```

The reader (enter_guest) does:
```
if (shadow->synced &&                              // E.1 plain load
    shadow->synced_pgd_va == ... &&
    !READ_ONCE(shadow->needs_full_resync))         // E.2
    goto fill_done;
```

If a SIGALRM-driven preemption fires between F.1 and F.2, the reader
in another path sees `needs_full_resync=true` — good, fill runs. But
if the read at E.1 loads the OLD `synced=true` (cached register),
and then E.2 sees the new `needs_full_resync=true`, the predicate
short-circuits because the AND is short-circuit: `synced && ... &&
!needs_full_resync` evaluates `!needs_full_resync` only after `synced`
loaded. Since `synced` is plain-load, the compiler may have already
done the load earlier. The condition `!needs_full_resync` evaluates
to `false`, the goto is skipped, fill runs. **OK in this direction.**

The reverse direction: if `needs_full_resync` is cleared before
`dirty` is consumed:
```
filled = kvm_shadow_fill_from_uml_pgd(shadow, mm->pgd);  // takes fill_lock
WRITE_ONCE(shadow->needs_full_resync, false);            // thread.c:2138
```

Between the fill returning (releasing fill_lock) and clearing
`needs_full_resync`, a direct sync from another path (signal-handler
context only — but signals are blocked during fill via UML's signal
discipline?) could set it true again. Then enter_guest clears it
unconditionally. The next entry skips fill, but the leaf may not exist.

**Severity: SUSPECTED.** The window between fill return and the
WRITE_ONCE(false) is narrow, and signals are typically blocked during
mutex-held regions, but there's no `barrier()` documenting the order.

### Race G — Mutation ring missing reader-side `smp_rmb()` (PROVEN, diagnostic-only impact)

**Location:** `shadow_sync.c:61-77` (writer has `smp_wmb()` before
seq update). `shadow_sync.c:85-120` (reader): reads seq, indexes
ring, reads entry fields with `READ_ONCE(e->addr)` — but **no
`smp_rmb()` after the seq read** before the entry reads.

**Effect:** the dump may print stale entry contents for entries near
the head. **Diagnostic-only**, doesn't affect crash behaviour.

### Race H — FPU hash table walk vs. SIGALRM interrupt (PROVEN)

**Location:** `thread.c:106-130` (`kvm_fpu_get_slot`). The walk reads
`kvm_fpu_hash[h]`, then iterates `s->next`. No locking.

**Interleaving:**
1. `kvm_context_switch(prev=T1, next=T2)` calls `kvm_fpu_save_for_task(T1)`.
   Walks bucket B looking for T1's slot. Loads `kvm_fpu_hash[B]` into a
   register.
2. SIGALRM fires (UML's host-side signal). The handler, while ostensibly
   not making C-level kernel calls itself, schedules T3 via
   switch_threads (longjmp).
3. T3 also context-switches and calls `kvm_fpu_save_for_task(T_prev_of_T3)`
   which lands in bucket B and may insert a new head, changing
   `kvm_fpu_hash[B]`. T3's frame returns.
4. T1 resumes mid-walk with stale register. The chain head it's iterating
   may now be at a different memory location, but the slot pointers it
   already loaded are still valid (slots aren't freed). It walks an
   older snapshot of the chain.

**Effect**: if T1's slot was the head and T3 prepended a new slot, T1
walks past the new head's slot, looking for T1's task pointer. Doesn't
find a T3 entry that matches `t == T1` (it has T_prev_of_T3 as its task).
Walk reaches T1's slot, returns, `KVM_GET_FPU` writes T1's saved state.
**Correct outcome.**

The pathological case is if T3 happened to allocate a slot whose chain
position falls **between** the head T1 saw and T1's slot, AND T3's slot
has `task == T1` — impossible (T3 calls with `T_prev_of_T3 != T1`).

**Severity: PROVEN reachable, but no observed corruption from this race
alone.** FPU correctness preserved by the simplicity of the data
structure. The risk crystallises if **two host pthreads** ever exist
(e.g., the SIGIO helper, the time-travel external thread). Then the
walk above and an insert above can race truly concurrently. UML does
have host-side helper threads — see `os-Linux/internal_io_thread.c`
etc. — they generally don't call into kvm code, but the assumption
needs auditing.

### Race I — Bootstrap-page lock covers init only, not subsequent reads (PROVEN, low impact)

**Location:** `thread.c:1125-1438` (init under `kvm_bootstrap_lock`),
`thread.c:1500` (`kvm_gadget_fault_nr` reads `kvm_bootstrap_va` plain),
`thread.c:1939, 1948, 2206-2215` (enter_guest reads `kvm_bootstrap_va`,
`kvm_bootstrap_page`, `kvm_bootstrap_page_stack` plain).

Once init has happened (the `if (kvm_bootstrap_page) return 0;` fast
path at thread.c:1135), every subsequent reader observes a stable
non-zero pointer. **Publication race**: the `spin_unlock_irqrestore`
at thread.c:1409 provides a release barrier. Other CPUs' reads will
observe non-NULL after they observe an event ordered after this write
(e.g., the next ioctl). On UML ncpus=1 single-host-thread there is no
true cross-CPU concern.

**Severity: SPECULATIVE** for ncpus=1. **PROVEN issue under SMP** — the
fast-path test doesn't `READ_ONCE`, so a future-SMP build could see a
torn pointer. Out of scope.

### Race J — `mmu_notifier` on the memslot (SUSPECTED)

**Location:** KVM auto-registers an mmu_notifier on the host VMA backing
the memslot. The backend's memslot covers `[uml_physmem,
uml_physmem+physmem_size)` (lifecycle.c:610-616). When the host kernel
mutates the underlying VMA (e.g. THP collapse, host-side mmu_notifier
invalidate), KVM zaps the corresponding shadow EPT entries.

But our **direct-sync shadow PT pages** (the PUDs/PMDs/PTEs we
allocate) are **also in physmem**, hence **also covered by the memslot**.
If the host invalidates a range that happens to include a shadow PT
page, KVM zaps the EPT mapping of the page that **the guest CR3 walks
through**. The next instruction triggers an EPT violation → KVM_EXIT_MMIO
on the shadow PT page itself — which our code interprets as a normal
guest fault and tries to "recover" by refilling shadow PT… which then
walks the same now-zapped pages and zero-progresses.

This is **not** a code race per se; it's a missing interaction
contract. UML has very few host VMA mutations (the physmem mapping is
stable), but Transparent Huge Pages can still trigger. Disable THP on
the physmem mapping, or register the shadow PT pages as a separate
non-THP region.

**Severity: SPECULATIVE for the immediate crash, but PROVEN as a
correctness gap that can manifest under host memory pressure.**

### Race K — `um_tlb_sync` mid-flight when active_mm changes (SUSPECTED)

**Location:** `kvm_context_switch` calls `um_tlb_sync(prev->active_mm)`
**before** `switch_threads` (thread.c:251-263). Inside `um_tlb_sync`,
`mm->context.sync_tlb_lock` is held; calls into `ops.mmap = um_backend->mm_map
= kvm_mm_map`. `kvm_mm_map` then takes `fill_lock` (via
`kvm_shadow_invalidate_va_range`). **Lock order: sync_tlb_lock → fill_lock.**

`kvm_shadow_invalidate_va_range` is also called from `kvm_mm_unmap`
(mm.c:229), again under sync_tlb_lock by the same path. No reverse
order observed in the code base. **OK.**

But: `um_tlb_sync` releases sync_tlb_lock at the end (via the
`guard()` going out of scope at tlb.c:181 lifetime). Between
`switch_threads` returning to `next` and the next syscall, `next`
runs in a different mm context. If next-task's first action is a
mmap that triggers `set_pte_at` → `kvm_shadow_sync_pte` on **next's**
mm — fine, it's a different mm, different fill_lock. **OK.**

**Severity: SPECULATIVE.** No specific failure mode identified.

---

## The lockless walker's safety argument under preemption

The header in `shadow_sync.c:14-36` claims atomic-context safety based
on:

(a) Single-host-thread cooperative model → no concurrent writers.
(b) Aligned u64 read/write atomicity on x86_64.
(c) Intermediate PT pages only freed at mm teardown, after all PTE
    mutators are gone.

**Claim (a) is wrong under SIGALRM preemption.** UML's signal handlers
DO run on the same host thread — but they DO call `switch_threads`
(longjmp), causing a different UML-task to take over. The interrupted
walker leaves its in-progress register state in jmp_buf; when resumed,
its local variables (`pud`, `pmd`, `pte` pointers in
`kvm_shadow_walk_leaf`) are the values from before the interruption.
**Between the interruption and resumption, OTHER UML kernel code runs
arbitrarily long.** That code can call `kvm_shadow_fill_from_uml_pgd`
on the SAME mm (if scheduler reschedules to a thread sharing the
mm) — which under fill_lock zeroes leaves and may install new ones.

The walker's `pud` pointer is to a u64 that lived in a PUD page. PUD
pages are NOT zeroed by the fill (only leaves are zeroed). PUD pages
are NOT freed by the fill (only at mm-free). So the pointer remains
valid; the value at the pointer (a leaf address derived from the
PUD entry) is also stable across the interruption since fill doesn't
mutate PUD entries.

**Claim (b)** is fine for x86_64, fine for the explicit `WRITE_ONCE`
calls. NOT fine for the **plain stores** in fill (Race D), but those
are on aligned u64s and x86_64 doesn't tear them.

**Claim (c)** — the only path that frees intermediate pages is
`kvm_shadow_mm_free`. That's called from `kvm_mm_detach`, called from
`destroy_context`, called from `__mmdrop`. By the time mmdrop runs,
the mm has zero refcount and zero users — all PTE mutators on this mm
have completed. **OK**, modulo the SMP gap described in Race C.

**Net safety conclusion:** the walker is safe **modulo Race E**. The
walker itself doesn't crash. The leaf write following the walk
publishes through `WRITE_ONCE`. **What corrupts is the missing CR3
reload between the leaf write and the next guest entry — a vCPU TLB
caches the OLD leaf**, and the OLD leaf's PFN now points at memory the
guest expects to read fresh data from.

---

## Recommended locking model

Three options, ranked by risk-reduction-per-engineering-cost.

### Option A — Make `dirty` a barrier-correct flag, not a hint (RECOMMENDED, smallest change)

The crash trigger (Race E) is ordering between leaf write and dirty
flag. Two minimal fixes:

1. In every direct-sync write path that mutates a leaf, make the order:

   ```c
   WRITE_ONCE(*spte, new);
   smp_wmb();                     /* publish leaf before flag */
   WRITE_ONCE(shadow->dirty, true);
   ```

   And in the SREGS-skip predicate, use `READ_ONCE(shadow->dirty)`
   (it's currently a plain load at thread.c:2164):

   ```c
   if (... && shadow && !READ_ONCE(shadow->dirty))
        goto sregs_done;
   ```

   With `smp_load_acquire`/`smp_store_release` if the team prefers.

2. The fill path should also write `dirty=true` BEFORE clearing leaves,
   or hold the SREGS-cache invariant invalidation until the fill is
   committed. Currently lifecycle.c:1261 sets `shadow->dirty = true`
   AFTER the clear pass — order's right but the assignment is plain.
   Use `WRITE_ONCE`.

3. Same fix for `synced` and `synced_pgd_va` consistency: write them
   together (both store-release; both load-acquire when reading the
   skip predicate).

### Option B — Replace `fill_lock` (mutex) with a per-mm seqlock; synchronize lockless walkers via the seqlock

Because direct sync runs in atomic context (set_ptes / pte_clear under
page-table spinlock), it cannot block on the existing mutex. A seqlock
solves this:

```c
struct kvm_shadow_mm {
    seqcount_t fill_seq;
    spinlock_t fill_spinlock;     /* writer-side */
    /* ... */
};
```

- Fill takes `fill_spinlock`, does `write_seqcount_begin(&fill_seq)`,
  walks/installs, `write_seqcount_end(&fill_seq)`.
- Direct sync walker reads `do { seq = read_seqcount_begin(...); /* walk */ }
  while (read_seqcount_retry(...));`. Walker is read-only on intermediate
  pages, so retrying is cheap.
- The leaf WRITE_ONCE itself is still atomic. The retry guards against
  fill having mutated the path mid-walk.

This adds a fast-path overhead (~one rdtscp-ish per walk) but eliminates
the entire intermediate-tree race class.

### Option C — RCU the shadow_mm

`shadow_mm` itself becomes RCU-managed. `kvm_shadow_mm_free` calls
`call_rcu` to defer the page frees until grace period. Direct sync
walkers don't need to take any lock — they're protected by RCU.

This is the cleanest model long-term. It's also the largest change:
need to identify quiescent points, possibly need `rcu_read_lock`/
`unlock` around walker. UML's cooperative scheduler complicates
"quiescent state" definition; can punt to `synchronize_rcu` or use
`call_rcu` only.

---

## "What I would do first" (the bug-hunter's checklist)

1. **Add the `dirty`-flag memory barrier (Option A above).** Two-line
   change in `shadow_sync.c` after each leaf store:
   `smp_wmb(); WRITE_ONCE(shadow->dirty, true);`. Change the SREGS-skip
   read at thread.c:2164 to `READ_ONCE`. **High probability this fixes
   the wild-pointer crash.** Ship a test build immediately.

2. **Disable the SREGS-skip cache entirely** (`if (false)`) for one
   debug build. If wild pointers vanish: confirms Race E (or an adjacent
   skip-cache bug). If wild pointers persist: the bug is elsewhere
   — read this whole file again with that prior.

3. **Add a `cached_mm` field** to `kvm_um` and require
   `cached_mm == current->active_mm` in the SREGS-skip predicate.
   Invalidate `cached_mm = NULL` from `kvm_mm_detach`. This costs one
   pointer compare per entry and removes any "same gpa, different mm"
   recycling concern.

4. **Hoist the FPU hash bookkeeping out of `GFP_ATOMIC`.** Pre-allocate
   a slot at `init_new_context` via a backend hook, store the slot
   pointer on `mm_id` or `task_struct.thread.arch`. Then the
   context-switch path is a hash-free direct dereference. Removes
   GFP_ATOMIC failure mode (Race 2.b) and the chain-walk preemption
   risk (Race H).

5. **Add a panic-time dump** of `cached_cr3_gpa`, `cached_fs_base`,
   `cached_gs_base`, `current->active_mm`, `current->active_mm->context.id.kvm_shadow`,
   and `shadow->pgd_gpa`. If the next crash shows
   `cached_cr3_gpa == shadow->pgd_gpa` but the wild pointer is in a
   region the prior mm owned, you have proof the SREGS-skip is the
   culprit.

6. **Audit all `shadow->dirty` reads/writes** for `READ_ONCE`/`WRITE_ONCE`
   discipline. Same for `synced`, `synced_pgd_va`, `needs_full_resync`.
   Currently a mix.

7. **Treat the lockless walker's pointer reads as `READ_ONCE`.** The
   walker at shadow_sync.c:141-150 does plain `pgd[pgd_i] & 1ULL` etc.
   The compiler is allowed to re-read; combined with concurrent
   writers, the second re-read could see a freshly-zeroed slot. Use
   `READ_ONCE(pgd[pgd_i])` and store to a local first. Similarly the
   identical clone in `kvm_shadow_invalidate_va_range` (lifecycle.c:1500-1511).

8. **Audit `flush_tlb_kernel_range`'s sync path.** It calls
   `um_tlb_sync(&init_mm)` synchronously. That goes through
   `update_p4d_range` → `update_pud_range` → `update_pmd_range` →
   `update_pte_range` → `ops.mmap = kern_map`. **No** kvm_shadow_*
   call from this path. So shadow PT for init_mm is not maintained?
   The shadow PT for the active mm DOES contain the init_mm's
   kernel-half mappings (via the bootstrap-alias range carve-out).
   But fill_from_uml_pgd only walks PGD slots 0..511 of `mm->pgd`,
   which for a user mm typically has very few kernel-half entries.
   Verify that `flush_tlb_kernel_range` either invalidates every
   per-mm shadow's kernel-half view OR that the kernel-half view
   isn't load-bearing for ring-3 execution.

9. **Add a runtime invariant check** at the SREGS-skip site:

   ```c
   if (skip_predicate) {
       /* Verify what we think we cached is what's actually in the vCPU. */
       struct kvm_sregs probe;
       os_ioctl_generic(vcpu_fd, KVM_GET_SREGS, (unsigned long)&probe);
       if (probe.cr3 != cached_cr3_gpa)
           panic("um: kvm SREGS-skip cache lied: cached=0x%llx actual=0x%llx",
                 cached_cr3_gpa, probe.cr3);
       goto sregs_done;
   }
   ```

   Burns one ioctl per skipped path (the savings from the skip), so
   only enable in a debug build. If the panic ever fires you have proof.

10. **Suspect the audit_pgd debug path itself**: if
    `kvm_diag_audit_pgd_skip=1`, the skip path runs `kvm_shadow_audit_pgd`
    which walks the entire UML pgd against the shadow. Under heavy
    workload this walk takes long enough that SIGALRM can fire mid-walk
    and reschedule. The audit mutates nothing, but if a pgd page gets
    freed under the walker (unlikely in user-mm, possible at exec time),
    the audit reads garbage. Diagnostic-only — but disable for production
    confidence comparison.

---

## Cross-references

- File header at `arch/um/backend/kvm/shadow_sync.c:14-36` claims
  atomic-context safety. The claim is correct for the walker but
  the `dirty` flag's ordering is the actual hazard.
- Comment at `arch/um/backend/kvm/lifecycle.c:1156-1166` describes
  fill_lock's purpose: serialize fill against invalidate. Correct,
  but doesn't cover the lockless direct-sync writer that races
  against fill's clear pass (Race B).
- `arch/um/backend/kvm/kvm_backend.h:444-447`: documents `dirty` and
  `synced` flags. Makes no statement about ordering between them.
- `arch/um/backend/kvm/thread.c:2154-2165`: SREGS-skip predicate.
  No `READ_ONCE` on `shadow->dirty`.
- `arch/um/backend/kvm/thread.c:88-96`: FPU-hash lock-free claim.
  Correct only under strict ncpus=1 + no SIGALRM-driven scheduler
  rescheduling — but UML DOES schedule from SIGALRM via switch_threads.

---

## Confidence summary

| Race | Severity | Crash-cause likelihood |
|------|----------|------------------------|
| A — cached_cr3_gpa recycle | SUSPECTED | Low (gpa already cleared on free of cached mm) |
| B — direct sync vs fill clear | PROVEN | Low (self-healing via refault) |
| C — walker vs free | SPECULATIVE under ncpus=1 | Low |
| D — plain stores in fill | PROVEN-but-harmless on x86_64 | Very low |
| **E — `dirty` flag ordering** | **PROVEN** | **HIGH — primary suspect** |
| F — `needs_full_resync` ordering | SUSPECTED | Medium |
| G — mut_ring rmb missing | PROVEN | Diagnostic-only |
| H — FPU hash walk preemption | PROVEN-reachable | Low (data structure tolerates) |
| I — bootstrap pointer publication | SPECULATIVE | Very low under ncpus=1 |
| J — mmu_notifier interaction | SPECULATIVE | Low under steady-state, real under THP |
| K — sync_tlb lock ordering | SPECULATIVE | None identified |

**Top recommendation:** fix Race E first (smp_wmb + WRITE_ONCE/READ_ONCE
on `shadow->dirty`); if the wild pointers survive, instrument the
SREGS-skip cache per checklist item 5.
