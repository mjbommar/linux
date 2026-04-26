# Cross-mm shadow PT design — race inventory and architectural options

Status: investigation memo (perspective: cross-mm shadow PT correctness +
cross-task TLB invalidation).
Date: 2026-04-26.
Scope: integrated KVM backend (`CONFIG_UM_BACKEND_KVM_INTEGRATED=y`),
post-keystone-fix `901213a8d2d1`. Cpython parity gate at 17/21; the
remaining 4-5 modules diverge only at multi-task / threading /
sub-interpreter scenarios.

---

## 0. Preamble — singleton vs. per-mm: where we actually are

The investigation prompt frames the design as "SINGLETON shadow page
table — one shadow PGD shared across all UML mm_structs". That
description fits the code as it stood **before** commit `b9d78a7eeb51`
("um: kvm: per-mm shadow PGD + skip os_map_memory (#275 + #276)").
Today the design is **mostly per-mm**, with vestigial singleton-era
artifacts that confuse reasoning about cross-mm correctness.

What is per-mm:

- The `struct kvm_shadow_mm` (defined at
  `arch/um/backend/kvm/kvm_backend.h:439-472`) hangs off
  `mm->context.id.kvm_shadow`. Allocated in `kvm_mm_attach`
  (`arch/um/backend/kvm/mm.c:46-54`); freed in `kvm_mm_detach`
  (`arch/um/backend/kvm/mm.c:75-80`).
- Each mm has its own PGD page (`shadow->pgd_page`), its own kernel
  VA (`shadow->pgd`), its own physical address (`shadow->pgd_gpa`),
  its own `dirty`/`synced`/`needs_full_resync` flags, and its own
  `fill_lock` mutex (`shadow_mm_alloc` at lifecycle.c:656-685).
- `kvm_shadow_mm_current()` walks `current->active_mm->context.id.kvm_shadow`
  (lifecycle.c:753-763); `kvm_enter_guest` loads CR3 from
  `shadow->pgd_gpa` of the active mm (thread.c:2110-2117).
- `kvm_shadow_pgd_clear_user()` is now a no-op stub that just marks
  the active shadow dirty (lifecycle.c:1480-1495). The clear-on-switch
  dance the prompt describes was removed in #275.

What is still singleton or shared:

- **`kvm_ctx.cached_cr3_gpa`** / `cached_fs_base` / `cached_gs_base`
  / `sregs_primed` — one per VM (kvm_backend.h:73-81). Used by the
  SREGS-skip predicate at thread.c:2235-2240.
- **`kvm_ctx.vcpu0_fd`** — one vCPU multiplexes every UML task
  (lifecycle.c:142-146 enforces ncpus=1).
- **vCPU FPU/XSTATE/MSRs/segment caches** — all live in the single
  vCPU and are saved/restored by hand through the per-task FPU hash
  (thread.c:104-178) plus the SREGS-skip cache.
- **The singleton field group on `struct kvm_um`** — `shadow_pgd_page`,
  `shadow_pgd`, `shadow_pgd_gpa`, `shadow_dirty`, `shadow_pgd_synced`,
  `shadow_pgd_synced_mm`, `shadow_pgd_synced_va` (kvm_backend.h:110-116)
  is "deprecated / vestigial" but **still exists**. Nothing reads it
  on the production path; it remains as a 0-initialised relic so KUnit
  test prototypes that reference `kvm_shadow_pgd_alloc` /
  `kvm_shadow_pgd_free` (now no-op stubs at lifecycle.c:772-786) still
  link.
- **The host VA mapping via `os_map_memory`** in `kvm_mm_map`
  (mm.c:135-139) — mapping is into the parent UML process, **not**
  per-mm. `MAP_FIXED` collisions across mms exist by construction.
  See 16-architecture-review/06-backend-comparison.md.

So the question the prompt actually wants answered is no longer
"should we go per-mm" (we are mostly there) but **"which residual
singleton-like sharing causes the remaining 4-5 multi-task failures,
and is the next architectural step per-mm-host-process or some
sharper reuse of the per-mm shadow we already have."**

---

## 1. Singleton-vs-per-mm design diagram

### Pre-#275 (singleton — the prompt's framing)

```
                    +---------------------+
                    |   kvm_um (singleton |
                    |    per-VM ctx)      |
                    +---------+-----------+
                              |
                              | shadow_pgd     ┌────────────────┐
                              +--------------> | one PGD page   |
                                               | for ALL mms    |
                                               └───────┬────────┘
                                                       |
                       cross-mm switch:                |
                       kvm_shadow_pgd_clear_user()     |
                       walks PGD slots 0..255,         v
                       clears every present leaf,    +-------+
                       preserves bootstrap aliases.  | empty |
                       dirty=true to force CR3       | user  |
                       reload on next entry.         | half  |
                                                     +-------+

                       kvm_enter_guest then:
                         kvm_shadow_fill_from_uml_pgd(active_mm->pgd)
                           → re-installs every present leaf
                             from the new mm's UML pgd

                       Cross-mm faults are slow (full re-fill on
                       every switch) but isolation is enforced by
                       the unconditional clear.
```

Trade-offs of the pre-#275 design (now historical):
- Single CR3 → no SREGS reload of CR3 needed on cross-mm; just dirty=true.
- O(pages-mapped) work on every cross-mm switch (clear pass + fill pass).
- Single shadow walker / writer keyed on "current's shadow"; race
  windows narrower because there's only one tree.
- Hard ordering requirement: clear must complete before next
  `kvm_enter_guest` enters guest, otherwise leaks across mms.

### Post-#275 (per-mm — the code today)

```
   mm A                          mm B                           mm C
 ┌──────────────────┐         ┌──────────────────┐         ┌──────────────────┐
 │ context.id       │         │ context.id       │         │ context.id       │
 │   .kvm_shadow ──┐│         │   .kvm_shadow ──┐│         │   .kvm_shadow ──┐│
 └──────────────────┘         └──────────────────┘         └──────────────────┘
                  │                            │                            │
                  ▼                            ▼                            ▼
          ┌────────────────┐           ┌────────────────┐           ┌────────────────┐
          │ kvm_shadow_mm  │           │ kvm_shadow_mm  │           │ kvm_shadow_mm  │
          │   .pgd_page    │           │   .pgd_page    │           │   .pgd_page    │
          │   .pgd         │           │   .pgd         │           │   .pgd         │
          │   .pgd_gpa  ←──┐           │   .pgd_gpa  ←──┐           │   .pgd_gpa  ←──┐
          │   .dirty       │ │         │   .dirty       │ │         │   .dirty       │ │
          │   .synced      │ │         │   .synced      │ │         │   .synced      │ │
          │   .fill_lock   │ │         │   .fill_lock   │ │         │   .fill_lock   │ │
          │   .mut_ring[]  │ │         │   .mut_ring[]  │ │         │   .mut_ring[]  │ │
          └─────┬──────────┘ │         └─────┬──────────┘ │         └─────┬──────────┘ │
                │            │               │            │               │            │
                ▼            │               ▼            │               ▼            │
         own PGD page        │        own PGD page        │        own PGD page        │
         own intermediate    │        own intermediate    │        own intermediate    │
         PUD/PMD/PT          │        PUD/PMD/PT          │        PUD/PMD/PT          │
         own leaves          │        own leaves          │        own leaves          │
                             │                            │                            │
            ╔════════════════╪════════════════════════════╪════════════════════════════╪════
            ║      shared singleton state on kvm_um:                                   ║
            ║         vcpu0_fd                                                         ║
            ║         cached_cr3_gpa  ──── set when KVM_SET_SREGS runs;                ║
            ║         cached_fs_base       compared to the active shadow's pgd_gpa,    ║
            ║         cached_gs_base       fs/gs from regs->gp[]. Skips SREGS reload   ║
            ║         sregs_primed         when match.                                 ║
            ║         (per-task FPU hash keyed by task_struct *)                       ║
            ╚════════════════════════════════════════════════════════════════════════════
                             │
                             ▼ on switch from mm A → mm B:
                      kvm_context_switch(thread.c:192-299):
                        if (prev->active_mm != next->active_mm)
                            kvm_shadow_pgd_clear_user();   /* now a no-op
                                                              that marks
                                                              CURRENT shadow
                                                              dirty */
                        um_tlb_sync(prev->active_mm);
                        kvm_fpu_save_for_task(prev)
                        kvm_fpu_restore_for_task(next)
                        switch_threads(prev, next)
                             │
                             ▼ on next kvm_enter_guest (running as next):
                      cr3_gpa = next.shadow->pgd_gpa     ← different from
                                                          cached_cr3_gpa,
                                                          so SREGS-skip
                                                          cache misses,
                                                          KVM_SET_SREGS
                                                          loads new CR3,
                                                          guest TLB flushed
```

What is preserved across cross-mm switch (per-mm design):
- The previous mm's shadow tree (every leaf, every intermediate
  table). Not cleared, not freed. Stays intact for whenever that mm
  is scheduled back.
- The previous mm's `dirty` / `synced` / `synced_pgd_va` state.

What needs to happen on cross-mm switch (per-mm design):
- `cached_cr3_gpa` must change → KVM_SET_SREGS programs new CR3 →
  guest TLB flushes via the CR3-write-to-VMCS contract (when KVM
  decides the value actually changed).
- Per-task vCPU state (FPU, FS/GS, …) restored.
- The new mm's pending pgd updates drained into its shadow (via
  direct sync; the verifier full-fill at thread.c:2193 catches any
  gaps).

What is wrong with the current state:
- The pre-switch sync (`um_tlb_sync(prev->active_mm)` at
  thread.c:251-263) is per the singleton-era invariant "drain prev
  before we lose access to it". Under per-mm shadow, the sync writes
  into `prev`'s shadow tree, which is correct, but the **invariant
  comment** at thread.c:217-237 still talks about "the singleton
  view stays consistent" — code is correct, comment is stale and
  misleading.
- `kvm_shadow_pgd_clear_user()` is called from `kvm_context_switch`
  (thread.c:214-215) on every cross-mm switch. After #275 this just
  sets `dirty=true` on the **current task's** shadow — which is
  `prev` at this point in `kvm_context_switch` because
  `set_current(to)` runs in `arch/um/kernel/process.c::__switch_to`
  AFTER `kvm_context_switch` returns. So we're marking prev's shadow
  dirty just before switching away from it. The next time prev
  scheduled back in, dirty=true forces a CR3 reload. Suboptimal but
  not incorrect; the CR3 reload would also be forced because
  `cached_cr3_gpa` would still point at next, not prev.

---

## 2. Race inventory — what can still go wrong with per-mm shadow + singleton vCPU

Below, races A–E are the ones the prompt explicitly asks about.
Races F–J are additional ones I found while walking the code.
Severity uses the 04-concurrency.md scale: PROVEN / SUSPECTED /
SPECULATIVE.

### Race A — clone(CLONE_VM) shared mm: torn shadow state across "tasks" sharing one shadow_mm

**Hypothesis (prompt):** task A and task B share an mm via
`clone(CLONE_VM)`. Both flow PTE updates into the same UML pgd → same
shadow PGD. A host signal preempts A mid-fault while B runs on the
vCPU. Can the shadow see torn state?

**Code citations:**
- Direct sync writer: `arch/um/backend/kvm/shadow_sync.c:153-298`,
  with `WRITE_ONCE(*spte, new); smp_wmb(); WRITE_ONCE(shadow->dirty, true);`
  (P0-1 keystone fix, lines 280-282).
- Walker reader: `shadow_sync.c:132-151` (`kvm_shadow_walk_leaf`) — no
  lock, reads intermediate `pgd[i] & 1ULL` etc. as plain loads.
- Fill writer: `lifecycle.c:1141-1379` (`kvm_shadow_fill_from_uml_pgd`),
  takes `shadow->fill_lock` mutex (lifecycle.c:1166).
- Invalidate writer: `lifecycle.c:1498-1564`
  (`kvm_shadow_invalidate_va_range`), takes `fill_lock` (line 1511).
- Per-mm hashed FPU save/restore: `thread.c:104-178`, called from
  `kvm_context_switch` (thread.c:292-295).

**Analysis.** Under ncpus=1, the host runs one thread. Host signals
can preempt UML-kernel code mid-function (e.g. SIGALRM in the timer
path). A's `set_pte_at` → `kvm_shadow_sync_pte` consists of:
1. Read `mm->context.id.kvm_shadow`.
2. Walk shadow to leaf slot.
3. Translate UML PTE → x86.
4. `WRITE_ONCE(*spte, new)`.
5. `smp_wmb()`.
6. `WRITE_ONCE(shadow->dirty, true)`.
7. Update counters and `mut_ring[]`.

If SIGALRM fires after step 4 but before step 6, the scheduler may
pick task B. B shares A's mm and A's `kvm_shadow_mm`. B's
`kvm_enter_guest` reads `cr3_gpa = shadow->pgd_gpa` (same value),
finds `cached_cr3_gpa == cr3_gpa` (no mm change), reads
`!READ_ONCE(shadow->dirty)` and **may see false** because A's
WRITE_ONCE-dirty hasn't run yet. The SREGS-skip predicate at
thread.c:2235-2240 fires, KVM_SET_SREGS not called, **no CR3 reload,
no guest TLB flush**.

The new shadow leaf at VA X (installed by A in step 4) is now
visible in shadow but the vCPU's TLB might still cache the OLD
mapping. B's guest reads at X return the OLD physical page's bytes.
**Wild pointer or stale read.**

This is exactly the Race E pattern from 04-concurrency.md (which
the keystone fix `901213a8d2d1` addressed by adding `smp_wmb()` +
WRITE_ONCE/READ_ONCE pairing). The **per-mm shadow design does NOT
eliminate Race E for clone(CLONE_VM) because A and B share the
shadow.** The smp_wmb closes the window for the WRITER side; the
READ_ONCE in `kvm_enter_guest` (thread.c:2239) closes the READER
side IF it's actually a READ_ONCE. Let me verify:

```c
// thread.c:2235-2240
if (ctx->sregs_primed &&
    ctx->cached_cr3_gpa == cr3_gpa &&
    ctx->cached_fs_base == cur_fs &&
    ctx->cached_gs_base == cur_gs &&
    shadow && !READ_ONCE(shadow->dirty))
    goto sregs_done;
```

`READ_ONCE(shadow->dirty)` — yes, fixed by P0-1. So the **bare** Race
E variant is closed. **However**, an important variant remains:

**Race A variant: the WRITE_ONCE for dirty races with the SREGS
write that consumes it.**

After KVM_SET_SREGS programs the new CR3 (thread.c:2352), the code
sets `shadow->dirty = false` (thread.c:2369) using a **plain store,
not WRITE_ONCE**. If a direct-sync writer on a shared shadow runs
between the dirty-clear and the next guest entry, the writer sets
dirty=true via WRITE_ONCE, but the reader (next entry) reads
dirty=true via READ_ONCE. So the cleared dirty is correctly observed
and overwritten — no torn state visible. **The plain store is racy
with respect to the C abstract machine, but on x86 the store is
atomic and the reader won't see a torn bit.**

The narrower variant: between the leaf write at shadow_sync.c:280
and the smp_wmb at line 281, a SIGALRM lands. The compiler is
allowed to do **anything** with that ordering relative to the
following dirty-write — including reorder the dirty-write before
the leaf-write. The **smp_wmb is what prevents that reorder**. So
the keystone fix is necessary AND sufficient on the writer side
**for this exact field**. Other fields (`synced`, `synced_pgd_va`)
do NOT have the same discipline:

- `shadow->synced = true; shadow->synced_pgd_va = (u64)pgd_va;`
  (lifecycle.c:1376-1377) — plain stores under fill_lock.
- `shadow->synced = false` (lifecycle.c:1561) — plain store under
  fill_lock.
- The reader (thread.c:2150-2152) reads `shadow->synced &&
  shadow->synced_pgd_va == ...` with plain loads, no READ_ONCE.

For the same-mm case: between two reads of `synced` (compiler may
re-fetch), one read could see the old value and the other the new.
The full-fill skip predicate compounds three plain loads
(`shadow->synced`, `shadow->synced_pgd_va`, `READ_ONCE(needs_full_resync)`).
Mixed READ_ONCE and plain loads is the canonical "the compiler
helped you" footgun documented in
Documentation/atomic_t.txt §"COMPILER SUPPORT".

**Severity: SUSPECTED.** No proven crash trigger by itself (the
fields are advisory — false-skip just means a spurious refault), but
contributes to "skip-cache lies, fill doesn't run when it should"
diagnostic noise. Plausible contributor to test_struct sub-interpreter
bring-up where many threads concurrently touch the same shared mm.

### Race B — invalidate vs. fill on same shadow_mm

**Hypothesis (prompt):** mm_unmap during context switch invokes
`kvm_shadow_invalidate_va_range`, which walks the shadow tree and
clears leaves. If the walk races with a fill from another path (since
fill_lock is per-shadow but the singleton is global), can we lose
updates?

**Code citations:**
- Both `kvm_shadow_fill_from_uml_pgd` (lifecycle.c:1166) and
  `kvm_shadow_invalidate_va_range` (lifecycle.c:1511) take
  `guard(mutex)(&shadow->fill_lock)`. They are mutually exclusive on
  the same shadow.
- The mm sync from `kvm_context_switch` calls
  `um_tlb_sync(prev->active_mm)` (thread.c:252) which drains
  PTE-updates via `ops.mmap = kvm_mm_map` and `ops.unmap =
  kvm_mm_unmap`, both of which call
  `kvm_shadow_invalidate_va_range(id->kvm_shadow, ...)`
  (mm.c:165, mm.c:229). The shadow argument is `id->kvm_shadow` — the
  prev mm's shadow, the right one (per the comment at mm.c:151-159).

**Analysis.** Per-mm fill_lock plus the explicit `id->kvm_shadow`
threading (#274 keystone fix) means invalidate from
`um_tlb_sync(prev)` writes into prev's shadow; concurrent fill from
the next task's `kvm_enter_guest` writes into next's shadow.
Different shadows → different fill_locks → no contention on the
mutex.

**However**, `clone(CLONE_VM)` is the case where two tasks A and B
share the same mm and therefore the same shadow. Imagine:
- A is mid-`kvm_enter_guest`'s full-fill, holds `shadow->fill_lock`.
- SIGALRM lands; signal handler longjmps to B.
- B's `set_pte_at` → `kvm_shadow_sync_pte` does NOT take `fill_lock`
  (it can't, it's atomic context — see shadow_sync.c:14-36).
- B's direct sync writes a leaf via `WRITE_ONCE(*spte, new)`.
- B's `kvm_enter_guest` is reached eventually. Its skip-fill check
  at thread.c:2150 reads `shadow->synced` (plain), sees A's
  half-completed fill state.
- Worst case: A's clear pass had already cleared the leaf B is
  trying to install; A's install pass is still running. B doesn't
  hold fill_lock so it can't install via `kvm_shadow_map_page` (which
  would deadlock on the mutex if it tried).

The walker for direct sync (`kvm_shadow_walk_leaf`,
shadow_sync.c:132-151) does plain reads of intermediate-table slots.
A's fill walks the **same** tree mid-clear-pass. The walker reads
`pgd[pgd_i]` plain — no READ_ONCE. If A is mid-clear of leaves below
this path, the parent is still present (clear only zeros leaves;
intermediate tables are kept attached per the comment at
lifecycle.c:1473-1478). So the walker reaches a leaf slot. The leaf
slot is **0 because A's clear pass just zeroed it**. B treats spte=0
as "leaf absent" via `if (!(old & 1ULL))`. B then either:

- Installs the new leaf (writes a new value into the slot), OR
- Sets `needs_full_resync` if path is missing (it isn't, the parent
  was kept).

If B installs the leaf, A's install pass arrives at the same VA
later, calls `kvm_shadow_map_page(shadow, va, x86e&~mask, x86e&mask)`
which writes `pte[pte_i] = phys_gpa | leaf_flags` (lifecycle.c:1439).
This overwrites B's just-written leaf with whatever A's translator
computes from the (current) UML pgd. **If between A's clear and A's
install, the UML pgd was updated by ANOTHER set_ptes (signal-handler
preemption again, or B's set_ptes that triggered B's direct sync), A
installs the latest value — same as B would have. Convergence.**

But the writer of B's sync also updates `shadow->dirty` via WRITE_ONCE
+ smp_wmb (the keystone fix). A's install pass writes a leaf and then
sets `shadow->dirty = true` (lifecycle.c:1441) via plain store. A's
fill returns. A's caller then writes `shadow->synced = true`
(lifecycle.c:1376) via plain store. Plain stores under fill_lock — but
B already exited fill_lock-less; B's view of `synced` is whatever the
compiler last cached.

**Concrete failure mode:** A finishes fill, writes `synced=true`. B's
next `kvm_enter_guest` reads `shadow->synced` plain, sees true.
Skip-fill fires. **But** in between A's `synced=true` write and B's
read, a third task C (also CLONE_VM-sharing) could have called
direct-sync that set `needs_full_resync=true` (alloc-fail path,
shadow_sync.c:251) but **not** set `synced=false`. The skip predicate
correctly observes `needs_full_resync=true` via READ_ONCE, falls through
to the full fill — repair. **OK.**

**Severity: SPECULATIVE under ncpus=1 with the keystone fix in place.**
The fill-vs-direct-sync race exists, but every reachable interleaving
either converges via the next fill or self-heals via a refault. **This
matches 04-concurrency.md's own Race B downgrade ("Race B alone
doesn't crash but elevates needs_full_resync churn").**

The **subtler** version: B's direct-sync between A's clear and A's
install installs a leaf at VA X with the **B-time** UML pgd
translation. A's install at X uses the **A-time** UML pgd value. If
B-time is more recent (say B's mmap) and A-time is the OLD value
because A re-fetched the pgd entry before SIGALRM, A's install
**clobbers B's install with stale data**. The next access by either
task faults; recovery refills from the now-stable UML pgd; resolves
correctly. So race B can produce extra faults under multi-task
mutation but does not corrupt persistent state.

**However**, the recovery path's effectiveness depends on
`shadow->dirty` being honestly true so the next `kvm_enter_guest`
flushes the guest TLB. A's install path **does not call smp_wmb**
between leaf write and dirty=true (lifecycle.c:1439 then 1441):
```c
pte[pte_i] = (phys_gpa & ~0xfffULL & 0x000ffffffffff000ULL) |
             leaf_flags;
shadow->dirty = true;
```
Both plain. On x86 this is fine for a single CPU but not architecturally
defended. **This is a residual Race E-like hole, in the fill path
rather than the direct-sync path. The keystone fix only covered
shadow_sync.c.**

### Race C — same-mm cross-task TLB invalidation (same-PFN remap)

**Hypothesis (prompt):** Threads A and B share mm. A maps a file at VA
X, reads it, B unmaps and maps a different file at same VA X. Does B's
TLB on the same vCPU see A's stale entry? (Even with the keystone
fix, only fires when `shadow->dirty` is set; if same-VA same-PFN,
dirty isn't set?)

**Code citations:**
- Direct sync sets `dirty=true` for both clear and install paths
  (shadow_sync.c:198-204, 280-282).
- The keystone CR3-toggle fix (thread.c:2339-2351) ONLY runs when
  `same_cr3 = ctx->sregs_primed && ctx->cached_cr3_gpa == cr3_gpa`.
  When SREGS skip-cache misses and KVM_SET_SREGS programs a different
  CR3, KVM does its own VMCS-level CR3-write that flushes the TLB —
  so the toggle is only needed for the same-CR3 case.
- The full-fill clear-and-reinstall in
  `kvm_shadow_fill_from_uml_pgd` sets `dirty=true` via plain store at
  lifecycle.c:1261 (only when `cleared > 0`).
- The fast direct-sync clear at shadow_sync.c:197 always sets
  `dirty=true`, even if the leaf already had value 0 (no — actually
  it only sets dirty when `old & 1ULL`, see line 196). The "absent"
  branch at line 187-191 doesn't touch dirty.

**Analysis of the same-VA same-PFN scenario:**

Stage 1: A maps file F1 at VA X. PFN P1.
- UML mm code sets pte at X to UM_PTE_PRESENT|RW|USER|ACC|... pointing
  at P1.
- `set_ptes` → `kvm_shadow_sync_pte`. Translator returns x86 PTE
  pointing at P1. `WRITE_ONCE(*spte, new); smp_wmb(); WRITE_ONCE(dirty, true);`
- Next entry: SREGS-skip cache compares cached_cr3_gpa to A's
  shadow's pgd_gpa — they match (we're staying in A's mm). dirty=true
  → predicate fails, KVM_SET_SREGS runs (with CR3-toggle to force
  TLB flush). dirty=false. shadow leaf X→P1, vCPU TLB clean.
- Guest reads X. Fault, refill, eventually TLB caches X→P1.

Stage 2: A reads X some times. TLB cached.

Stage 3: B unmaps X. UML pte_clear at X.
- `kvm_shadow_sync_pte` with !UM_PTE_PRESENT. Walks to existing
  leaf, finds it present, clears via WRITE_ONCE(0); smp_wmb;
  WRITE_ONCE(dirty, true).

Stage 4: B mmaps file F2 at same VA X. PFN P2.
- Direct sync. Translator returns x86 PTE pointing at P2.
  WRITE_ONCE(*spte, new = P2|flags); smp_wmb; WRITE_ONCE(dirty, true).

Stage 5: B's `kvm_enter_guest`.
- `cached_cr3_gpa` still equals shadow's pgd_gpa (same mm). FS/GS
  unchanged (intra-thread switch FS may differ; let's say it
  doesn't). `READ_ONCE(shadow->dirty) = true` → skip predicate fails.
  KVM_SET_SREGS path runs. The CR3-toggle (thread.c:2339-2351) fires
  because `same_cr3 = true`. KVM observes a CR3 change → flushes TLB.
- `shadow->dirty = false`.

Stage 6: B's guest reads X. TLB miss → walks shadow → leaf X→P2.
B reads file F2's bytes.

**This is correct.** The keystone fix's exact purpose is to flush
the TLB on same-CR3 dirty entries. Same-VA same-PFN isn't a special
case — every direct-sync set sets dirty=true.

**But: when can dirty NOT be set?** The hypothesis in the prompt
says "if same-VA same-PFN, dirty isn't set?" Let's audit:

- shadow_sync.c:280-282: install path always sets dirty.
- shadow_sync.c:197-199: clear path sets dirty when old leaf was
  present.
- shadow_sync.c:228-230: !x86e (PROT_NONE/!ACCESSED) clear path
  always sets dirty when old leaf was present.
- shadow_sync.c:251-252: alloc-fail-path sets dirty=true (and
  needs_full_resync=true).
- lifecycle.c:1441: `kvm_shadow_map_page` sets dirty=true on every
  install — including bootstrap aliases on every entry.
- lifecycle.c:1561: `kvm_shadow_invalidate_va_range` sets
  dirty=true unconditionally (even when no leaf was cleared).

So **every shadow mutation sets dirty=true**. The hypothesis "same-VA
same-PFN, dirty isn't set" is incorrect for the current code. But:

**The same-PFN-with-different-flags case is a potential gap.** If a
PTE transitions from RW to RO (mprotect) but PFN unchanged, the
translator returns a different x86 PTE (with KVM_X86_PTE_RW masked
off). Direct sync writes the new leaf, sets dirty. CR3-toggle on
next entry flushes TLB. **OK.**

What if **only the A bit changes** (e.g. ptep_test_and_clear_young)?
The translator's return is the same modulo the A bit, which is masked
in the equality check at lifecycle.c:1648 but **not masked at the
translator output**. So the leaf write happens, dirty is set. **OK.**

**Severity: PROVEN-not-an-issue.** The keystone fix actually does
cover same-VA cross-task remapping in shared mm. The hypothesis in
the prompt is a mis-read; same-mm cross-task TLB invalidation works.

But there IS a related real concern:

**Race C variant: cross-task FS/GS leak via SREGS-skip cache.**

The SREGS-skip cache predicate (thread.c:2235-2240) compares
`ctx->cached_fs_base == cur_fs` and `ctx->cached_gs_base == cur_gs`,
where `cur_fs = regs->gp[HOST_FS_BASE]` (per-task). If task A used
FS_BASE=X, then task B (same mm via clone(CLONE_VM), different FS via
distinct TLS) is scheduled, with FS_BASE=Y. `cached_fs_base = X`
(from A's last entry). `cur_fs = Y`. Predicate fails (X != Y) →
KVM_SET_SREGS reprograms FS_BASE to Y. **OK.**

But: imagine task C also exists, also CLONE_VM-sharing mm with A and
B, using FS_BASE=X (same as A). After B runs, `cached_fs_base = Y`.
C is scheduled. `cur_fs = X`. Predicate fails (Y != X). KVM_SET_SREGS
reprograms FS_BASE to X. **OK.**

The hazard requires FS_BASE collision across tasks where one task
sets up state via SREGS that the next task incorrectly inherits.
With FS/GS in the predicate this seems closed.

What's NOT in the predicate (per kvm_backend.h:73-95): MSR_LSTAR,
MSR_FMASK, MSR_KERNEL_GS_BASE, MSR_STAR, FPU/XSTATE (handled by FPU
hash separately), debug registers, KVM_GET/SET_VCPU_EVENTS state.
The 06-backend-comparison.md §5.6 enumerates this gap. **For the
multi-task tests in question (test_decimal DocTestCase, test_list
test_repr_deep), MSR drift seems unlikely; FPU drift is the more
plausible silent corruption channel.**

### Race D — shadow PT page reuse across mms

**Hypothesis (prompt):** When one mm unmaps, its shadow PT pages
(PUD/PMD/PTE intermediate pages) might be reused for another mm. Are
they zero'd properly?

**Code citations:**
- Intermediate-page allocation: `kvm_shadow_table_step`
  (lifecycle.c:1026-1047), uses `alloc_page(GFP_KERNEL | __GFP_ZERO)`.
- Intermediate-page free: `kvm_shadow_mm_free` walks PGD →
  recursively `__free_page`s every present PUD/PMD/PT page
  (lifecycle.c:715-745).
- Mid-life: intermediate pages are NOT freed by
  `kvm_shadow_invalidate_va_range` (lifecycle.c:1473-1478, "kept
  attached so the next mm fill can reuse them"). They're only freed
  at mm teardown.

**Analysis.** Shadow PT pages are buddy-allocator-managed pages.
When a mm is destroyed and its shadow freed, the intermediate pages
go back to the buddy. Subsequent `alloc_page` calls — including for
new shadow PT pages of OTHER mms, OR for kernel allocations
unrelated to KVM — can receive those same physical pages.

**Re-allocation via `__GFP_ZERO`** ensures the new content is zero
before any mutation. If a freshly-alloc'd page is used as a new mm's
intermediate PUD, it's zeroed first. So no stale-PFN leakage at the
shadow PT level.

**However**, the **freed-but-still-referenced** window is real for
the singleton-cached SREGS:

Scenario:
1. mm A with `pgd_gpa = Pa`. KVM_SET_SREGS programs CR3 = Pa.
   `cached_cr3_gpa = Pa`.
2. Switch to a kernel thread with `active_mm = A`. No SREGS reload.
3. mm A is torn down via `__mmput → __mmdrop` → `destroy_context`
   (mmu.c:96, this assumes mm refcount has hit zero, which only
   happens after the kernel thread releases its borrowed reference).
4. `kvm_shadow_mm_free(A)` runs. Sees
   `ctx->cached_cr3_gpa == shadow->pgd_gpa`, sets `cached_cr3_gpa = 0`
   (lifecycle.c:708-709). Frees A's pages back to buddy.
5. mm B is created. `alloc_page` for B's PGD might return A's old
   physical page → `pgd_gpa(B) = Pa`.
6. B becomes active. `kvm_enter_guest` reads `cr3_gpa = Pa`,
   compares to `cached_cr3_gpa = 0`, predicate fails → KVM_SET_SREGS
   programs CR3 = Pa. **OK.**

So `__GFP_ZERO` + the `cached_cr3_gpa = 0` guard at
shadow_mm_free do close the obvious page-reuse hazard. **But two
gaps remain:**

**D-gap-1.** The cache guard only fires if the freed shadow's
`pgd_gpa` matches `cached_cr3_gpa`. If the freed shadow is **not**
the one currently cached (because the active task switched to a
different mm B between A's last entry and A's free), `cached_cr3_gpa`
still holds B's gpa. A's page is freed. Page reuse: a new mm C
arises with `pgd_gpa(C) = Pa = old A's pgd_gpa`. C becomes active.
`kvm_enter_guest`: `cached_cr3_gpa = Pb` (B's), `cr3_gpa = Pa`.
Mismatch → SREGS reload. **OK.**

The hazard would be: `pgd_gpa(C) = Pb` (B's gpa) somehow. Only
possible if B is also torn down and C reuses B's page. Then
`cached_cr3_gpa = Pb`, `cr3_gpa = Pb`, **match → SREGS skipped**. C
walks B's old shadow tree contents — except C's shadow is now in B's
old PGD page, freshly zeroed by `__GFP_ZERO`, so the contents are
zero, not B's old contents. So the guest CR3 walk hits a zero PGD
slot and faults at the very first instruction. Recovery refills C's
shadow from C's pgd. Slow first entry but no corruption.

**D-gap-2 (the real one).** Intermediate pages aren't tracked
individually in the cache. When fill clears + reinstalls leaves on
a re-fill, the **intermediate tables stay**. So a PMD page
allocated for mm X at fill time persists for X's lifetime. If X is
torn down and its PMD page is freed, the PMD page goes back to the
buddy. **OK as long as no hardware walker is mid-walk through it**
— the guest CPU only walks via CR3; CR3 was changed (SREGS reload
on mm change); the old PMD page can't be reached by any hardware
walker after the change, modulo TLB caching.

The **TLB caching** is the variant: x86 hardware TLB caches
intermediate-translation results too (paging-structure cache). If
the TLB cached "PGD slot 5 → PUD page Pud5" and the PUD page is
freed and reused as some other allocation, the next walk hitting
that PGD slot will reload (because CR3 changed → invalidate paging-
structure caches for mismatched ASIDs / non-PCID setup → KVM
invalidates everything on KVM_SET_SREGS). KVM's CR3-write helper
flushes paging-structure caches. **OK on the contract level.**

**Severity: PROVEN-not-corruption-via-reuse,** but PROVEN-mediated-by
the `cached_cr3_gpa` guard correctness. If the guard ever stops
firing (e.g. somebody adds a fast-path that bypasses
`shadow_mm_free`), this becomes a real bug.

### Race E — cross-task signal delivery interrupting kvm_enter_guest

**Hypothesis (prompt):** UML uses host signals for preemption. If a
signal interrupts kvm_enter_guest mid-SREGS-program, can we leave
inconsistent state?

**Code citations:**
- `kvm_enter_guest` is the build-up phase before KVM_RUN
  (thread.c:1906-2498).
- Sequence (per the trace through thread.c:2236 onward):
  1. Compute `cr3_gpa` from active mm's shadow.
  2. SREGS-skip predicate test → if hit, jump to sregs_done.
  3. `KVM_GET_SREGS` (thread.c:2248).
  4. Modify sregs (CR3, FS/GS, IDT, TR, etc.).
  5. Optional CR3-toggle KVM_SET_SREGS (thread.c:2339-2350).
  6. KVM_SET_SREGS (thread.c:2352).
  7. Update `cached_cr3_gpa`, `cached_fs_base`, `cached_gs_base`,
     `sregs_primed = true`, `shadow->dirty = false` (thread.c:2363-2369).
  8. Build IRETQ frame on IST stack (thread.c:2410-2421).
  9. KVM_SET_REGS or update run->s.regs.regs (thread.c:2429-2444).
  10. KVM_SET_MSRS for LSTAR (thread.c:2480).
  11. KVM_SET_MSRS for KERNEL_GS_BASE (thread.c:2493).

**Analysis.** UML's signal handling — block_signals_trace /
unblock_signals — is supposed to wrap critical regions. Let me check
whether `kvm_enter_guest` is inside a signal-blocked region. Looking
at the call chain: `kvm_run_userspace` (thread.c:3040) is called
from the outer `userspace()` loop in
`arch/um/os-Linux/skas/process.c::userspace()`. That outer loop
manages signals via `interrupt_end()` and friends. The kvm_enter_guest
itself does NOT block signals.

**So a host signal CAN interrupt `kvm_enter_guest` between any of
steps 1-11.** The handlers UML installs (SIGALRM for timer,
SIGCHLD for stub-child reaper, SIGIO for IRQ relay) may longjmp via
`switch_threads` to a different UML task.

What state can be left inconsistent?

- Between step 4 and step 6, `sregs` local is modified but not
  pushed. No persistent state changes.
- Between step 6 and step 7, KVM_SET_SREGS has programmed the vCPU
  but `cached_cr3_gpa` hasn't been updated. Next entry will read
  stale cache, see mismatch (or match by accident), reprogram SREGS.
  Worst case: redundant SREGS write. **OK.**
- Between step 7 (cached fields updated, `shadow->dirty = false`) and
  step 9 (KVM_SET_REGS). The vCPU has the new SREGS but not the new
  GP regs. If the signal handler longjmps and the next task runs its
  own `kvm_enter_guest`, that task's cached predicate may match (if
  the cached state happens to match the next task's needs) and skip
  to sregs_done. Then it runs KVM_SET_REGS with its own GP regs.
  **OK.** But: if `cached_cr3_gpa` matches (same shadow), `cached_fs/
  gs_base` match (same FS/GS, e.g. zero or unset), and the next task
  finds `!shadow->dirty` (we just cleared it), the SREGS-skip fires.
  The vCPU's CR3 is whatever the previous task last wrote — which is
  the previous task's cr3_gpa, IF the previous task got to step 6.
  If the previous task had been about to write a different cr3_gpa
  (mid-flight), the vCPU still has the OLD cr3_gpa. **Cached
  cr3_gpa was already updated in step 7 to the previous task's
  pre-signal cr3_gpa.** The next task's cr3_gpa may differ; in that
  case the predicate fails, SREGS reload. **OK.**

The nasty case: **the previous task was at step 5 (CR3-toggle
KVM_SET_SREGS) and then step 6 (real KVM_SET_SREGS), and the signal
interrupts BETWEEN them.** Step 5 wrote a sentinel CR3 (XOR bit 12
of the real CR3). Step 6 was supposed to write the real CR3. If we
longjmp away after step 5, the vCPU has the SENTINEL CR3 in its
SREGS. Cached fields (set in step 7) haven't been written. Next
task's `kvm_enter_guest`:
- Reads its `cr3_gpa` from active mm's shadow.
- SREGS-skip check: `cached_cr3_gpa` might be PRE-step-5's value
  (from the run before this one). Does it match the next task's
  cr3_gpa? Maybe; maybe not.
- If not, KVM_SET_SREGS path. KVM_GET_SREGS reads back the **sentinel
  CR3 the previous task wrote in step 5**. The next task's modify
  pass sets sregs.cr3 = next's cr3_gpa. If the next's cr3_gpa happens
  to match the sentinel via XOR coincidence (fantastically unlikely
  but algorithmically reachable: cr3_gpa(prev) XOR 0x1000 == cr3_gpa(next)),
  KVM_SET_SREGS write doesn't detect a CR3 change and may not flush
  the TLB. **The CR3-toggle wouldn't help because we'd re-toggle to
  the sentinel which is already loaded.** Probability: cr3_gpa is
  randomized via alloc_page; collision via XOR-bit-12 is 1 in 2^N
  for N effective bits. Effectively impossible in practice.
- The non-pathological case: KVM_SET_SREGS programs next's CR3,
  TLB flushes, all is well.

The other nasty case: **the previous task was past step 6 but not
step 11 (MSR_KERNEL_GS_BASE).** vCPU has next-CR3, sregs are correct,
but MSR_KERNEL_GS_BASE points at the previous task's gadget state.
Next task's `kvm_enter_guest` runs `kvm_enter_guest_program_kernel_gs_base`
(thread.c:2493) which has its own `kernel_gs_base_primed` short-circuit
(see kvm_backend.h:48 and its description). After the recent
B-GS-PRIMED fix (commit `0295cb8b4411`), the short-circuit was
disabled to always reprogram MSR_KERNEL_GS_BASE. So next task does
write the MSR, stomping the previous task's. **OK.**

**Severity: SUSPECTED.** Most interleavings self-heal because the
next entry's KVM_SET_SREGS path reprograms the missed fields. The
CR3-toggle XOR-collision case is a theoretical hole; in practice
unreachable.

### Race F — kernel-thread borrowing active_mm

**Code citations:**
- `kvm_context_switch` reads `prev->active_mm` (thread.c:214, 251).
- `kvm_shadow_mm_current` returns `current->active_mm->context.id.kvm_shadow`
  (lifecycle.c:759-762).

**Analysis.** When UML schedules a kernel thread (e.g. softirq, timer
work), the kernel thread has `mm = NULL` and `active_mm` borrows the
previous user task's mm. During that kernel-thread interval:
- Any direct sync called on a kernel-thread context will use
  `current->active_mm->context.id.kvm_shadow` of the BORROWED mm.
  That's correct only if the PTE mutation IS for the borrowed mm,
  which it isn't if the kernel thread is touching `init_mm` (no
  shadow) or some other mm via mm_walk helpers.
- `set_ptes` for `init_mm` calls `kvm_shadow_sync_pte(&init_mm, ...)`
  which checks `mm->context.id.kvm_shadow` — `init_mm`'s context.id
  is initialized with kvm_shadow=NULL (by skas/mmu.c:74 only for
  user mms; init_mm's context is statically init'd with whatever
  static initializer applies). So shadow=NULL → bail at
  shadow_sync.c:169. **OK.**

The hazard: `set_ptes` from a generic mm walker (mm walk that
processes user mms while running in kernel context) on a
non-current-active mm. `kvm_shadow_sync_pte(other_mm, ...)` would
mutate `other_mm`'s shadow. **Correct on the surface** — that's the
right shadow for the right pgd. But `other_mm`'s shadow tree might
be currently being walked by the guest CPU IF some other task is in
KVM_RUN with `cached_cr3_gpa = other_mm->shadow->pgd_gpa`. Under
ncpus=1 only one task runs at a time; KVM_RUN is synchronous in the
ioctl caller's host thread; that host thread is not running the
walker simultaneously. **OK under ncpus=1.**

**Severity: SPECULATIVE; PROVEN bug under SMP.**

### Race G — kvm_mm_unmap with init_new_context heuristic and the post-#275 "no clear needed"

**Code citations:**
- `kvm_mm_unmap` short-circuits virt=0 + len >= 64 TB
  (mm.c:211-215). Returns 0 without doing anything.
- `init_new_context` issues `mm_unmap(new_id, 0, STUB_START)`
  (skas/mmu.c:86) for every freshly attached mm.

**Analysis.** The heuristic is correct under per-mm shadow because
the new mm's shadow tree was just allocated empty by
`kvm_shadow_mm_alloc` — there's nothing to clear. **But** the
heuristic also masks legitimate user-mode `munmap` calls that span
[0, 64TB), which would then NOT propagate to shadow invalidation.
A user process is unlikely to munmap such a giant range, but
malicious or buggy user code could. Failure mode: shadow leaves
stale entries pointing at since-freed PFNs; guest reads return
stale data.

**Severity: SPECULATIVE for benign workloads; PROVEN-reachable for
adversarial inputs.**

### Race H — the post-#275 stale comment in kvm_context_switch

**Code citations:**
- thread.c:217-237 — the comment block above `um_tlb_sync` describes
  the singleton-era invariant ("the kvm shadow PT is a singleton
  (#242 + #243-pending)") which is no longer true after #275.

**Analysis.** Code is correct (drains prev's pending PTE updates
into prev's shadow, which is right). Comment is actively misleading
to a reader trying to reason about cross-mm correctness. **Diagnostic
hazard rather than correctness hazard.**

**Severity: documentation only.**

### Race I — same-mm dirty-flag write via plain store in lifecycle.c

**Code citations:**
- lifecycle.c:1261, 1441, 1473, 1494, 1561 — all set `shadow->dirty = true`
  (or shadow->synced) via plain stores. Most are under fill_lock.

**Analysis.** Direct sync uses `WRITE_ONCE(shadow->dirty, true)` after
`smp_wmb()` (the keystone fix). The fill / invalidate / map_page
paths use plain stores. These run under fill_lock, so they're
serialized against each other. They're NOT serialized against
`READ_ONCE(shadow->dirty)` in the SREGS-skip predicate (which doesn't
take fill_lock).

Concrete interleaving:
- Task A is mid-fill, holds fill_lock. Executes
  `shadow->dirty = true` at lifecycle.c:1261 via plain store. Compiler
  is allowed to re-fetch or split this write… but on x86, an aligned
  bool store is atomic.
- Signal preempts A. Task B's `kvm_enter_guest` runs.
- B reads `READ_ONCE(shadow->dirty)`. On x86 the read is atomic; sees
  either true (if A's store landed) or the old value.
- If old value was false, B skips SREGS reload. But A is in the
  middle of fill which **just** mutated leaves; the vCPU TLB may
  still cache pre-A-fill mappings. **Stale TLB hazard.**

The keystone fix only covered the direct-sync path. The fill path's
plain stores are still vulnerable to the same race. The mitigating
factor: fill almost always runs under `synced=false || needs_full_resync=true`,
which means the next entry **does** the full fill before checking dirty.
The next entry's flow:
1. Skip-fill predicate at thread.c:2150 fails (synced=false), full
   fill runs, sets `synced=true; dirty=true; needs_full_resync=false`.
2. SREGS-skip predicate at thread.c:2235. `shadow && !READ_ONCE(dirty)`
   evaluates: dirty is now true (just set), READ_ONCE returns true,
   predicate fails, SREGS reload.

So fill-as-side-effect-of-skip-fill-miss correctly forces SREGS reload.
The hazard requires a path where fill runs but dirty isn't set OR
isn't observed. Hard to reach.

**Severity: SUSPECTED, low priority** because of self-healing via
the next entry's fill-then-SREGS path.

### Race J — FPU state pollution across CLONE_VM siblings

**Code citations:**
- `kvm_fpu_save_for_task` / `kvm_fpu_restore_for_task` keyed on
  `task_struct *` (thread.c:104-178).
- Hash never freed (thread.c:88-91 acknowledges).

**Analysis.** clone(CLONE_VM) gives sibling tasks distinct
`task_struct` pointers. So the FPU hash keys differently for each
sibling. Task A's FPU state saved under key A; restored on switch
back to A. Task B's FPU saved under key B; restored on switch to B.
**No leak between siblings.**

But: if A exits and its `task_struct` is slab-recycled to a new
task A' that lands in the same hash bucket as A's stale entry, A'
could find A's stale slot and inherit A's FPU state. **Non-CLONE_VM-specific
issue, covered as B-FPU-HASH-UAF in 16-architecture-review/00-synthesis.md.**

**Severity: PROVEN-reachable, low corruption likelihood for CLONE_VM
specifically; orthogonal hazard.**

---

## 3. Failure-mode mapping — which residual test failures map to which race

| Test | Failure mode | Most plausible race | Reasoning |
|------|--------------|---------------------|-----------|
| `test_struct.test_endian_table_init_subinterpreters` | Subinterpreter init creates per-interpreter cpython data structures. Each subinterpreter has its own thread (CLONE_VM). | **A** + **C-variant (FS/GS via SREGS-skip)** | Sub-interpreters share mm (CLONE_VM); each has different thread-local state. FS/GS drift across subinterpreters via the SREGS-skip cache + the residual plain-store-of-dirty in the fill path is the most plausible silent corruption channel. |
| `test_decimal` DocTestCase 731 | Decimal arithmetic with a large number of contexts. | **I** (fill path's plain dirty store) | Decimal allocates many small objects, triggering many set_pte_at + COW transitions. Multi-task only marginally; main correlate is mutation density. |
| `test_bytes.FreeThreadingTest.test_free_threading_bytearrayiter` | Free-threading test (no GIL); multiple threads mutate a bytearray. | **A** + **J (FPU)** | Free-threading explicitly stresses CLONE_VM siblings touching shared data. FPU drift via the per-task hash + same-mm direct-sync race E remnant in fill path. |
| `test_list.test_repr_deep` (NULL-deref during deep recursion) | Deep recursion in repr() → very deep call stack → many stack-page faults. | **B** (invalidate vs. fill) + **F (kernel-thread borrow)** | Deep recursion expands stack with many anonymous-page faults, each going through set_pte_at + direct sync. NULL-deref pattern matches the post-#PF recovery returning before the shadow is in sync. |
| `test_set.TestWeirdBugs.test_merge_and_mutate` | Set merge while mutating, hash table resizes. | **A** + **B** | Hash table resize is many concurrent set_pte_at + memory remapping; same-mm cross-task is plausible if the test uses threads (it might not). |

The pattern: the remaining failures are dominated by tests that
either (a) do CLONE_VM thread sharing where many siblings share a
single shadow_mm, OR (b) do mutation density high enough to expose
race I (fill path's plain-store-dirty hole). **Race E is closed for
the direct-sync path; a parallel keystone fix is needed for the
fill path.**

---

## 4. Two architectural options

### Option a — Per-mm shadow PT (already implemented; sharpen)

**Status:** Mostly done as of #275. The remaining work is to make
the per-mm design **fully** per-mm by removing the residual singleton
state and tightening the data flow.

**What's still singleton:**

1. `kvm_ctx.cached_cr3_gpa` / `cached_fs_base` / `cached_gs_base` /
   `sregs_primed` (kvm_backend.h:73-81). One value per VM, but the
   state it caches is per-task (FS/GS) and per-mm (CR3). The current
   cache uses the ACTUAL field values as the cache key; mismatch
   correctly forces a SREGS reload. Sufficient for correctness given
   the keystone fix, but conceptually messy.

2. The single vCPU0_fd. `ncpus=1` enforced (lifecycle.c:142). All
   tasks multiplex onto one vCPU; per-task vCPU state must be saved
   and restored manually. The 06-backend-comparison.md §5.6 list of
   not-tracked-per-task fields (segment caches, debug regs,
   KVM_VCPU_EVENTS, MSR_FMASK, MSR_STAR, etc.) is open-ended.

3. The deprecated singleton fields on `struct kvm_um`
   (kvm_backend.h:110-116) — vestigial but linked in.

**Cost of pushing per-mm further:**

- **Remove deprecated singleton fields.** ~50 lines, mostly mechanical
  deletion. Update KUnit prototypes that reference `kvm_shadow_pgd_*`.
  Risk: zero (the fields are 0-initialised vestiges).

- **Make the SREGS cache per-mm.** Move `cached_cr3_gpa` /
  `cached_fs_base` / `cached_gs_base` onto `struct kvm_shadow_mm` (or
  parallel per-task storage in `thread_struct`). Cache match becomes
  `shadow->cached_cr3_gpa == cr3_gpa` (always true if the cache is
  per-shadow → trivially OK; the cache provides no additional info
  when scoped to its own mm).

  More usefully, hoist the FS/GS cache to per-task. The current
  field on `kvm_um` is overwritten on every task switch; making it
  per-task lets you actually reason about "did this task's FS_BASE
  change since I last programmed it." Cost: ~80 lines, plus
  test-build risk.

- **Add `cached_mm` to the cache predicate.** Per
  04-concurrency.md §"What I would do first" item 3, add a
  `cached_mm` field and require `cached_mm == current->active_mm` in
  the SREGS-skip predicate. Defends against any "same gpa, different
  mm" hazard. Cost: ~20 lines.

- **Make the fill path use WRITE_ONCE + smp_wmb for dirty.**
  Mirror the keystone fix into lifecycle.c:1261, 1441, 1473, 1561.
  Cost: ~10 lines. **High value for race I.**

- **Make `synced` and `synced_pgd_va` writes paired (store_release)
  and reads paired (load_acquire).** Prevents the compiler-reordering
  variant of race A. Cost: ~10 lines.

**Cost of full per-mm vCPU state:**

- **Per-task save/restore of vCPU events** (KVM_GET/SET_VCPU_EVENTS).
  The relevant per-task state is pending exception, NMI pending,
  interrupt-shadow state. ~50 lines + ioctl overhead per
  context_switch. Cost is real (~5 µs per switch).

- **Per-task save/restore of debug registers.** Add `kvm_debugregs`
  to the per-task save. Most workloads don't care; gdb / ptrace-on-
  KVM-task sometimes does.

- **Audit MSR_FMASK / MSR_STAR / MSR_LSTAR for per-task drift.**
  These are programmed per `kvm_enter_guest` (thread.c:2480-2495)
  but the `msrs_primed` flag short-circuits after first write. If
  any path mutates these MSRs (it shouldn't on the production path),
  they leak across tasks. Audit + add per-task save if found.

**Migration plan:**

1. (1 day) Add `cached_mm` to SREGS-skip predicate. Ship.
2. (1 day) WRITE_ONCE + smp_wmb on fill-path dirty stores. Ship.
3. (1 day) READ_ONCE on `shadow->synced` and `synced_pgd_va`; pair
   the writes with `smp_store_release`. Ship.
4. (3 days) Remove deprecated singleton fields. Test KUnit.
5. (1 week) Per-task FS/GS cache. Test parity gate.
6. (2 weeks) Per-task KVM_GET/SET_VCPU_EVENTS. Test parity gate.

Total: ~4 weeks for full per-mm convergence. **Each step is
shippable independently and each closes a specific race or
diagnostic gap.**

**Correctness improvement vs. current:**
- Closes race A's residual variants by tightening cache invariants.
- Closes race I (fill-path dirty-store).
- Closes race C-variant (FS/GS leak across CLONE_VM siblings, when
  the cache key includes mm or task identity).
- Does NOT close: race D (page reuse via buddy — handled by
  `__GFP_ZERO`), race F (kernel-thread mm borrow under SMP — out of
  scope for ncpus=1), or the per-task vCPU state gap (open-ended).

**What it does NOT solve:** the **parent-VA contamination** problem
(B-PARENT-VA-CONTAMINATION in 00-synthesis.md). `os_map_memory` still
runs and the parent process still has user-VA mappings. Wild guest
pointers can still land on UML kernel data. This option only fixes
the shadow PT side; the host VA side remains.

### Option b — Per-mm host worker (memo 15 Phase 4 Option B)

**Status:** Not implemented. Memo 15 §"Host VA Mapping Policy"
Option B describes the design; 06-backend-comparison.md §7 Phase 3
sketches the migration plan.

**What it gives:** Each UML mm has a worker host process whose only
job is to own the user-VA mappings (`mmap`/`munmap` against the
worker's address space, not the parent's). The parent UML process
has NO user-VA mappings ever. The vCPU still runs in the parent and
walks the per-mm shadow PT as today.

**Correctness improvement vs. current:**

- **Eliminates parent-VA contamination.** Wild guest pointers crash
  the worker (host SIGSEGV), not the parent. Loud failure instead
  of silent corruption.
- **Removes the cross-mm `MAP_FIXED` collision class** — different
  mms map into different worker processes; collisions impossible.
- **Per-mm isolation parity with ptrace/seccomp.** Parent has no
  user-VA mappings to dereference; uaccess goes through the
  walk-UML-pgd-to-physical path (`arch/um/kernel/skas/uaccess.c:64`)
  unconditionally. The "two views must agree" structural problem is
  gone for the host VA side.
- **The init_new_context heuristic at mm.c:211-215 becomes
  unnecessary** — worker has nothing mapped in [0, STUB_START), so
  mm_unmap(0, STUB_START) is a no-op naturally.

**What it does NOT solve:**

- Shadow PT correctness — the per-mm shadow tree still exists and
  still has races A, I (and any future ones). The shadow has to
  mirror the worker's mappings; "two views must agree" becomes
  "shadow PT must mirror worker's pgd" which is the same shape
  ptrace/seccomp have. The per-mm shadow makes this manageable; the
  options above (R1, fill-path dirty fix, etc.) still apply.

- vCPU state per-task gap — still one vCPU shared across tasks.

- KVM SMP — orthogonal.

**Cost:**

- **Engineering complexity: substantial.** Recreates much of
  `start_userspace`'s machinery (worker fork, sock-pair RPC,
  SCM_RIGHTS for phys_fd, signal handling for worker death). Memo
  15's recommendation is to attempt Option A (KVM-native uaccess —
  remove the `os_map_memory` dependency entirely) before Option B
  because Option A is much smaller.

- **Per-mm fork cost.** `kvm_mm_attach` would need to fork a worker
  process per mm. Forking a stub child takes ~ms; for fork-heavy
  workloads (e.g. CI test suites) this adds material latency. The
  ptrace/seccomp backends already pay this cost; KVM would pay it too
  when it shouldn't have to (the vCPU runs in the parent regardless).

- **Per-mm-map RPC cost.** Every `kvm_mm_map`/`kvm_mm_unmap`
  becomes a sock-pair round-trip to the worker. Latency increases
  per-syscall on the mapping hot path. Amortized OK for most
  workloads (mappings are bursty during fork/exec, less common
  steady-state).

- **More state to track.** Per-mm worker pid, sock fd, last-known
  alive timestamp; reaper for dead workers; restart logic? Or
  treat worker death as fatal to the mm and SIGKILL the user
  process? The seccomp backend already deals with this.

**Migration plan (per memo 15 / 06-backend-comparison Phase 3):**

1. (Week 1-2) **Phase 1 from 06-backend-comparison.md.** Add
   `mprotect(PROT_NONE)` over `[0, TASK_SIZE)` in the parent UML
   process before every `KVM_RUN`, restore after. Make the bug
   loud. Audit which paths still try to dereference user VAs from
   the parent. This is a **diagnostic phase** — it doesn't change
   correctness, it changes failure mode from silent → loud. Run
   the parity gate; see whether anything dies before it would have
   silently corrupted.

2. (Week 3-4) **Implement KVM-native uaccess (Option A).** Audit
   the dereference sites identified in Phase 1; replace with
   walk-UML-pgd-to-physical-page calls (use the existing
   `do_op_one_page` / `maybe_map` helpers). Remove `os_map_memory`
   from `kvm_mm_map` / `kvm_mm_unmap`. Verify parity gate.

3. (Week 5-12) **If Option A is impractical, implement per-mm
   worker (Option B).** Add `kvm_um_worker` field to `mm_id`.
   Have `kvm_mm_attach` clone a worker via `start_userspace`. Route
   `kvm_mm_map`/`kvm_mm_unmap` to the worker. Drop parent-VA
   `os_map_memory` calls. Drop the STUB_START heuristic.

4. (Week 13+) **Per-mm vCPU state.** Per-mm save/restore of vCPU
   events, debug registers, MSR audit. Multi-vCPU SMP unblocked
   by per-mm worker (if there's a worker per mm, vCPU per mm
   becomes plausible).

Total: 12 weeks for full Option B. Phase 1 alone is ~1 week and
provides immediate diagnostic value; Phase 2 (Option A) is ~3 weeks
and may be sufficient.

---

## 5. Recommendation

### Pursue Option a tightening first (Race-I fix + per-task FS/GS + cached_mm). Plan for Option a Phase-1-style diagnostic mprotect parallel.

The parity gate moved from 0/21 to 17/21 on a SINGLE keystone fix
(P0-1, the smp_wmb for shadow->dirty). That move tells us:

1. The structural design is **fundamentally sound** in its current
   per-mm shape. It produced 17/21 with one targeted fix. The
   structure isn't blocking us.
2. The remaining 4 failures all share the multi-task pattern. Same
   pattern, same race class. **One more keystone fix could move
   parity to 21/21.**
3. The most likely candidate is **Race I (fill-path dirty store)**
   because it's the exact same pattern as Race E but in the OTHER
   shadow writer. The keystone fix only covered the direct-sync
   side; the fill side has identical-shape stores at lifecycle.c:1261,
   1441, 1473, 1561 with no smp_wmb and no WRITE_ONCE.

**Concrete first action:** Apply the smp_wmb / WRITE_ONCE pair to
all `shadow->dirty = true` writes in lifecycle.c. Concurrent change
to `shadow->synced` and `synced_pgd_va`: add `smp_store_release` /
`smp_load_acquire` for these as a pair. This is ~30 lines, has the
same low-risk profile as the original keystone fix, and is the
cheapest test of the hypothesis "all four remaining failures are
race I variants."

**If parity moves to 21/21:** ship and revisit Option B at leisure.
The structural argument for Option B (parent-VA contamination
elimination, per-mm worker) is real but isn't blocking the parity
gate.

**If parity moves but only marginally:** apply the per-task FS/GS
cache + cached_mm guards from Option A's tightening. Cost ~1 week.
Re-test.

**If parity doesn't move:** the bug is elsewhere — most likely in
the per-task vCPU state gap (KVM_VCPU_EVENTS, debug regs, MSR
drift). At that point Option B Phase 1 (mprotect-as-canary) is the
cheapest discriminator. If mprotect crashes the parent during
KVM_RUN, the bug is parent-VA contamination and Option B Phase 2
(KVM-native uaccess) is the right path. If mprotect doesn't fire,
the bug is per-task vCPU state and Option a Phase 3+ is the right
path.

**Why NOT Option B as the first step:**

- It's a 4-12 week investment with no guarantee the bug is what it
  fixes. The 17/21 → 21/21 jump is much more plausibly Race I (one
  more keystone fix) than parent-VA contamination (which would
  affect every test, not just multi-task ones).
- Option B's correctness improvement is the parent-VA
  contamination class, but the parent-VA contamination would
  manifest as wild-pointer crashes in the parent UML kernel — which
  isn't what the remaining 4 failing tests look like (they look
  like sub-interpreter / multi-thread scenarios, not random kernel
  crashes).
- Option B does NOT fix shadow-PT correctness. The races analyzed
  here all live in the per-mm shadow tree, which Option B keeps. So
  Option B is necessary-but-not-sufficient for the multi-task case.
- Option A (Option B's lighter sibling — KVM-native uaccess
  without the worker process) is itself ~3 weeks and is the
  recommended structural step per memo 15. Even within the
  structural fixes, A is preferred over B.

**Why per-mm shadow Phase 3 (collapse to single source of truth, R1
from 16-architecture-review/01-mm-architecture.md) is also a fit:**

R1 says "remove the deferred chain entirely; direct sync is the
sole shadow writer; full fill becomes a debug-only audit." This is
the structural endgame for shadow correctness. It removes Race B
(invalidate vs. fill) and Race I (fill-path dirty stores) by
removing the fill from the hot path entirely. Cost is moderate
(memo 15 Phase 3) and aligns with what the per-mm design enables.

Combining: **Apply the Race I keystone fix first (1 day). If parity
goes to 21/21, ship and follow up with Phase 3 to remove the fill
on the hot path (cleaner architecture, removes the race class
entirely). If parity doesn't move, escalate to Option B Phase 1
mprotect-canary to discriminate parent-VA from vCPU-state hypotheses.**

The recommendation in priority order:

1. **Race I keystone fix.** ~30 lines. 1 day. Probability of moving
   parity to 21/21: high (same pattern as P0-1).
2. **cached_mm in SREGS-skip predicate.** ~20 lines. 1 day. Defends
   against any latent same-gpa-different-mm hazard.
3. **Per-task FS/GS cache.** ~80 lines. 1 week. Closes Race C-variant.
4. **Memo 15 Phase 3 (full-fill out of hot path).** ~200 lines. 2
   weeks. Removes fill races structurally.
5. **Option B Phase 1 (mprotect canary).** ~50 lines. 1 week. Diagnostic.
6. **Option B Phase 2 (KVM-native uaccess).** ~500 lines. 3 weeks.
   Eliminates parent-VA contamination structurally.
7. **Option B Phase 3 (full per-mm worker).** ~2000 lines. 8+ weeks.
   Reserve until 1-6 are insufficient.

Items 1-2 are immediate, low-risk, and high-EV. Item 3 is the next
correctness rung for multi-task scenarios specifically. Item 4 is
the cleanest architectural improvement and removes Race B/I as
classes. Items 5-7 are the structural redesign for parent-VA
correctness, deferred until the multi-task races above are closed.

---

## Appendix A — Comparison with seccomp's design

Seccomp's stub-child model trivially avoids every race A-J because:

- Each mm has its own host process. PTE updates flow through the
  stub-child's host pgd via `mmap`/`munmap` syscalls queued from
  UML. The host kernel's MMU is the source of truth.
- No shadow PT to maintain. No fill_lock. No direct sync. No
  invalidate-vs-fill race. No singleton SREGS cache.
- Per-task isolation is by construction (each stub child is a Linux
  task).
- Cross-mm switch is "kill mm A's stub, run mm B's stub." Real CR3
  reload happens at the host kernel level when the scheduler picks
  the new stub child.
- Wild pointer in user code crashes the stub child, host kernel
  delivers SIGSEGV, UML reads it via signal-info dispatch. Loud.

The price seccomp pays: every guest syscall is a SIGSYS round-trip
through host signal handling (~µs cost). KVM's vCPU runs at native
speed and traps via VMEXIT (~ns cost). KVM's perf is much better;
its correctness story is much worse for the reasons above.

A per-mm worker (Option B) doesn't bring the worker process to
running guest code — KVM_RUN still happens in the parent. The
worker just owns the **user-VA mapping** so that wild pointers
crash the worker rather than the parent. This is the lighter half
of the seccomp design (no PTRACE_SYSEMU / SIGSYS round-trip, just
isolated address space for mappings).

---

## Appendix B — File:line index for cited code

| Citation | Description |
|----------|-------------|
| `arch/um/backend/kvm/kvm_backend.h:34-99` | `struct kvm_um` — singleton fields: vcpu0_fd, run0, sregs_primed, cached_cr3_gpa, cached_fs_base, cached_gs_base. |
| `arch/um/backend/kvm/kvm_backend.h:110-116` | Vestigial pre-#275 singleton fields: shadow_pgd_*, shadow_dirty, shadow_pgd_synced*. |
| `arch/um/backend/kvm/kvm_backend.h:439-472` | `struct kvm_shadow_mm` — per-mm shadow PGD container. |
| `arch/um/backend/kvm/lifecycle.c:142-146` | `ncpus=1` enforcement. |
| `arch/um/backend/kvm/lifecycle.c:656-685` | `kvm_shadow_mm_alloc` — per-mm shadow allocation. |
| `arch/um/backend/kvm/lifecycle.c:688-750` | `kvm_shadow_mm_free` — per-mm shadow teardown, walks tree to free intermediate pages. |
| `arch/um/backend/kvm/lifecycle.c:708-709` | `cached_cr3_gpa` invalidation guard. |
| `arch/um/backend/kvm/lifecycle.c:753-763` | `kvm_shadow_mm_current` — derefs `current->active_mm->context.id.kvm_shadow`. |
| `arch/um/backend/kvm/lifecycle.c:772-786` | Vestigial `kvm_shadow_pgd_alloc` / `_free` / `_gpa` no-op stubs. |
| `arch/um/backend/kvm/lifecycle.c:1141-1379` | `kvm_shadow_fill_from_uml_pgd` — full fill, takes `fill_lock`. |
| `arch/um/backend/kvm/lifecycle.c:1166` | fill_lock acquisition. |
| `arch/um/backend/kvm/lifecycle.c:1216-1259` | Fill clear pass (transactional). |
| `arch/um/backend/kvm/lifecycle.c:1261, 1441, 1473, 1494, 1561` | Plain stores of `shadow->dirty = true` — Race I sites. |
| `arch/um/backend/kvm/lifecycle.c:1376-1377` | Plain stores of `shadow->synced` and `synced_pgd_va`. |
| `arch/um/backend/kvm/lifecycle.c:1381-1443` | `kvm_shadow_map_page` — installs leaf, sets dirty. |
| `arch/um/backend/kvm/lifecycle.c:1480-1495` | `kvm_shadow_pgd_clear_user` — post-#275 no-op stub. |
| `arch/um/backend/kvm/lifecycle.c:1498-1564` | `kvm_shadow_invalidate_va_range` — clears leaves under `fill_lock`. |
| `arch/um/backend/kvm/lifecycle.c:1511` | fill_lock acquisition (invalidate side). |
| `arch/um/backend/kvm/lifecycle.c:1561` | Plain store of `shadow->dirty = true` and `synced = false`. |
| `arch/um/backend/kvm/mm.c:30-69` | `kvm_mm_attach` — allocates per-mm shadow. |
| `arch/um/backend/kvm/mm.c:71-102` | `kvm_mm_detach` — frees per-mm shadow. |
| `arch/um/backend/kvm/mm.c:119-181` | `kvm_mm_map` — `os_map_memory` into parent + invalidate target shadow. |
| `arch/um/backend/kvm/mm.c:135` | `os_map_memory` parent-VA mapping (B-PARENT-VA-CONTAMINATION). |
| `arch/um/backend/kvm/mm.c:165` | `kvm_shadow_invalidate_va_range(id->kvm_shadow, ...)` — #274 keystone fix. |
| `arch/um/backend/kvm/mm.c:183-236` | `kvm_mm_unmap` — symmetric. |
| `arch/um/backend/kvm/mm.c:211-215` | `init_new_context` STUB_START heuristic. |
| `arch/um/backend/kvm/shadow_sync.c:14-36` | Atomic-context contract; relies on cooperative single-host-thread invariant. |
| `arch/um/backend/kvm/shadow_sync.c:132-151` | `kvm_shadow_walk_leaf` — lockless walker. |
| `arch/um/backend/kvm/shadow_sync.c:153-298` | `kvm_shadow_sync_pte` — direct-sync writer. |
| `arch/um/backend/kvm/shadow_sync.c:280-282` | P0-1 keystone fix: WRITE_ONCE leaf, smp_wmb, WRITE_ONCE dirty. |
| `arch/um/backend/kvm/thread.c:88-178` | Per-task FPU hash (B-FPU-HASH-UAF). |
| `arch/um/backend/kvm/thread.c:192-299` | `kvm_context_switch`. |
| `arch/um/backend/kvm/thread.c:214-215` | `kvm_shadow_pgd_clear_user` call (now no-op marker). |
| `arch/um/backend/kvm/thread.c:217-237` | Stale singleton-era comment. |
| `arch/um/backend/kvm/thread.c:251-263` | `um_tlb_sync(prev->active_mm)` pre-switch drain. |
| `arch/um/backend/kvm/thread.c:292-295` | FPU save/restore on switch. |
| `arch/um/backend/kvm/thread.c:1906-2498` | `kvm_enter_guest`. |
| `arch/um/backend/kvm/thread.c:2110-2117` | CR3 source from per-mm shadow. |
| `arch/um/backend/kvm/thread.c:2150-2158` | Skip-fill predicate (synced + !needs_full_resync). |
| `arch/um/backend/kvm/thread.c:2235-2240` | SREGS-skip predicate (READ_ONCE dirty). |
| `arch/um/backend/kvm/thread.c:2339-2351` | P0 keystone fix: CR3 sentinel toggle. |
| `arch/um/backend/kvm/thread.c:2352-2370` | KVM_SET_SREGS + cached field updates + plain `shadow->dirty = false`. |
| `arch/um/backend/kvm/thread.c:2480-2495` | KVM_SET_MSRS for LSTAR + KERNEL_GS_BASE. |
| `arch/um/backend/kvm/thread.c:3040-3134` | `kvm_run_userspace`. |
| `arch/um/backend/kvm/thread.c:3124-3130` | `um_tlb_sync(current->mm)` — pre-entry drain. |
| `arch/um/include/asm/kvm_mmu_sync.h:23-91` | Direct shadow sync interface contract. |
| `arch/um/kernel/skas/mmu.c:44-94` | `init_new_context` — initializes `kvm_shadow=NULL` (#275 follow-on at line 74). |
| `arch/um/kernel/skas/mmu.c:96-125` | `destroy_context` — calls `mm_detach`. |

---

## Appendix C — What changed since the prompt was written

The investigation prompt assumes the singleton pre-#275 design is
the active code. Two subsequent commits substantially changed the
landscape:

1. `b9d78a7eeb51` — **Per-mm shadow PGD + skip os_map_memory (#275 +
   #276).** Moved shadow PGD from singleton on `struct kvm_um` to
   per-mm on `struct kvm_shadow_mm`. The cross-mm clear-and-refill
   dance is gone; cross-mm switches just point CR3 at a different
   tree. `kvm_shadow_pgd_clear_user` became a no-op stub.

2. `901213a8d2d1` — **P0 keystone TLB-flush fix.** Added the CR3
   sentinel toggle in `kvm_enter_guest` to force guest TLB flush on
   same-CR3 dirty entries. Moved parity gate from 0/21 to 17/21.

The prompt's listed remaining failures (`test_struct.test_endian_table_init_subinterpreters`,
`test_decimal` DocTestCase 731, `test_bytes.FreeThreadingTest.test_free_threading_bytearrayiter`,
`test_list.test_repr_deep`, `test_set.TestWeirdBugs.test_merge_and_mutate`)
are post-keystone failures, all multi-task / threading-flavored.
The cross-mm singleton-clear-on-switch race the prompt asks about
literally cannot fire today (the function is a no-op). The
remaining failures are cross-task / cross-thread races on the **per-mm
shadow** itself (Race A) plus the residual **fill-path** plain-store
hole that mirrors Race E (Race I). Those are the primary targets
for the next round of work.
