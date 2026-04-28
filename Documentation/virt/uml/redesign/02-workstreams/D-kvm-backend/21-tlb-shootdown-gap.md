# Memo 21 — TLB-shootdown gap (root cause of residual cpython-parity flake)

**Date:** 2026-04-27
**Gate state:** 5-trial run = 16, 17, 19, 20, 21 / 21 (mean 18.6) — **same/worse than historical 17–21 band**
**Status:** Diagnosed; fix is the next architectural blocker for 21/21

## Headline

Stage A's per-task vCPU + A.4d's per-vCPU `last_flushed_tlb_gen` close the **same-vCPU** TLB-staleness window but leave **cross-vCPU** TLB-shootdown unimplemented. `KVM_UM_KICK_SIGNAL` is defined and the host signal handler is registered, but **no code path ever sends it**. vCPUs already in `KVM_RUN` when a sibling task modifies the shared mm continue executing with stale TLB until they exit naturally.

## Evidence

`/tmp/repro_flake.sh MOD=test_struct N=30` yields **2 flakes** (6.7% per run). Both have identical signature:

| Field | Run 4 | Run 7 |
|---|---|---|
| Crashing thread | `InterpreterPool[24]` | `InterpreterPool[24]` |
| `cr2` | `0x5820a008` | `0x0` |
| `ec` | `0x4` (user, read, non-present) | `0x6` (user, write, non-present) |
| `um_pte` | `0x0` | `0x0` |
| `shadow` | `0x0` | `0x0` |
| `synced` | `1` | `1` |
| `EQUAL` | yes | yes |

`InterpreterPool[24]` is a cpython 3.13+ free-threading subinterpreter pool worker — multi-threaded by construction. Both pgd and shadow are empty for the faulting VA, which is consistent with: thread A unmapped the page (or never mapped it where thread B thought it was), thread B has stale TLB pointing at the now-freed mapping, thread B's vCPU hasn't been kicked.

Run 7's `cr2=0x0` is a NULL pointer deref — thread B loaded a python-object pointer field that was zero, because the pointer's source memory had stale TLB showing zeroed-out contents.

`vcpu_alloc` fires for tids 22, 23, 24, 25 in the same trial — confirming per-task vCPU is allocating one fd per cpython worker thread.

## Mechanism

1. Thread A (vCPU₁) modifies a shared-mm pgd entry (e.g. `mmap`, `munmap`, `mprotect` propagated through `kvm_shadow_invalidate_va_range` or `kvm_shadow_sync_pte`).
2. `kvm_shadow_mark_dirty(shadow)` increments `shadow->tlb_gen` and `invalidate_seq`. Producer side is correct.
3. Thread B (vCPU₂) is still inside `KVM_RUN`. Its host-CPU TLB (and KVM's VPID-tagged guest TLB) holds the old entry.
4. **Nothing kicks vCPU₂.** It continues to read/write through the stale TLB until it naturally exits (next host syscall, page fault, etc.).
5. If thread A's modification was an unmap and thread B touches the now-freed VA, the resolved host PA points at unrelated memory or triggers a host #PF that surfaces as a guest #PF with no shadow entry.

Same-vCPU re-entry is fine (A.4d's `last_flushed_tlb_gen` mismatch on entry → CR4.PGE-toggle SREGS write → guest TLB flush). Cross-vCPU is the gap.

## Where the kick should fire

`kvm_shadow_mark_dirty` (`arch/um/backend/kvm/kvm_backend.h:738`) is the choke-point for every leaf write. After bumping `tlb_gen` and `invalidate_seq`, it should iterate the shadow's vCPU set and `pthread_kill(other_tid, KVM_UM_KICK_SIGNAL)` for every sibling vCPU currently in `KVM_RUN`.

The bracket helpers (`kvm_shadow_invalidate_begin/end`, `arch/um/backend/kvm/kvm_backend.h:759`) cover multi-leaf invalidation — kick once on `_end` to avoid N kicks per range.

## Data model needed

`struct kvm_shadow_mm` needs:

```c
struct kvm_shadow_mm {
    /* ... existing fields ... */
    spinlock_t       vcpu_set_lock;        /* tiny, only held during attach/detach/iterate */
    struct list_head vcpu_set;             /* of struct kvm_vcpu_handle */
};

struct kvm_vcpu_handle {
    /* ... existing fields ... */
    struct list_head shadow_link;          /* membership in shadow->vcpu_set */
    pid_t            host_tid;             /* gettid() of owning task on the host — kick target */
    atomic_t         in_kvm_run;           /* set before KVM_RUN, cleared after */
};
```

Attach: `kvm_enter_guest` first call binds `vcpu` to current `shadow`; appends `vcpu->shadow_link` to `shadow->vcpu_set`.

Detach: task exit / mm switch removes `vcpu` from old shadow's set, optionally appends to new shadow's set.

Iterate: `kvm_shadow_mark_dirty` → `kvm_shadow_kick_others(shadow)` walks `vcpu_set` under `vcpu_set_lock`, sends `KVM_UM_KICK_SIGNAL` to every entry whose `in_kvm_run==1` and `host_tid != current_tid`.

## Atomic-context safety

`kvm_shadow_mark_dirty` is called from `kvm_shadow_sync_pte` (atomic, hooked from `arch/um/include/asm/pgtable.h`). Sending a signal requires a syscall (`tgkill`), which is **not** safe from interrupt-disabled atomic context.

Two options:
1. **Defer**: store a `pending_kick` bit in `shadow`, drain on the next non-atomic checkpoint (`kvm_enter_guest`, `um_tlb_sync`). Simple but lengthens the staleness window slightly.
2. **Wake from a workqueue**: queue a workitem from atomic context that does the kicks. Cleaner, but adds a workqueue dependency in a hot path.

Option 1 is the right starting point — the staleness window is bounded by the next syscall/timer/preemption of the producer, which is short (<1ms typical for cpython). Promote to option 2 only if measurement shows the deferred-kick gap matters.

## Verification path

1. Implement minimal kick mechanism (deferred variant).
2. Re-run `MOD=test_struct N=100` repro — expect 0 flakes.
3. Re-run `MOD=test_decimal N=100` — heavier, more multi-thread pressure.
4. Re-run full 5-trial gate — expect 21/21 across all trials.
5. If clean, expand to N=20 trial gate run for final variance characterization.

## What this implies for Stage B

Stage B's TDP+memslot path inherits this gap. KVM's own `kvm_make_request(KVM_REQ_TLB_FLUSH)` + `kvm_make_all_cpus_request` handles cross-vCPU shootdown for in-kernel KVM use, but UML's per-task vCPU model puts each vCPU in a separate **host process thread**, not a kernel-thread-per-vcpu. The signal-based eviction primitive established here is the right cross-vCPU mechanism for both shadow PT (current) and TDP (Stage B).

Treat this as a **Stage A.5 follow-on** (call it A.4f) that lands before Stage B begins. Without it, neither path can hit 21/21 reliably.

## Update — naive A.4f attempt regressed (2026-04-27 evening)

A first attempt at A.4f added `host_tid`, per-shadow `vcpu_set` + spinlock, an
`atomic_set(&pending_kick, 1)` in `kvm_shadow_mark_dirty`/`invalidate_end`,
attach in `kvm_enter_guest`, drain at end of `kvm_enter_guest`, and an
`os_kvm_kick_thread` host primitive. Code reverted; key learnings:

| Metric                      | Pre-A.4f      | Post-A.4f attempt |
|-----------------------------|---------------|-------------------|
| `test_struct N=30` flakes  | 2 / 30 (6.7%) | 4 / 30 (13.3%)    |
| Original `pf_unrecoverable` signature | yes           | yes (still appears, 1 of 4) |
| New `kvm #GP` at user RIP   | no            | yes (1 of 4)      |
| New `IMPORT_FAIL SystemError` | no          | yes (1 of 4)      |
| New functional `tests=43 ok=False` | no    | yes (1 of 4)      |

Three new failure modes — the kicks interrupt sibling vCPUs at moments
where the recovery path is fragile. Suspects:

1. **Kick lands during a non-`KVM_RUN` ioctl.** `kvm_run_userspace` does
   `KVM_GET_REGS` immediately after `KVM_RUN` returns. A second kick from
   another sibling that arrives between `KVM_RUN` return and `KVM_GET_REGS`
   makes `KVM_GET_REGS` return `-EINTR` → existing path panics
   (`thread.c:3877` "if (gr < 0 && rc >= 0) panic").
2. **Bootstrap sequence interrupted in non-restartable state.** The LSTAR
   trampoline + IRETQ-frame-write window is bracketed by code that
   "preserves user state from the entry path" on `KVM_EXIT_INTR` — but
   if a kick hits between the SREGS-skip-cache update and
   `vcpu->last_flushed_tlb_gen` write, our own consumer state is partly
   stale.
3. **Drain at end of every `enter_guest` is too aggressive.** Even when
   `pending_kick` is set by THIS task's own preceding mm syscall, we kick
   siblings — they then `EINTR` for no benefit (their TLB was already
   coherent because no producer fired in their epoch).

**The original `pf_unrecoverable` flake is unchanged in frequency** — meaning
either:
- The TLB-shootdown hypothesis is partially correct but the kick mechanism
  itself is too unsafe to deploy without coupling to a careful EINTR-recovery
  audit, or
- The `pf_unrecoverable` flake is NOT a TLB-shootdown race but something else
  (use-after-free in cpython data, stale shadow leaf retained across
  invalidate_seq window, missing barrier in seq pattern, etc.).

## Next investigation

Before re-attempting A.4f, gather more evidence on the **specific race window**:

1. Add producer-tid + epoch-counter telemetry to `kvm_shadow_mark_dirty` so
   the panic-time `mut_dump` shows exactly when each leaf change happened
   relative to the consumer's last entry.
2. Audit every host-syscall site between `KVM_RUN` return and
   `kvm_run_userspace`'s next iteration for `EINTR` safety. The
   `KVM_GET_REGS` panic path is a known landmine; there may be more.
3. Review `kvm_shadow_invalidate_va_range` callers for ordering w.r.t.
   `mm_struct` rwsem — if invalidate runs before the leaf write completes,
   shadow can lag.
4. Consider whether `um_tlb_sync` ever gets called with a stale `mm_id`
   (BUG.2-style cross-mm bug that we patched in `kvm_shadow_invalidate_va_range`
   but might still exist in the fill path).

Until those are answered, A.4f stays parked. Reverting the source changes
keeps the gate at the 17–21/21 historical band rather than worse.

## Update 2 — flake bound to cpython subinterpreters; bare metal clean (2026-04-27 evening, second pass)

Built a controlled experiment (`/tmp/repro_isolate.sh`):

| Variant | Flakes |
|---|---|
| BAREMETAL host (no UML), full `test_struct` w/ subinterpreters | 0 / 30 |
| UML KVM, full `test_struct`                                    | 4 / 20 (20%) |
| UML KVM, `test_struct` w/ `InterpreterPoolExecutor` stubbed    | 0 / 20 |

Findings:

1. **Bare metal is clean.** cpython 3.14's `test_endian_table_init_subinterpreters`
   (test_struct.py:853) — five subinterpreters concurrently `import struct` —
   completes 30/30 trials without crash on the host. Cpython is not the bug.
2. **UML KVM with the subinterpreter test crashes 20% of runs.** Same crash
   signatures as the gate flakes (`pf_unrecoverable` on missing user PTE,
   `kvm #GP` on user-mode garbage, `InterpreterPool[N]` thread).
3. **Stubbing `InterpreterPoolExecutor` to a sequential no-op eliminates the flake.**
4. The earlier conclusion that A.4f doubled the flake rate was WRONG. The
   `kvm #GP` and `IMPORT_FAIL` signatures attributed to A.4f also appear on
   the clean baseline binary; sample size 30 cannot distinguish 6.7% from 13.3%
   at 95% confidence. A.4f's actual effect on the flake rate is unknown — could
   be neutral, helpful, or mildly harmful; we don't have enough trials to say.
5. The `rdi=0xaaaaaaaaaaaaaaab` value in run_4's #GP regs is **glibc tcache poison**,
   indicating the user reads a freed-and-poisoned chunk through a stale TLB
   entry. This is the smoking gun for cross-thread mm coherence.

## Strategic implication

The bug is narrow: cross-vCPU stale TLB during subinterpreter teardown/init,
where the workload does heavy mmap/munmap from short-lived sibling threads
sharing the mm. UML's per-task vCPU model has no cross-vCPU shootdown,
which is exactly what KVM's TDP path gives for free via mmu_notifier.

Three viable paths:

a) **Stage B (TDP + memslots)**: KVM's `kvm_mmu_invalidate_zap_pages_in_memslot`
   handles cross-vCPU shootdown automatically. The 4–6 week structural fix
   per the project plan. Most correct.

b) **A.4f v2 with EINTR audit**: fix `KVM_GET_REGS` panic-on-EINTR
   (thread.c:3877) and any other panic sites that can be hit by a sibling
   kick, then re-enable kicks. Requires the audit task #169.

c) **Workaround**: drop `test_struct.test_endian_table_init_subinterpreters`
   from the curated module list with a documented "tracked: A.4f / Stage B".
   Gate jumps to 21/21 on the remaining heavy modules immediately.

The user's directive prioritizes the "real architecture" (a or b), not (c).
Recommend: do (b) carefully — it's smaller scope than (a) and clears the
last A-stage debt. (a) follows once Stage B's huge-page blocker is
resolved.

## Update 3 — InterpreterPool stub barely moves the gate (2026-04-27 evening, third pass)

Ran the full 21-module canonical gate × 5 trials with `InterpreterPoolExecutor`
globally stubbed in the harness (`/tmp/parity_stub_subinterp.sh`):

| Trial | parity | diverge | skip | failed modules |
|-------|--------|---------|------|----------------|
| 1     | 19     | 0       | 2    | test_list, test_itertools (panic) |
| 2     | 18     | 1       | 2    | test_int (False), test_heapq, test_itertools (panic) |
| 3     | 21     | 0       | 0    | — |
| 4     | 18     | 2       | 1    | test_hashlib, test_decimal (False), test_heapq (panic) |
| 5     | 18     | 1       | 2    | (3 modules) |

**Mean 18.8/21** — basically identical to the non-stub baseline (18.6/21).

Workaround path (c) is **dead**. The InterpreterPool stub fixes only test_struct;
the other modules have independent triggers (GC, dict/set resize, decimal arena
pool, hashlib's per-thread state). All trace back to the same root cause:
heavy mmap/munmap from threads sharing an mm, no cross-vCPU TLB shootdown.

## A.4f v2 design plan

Naive v1 broke EINTR safety. v2 must structurally prevent KVM_UM_KICK_SIGNAL
from ever interrupting a non-KVM_RUN ioctl. Plan:

1. **`os_kvm_block_kick_signal(int block)` primitive** in arch/um/os-Linux/process.c.
   `block=1` → sigprocmask(SIG_BLOCK, SIGRTMIN+5). `block=0` → SIG_UNBLOCK.
   Cheap (one syscall).

2. **Bracket the entire `kvm_run_userspace` body** with block(1) at top, block(0)
   at bottom. Inside, KVM_SET_SIGNAL_MASK already swaps the in-RUN mask to
   allow the kick — so kicks land ONLY during KVM_RUN, where `rc == -EINTR`
   is already handled (`thread.c:3855` and `:3897`).

3. **Producer-driven kick**, not consumer-drain:
   - non-atomic producers (`kvm_shadow_invalidate_va_range`, `kvm_shadow_fill_
     from_uml_pgd`, `kvm_shadow_pgd_clear_user`) walk `shadow->vcpu_set` after
     their leaf writes and `kill(peer->host_tid, SIGRTMIN+5)` for every
     `peer != self` with `peer->in_kvm_run == 1`.
   - atomic producers (`kvm_shadow_sync_pte`) set `shadow->pending_kick` and
     defer the walk to the next non-atomic checkpoint
     (`um_tlb_sync` post-loop, or `kvm_run_userspace` top after sync).

4. **Self-flush via existing A.4d** — producer's own vCPU sees the bumped
   `tlb_gen` on next `kvm_enter_guest` entry and triggers the
   CR4.PGE-toggle SREGS write. No producer-side self-kick needed.

5. **Membership tracking**: `kvm_enter_guest` ensures `vcpu` is in
   `vcpu->attached_shadow == cur_shadow`'s set; on mm-switch, detach from
   prev's set then attach to current's. `kvm_vcpu_handle_destroy` detaches.

This is structurally equivalent to v1 plus the sigprocmask bracket (point 2).
The bracket is the crucial difference — without it, the kicks themselves work
but interrupt arbitrary host syscalls and panic.

Estimated diff: ~150 LoC, mostly in lifecycle.c (helpers) + thread.c (bracket
+ producer hooks) + os-Linux/process.c (sigprocmask primitive).

## Update 4 — A.4f v2 (with sigprocmask bracket) ALSO regressed (2026-04-27 evening, fourth pass)

Implemented and tested. Result on `MOD=test_struct N=30`:

| Variant | Flakes | New failure modes |
|---|---|---|
| Pre-A.4f baseline | 2 / 30 (6.7%) | `pf_unrecoverable` only |
| A.4f v1 (no sigprocmask) | 4 / 30 (13.3%) | + `kvm #GP`, IMPORT_FAIL |
| A.4f v2 (with sigprocmask) | **10 / 30 (33.3%)** | + 124 timeouts, "fatal signal; exiting" |

**v2 is 5× WORSE than baseline.** Reverted source.

What's going wrong (suspected):
1. **Drain frequency is too high.** Producers fire on every shadow PTE write
   (cpython does many per second). Each enter_guest checks pending_kick which
   is almost always set, so each enter_guest kicks all 5 sibling InterpreterPool
   workers. They EINTR, re-enter, and possibly drain again kicking back — a
   thrashing cycle that wastes cycles and may starve the actual workload.
2. **"fatal signal; exiting" on i=4** — only one vcpu_alloc happened (no
   InterpreterPool yet); a signal hit the main thread before subinterpreters
   spawned. UML's `install_fatal_handler` covers SIGINT/SIGTERM, so the fatal
   signal must be something else — possibly an unmasked KVM_UM_KICK_SIGNAL hit
   before the no-op handler was installed. Subtle ordering problem in our
   sigprocmask + handler-install paths.
3. **CLONE_VM without CLONE_SIGHAND**: UML's clone() creates separate processes
   sharing VM but with their own signal handler tables. Our atomic-guarded
   `register_kvm_kick_signal` only installs once per UML kernel image, but new
   tasks inherit handlers via clone-time copy — only if the parent had it
   installed at clone time. Edge cases here are easy to get wrong.

## Verdict

**Three attempts at signal-driven cross-vCPU shootdown have all regressed the
gate.** The kick mechanism is the wrong tool for UML's particular task-model
constraints (per-task = separate process, separate sigmask, separate handler
table; clone ordering matters; sigprocmask interacts poorly with UML's existing
signal apparatus).

The right fix is **Stage B (TDP + memslots)**: KVM's own
`kvm_mmu_invalidate_zap_pages_in_memslot` handles cross-vCPU shootdown via
`kvm_make_all_cpus_request(KVM_REQ_TLB_FLUSH)` automatically when memslot
contents change. No userspace signal required.

Task #168 (A.4f) is **parked** until either:
- Stage B is done and the gap is moot, OR
- Someone designs a kick mechanism that respects UML's signal-table topology
  (likely requires per-task handler installation in fork_handler's setup, plus
  careful audit of every interaction with UML's `block_signals`/`unblock_signals`
  software flag).

Gate stays at 17–21/21 historical band. The committed BUG.1/SEC.1/SEC.2/audit
fixes from the previous iteration (commit a5a87536d187) keep us at the upper
end of that band. Real progress on the gate now requires Stage B.
