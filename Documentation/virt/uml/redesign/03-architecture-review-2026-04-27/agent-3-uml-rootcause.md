# Agent 3 — Ruthless First-Principles Audit of the UML KVM Backend

Status: 2026-04-27. Independent angle on the timing-sensitive flake (~25 % per
trial on heavy long-running Python workloads, ~14 % on the `import
test.test_decimal` reproducer, plus a NEW class — wrong-answer corruption mid
big-int compute — that the snapshot fix did not touch).

This memo deliberately ignores how gVisor / Firecracker / qemu would do it.
It looks only at what the current code (`arch/um/backend/kvm/`) does, asks
why each timing window exists, and proposes a structural shape that makes
the bug class impossible.

---

## 1. Race-by-race analysis (smoking guns, with file:line)

The "25 % flake" is not one race. The data in `00-playbook.md` plus the code
shows at least FIVE independently-sufficient race classes living in the
current shape. Each one has been fixed locally and the residual flake just
shifts to the next. That pattern is itself the diagnosis.

### Race A — vCPU-mmap aliasing across cooperative tasks (the dominant ld-linux NULL-deref)

**Where:** singleton `kvm_um.run0` (`arch/um/backend/kvm/lifecycle.c:71-77`)
plus singleton `vcpu0_fd`, plus singleton `kvm_bootstrap_page_stack` (the
IST page in `thread.c`), versus
`unblock_signals()` at `arch/um/backend/kvm/thread.c:3552`.

**The window** (post-snapshot fix, what's left):
1. Task A returns from `KVM_RUN` (line 3464). The exit-state snapshot block
   at 3483-3530 copies `exit_reason`, `kregs`, `exit_sregs`, `mmio_*`, and
   40 bytes of IST stack into stack locals.
2. `unblock_signals()` fires at line 3552. SIGALRM is now servicable.
3. Inside the dispatch (e.g. the syscall path at 3604, the `#PF` path at
   3631), the handler walks UML kernel code (`handle_syscall` →
   `sys_call_table[N]`) which can call `schedule()`, which calls
   `switch_threads` (`arch/um/os-Linux/skas/process.c:627`), which
   `longjmp`s into task B's stack.
4. Task B re-enters `kvm_run_userspace`, re-runs `kvm_enter_guest`, calls
   its own `KVM_RUN`. **The singleton `vcpu0_fd` is now executing B's
   guest code; the singleton `run0->s.regs.regs` reflects B's exit; the
   singleton `kvm_bootstrap_page_stack` IST page now holds whatever B's
   recent #PF wrote.**
5. Eventually scheduler returns to task A. A's stack-local snapshots are
   still good for *its* exit, but any code that touches `current->mm`,
   `current->active_mm->context.id.kvm_shadow`, or any per-mm state now
   sees the post-B view, and any subsequent `KVM_GET_*` ioctl reads B's
   residue.

The snapshot fix (commit `b516bee62eb2`) closed the easy case (reading
`run->exit_reason` post-unblock). But the fix is incomplete *by class*:
**every `KVM_*` ioctl issued after `unblock_signals` is racing**. Examples
still in tree:

- `out_read_regs` path (the `EINTR` branch) issues `KVM_GET_SREGS` /
  `KVM_GET_REGS` after unblock — see the comment block at thread.c:4622.
- The diagnostic `KVM_GET_VCPU_EVENTS` dump on panic (acknowledged
  "best-effort" at thread.c:3547).
- `kvm_fpu_save_for_task(prev)` in `kvm_context_switch` (line 339) issues
  `KVM_GET_FPU` against the singleton vCPU — but `prev`'s FPU was
  *already overwritten by next's KVM_RUN* by the time we get here on the
  fall-through path, because the snapshot of FPU happens AFTER
  `switch_threads` will land us in next, and `KVM_GET_FPU` then races
  whichever task has the vCPU.

**Why every previous fix missed it:** the fixes have been at the
read-site granularity (snapshot the exit, snapshot the IST). The race
class is at the *ownership* granularity: the singleton `vcpu0_fd`/`run0`
have N concurrent owners under the cooperative scheduler, and there is no
lock — only the false hope that "block_signals around KVM_RUN" is enough.
It is not enough, because the `interrupt_end()` in `userspace()` (line
609 of `arch/um/kernel/process.c`), the syscall handler's own `schedule()`
calls (e.g. `arch/um/kernel/skas/syscall.c:95`), and `do_signal` all yield
*outside* the block window. The post-VMEXIT region has no protection at
all.

**Why ~50 µs of latency hides it:** `printk(KERN_INFO …)` blocks SIGALRM
internally while it runs (logbuf lock + console UART flush). 50 µs of
busy-spin with signals unblocked covers the worst-case SIGALRM-to-handler
latency on the host. So the diagnostic latency is doing two things at
once: (a) ensuring the next SIGALRM wakeup *fires inside the spin* rather
than between snapshot and next dispatch, (b) draining any in-flight
syscall side-effects. Either way it's masking the lack of ownership.

### Race B — direct shadow sync vs full fill, even WITH the seqlock

**Where:** `arch/um/backend/kvm/shadow_sync.c:153` (`kvm_shadow_sync_pte`)
versus `arch/um/backend/kvm/lifecycle.c:1173` (`kvm_shadow_fill_from_uml_pgd`).

The "task #90 seqlock" added at lifecycle.c:1228 (snap `mut_head_seq`) and
1485 (re-check) is meant to detect the case where a direct-sync writer
fires during the fill walk. **But the seqlock catches the producer's
existence, not the destination of its write.** Specifically:

1. Fill clear-pass clears slot S (lifecycle.c:1315 `spte[pte_i] = 0`).
2. Fill install-pass arrives at slot T (T ≠ S), starts walking
   `kvm_shadow_table_step` to allocate intermediate tables.
3. **Concurrent direct sync writes slot S** to a fresh PTE (shadow_sync.c
   line 287, the `WRITE_ONCE(*spte, new)` install path) — but this
   requires the path to *already exist* (line 252-265). It does, because
   the clear pass left the intermediate tables; only the leaf was zeroed.
4. Direct sync bumps `mut_head_seq` (kvm_shadow_record_mut →
   shadow_sync.c:76 `WRITE_ONCE(shadow->mut_head_seq, seq+1)`).
5. Fill install-pass finishes T; fill loop reaches S; fill writes S with
   the value translated from a UML PTE it read possibly BEFORE direct
   sync wrote slot S (the install pass does `ume = pte[pte_i]` at
   lifecycle.c:1390, then translates and `kvm_shadow_map_page` at 1411).
6. Fill checks `mut_head_seq` post-walk (line 1485). It advanced. So fill
   sets `needs_full_resync = true` (line 1486).
7. **But fill does NOT undo the install-pass write to slot S.** Worse,
   fill sets `WRITE_ONCE(shadow->dirty, true)` (line 1487) and returns
   `installed`. The KVM_RUN that follows now uses CR3 → shadow tree with
   slot S = stale-translated-X, even though `needs_full_resync` is true,
   because the consumer in `kvm_enter_guest` only checks
   `needs_full_resync` AT PREDICATE TIME (thread.c:2239). The current
   entry's predicate already passed.

The seqlock is a *post-hoc* signal. It tells the next entry to repair.
But it cannot rewind THIS entry's KVM_RUN, which is already happening
with the broken shadow. So the fill-vs-direct-sync race produces ONE
incorrect KVM_RUN per occurrence. With heavy malloc/free churn this is
exactly the test_int big-integer wrong-answer corruption pattern (memo 19
"NEW OBSERVATION 2026-04-26").

**Why every previous fix missed it:** every fix added another writer
producer-set / consumer-set transition, each one carefully ordered against
its own read. But the seqlock pattern only works when readers can RETRY.
KVM_RUN cannot retry a guest mutation. The architecture asks for
single-writer-per-shadow semantics; we have three concurrent writers
(fill, direct-sync, invalidate) and an in-band "next entry will repair"
that is *too late*.

### Race C — `kvm_mm_unmap` invalidates ONE shadow tree but `os_unmap_memory` mutates the host VA — and the guest may already be running off a different mm's CR3 by the time we get here

**Where:** `arch/um/backend/kvm/mm.c:198` (`kvm_mm_unmap`) +
`arch/um/backend/kvm/mm.c:243` (`kvm_shadow_invalidate_va_range(id->kvm_shadow,…)`).

`kvm_mm_unmap` is called from `um_tlb_sync(mm)` paths. The fix at #274
keystone (mm.c:175) routed invalidate to `id->kvm_shadow`, not
`current`'s shadow. Good. But:

- `os_unmap_memory((void*)virt, len)` (mm.c:233) is a `munmap(2)` on the
  *host's* address space. The host VA (e.g. user VA `0x4003b000`) is
  unmapped from the UML host process.
- This is required because UML kernel code does `copy_*_user` via the
  host VA alias (the `os_map_memory` mention at mm.c:140-149 confirms
  this is load-bearing — without it cumulative imports crash).
- BUT the SHADOW PT for that mm still maps GVA `0x4003b000` → GPA
  (the original PFN inside the memslot). The KVM memslot covers
  `[uml_physmem, uml_physmem + physmem_size)` (a slab of host memory
  underneath UML's physmem region — confirmed by the diag log
  `host_va=60000000 size=20000000` at memo 19 line 313). The leaf is
  zeroed by the invalidate, but **only after the host munmap has
  returned**.
- Between `os_unmap_memory` and the leaf clear, the guest can fault on
  GVA `0x4003b000` if it's resumed. The guest #PF handler (thread.c:3631)
  computes `cr2`, calls `handle_page_fault` which walks UML's vmas, finds
  no VMA there (it's been unmapped), and tries to deliver SIGSEGV — but
  the user code that faulted was in the middle of a glibc routine that
  expected the page to be there. End result: a glibc-internal SIGSEGV.

This window is small but reachable. With cooperative scheduling, signal
delivery between `os_unmap_memory` and the invalidate IS one of the spots
the 50 µs of latency would skip past.

### Race D — `set_pte_at` from one task races `kvm_shadow_fill_from_uml_pgd` from another task on the same mm

**Where:** `arch/um/include/asm/pgtable.h:341-395` (`set_ptes`) calls
`kvm_shadow_sync_pte` for each new PTE. If two tasks share an mm
(CLONE_VM threads), both can be inside `set_ptes` and one can be inside
`kvm_enter_guest`'s fill path simultaneously.

Memo 17 Phase I added `mmap_read_lock(mm)` (thread.c:2304) around the
fill, which protects against concurrent unmap freeing intermediate page
tables. But it does NOT serialize against `set_pte_at` on present pages
— `set_pte_at` is called *under* `mmap_read_lock` already, and N
readers can hold the lock simultaneously. So:

1. Task X holds `mmap_read_lock(mm)`, is inside `set_ptes` writing PTE
   for VA V, has just done `set_pte` (pgtable.h:375) but not yet reached
   the `kvm_shadow_sync_pte` call (line 393).
2. Task Y holds `mmap_read_lock(mm)` (entered `kvm_enter_guest`), takes
   `fill_lock`, walks the UML pgd, reads PTE for VA V → sees the
   *new* value (X's set_pte already wrote it).
3. Y translates and writes shadow leaf for V.
4. X finally calls `kvm_shadow_sync_pte` for V, writes the *same* value
   — fine, idempotent.

That sub-case is fine. But the race that ISN'T fine:

1. Task X is in `set_ptes` for VA V; has NOT yet executed `set_pte(ptep,
   pte)` (pgtable.h:375). UML PTE still holds OLD value.
2. Y walks UML pgd, reads OLD value, writes shadow OLD.
3. X writes UML PTE NEW (line 375).
4. X calls `kvm_shadow_sync_pte` (line 393) — direct sync. Direct sync
   walks shadow, finds slot, `READ_ONCE(*spte)` returns OLD (Y just
   wrote it), then `WRITE_ONCE(*spte, NEW)` (shadow_sync.c:287). Good.
5. **But X's `kvm_shadow_sync_pte` ran with `shadow->dirty = true`
   already set by Y's fill. Y now finishes fill, `mut_head_seq` has
   advanced, Y sets `needs_full_resync` and returns.**

The problem is more subtle: between Y's fill completing and X's direct
sync running, Y can drop fill_lock, drop mmap_read_lock, fall through
the `cmpxchg(needs_full_resync, true, false)` block (thread.c:2321),
and proceed into KVM_SET_SREGS / KVM_RUN with NEW. In this case Y read
NEW so it's fine.

The actual broken interleaving is when X's `set_pte` hasn't happened
*before* Y reads the PGD. That's just the basic point — UML's pgd is
mutated lock-free w.r.t. KVM's view. The seqlock catches it AFTER, but
"after" means "next entry"; this entry runs with whatever view fill
locked in. **The "next entry repairs" recovery contract is incompatible
with the guest having performed mutations in between.**

### Race E — IRETQ frame is per-mm, but multiple tasks share an mm

**Where:** `arch/um/backend/kvm/kvm_backend.h:564-567` (per-mm
`iretq_frame_*`). Memo 18 Phase 2 promoted the IRETQ frame from singleton
to per-mm. The comment at thread.c:3370-3387 acknowledges *exactly* this
race remains: same-mm tasks share the per-mm IRETQ frame page.

Block-signals at thread.c:3388 covers the kvm_enter_guest → KVM_RUN
window. But:

- Block_signals only blocks SIGALRM (the timer). It does not prevent
  another host kernel thread from running. UML itself is single-host-
  threaded by design, so on a vanilla UML config this is OK.
- However `kvm_enter_guest` itself can SLEEP — `mmap_read_lock`
  (thread.c:2304) can block. Inside that block `block_signals` is on.
  But the moment `mmap_read_lock` returns, control resumed via the host
  kernel scheduler — which restored signals on syscall return. The
  comment at thread.c:3382-3387 implicitly assumes block_signals on the
  UML-side is enough. It is not, because UML's `block_signals` is just a
  flag check inside the SIGALRM/SIGIO handler stub
  (`arch/um/os-Linux/signal.c:333-355`). It does NOT call sigprocmask.
  Pending signals can still arrive at the host kernel level and wake
  blocked syscalls.

So: even within block_signals, mmap_read_lock can yield via futex, and
when we return, the per-mm IRETQ frame may have been overwritten by
sibling-thread B that ALSO went through kvm_enter_guest in between.

### Summary

| Race | Singleton resource at fault | Smallest fix that closes it |
|------|-----------------------------|-----------------------------|
| A | vCPU0 fd + run0 mmap + IST page | Per-task vCPU |
| B | shadow PT (seqlock retry impossible mid-KVM_RUN) | Single-writer (no fill) |
| C | host-VA aliasing (os_map_memory) | Stop using host VA — KVM-native uaccess |
| D | `set_pte_at` lock-free vs fill lock-with-acquire | Single-writer or full-fence |
| E | Per-mm IRETQ frame shared across same-mm tasks | Per-task IRETQ frame OR per-task vCPU |

Note that **A and E both end at "per-task vCPU"**, **B and D both end at
"single-writer" (which is what TDP/EPT gives you — KVM owns the shadow,
no UML-side fill at all)**, and **C ends at "stop pretending host-VA
aliases the guest"**. The three structural fixes collapse the entire
five-race table.

---

## 2. Root cause catalog (3-7 structural problems, by impact)

### RC-1 (highest impact): Singleton vCPU multiplexed by cooperative threading

`kvm_um` holds ONE `vcpu0_fd` (`lifecycle.c:74`), ONE `run0`
(`lifecycle.c:75`), ONE `kvm_bootstrap_page_stack` (IST). Every UML
"task" `longjmp`s onto this same vCPU. After `KVM_RUN` returns,
ALL state on the vCPU (FPU, MSRs, sregs, sync_regs mmap, GP regs, the
IST stack memory) belongs to whoever ran last. The `arch_thread.kvm`
per-task FPU/events save (thread.c:338-341) is a band-aid catch-up; the
fundamental problem is that the vCPU isn't owned by a task.

**This is the parent of Race A and Race E**, and it's the parent of the
"50 µs latency hides it" observation: latency is masking ownership
violations.

### RC-2: Shadow PT with multiple concurrent writers and a "next-entry repairs" recovery contract

`kvm_shadow_mm` has FOUR write paths:
- lazy fill on entry (`kvm_shadow_fill_from_uml_pgd`, lifecycle.c:1173)
- direct per-PTE sync on `set_pte_at` (`kvm_shadow_sync_pte`,
  shadow_sync.c:153)
- range invalidate from `kvm_mm_unmap` (`kvm_shadow_invalidate_va_range`,
  lifecycle.c:1650)
- direct invalidate from `flush_tlb_*` (`kvm_shadow_sync_va/range_atomic`,
  shadow_sync.c:393, 404)

The seqlock added at task #90 only DETECTS races between fill and
direct-sync. It does not prevent mid-entry corruption — the recovery
mechanism (`needs_full_resync`) is consumed at the START of the NEXT
entry, never the end of the current one. **An entry that started with a
clean predicate and got passed by a concurrent writer mid-fill commits a
known-bad shadow to KVM_RUN.** This is exactly the test_int big-integer
mid-compute corruption.

The "12 transition counters and 3 flags" (`dirty_set_*`,
`synced_*`, `needs_full_resync_*` — `kvm_backend.h:580-616`) is the
symptom: the state machine has accreted complexity to track WHICH
producer wrote, because the consumer needs the information to know what
to repair next. **The complexity is load-bearing for telemetry but not
for correctness — it cannot make a multi-writer scheme correct in the
face of mid-KVM_RUN concurrency.** The complexity is papering over
RC-2's structural flaw.

### RC-3: Memory bridge through host-VA aliasing (`os_map_memory`/`os_unmap_memory`) instead of KVM memslots

The KVM memslot covers `[uml_physmem, uml_physmem + physmem_size)` only
— a single fixed slab of host memory. Guest user VAs (`0x4003b000`
etc.) live OUTSIDE the memslot. They are reached at guest-side via the
shadow PT walking GPAs that DO lie inside the memslot, but at host-side
via host VAs (`mm.c:150 os_map_memory`). UML kernel code does
`copy_*_user`, sigframe setup, and io_uring buffer-fixed setup THROUGH
the host VA alias.

Three independent failure modes flow from this:
- Race C above (host munmap before shadow invalidate).
- Stale host-VA mapping after a COW / mremap that changes which physical
  PFN backs a user VA, while the shadow leaf still points at the old PFN
  — memo 19 ld-linux scenario at line 333-346.
- Memory-content audit (`kvm_shadow_audit_content_va` in `kvm_backend.h
  :682`) had to be added because the **three views (UML pgd → __va,
  shadow → memslot, host-VA dereference) can disagree**. That such an
  audit is necessary is itself diagnostic — they shouldn't be able to
  disagree.

### RC-4: `block_signals`/`unblock_signals` are not lock equivalents but are used as if they were

`unblock_signals` (`signal.c:345`) just sets a flag and re-runs queued
SIGALRM handlers. It does not prevent host-side preemption, futex wakes,
or in-host-syscall yields. The KVM backend uses block_signals as if it
were a mutex protecting the singleton vCPU + IST + run0. It is not.
`mmap_read_lock` inside `kvm_enter_guest` (thread.c:2304) is the obvious
yield point; less obvious are the implicit yields inside any host
syscall (KVM_SET_SREGS itself can block on KVM internal mutexes).

### RC-5: `set_pte_at` hooks are inline at the pgtable layer with no atomic/sleepable distinction

`pgtable.h:147-152` (`pte_clear`), `:341-395` (`set_ptes`), and the
`flush_tlb_*` hooks (`tlbflush.h:48-77`) call into `kvm_shadow_sync_pte`
unconditionally. The hook is documented "atomic-context safe" because
generic mm code calls these under `pte_lockptr` spinlock — so the hook
cannot allocate. Allocation failure is signaled via `needs_full_resync`
(shadow_sync.c:253), deferred to "next entry". **This forces the
multi-writer architecture: we cannot fail back to "let kvm_enter_guest
do it" without also accepting "current entry runs with a stale
shadow"**. RC-5 is the inline-hook constraint that locks in RC-2.

### RC-6: Cooperative threading via `longjmp`/`switch_threads` is fundamentally incompatible with KVM's "vCPU is a thread" model

`switch_threads` (`os-Linux/skas/process.c:627`) is a `setjmp`/`longjmp`
pair on the SAME host thread. The host kernel does not know UML
context-switched. The KVM vCPU FD is bound to the host thread that
called `KVM_CREATE_VCPU` (it can be moved with `pthread_setaffinity` /
`KVM_RUN` on a different thread, but state like `kvm_run` mmap and the
sregs cache still live on the singleton FD). KVM's threading model
assumes ONE host thread = ONE vCPU; UML's cooperative model assumes ONE
host thread = N UML tasks. **These are incompatible at the design
level.** Every "save FPU on switch_threads" patch is fighting that
incompatibility.

### RC-7: SIGALRM-driven scheduler tick lives in the same signal namespace as KVM exit-and-host-signal handling

UML's scheduler is driven by host SIGALRM (see UML timer code).
`KVM_RUN` returns `-EINTR` when a host signal arrives. The post-VMEXIT
dispatch path has to handle EINTR (`out_read_regs` branch) and not
confuse "I got EINTR'd" for "I exited normally". The current code does
this case-by-case (the snapshot block at thread.c:3483 handles both
`rc >= 0` and `rc == -EINTR`). But the deeper issue: **the same SIGALRM
that drives scheduling can interrupt the post-VMEXIT dispatch and yield
to another task BEFORE this task's interrupt_end runs**, dropping the
HOST_IP that this task's syscall handler was about to install (see the
"per-trap shape" discussion at thread.c:3396-3424). Per-trap exit was
adopted as a band-aid; the underlying issue is shared signal namespace.

---

## 3. Why incremental fixes can't reach 0 %

The flake floor is ~10-25 % for a structural reason. Each fix is a
read-or-write reordering inside one of the four shadow writers, or a
snapshot of one more singleton field, or a tighter `block_signals`
window. **None of these change the cardinality of writers, the cardinality
of vCPUs, or the cardinality of host-VA aliases.** They reduce the
*probability* of each race firing, but each race is independently
sufficient to corrupt one KVM_RUN.

Concretely:
- Snapshot fix (commit `b516bee62eb2`) closed Race A's read-side. The
  write-side of A (singleton vCPU FD) is untouched. Diagnostic runs
  now show test_int wrong-answer corruption (Race B/D class) appearing
  in slots that the snapshot fix left exposed.
- Each cmpxchg-on-flag-clear adds reorder safety to ONE producer/consumer
  pair, but ANOTHER producer can still fire mid-fill (Race B).
- Per-mm IRETQ frame (Memo 18 Phase 2) closed cross-mm contamination but
  Race E (same-mm sharing) remains and is the dominant residual.
- The seqlock at task #90 detects fill-vs-direct-sync but cannot rewind
  the install-pass write that the seqlock then reports as racy.

The math:
```
P(failure) = 1 - product over each independent race of P(no-race)
```
Each race window is in the microsecond range; signal latency is also
microseconds. Even at 99 % no-race per race, five independent races
gives `1 - 0.99^5 ≈ 4.9 %` per KVM_RUN. With ~thousands of KVM_RUNs per
test, the per-test flake floor is asymptotically 100 % unless each
individual race is brought to ZERO probability — and the only way to
make a race zero-probability is to remove the resource it races on.
**Hence: structural fix, not probabilistic fix.**

The 50 µs udelay diagnostic experimentally confirms this: it reduces
ld-linux failures to 0/30 in the unit-test reproducer, but only because
50 µs > the worst-case window for ALL the races A-E to fire on one
trial. The cost is 5 % overhead AND it shifts the failure mode (test_int
now fails mid-compute), which means the wider window simply re-exposes a
DIFFERENT race. There is no single delay value that is simultaneously
big enough to dominate every race window AND small enough to not perturb
the workload.

---

## 4. Redesign blueprint — make the bug class impossible

The redesign is opinionated and addresses each RC at the structural
level. It is presented as ONE coherent shape, not a menu of options,
because the RCs interlock: half-doing it makes things worse.

### Blueprint shape

```
+-----------------------------------------------------------+
|  UML host process                                         |
|                                                           |
|  Scheduler thread (the existing UML "kernel thread") -----+
|    Owns: UML mm/task/sched state                          |
|    Drives KVM via per-mm host worker pthreads, each       |
|    pinned to a vCPU FD.                                   |
|                                                           |
|  Per-mm worker pthread (one pthread per UML mm)           |
|    Owns: ONE vcpu_fd, ONE run mmap, ONE IST stack page    |
|    Owns: this mm's shadow PT — except we don't have one   |
|    Receives: register snapshot + "go" signal              |
|    Replies:  exit reason + register snapshot              |
|                                                           |
|  Memory bridge: ONE memslot per UML mm, registered at     |
|    mm_attach time, covering uml_physmem + each currently  |
|    mapped user VA range as a separate slot (or a single   |
|    overflow slot if KVM_USER_MEM_SLOTS is tight).         |
|    KVM's TDP/EPT walks UML's pgd directly via             |
|    mmu_notifier — no shadow PT we maintain.               |
+-----------------------------------------------------------+
```

### How this kills each race / RC

| RC | Eliminated by |
|----|---------------|
| RC-1 | Per-mm worker pthread — vCPU is owned by the worker, not multiplexed. UML scheduler can yield freely; the worker is just blocked in KVM_RUN. |
| RC-2 | Eliminate the shadow PT entirely. Use TDP/EPT. KVM is the single writer. UML's pgd changes propagate via `mmu_notifier` invalidation that the host kernel ALREADY supports. |
| RC-3 | Memslot-per-mm, no host-VA aliasing for guest memory. UML kernel-side `copy_*_user` becomes "ask the worker to do it" or uses a dedicated kernel-VA window. |
| RC-4 | Real lock semantics: per-mm worker has a real pthread mutex. block_signals goes away as a synchronization primitive. |
| RC-5 | `set_pte_at` no longer hooks shadow at all — it just updates UML's pgd; the mmu_notifier path tells KVM. |
| RC-6 | UML scheduler stays on its host thread; KVM stays on the worker thread. Each respects its own threading model. |
| RC-7 | Worker thread uses `pthread_sigmask` to block SIGALRM; scheduler thread receives SIGALRM. They are decoupled. |

### Concrete components

1. **Per-mm worker pthread** (`arch/um/backend/kvm/worker.c`, new):
   - On `kvm_mm_attach`: `pthread_create` the worker, which calls
     `KVM_CREATE_VCPU` and mmaps `kvm_run`. Worker blocks on a
     condition variable waiting for "go".
   - On `kvm_mm_detach`: signal worker to exit, `pthread_join`.
   - Worker loop:
     ```
     for (;;) {
         wait_for_go();
         ioctl(vcpu_fd, KVM_RUN);
         signal_back_with_exit();
     }
     ```
   - "go" payload: full uml_pt_regs. "back" payload: full uml_pt_regs +
     exit_reason.
   - The worker's pthread_t is stashed in `mm->context.id.kvm_worker`.

2. **`kvm_run_userspace` becomes a synchronous request to the worker:**
   ```c
   void kvm_run_userspace(struct uml_pt_regs *regs) {
       struct kvm_worker *w = current->active_mm->context.id.kvm_worker;
       kvm_worker_send(w, regs);  // copies regs, signals cond
       kvm_worker_recv(w, regs);  // blocks; signal-safe wait
       dispatch_exit(regs, w->last_exit);
   }
   ```
   The UML scheduler can yield at the recv. The worker is blocked in
   `KVM_RUN` until the guest exits. Switching UML tasks doesn't touch
   the vCPU. The vCPU's FPU/MSRs/sregs are stable across an arbitrary
   UML scheduling sequence — they only change when the worker's mm runs.

3. **Eliminate shadow PT.** Use KVM_TDP via `KVM_CAP_X86_TDP` (always
   enabled on modern KVM with EPT/NPT). Memslots-per-mm:
   - At `kvm_mm_attach`: register memslot for `uml_physmem` slab.
   - At `kvm_mm_map`: register an additional memslot for the new user VA
     range, or extend an existing one.
   - At `kvm_mm_unmap`: deregister.
   - KVM internally maintains EPT/TDP based on memslot + mmu_notifier
     callbacks fired by host mm changes. UML pgd changes that affect
     guest visibility happen via the memslot register/deregister
     interface — synchronous, single-writer (KVM).
   - The "shadow PT" code (`lifecycle.c` shadow_*, `shadow_sync.c`,
     all the dirty/synced/needs_full_resync state) is DELETED.
   - The cr3 the guest sees IS UML's mm->pgd, walked by EPT; we don't
     intermediate it.

4. **Bootstrap pages (LSTAR trampoline, IST, gadget state, vvar)
   become a fixed memslot** registered once at `kvm_mm_attach`. These
   are kernel-half guest VAs pointing at PFNs in the bootstrap
   memslot. Mapping is via UML's pgd at the appropriate kernel-half
   slot, which UML kernel code sets up at mm_attach.

5. **Host-VA aliasing for `copy_*_user` is replaced by the memslot
   userspace_addr.** UML kernel code can still do `(char __user
   *)addr` reads, but the read goes through the memslot's
   userspace_addr (= host VA backing the memslot region). Since the
   memslot covers exactly the user VA range, there's no aliasing to a
   different host PFN — the host VA IS the GPA's backing, and changes
   propagate via mmu_notifier.

6. **Cooperative threading lives only on the scheduler side.**
   `switch_threads` continues to longjmp between UML tasks on the
   scheduler thread. But it never touches a vCPU FD. The worker
   threads don't participate in cooperative scheduling.

### What this does NOT do

- Does not require per-task vCPU. Per-mm is sufficient and matches
  Linux's notion of a "thread group sharing an mm". Per-task would
  require N vCPUs per process, which exhausts KVM_MAX_VCPUS quickly.
- Does not require per-task host pthread. UML tasks remain
  cooperative on the scheduler thread; they just delegate guest
  execution to the per-mm worker.
- Does not require copy from gVisor / Firecracker. The shape is
  derivable from "what does KVM's threading and memory model
  actually want, and how can UML provide it without lying."

---

## 5. Migration path — file-by-file

### Phase A: introduce per-mm worker without removing shadow PT (de-risking)

| File | Change |
|------|--------|
| `arch/um/backend/kvm/worker.c` (new) | pthread create/join, condvar protocol, `kvm_worker_run(regs)`. |
| `arch/um/include/skas/mm_id.h` | Add `void *kvm_worker` field. |
| `arch/um/backend/kvm/mm.c` | `kvm_mm_attach` creates worker (after shadow alloc); `kvm_mm_detach` joins. |
| `arch/um/backend/kvm/lifecycle.c:kvm_init` | Stop creating singleton `vcpu0_fd`/`run0`. Workers create their own. |
| `arch/um/backend/kvm/thread.c:kvm_run_userspace` | Replace direct ioctl with `kvm_worker_run(regs)` round-trip. |
| `arch/um/backend/kvm/thread.c:kvm_enter_guest` | Move SREGS/REGS setup into the worker; kvm_enter_guest just builds the request payload. |
| `arch/um/backend/kvm/thread.c:kvm_context_switch` | Drop FPU save/restore (worker owns the vCPU; FPU stays). Keep `um_tlb_sync(prev->active_mm)` — still needed for shadow PT during phase A. |

After Phase A: Race A and Race E die. ~50-70 % flake reduction expected
(Race A is dominant; Race E is the post-Race-A residual).

### Phase B: replace shadow PT with TDP + memslots

| File | Change |
|------|--------|
| `arch/um/backend/kvm/lifecycle.c` | Delete `kvm_shadow_*` (fill, invalidate, map_page, audit). ~1500 lines gone. |
| `arch/um/backend/kvm/shadow_sync.c` | DELETE entire file. |
| `arch/um/backend/kvm/kvm_backend.h` | Delete `struct kvm_shadow_mm` + lifecycle prototypes + the 12 transition counters. |
| `arch/um/include/asm/pgtable.h` | Remove `kvm_shadow_sync_pte` calls from `set_ptes`/`pte_clear`. Restore `set_ptes` to upstream shape. |
| `arch/um/include/asm/tlbflush.h` | Restore `flush_tlb_*` to non-KVM-aware versions. |
| `arch/um/backend/kvm/mm.c:kvm_mm_map` | Replace `os_map_memory + invalidate` with `KVM_SET_USER_MEMORY_REGION` (add slot for new range). |
| `arch/um/backend/kvm/mm.c:kvm_mm_unmap` | Replace with `KVM_SET_USER_MEMORY_REGION` (remove slot). |
| `arch/um/backend/kvm/thread.c:kvm_enter_guest` | CR3 = UML mm->pgd directly (TDP walks it). Remove shadow_* calls (already in Phase A this is mostly mechanical). |

After Phase B: Races B, C, D die. Residual flake should be at host-KVM-bug
level (rare).

### Phase C: cleanup

- Delete `kvm_shadow_audit_*` diagnostics.
- Delete `kvm_diag_skip_fpu_save` and related kparams.
- Delete the `dirty/synced/needs_full_resync` state machine docs.
- Update `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  memos to reflect the new shape.
- ~3000-4000 lines of code deleted across the backend.

---

## 6. Estimated effort and risk

| Phase | Effort | Risk |
|-------|--------|------|
| A (per-mm worker, keep shadow) | 2-3 weeks | Medium — pthread interaction with UML's existing signal handling needs careful design. UML may need a dedicated SIGRT signal for worker→scheduler completion. Validation: cpython parity gate must hit at least baseline (17/21) AND ld-linux NULL deref must drop to 0/30 in the test_decimal reproducer. |
| B (TDP + memslots) | 4-6 weeks | High — `KVM_SET_USER_MEMORY_REGION` has a per-VM slot count limit (typically 32k on modern KVM, but each slot has overhead). Heavy malloc workloads with many distinct vmas may hit it. Mitigation: coalesce adjacent slots; fall back to a single overflow slot for pathological cases. Validation: cpython parity gate to 21/21 OR a documented limit. |
| C (cleanup) | 1 week | Low. |

Total: ~7-10 weeks for a complete redesign that makes the entire bug
class impossible.

The risk of NOT doing this: the current shape can be patched indefinitely,
each patch reduces P(flake) by ~30 %, but the asymptote is non-zero. The
~25 % flake floor is structurally guaranteed by the cardinality of races
and singletons. Continuing to patch will produce diminishing returns and
eventually the "next-residual race" will be one we cannot identify
because the diagnostic latency required to expose it perturbs the
workload more than it helps.

---

## Appendix — answers to the specific questions

**Q1 (exact race for heavy Python heap-alloc):** Combination of Race B
(direct sync vs fill, mid-KVM_RUN corruption) and Race D (set_pte_at
lock-free vs fill mmap_read_lock). Heavy Python workloads do millions of
`brk()` / `mmap()` / `munmap()` cycles which translate to millions of
`set_pte_at` calls; each one is a chance to interleave with a
`kvm_enter_guest` that's mid-fill. The seqlock detects but cannot
rewind; the current entry's KVM_RUN runs with corrupted shadow.

**Q2 (cooperative threading + singleton vCPU + per-mm shadow PT
compatibility):** No. KVM's design is "1 host thread = 1 vCPU". UML's
`switch_threads` violates that by putting N tasks on 1 host thread that
owns 1 vCPU. Every "save vCPU state on switch_threads" patch is fighting
the design, and per-mm shadow PT compounds the problem because it makes
mm changes another implicit vCPU-state change.

**Q3 (12 transition counters + 3 flags):** The complexity is papering
over RC-2. The flags themselves describe a state machine that *cannot
be correct* under multi-writer mid-KVM_RUN concurrency — the counters
are telemetry to debug what producer fired, but no telemetry can fix the
"the entry already ran with the broken state" problem.

**Q4 (multi-writer architecture salvageable?):** No. Single-writer is
the only sound shape. KVM's TDP/EPT IS that single writer (KVM owns the
nested page tables; UML notifies via memslot register/unregister and
mmu_notifier).

**Q5 (UML idioms incompatible with KVM):**
- `switch_threads` (longjmp) on a thread that owns a vCPU FD: vCPU
  state must be checkpointed on every switch, and KVM state checkpointing
  is not free.
- SIGALRM-driven scheduler tick: same signal namespace as host-signal
  EINTR for KVM_RUN, leading to confusing post-VMEXIT state.
- `os_map_memory`/`os_unmap_memory` host-VA aliasing: the host kernel
  knows nothing about the KVM memslot view, so munmap-then-shadow-clear
  has a window.
- `block_signals`/`unblock_signals` as a synchronization primitive: only
  blocks SIGALRM at the UML stub layer; does not prevent host kernel
  preemption or futex yields.

The fix is not to remove these idioms from UML — they are correct for
ptrace/seccomp backends. The fix is to keep them on the UML scheduler
side and decouple the KVM side onto its own pthread per mm, where KVM's
threading model is honored.
