# 05 — Observability / Debuggability Review

Status: architecture review (perspective: observability)
Date: 2026-04-26
Scope: the diagnostic infrastructure of the integrated KVM backend
(`backend=force=kvm`), why it has not localised the cumulative-imports
corruption that has consumed roughly 40 commits of attempted fixes, and
what should exist instead.

This review does not propose to fix the bug. It proposes to make the
bug findable.

---

## Executive summary — why current diagnostics did not catch the bug

The integrated KVM backend has a respectable amount of post-fault
forensic instrumentation: a per-mm shadow mutation ring, a bidirectional
pgd↔shadow audit, a single-VA shadow-equivalence audit at fatal #PF, an
always-on mini register dump, gated full register / lockstep dumps, and
five direct-sync counters per shadow. None of it has localised the bug,
and the failure mode is structural, not a question of "log harder."

Six diagnostic-design failures account for the impasse:

1. **All instrumentation fires *at* or *after* the fault, none of it
   *before* the corruption.** When a Python interpreter dereferences
   `self->ob_type` inside `_PyObject_MakeTpCall` and reads zero, the
   fault is the *consequence*; the corruption was a `*ptr = 0` (or a
   misdirected `memcpy`) committed somewhere between *zero and many
   thousand* host-VA writes earlier. The fault tooling tells us exactly
   where the consumer crashed and lets us audit shadow consistency at
   that VA — invariably finding it consistent (`EQUAL` on every run, per
   `STATUS.md` line 52). The corruption never went through the shadow,
   so the shadow audit is structurally incapable of detecting it.

2. **The mut ring observes only one of three independent write
   surfaces.** The KVM build supports three paths into guest-visible
   memory:
   - guest-issued stores via the shadow PT (would mutate the shadow's
     A/D bits but not the leaves; not relevant to corruption);
   - host-side writes through UML PTEs (set_pte_at → direct shadow
     sync → mut ring entry — observed);
   - host-side writes through the *host VA mapping* installed by
     `kvm_mm_map`'s `os_map_memory` call, used by
     `arch/um/kernel/skas/uaccess.c:147:raw_copy_from_user` and
     `arch/um/kernel/skas/uaccess.c:162:raw_copy_to_user` and every
     other `memcpy`-family helper that touches a `__user` pointer
     (**unobserved**).

   The ptrace and seccomp backends are bit-perfect because they own
   only one host VA → guest physical mapping (the stub child). The KVM
   build has *two*: shadow PT and host VA mapping, kept in sync by
   *separate* code paths. Anything that desynchronises them — a freed
   physical page reused while the host VA still points at it, an
   `os_unmap_memory` that happens after the shadow has been refilled,
   a mremap that adjusts the shadow but not the host VA — produces the
   exact symptom we see: a host-side `copy_to_user` writes correctly
   *to a stale physical page* that the guest no longer maps, and the
   guest later reads garbage from the *new* physical page mapped at the
   same VA. The mut ring is silent because nothing wrote a shadow PTE.

3. **`mut_dump_for(target_addr)` searches by VA, but the corrupting
   write is at a different VA than the read.** `cr2` is where the
   guest *read*; the bad store may have been at a totally unrelated
   VA whose page got freed and reallocated to back `cr2`'s page. The
   ring has no index by *physical page*, no concept of "what wrote
   the page that backs this VA at this moment." Even if the
   corruption *had* gone through the shadow, a VA-keyed search would
   miss it.

4. **The ring is 256 entries per mm.** A single CPython `import
   subprocess` produces tens of thousands of PTE mutations. By the
   time we fault inside `dl_main` or `_PyObject_MakeTpCall`, the
   relevant mutation — if there was one — wrapped out of the ring
   long ago. `mut_dump_for` reports "no recent mutations" not because
   none happened but because the ring is two orders of magnitude too
   small for the workload size.

5. **The "fail-loud" panic sites (F4 / F5) have, by design, never
   fired in this failure mode.** Both `panic("um: kvm pf-recovery:
   shadow fill failed (%d)", ...)` (`thread.c:3435`) and
   `panic("um: kvm context_switch: um_tlb_sync(prev->active_mm)
   failed (%d)", ...)` (`thread.c:261`) trigger only when the *sync
   path itself* returns an error. The corruption hypothesis is that
   the sync path *succeeds* and still leaves the system divergent —
   precisely the case those panics will not catch. A fail-loud
   diagnostic that never fires has a coverage of zero, regardless of
   how loudly it would shout if it did.

6. **Heisenbug protection is justified but defensive.** `STATUS.md`
   lines 116–125 document that the `audit_pgd` lockstep walk on every
   cached-skip drops hashlib smoke from ~95% to ~30%, and that even
   the dead-code regs-dump bytes drop it to ~80%. So the most
   high-fidelity diagnostics are gated behind `kvm_diag_*` knobs and
   left off in the failing build. The diagnostics that *would* see
   the corruption (continuous tracing) are exactly the ones we cannot
   afford to leave on. We are debugging a bug whose presence is
   trigger-rate-coupled to the cost of observing it. This is the
   single hardest property of the system to instrument around, and
   the codebase has not yet reckoned with it.

The bug is not eluding detection because the diagnostics are sloppy.
It is eluding detection because every diagnostic surface we have
points at the *destination* of corruption (a faulting VA's shadow PTE)
and we have nothing pointing at the *source* (the host-side write that
went to the wrong physical page). Until we observe the source, no
amount of audit refinement on the destination will close the loop.

---

## Inventory: every existing diagnostic, its useful signal, its blind spots

### `kvm_shadow_audit_va` — `lifecycle.c:1565`

- **Fires at:** the `!touched` branch of the `UM_KVM_PF_PORT` handler
  in `thread.c:3474`, just before queuing SIGSEGV.
- **Useful signal:** for the faulting `cr2`, prints
  `(um_pte, expected_x86_pte, shadow_pte, EQUAL/DIVERGE)`.
- **Cost:** O(1) walk per fault.
- **Blind spots:**
  - Looks only at `cr2`. If the corruption is "wrong PFN points to
    valid mapping," `expected == shadow` because both translate the
    *current* UML PTE — the divergence was in a *previous* PTE that
    has since been overwritten correctly.
  - Has no concept of "host VA mapping" at all. A perfectly consistent
    shadow + UML pgd can still be incoherent with the host VA mapping
    that `raw_copy_to_user` uses.
  - Returns 0 (`EQUAL`) on every reproducer run we have logged
    (`STATUS.md` line 52); it is consistent with the bug being
    elsewhere.

### `kvm_shadow_audit_pgd` — `lifecycle.c:1664`

- **Fires at:** the `kvm_diag_audit_pgd_skip=1` cached-skip branch of
  `kvm_enter_guest` in `thread.c:2094`. Off by default for the
  Heisenbug reason.
- **Useful signal:** bidirectional walk: every present UML leaf →
  matching shadow leaf, plus every present shadow leaf in the user
  half → corresponding UML leaf. Counts `(leaves, matches, diverges,
  shadow-extra)`.
- **Cost:** O(pages mapped). Empirically 2253 leaves on the
  reproducer, demonstrably perturbs the bug rate.
- **Blind spots:**
  - Same fundamental limitation as `audit_va`: it can only catch
    "shadow leaf disagrees with UML PTE." If both agree but the host
    VA mapping points elsewhere, the audit reports `DIV=0` and we
    are no wiser.
  - O(pages) cost is too expensive to leave on. The data point
    "DIV=0 at 2253 leaves" is interesting *negatively* (rules out
    the cache-lying hypothesis), but only after we paid in
    Heisenbug rate.

### `kvm_shadow_mut_dump_for` + `kvm_shadow_record_mut` — `shadow_sync.c:61`

- **Fires at:** every `kvm_shadow_sync_pte` call (writer);
  `mut_dump_for` reads at fatal #PF in `thread.c:3537`.
- **Useful signal:** circular ring of `(addr, ume, old_spte, new_spte,
  action)` per mm, dumped by VA-mask near the faulting cr2.
- **Cost:** atomic single-writer ring write per direct sync; cheap.
- **Blind spots:**
  - 256 entries per mm. CPython startup blows through this in tens
    of milliseconds.
  - Indexed by VA only. The corrupting write may be at a different
    VA whose page got reused; the search-by-mask cannot find it.
  - Records only events that *passed through `kvm_shadow_sync_pte`*.
    Misses every host-VA write, every `os_map_memory` /
    `os_unmap_memory` call, every `flush_tlb_*` event that didn't
    drain through the direct-sync path, every kvm_mm_map /
    kvm_mm_unmap event, every CR3 reload.
  - No timestamp, no PID, no syscall context, no caller stack — we
    cannot correlate ring entries with the workload's execution
    timeline.

### Mini regs dump (always-on) — `thread.c:3493`

- **Fires at:** every `!touched` SIGSEGV-bound #PF.
- **Useful signal:** RIP, cr2, error code, GP regs.
- **Cost:** one `pr_info` per fatal fault; negligible.
- **Blind spots:** still post-corruption. Tells us where the
  consumer crashed; says nothing about who corrupted the consumer's
  inputs. Has no FPU / XMM state, despite known FPU-leak class
  bugs (commit history shows `kvm_diag_skip_fpu_save` knob exists
  precisely because FPU drift was suspected at one point).

### Full GP regs dump (gated) — `thread.c:3566`

- **Fires at:** same site as mini, when `kvm_diag_pf_dump_regs=1`.
- **Useful signal:** all GP regs including those omitted from mini
  dump.
- **Cost:** dead-code measurably perturbs the trigger rate (per
  `STATUS.md` line 121). On by default would deform the workload
  enough to mask or shift the bug.
- **Blind spots:** same as mini, plus has no SREGS, no MSRs, no XSTATE,
  no CR2 history, no IST stack contents.

### Per-mm direct-sync counters — `kvm_backend.h:459`–`463`

- **Fires at:** every direct-sync action (`install`, `clear`, `absent`,
  `alloc_fail`, `range_clear`).
- **Useful signal:** "did the shadow get maintained via direct sync
  (counters bumping) or via deferred fallback?"
- **Cost:** atomic `WRITE_ONCE` increment; trivial.
- **Blind spots:**
  - Monotonic counters only. No histogram of "install events per
    second across the workload" — we cannot ask "did the install
    rate spike before the fault?"
  - No correlation with PIDs, mms, syscalls.
  - No counter for "needs_full_resync triggered then repaired."
  - No counter for `kvm_mm_map` / `kvm_mm_unmap` events — the host
    VA / shadow boundary is uninstrumented.

### `pr_info_ratelimited` and the "panic loudly" sites

- **Fires at:** various error/edge paths (KVM_RUN failure, KVM_GET_REGS
  failure, fill failure, sync failure, double-fault, unknown exit).
- **Useful signal:** when the *machinery* breaks, you get a clean
  backtrace.
- **Cost:** ratelimited prints; effectively free.
- **Blind spots:** **the entire current bug class flows through
  paths where every one of these checks passes.** A diagnostic that
  fires only on machinery failure cannot detect a correctness failure
  in the protocol itself. F4 and F5 have not fired in any reproducer
  we have logged — the failure isn't "shadow fill returned -ENOMEM,"
  it's "shadow fill *succeeded* and the system is still wrong."

### Summary

We have rich post-mortem instrumentation pointing at the destination
of the corruption (the faulting VA's shadow PTE), and effectively zero
instrumentation pointing at the source (host-VA writes, host-VA mapping
lifecycle, the relationship between shadow PT and host VA mapping).

The destination is consistent in every reproducer we have. That is the
diagnostic outcome that should have shifted the search to the source
20 commits ago.

---

## What SHOULD exist: a comprehensive observability stack

### Layer 0 — recover the cheap continuous trace surface

Every other layer below relies on having a circular trace that can be
left on without deforming the workload. We do not have one. We have
`pr_info_ratelimited` which is *too expensive* (one printk per call),
and an in-memory ring (`mut_ring`) which is *too small*.

The right primitive is a per-CPU circular bytebuffer (one page per
CPU, lock-free single-producer single-consumer) of fixed-size
binary trace records. Cost per record: one cmpxchg-free atomic
increment + one struct copy ≈ 20 ns on contemporary x86. That is
small enough to leave on continuously across an entire CPython
import without measurably moving the trigger rate.

Records are decoded post-mortem (panic dumps the buffer; `/proc`
exposes it for live inspection). No printk in the hot path.
Ring size is configurable; default 64 KiB per CPU = ~2000 records,
enough for a one-second window of CPython activity.

This is the foundation. None of the per-layer plans below work
without it.

### Layer 1 — instrument the source, not the destination

Add trace records (Layer 0) at every event that mutates a host-side
write surface or guest-visible memory:

| Event | Site | Fields |
|-------|------|--------|
| `set_pte_at` mutation | `arch/um/include/asm/pgtable.h:300` | `mm`, `va`, `old_pte`, `new_pte`, `caller_PC` |
| Direct shadow sync | `shadow_sync.c:153` | `mm`, `va`, `ume`, `old_spte`, `new_spte`, `action` |
| `kvm_mm_map` (host VA install) | `mm.c:119` | `mm`, `va`, `len`, `prot`, `phys_fd`, `offset` |
| `kvm_mm_unmap` | `mm.c:183` | `mm`, `va`, `len` |
| `os_map_memory` direct call | wherever it's called | `va`, `len`, `phys_fd`, `offset`, `prot` |
| `raw_copy_to_user` | `uaccess.c:162` | `dst_va`, `src_va`, `n`, `mm` |
| `raw_copy_from_user` | `uaccess.c:147` | `dst_va`, `src_va`, `n`, `mm` |
| `flush_tlb_*` family | various | `mm`, `start`, `end` |
| CR3 reload | `kvm_enter_guest`, `kvm_context_switch` | `prev_cr3_gpa`, `next_cr3_gpa`, `mm`, `reason` |
| KVM_RUN entry / exit | `thread.c:3053` | `vcpu_fd`, `exit_reason`, `cycles_in_guest` |
| #PF (any, not just fatal) | `thread.c` UM_KVM_PF_PORT | `cr2`, `rip`, `error_code`, `cpl`, `mm` |
| #GP (any) | `thread.c` UM_KVM_GP_PORT | `rip`, `cs`, `mm` |
| Page free at PFN | hook into `__free_pages` for guest-visible PFNs | `pfn`, `caller`, `mm` |

The key new events are the `raw_copy_*` and the `__free_pages`
records: they are exactly the surfaces the current ring does not
cover. With them, a post-mortem trace of "what wrote PFN 0xXXXXX
in the last 10000 events" becomes a one-line query.

### Layer 2 — index by physical page, not just by VA

Add a second ring per mm (or globally) keyed by *physical page
number*. Every event in Layer 1 that touches a guest-visible PFN
appends to both the time-ordered ring and a per-PFN ring head
(at most last 8 events per PFN, hash-indexed by PFN).

At fatal fault, after dumping mut_ring near `cr2` (current behaviour),
also do:

1. Walk the shadow PT to translate `cr2` → current PFN.
2. Walk the host VA mapping (UML pgd → PFN) for `cr2`'s VA.
3. Diff the two PFNs. If they disagree, this is the bug.
4. Dump the per-PFN ring for both PFNs ("who last wrote this page?
   who last installed/removed this PFN from a mapping?").

This single query ("are shadow PT and host VA mapping pointing at the
same physical page for this VA?") would have closed the
debugging loop on the first reproducer if it had existed. See "the
one diagnostic" section below.

### Layer 3 — a stable trace-event ABI (ftrace integration)

Replace the bespoke `pr_info`s with `TRACE_EVENT_DEFINE` macros under
`include/trace/events/uml_kvm.h`. Suggested events (with format
strings):

```c
TRACE_EVENT(uml_kvm_shadow_sync_pte,
    TP_PROTO(struct mm_struct *mm, unsigned long va,
             u64 old_spte, u64 new_spte, u8 action),
    TP_ARGS(mm, va, old_spte, new_spte, action),
    TP_STRUCT__entry(
        __field(struct mm_struct *, mm)
        __field(unsigned long, va)
        __field(u64, old_spte)
        __field(u64, new_spte)
        __field(u8, action)
    ),
    TP_fast_assign(
        __entry->mm = mm;
        __entry->va = va;
        __entry->old_spte = old_spte;
        __entry->new_spte = new_spte;
        __entry->action = action;
    ),
    TP_printk("mm=%p va=0x%lx old=0x%llx new=0x%llx action=%u",
              __entry->mm, __entry->va,
              __entry->old_spte, __entry->new_spte, __entry->action)
);

TRACE_EVENT(uml_kvm_mm_map,
    TP_PROTO(struct mm_id *id, unsigned long virt, unsigned long len,
             int prot, int phys_fd, u64 offset),
    /* ... */
    TP_printk("id=%p virt=0x%lx len=0x%lx prot=0x%x phys_fd=%d off=0x%llx",
              ...)
);

TRACE_EVENT(uml_kvm_raw_copy_to_user,
    TP_PROTO(void __user *to, const void *from, unsigned long n),
    /* ... */
    TP_printk("to=%p from=%p n=%lu", __entry->to, __entry->from,
              __entry->n)
);

TRACE_EVENT(uml_kvm_pf,
    TP_PROTO(u64 cr2, u64 rip, u32 error_code, u8 cpl,
             struct mm_struct *mm, u64 shadow_pfn, u64 hostva_pfn),
    /* ... */
    TP_printk("cr2=0x%llx rip=0x%llx ec=0x%x cpl=%u mm=%p "
              "shadow_pfn=0x%llx hostva_pfn=0x%llx %s",
              ..., __entry->shadow_pfn == __entry->hostva_pfn ?
                   "COHERENT" : "INCOHERENT")
);

TRACE_EVENT(uml_kvm_kvm_run,
    TP_PROTO(int vcpu_fd, u32 exit_reason, u64 cycles),
    /* ... */
    TP_printk("vcpu=%d exit=%u cycles=%llu", ...)
);

TRACE_EVENT(uml_kvm_phys_page_free,
    TP_PROTO(u64 pfn, void *caller),
    /* ... */
    TP_printk("pfn=0x%llx caller=%pS", ...)
);

TRACE_EVENT(uml_kvm_cr3_reload,
    TP_PROTO(u64 prev_cr3, u64 next_cr3, struct mm_struct *next_mm,
             const char *reason),
    /* ... */
    TP_printk("prev_cr3=0x%llx next_cr3=0x%llx next_mm=%p reason=%s",
              ...)
);
```

Why this is worth doing despite the upfront cost:
- ftrace's per-CPU rings are exactly the Layer-0 primitive we need,
  and they are battle-tested. We should not roll our own.
- `trace-cmd`, `perf`, and `bpftrace` *just work* with
  `TRACE_EVENT_DEFINE` events; we get filtering, decoding, post-
  processing, and live-streaming for free.
- An `awk` over a `trace-cmd report` of one reproducer run is the
  fastest way to ask "for every cr2 that faults, what was the last
  raw_copy_to_user that touched the same physical page?"
- Tracepoints have static-key gating built in: events are zero-cost
  when no consumer is attached, so we can leave them compiled in
  permanently and pay nothing in production.

### Layer 4 — snapshot-at-N: bisect by operation count

UML already has snapshot infrastructure (`arch/um/backend/kvm/snapshot.c`,
`um_snapshot_ready` hook in `arch/um/kernel/snapshot.c`). It captures
vCPU state + memslot. The integration into `um_snapshot_ready` is
deferred (#250 in `STATUS.md`); deferring it has cost us the ability
to bisect this bug.

Plan:
- Add `kvm_diag_snapshot_every_n_syscalls=N` knob. When set, every N
  syscalls the dispatcher takes a snapshot to memory (cheap with
  `kvm_snapshot_capture_regs_only`) and stamps it with a serial.
- At fatal fault, dump the most recent K snapshot serials.
- Add an offline tool `tools/testing/selftests/um/kvm-bisect-snap/`
  that, given two snapshots, performs a structured diff: SREGS,
  GP regs, MSRs, FPU, dirty memslot pages.
- The first snapshot whose state is already "wrong" (some heuristic
  to be developed; e.g., "any guest-visible page contains a
  PyObject with `ob_type == NULL`") localises the corruption
  window to N syscalls.

This requires #250 to land. Closing #274 without #250 means the
*next* "100% reproducible bug whose trigger is workload-size-coupled"
will face the same wall again. They are the same diagnostic capability.

### Layer 5 — full vCPU-state snapshot at every fault

We already issue `KVM_GET_REGS` at every VMEXIT. Add one panic-time
helper that issues `KVM_GET_REGS + KVM_GET_SREGS + KVM_GET_FPU +
KVM_GET_MSRS + KVM_GET_VCPU_EVENTS` and dumps the lot in a single
parseable block.

`kvm_snapshot_capture` already does most of this (without the memslot
part — too expensive at every fault). Extract the regs-only path
(`kvm_snapshot_capture_regs_only`) and call it from every fatal fault
site, panicking after.

This data is the input to the differential testing below.

### Layer 6 — differential testing (kvm vs seccomp vs ptrace)

The strongest debugging asset we have is that *the same workload
succeeds bit-perfectly under seccomp*. We have not exploited it.

Build a parallel-runner harness:

```
tools/testing/selftests/um/kvm-diff/run-kvm-diff.sh
```

For one curated workload (the smallest reliable reproducer of the
import-unittest crash):

1. Run under `backend=force=seccomp` with strace-equivalent tracing
   on (capture every syscall number, args, return value, and any
   register state the trace can extract). Save to `seccomp.trace`.
2. Run under `backend=force=ptrace` similarly. Save to `ptrace.trace`.
3. Run under `backend=force=kvm` similarly. Save to `kvm.trace`.
4. Diff the three traces:
   - The seccomp / ptrace pair must match exactly (sanity check —
     if they don't, the trace harness itself is broken).
   - The first divergence between either of those and `kvm.trace`
     is the syscall (or memory operation) where KVM behaviour
     drifts.

The trace records can ride on Layer 3 (ftrace events). We need a
new `uml_diag_syscall_observe` event that records every syscall NR,
args, return, and a hash of any guest-visible buffer the syscall
touches. With that, the diff-three runner is just a bash + Python
script.

The infrastructure already exists in the record/replay machinery
(`arch/um/backend/kvm/record.c`, `kvm_record_observe_syscall`).
Today it is gated behind `um_kvm_record_enabled` static key and
only used for replay. For diff-mode, we want the same record path
under all three backends and the comparator to flag the first
delta. That is one new `static_branch` and a per-backend writer.

### Layer 7 — single-step until divergence

KVM supports `KVM_GUESTDBG_SINGLESTEP` (the `KVM_SET_GUEST_DEBUG`
ioctl with `KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP`). At each
step it returns to the host with a debug VMEXIT. Cost is enormous
(every instruction is a VMEXIT, ~150k cycles), but the bug is 100%
reproducible under nokaslr — we can afford one slow run to localise
the corrupting instruction.

Plan: a `kvm_diag_singlestep_after_n_syscalls=N` knob. When the
syscall counter hits N, switch the vCPU into single-step mode for
the next M instructions. At each step, snapshot a few key bytes
(the page where the eventual `ob_type` zeroing is observed).
First step at which those bytes change identifies the culprit
instruction.

This needs a quick way to identify the "page where the corruption
will be observed." That is provided by Layer 5 (snapshot at fault)
+ Layer 4 (bisect by N): bisect tells us "corruption window is
syscalls 1500–1750"; single-step from snapshot 1500 narrows to
the instruction.

### Layer 8 — a CPython-bytecode-granular parity gate

`cpython-parity.sh` runs 21 modules and reports module-level pass/
fail. With parity at 0/21, every commit is a coin flip on the
*entire suite*. No granularity to bisect.

Build a `cpython-bytecode-parity.sh` that for one chosen module:
- runs the module under both backends with `python3 -X dev` and
  hashed-output instrumentation;
- emits a per-bytecode trace (e.g., via `sys.settrace` writing
  one line per `LINE` event with `(filename, lineno, locals_hash)`);
- diffs the two traces line-by-line.

The first line where the local-state hash diverges identifies the
exact Python bytecode where KVM and seccomp produce different
results. That is at-most a few-line CPython context, and from there
the user-visible state (`PyObject` content, etc.) is fully
inspectable on both sides.

This is much more discriminating than the module-level gate.

### Layer 9 — coherence audit at every kvm_enter_guest, not at fault

Currently `kvm_shadow_audit_pgd` runs only on the cached-skip path
when `kvm_diag_audit_pgd_skip=1`, and it audits only shadow vs UML
pgd.

Add a `kvm_diag_coherence_audit_every_n_entries=N` knob that, every
N entries, runs the *three-way* audit:

1. Sample 32 random *present* leaves from UML pgd.
2. For each, walk shadow PT → PFN_shadow.
3. For each, look up host VA mapping → PFN_hostva.
4. Sample the bytes at each PFN through both routes. They must agree.

Any disagreement is the bug, and the audit pinpoints the exact
mapping that's stale.

The Heisenbug concern from `STATUS.md:121` applies — running this
on every entry is too expensive — but with `N` configurable we can
sample at e.g. every 1000 entries and pay ~0.1% overhead while
still catching the corruption within seconds.

---

## Differential testing plan (concrete)

### Step 1 — record-mode parity runner

Modify `record.c`'s `kvm_record_observe_syscall_*` family so the
observation path is available *under any backend*, not just KVM.
The minimum new function:

```c
void um_diag_record_syscall(unsigned long nr,
                            const struct uml_pt_regs *pre,
                            const struct uml_pt_regs *post,
                            long ret_value);
```

Called from `arch/um/kernel/skas/syscall.c` before and after every
syscall, gated by a new `um_diag_record_enabled` static key.

### Step 2 — record output format

Single binary file per run, magic header + `(syscall_nr, in_regs,
out_regs, return_value, hash_of_user_pages_touched)`. Hash any
buffer pointed to by `__user` arguments before *and* after the call.

### Step 3 — runner harness

```bash
tools/testing/selftests/um/kvm-diff/
  run-kvm-diff.sh         # main entry point
  diff-records.py         # binary-format diffing tool
  workload-import.py      # the chosen reproducer
```

Driver:
```
$BIN backend=force=seccomp ... > seccomp.bin
$BIN backend=force=ptrace ... > ptrace.bin
$BIN backend=force=kvm    ... > kvm.bin

diff-records.py seccomp.bin ptrace.bin   # sanity: must be empty
diff-records.py seccomp.bin kvm.bin      # the answer
```

### Step 4 — first-divergence report

The runner emits:
```
DIVERGENCE at syscall #1547 (mmap)
  seccomp: ret=0x7f1234567000  in_arg2=0x1000 out_user_hash=0x...
  kvm:     ret=0x7f1234567000  in_arg2=0x1000 out_user_hash=0xXXXX
  PRIOR SYSCALL: #1546 (read) seccomp_ret=0x80 kvm_ret=0x80
  PRIOR SYSCALL: #1545 (mprotect) seccomp_ret=0  kvm_ret=0
```

The first row whose `out_user_hash` differs is the syscall whose
output went into a different physical page on KVM. From there it is
a small step to "look at what's on the other physical page."

### Step 5 — productionise as a kselftest

`make -C tools/testing/selftests/um/kvm-diff run` is part of CI.
Failure mode: any divergence is a regression.

---

## The one diagnostic that would have caught this fastest

If only one diagnostic were added, it should be:

> **Three-way coherence assertion at every fatal fault: shadow PT
> PFN, UML pgd PFN, and host VA mapping PFN must all agree for
> `cr2`.**

Today's `kvm_shadow_audit_va` answers two of those three (UML pgd,
shadow PT). The missing third is: walk the *host process's own page
tables* (the kernel's virtual memory layer below UML — i.e., the
mapping `os_map_memory` installed) and read what physical page is
currently mapped at `cr2`'s host VA. Compare PFNs across all three.

```
cr2 = 0x300
shadow_pt(cr2)  → PFN 0xAAAAA
uml_pgd(cr2)    → PFN 0xAAAAA   (audit_va says EQUAL)
host_va(cr2)    → PFN 0xBBBBB   ← divergence
```

That single line in the panic log would have:
- redirected the entire investigation away from "shadow PT bug" the
  moment it appeared;
- told us that `raw_copy_to_user` for that VA writes into the wrong
  physical page;
- pointed at `kvm_mm_map` / `kvm_mm_unmap` / `os_map_memory`
  ordering as the bug class;
- given a load-bearing reproducer signal (the first cr2 with
  three-way disagreement is the test target).

It is implementable in ~50 lines. It has been absent for 40
commits.

The reason this one is the highest-yield: every other diagnostic on
the list above gives us *more* of the same kind of information
(time, place, count, context). This one gives us a *new dimension* —
the third coordinate of memory mapping that the existing audit
infrastructure does not consider exists. The bug class we are
chasing is, in retrospect, almost certainly a coherence bug between
the two host-side memory views, and no audit that ignores one of
those views can find it.

---

## What I would build first

In strict priority order, the first three items below would, in my
estimation, take less than 50 commits to implement and would have
made the current bug findable in hours.

### 1. Three-way coherence dump at every fatal fault (~50 lines)

Add `kvm_shadow_audit_va_threeway(va)` next to `kvm_shadow_audit_va`.
Performs the existing audit *plus* walks the host-process pgd via
`get_user_pages_fast(current, va, 1, 0, page)` (or the equivalent
internal helper that works without a userspace fault) to obtain
PFN_hostva. Logs all three. Wire into the same `!touched` site as
the existing audit.

Rationale: highest information per byte of code added. Catches the
specific bug class we're stuck on.

### 2. ftrace TRACE_EVENT for `set_ptes` / `kvm_mm_map` /
   `kvm_mm_unmap` / `raw_copy_to_user` / `raw_copy_from_user` /
   `__free_pages` (~200 lines + a header)

Define `include/trace/events/uml_kvm.h` with the events listed in
Layer 3 above. Replace the per-mm circular ring with the
per-CPU ftrace ring. Tracepoints are static-key-gated, so the
in-tree default cost is zero.

Rationale: the foundation that every subsequent diagnostic stands
on. Standardised, well-tested, "just works" with `trace-cmd`. Lets
a developer answer ad-hoc questions ("show me all kvm_mm_map calls
in the 100ms before the fault") without writing new C code.

### 3. Differential record-and-diff runner (~300 lines of bash +
   Python + C glue)

Build the kvm-diff/ kselftest as described above. Use the existing
record.c machinery. Make it the first port of call when "kvm fails
where seccomp succeeds."

Rationale: the most general purpose tool. Future "KVM-only fails on
new workload X" investigations begin and end with this one runner.

### Items 4 onward (in order)

- snapshot-every-N-syscalls + offline diff (#250 integration),
- single-step bisect tool,
- per-PFN ring,
- bytecode-granular CPython parity test,
- in-flight 3-way coherence sampling.

Each is independently valuable. Build in priority order; stop when
the bug is closed; resume the build when (not if) the next bug of
this class appears.

---

## Closing observation

Every "this should have been caught earlier" debugging investment
has the same ROI argument: high upfront cost, no payoff until the
*next* hard bug, by which time the team has forgotten why the
investment was made.

That argument is structurally weaker for this codebase than for most
because the structural-coherence properties of the integrated KVM
backend mean we will hit this bug class — "two host-side views of
guest memory drift" — repeatedly. The host-VA mapping, the shadow
PT, the snapshot memslot, and (eventually) per-mm worker mappings
are *four* views. The current diagnostic stack covers one. We cannot
afford to discover each new view's failure mode the way we
discovered the host-VA / shadow-PT one.

The user's frustration is well-founded. The diagnostic stack was
designed for the bugs that had already been fixed (audit_va caught
the dl_main rtld.c:1953 fault); it was not extended to the bug class
that fixing those exposed (host-VA / shadow-PT incoherence). The
recommendation is to invest in the three items above before the next
"why is this still failing?" cycle begins.
