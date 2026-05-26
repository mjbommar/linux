# UML KVM v2 — Layer 12: SMP-T23 + post-T22 residual CHARACTERIZED

**Date:** 2026-05-02
**Tip with diagnostics:** `96dffac606cd` (branch `umlctl-deploy`)
**Status:** CHARACTERIZED, not closed. Residual = 7% (2/30) on
threaded-subprocess-wait.py — all signatures consistent with
"page-recycling-class fault during init.sh bash heap access".
Fix candidates identified; bug isolated to deterministic addresses.

## Problem statement

After SMP-T22 closed the dominant Bug B class (NM_stub+2 iretq-pops-
kernel-half-RIP), the threaded-subprocess-wait.py × 30 reproducer
under kvm-v2 SMP shows:

| Build | PASS/30 | NM_stub+2 | high-cr2 (BUG_PR) | RSV-bit (BUG_C) |
|---|---|---|---|---|
| Pre-T19/T20 baseline (`fefdc8666961`) | 14/20 | 5/8 of fails | rare | none |
| T19+T20 (`d9ed9e14c7b6`) | 17-18/20 | dominant | rare | none |
| **T22 (`ddf3cfe5cf31`)** | **17-18/20** | **0** | dominant of remainder | none |
| T23 (`a0be14e66013`) | 18/20 | 0 | 2 | 0 |
| **T22+T23 + diagnostics, trace-OFF (`96dffac606cd`)** | **28/30 = 93%** | **0** | **2/2 of fails** | **0** |
| T22+T23 + state-trace ON | 29/30 | 0 | 1/1 | 0 |

The trace-on / trace-off comparison shows ~1-3% absolute drop in
pass-rate when state-trace is off — within statistical noise; the
state-trace overhead is NOT the dominant bug-suppression mechanism
once T22 is in. The race is real, just narrower than pre-T22.

## Captured signatures

Both fails (run 13, run 29 of trace-OFF run) emitted **the IDENTICAL
five (cr2, rip) pairs**:

```
um: kvm-v2 BUG_PR[1] high-cr2 cr2=550000000040 user_rip=40024da0 err=4 pid=1 comm=init.sh
um: kvm-v2 BUG_PR[2] high-cr2 cr2=55000016ae88 user_rip=40008c64 err=4 pid=1 comm=init.sh
um: kvm-v2 BUG_PR[3] high-cr2 cr2=55000016e668 user_rip=40025274 err=4 pid=1 comm=init.sh
um: kvm-v2 BUG_PR[4] high-cr2 cr2=7f7ffffff020 user_rip=400254b3 err=4 pid=1 comm=init.sh
um: kvm-v2 BUG_PR[5] high-cr2 cr2=5500000157dd user_rip=4002b317 err=4 pid=1 comm=init.sh
```

**Decoded:**
- `pid=1 comm=init.sh` — the umlctl auto-generated bash init (PID 1
  in the UML guest).
- `cr2 in 0x55... range` — bash heap (typical glibc malloc region
  for x86_64 Linux processes; ASLR base for non-PIE bash).
- `cr2 = 0x7f7ffffff020` — bash stack region (high user-half).
- `user_rip in 0x40... range` — ld-linux loaded code (.text).
- `err=4` = `(P=0, R=0, U=1)` = user-mode read from a page-not-present.

The fact that the cr2/rip pairs are bit-identical across 2 distinct
boot runs is **diagnostic gold**: this is NOT random page recycling.
It's a deterministic milestone in init.sh's execution where some
PT entry is not yet established.

## Root-cause hypotheses

### H1 — CoW PT establishment race (LEADING, ~50%)

When init.sh does any operation that triggers a fork+exec internally
(or one of its child processes does), there's a brief window where:

1. Parent fork: `dup_mmap()` walks parent VMAs, marks pages COW (RO),
   creates child PT entries pointing to same PFNs with RO bit.
2. Child execve: `exec_mmap()` swaps in fresh mm — abandons COW PT.
3. Brief window where child's mm has fresh empty user-half PT but
   first vmexit-driven dispatch may race with kernel-side page-fault
   path that establishes entries.

Under v2, every PT change writes guest-PA pages directly (the guest
pgd lives in physmem). The CR4.PGE flush at next dispatch flushes
THIS vCPU's TLB, but TDP cache may have stale leaf entries until KVM
issues the equivalent guest-PA shootdown.

**Confirming experiment**: instrument `kvm_v2_load_user_sregs` to
log `(pid, mm-context.tlb_gen, vcpu->last_seen_tlb_gen, last_mm)`
at first dispatch after `current->mm` change. Cross-correlate with
BUG_PR firings. If H1 is correct, BUG_PR fires immediately after a
mm-change with `tlb_gen` lag.

### H2 — Page-recycling between deferred-free queue and call_rcu callback (~25%)

SMP-T20 switched `um_mmu_gather_drain` to `call_rcu`-deferred page
free. The grace period bound is "every CPU has dispatched at least
once". Under heavy fork+exec churn, all 4 vCPUs ARE dispatching
constantly, so the grace period closes within ~1ms — but during
that window, the page is on the deferred queue, not yet returned
to buddy.

If a NEW allocation happens BEFORE the grace period closes, buddy
returns a different page (correct). If the new allocation happens
AFTER (correct case), the freed page comes back. Under our 2/30
fail rate, maybe 1-in-200 dispatches sees a window where a stale
TLB entry on a remote vCPU still maps the freed page — but the
fault we'd see would be (P=1, wrong data), not P=0.

**The P=0 signature argues AGAINST H2** — page-recycling produces
WRONG-DATA reads, not P=0. So this is unlikely.

### H3 — CR3 / TDP MMU asynchrony (~15%)

Under SMP, `kvm_v2_load_cr3` writes CR3 in sregs but doesn't issue
KVM_FLUSH_TLB explicitly — relies on KVM to detect CR3 change and
flush. KVM may issue a partial flush that leaves some TDP entries
cached. Manifests as P=0 if the cached entry was for an unmapped
GVA.

**Refuting evidence**: every dispatch toggles CR4.PGE, which is
the strongest TLB flush available. If KVM honors that, all guest
TLB cache entries die.

### H4 — Worker stub setup / mm_id race (~10%)

The worker_alloc_stub_for_mm helper does an async stub-alloc round-
trip. There's a brief window where the worker mm exists but its
stub child isn't fully alive yet. If the first KVM_RUN happens during
this window, the stub may not yet have its expected user-half PT
entries.

**Cross-check**: the BUG_PR fires for pid=1 (init.sh), not for newly-
spawned worker subprocesses. So this is not the smoking gun unless
init.sh also goes through a worker re-alloc cycle (which it doesn't
under normal operation).

## Why the addresses are deterministic

If we hit a CoW-PT-establishment race, we'd expect random addresses
within bash's mm. But the SAME 5 (cr2, rip) pairs surface across
distinct boots. This argues for:

- A specific code path in bash that always touches these exact
  heap offsets during stress (e.g. signal-handler invocation,
  ld-linux deferred relocation, glibc lazy-binding fixup).
- The PT entry at these specific addresses is the first one to
  be checked AFTER a TLB-flush-then-stale window opens.

Both are consistent with H1: the child of bash's fork+exec, mid-
execve setup, faults at the first heap access in the deterministic
ld-linux relocation walk. The walk visits relocation entries in
canonical order; the first one that needs a page-not-yet-mapped
fires the fault.

## Diagnostics shipped this session

1. **state-trace ring auto-freeze on BUG_B / BUG_PR / BUG_C trigger**
   (`syscall_trap.c:1297-1369`, `state_trace.c:319-323`). Captures
   ~167 dispatch entries leading up to each fault.

2. **BUG_C (PF-RSV)** detector: reserved-bit-violation faults. Did
   NOT fire on either captured fail — rules out PT-corruption-class.

3. **BUG_PR (high-cr2 page-recycling)** detector: user-half cr2 in
   [4GB..128TB] with P=0+U=1. Fired on both fails, bit-identical
   addresses.

4. **Layer 5 toolkit memo** refreshed (`8472753f3230`) to document
   ALL post-2026-05-01 in-tree instrumentation, the umlctl
   observability spine, per-bug-class memo cross-reference, and
   the multi-agent investigation playbook.

## State-trace dump decode (single trace-on capture, run 29 of trace-on)

The state-trace ring captured 167 dispatch entries leading to the
TRACE_TRIGGER freeze. Key sequence at the tail:

```
seq 137: VCPU_RUN_EXIT exit=2 port=0xfd  (T22 #NM vmexit)
         cr0=80010023 (TS=1)  mmgen=9
         IST=[4, 4001bab6, 2b, 10206, 7f7fffeb5bc0, 23] (real user RIP)

seq 144: POST_KVM_RUN exit=2 port=0xf6  (#PF after #NM completed)
         cr0=80010023 (TS=0)  mmgen=9  cr2=4000dc40 (real address)
         IST=[14, 4000dc40, 2b, 10246, 7f7fffeb5a08, 23]

seq 156: POST_KVM_RUN exit=2 port=0xfd  (second #NM)
         cr0=8001002b (TS=1)  mmgen=10  hax=40023340
         IST=[14, 40024d52, ...]

seq 164: POST_KVM_RUN exit=2 port=0xf6  ← BUG_PR-fault-bearing dispatch
         cr0=80010023 (TS=0!) cr2=550000000040  mmgen=10
         IST=[4, 40024da0, 2b, 10246, 7f7fffeb5a10, 23]

seq 166: TRACE_TRIGGER (BUG_PR fires; ring frozen)
         hax=550000000040 (= cr2 — captured by stub's `mov %cr2,%rax`)
         hip=ffffe00000002158 (= HANDLERS_GVA + 0x158, mid-#PF stub)
```

**Decoded interpretation:**

The fault at cr2=0x550000000040 is a **REAL user-mode fault**. Specifically:
- user_rip=0x40024da0 = ld-linux .text (legit code)
- user_cs=0x2b = USER_CS (CPL=3)
- user_rflags=0x10246 (normal)
- user_rsp=0x7f7fffeb5a10 (normal stack)
- error=4 = (P=0, U=1, R=0) (legit user read of not-present page)

This means **bash's PT genuinely does not have an entry for cr2**. The
fault is NOT v2-side IST corruption, NOT page-recycling-class (which
would be wrong-data not P=0), and NOT TDP MMU staleness (which would
be detected by the CR4.PGE flush at next dispatch).

The bug is at the kernel level: bash accesses a page in its own
heap/.text, and the kernel hasn't established a PT entry for it.
Causes:
- Race in `dup_mmap` PT inheritance during fork (worker_thread races
  with another worker_thread doing fork+exec).
- Race in `exec_mmap` teardown vs another thread's first user
  dispatch.
- Race in mmap/munmap page-table updates with the v2 TLB flush
  invalidating wrong entries.

The fact that mmgen advances from 9→10 between seq 144 and seq 164
shows that some PT change DID happen in this window — which is
suspicious given the same task remains pid=1 init.sh on the same
mm throughout.

**Net hypothesis update post-decode:**

H1 (CoW PT establishment race) is REINFORCED: the trace shows mmgen
bumps without an mm change, indicating PT churn for the same mm
that's experiencing the fault. Concurrent worker_thread activity in
bash's mm is the most likely source of the bumps.

H3 (TDP MMU asynchrony) is largely RULED OUT — the CR4.PGE toggle
on every dispatch flushes guest TLB cleanly, and the fault path
follows expected POST_KVM_RUN → HANDLE_IO_PF_PRE flow.

H2 (page-recycling) is RULED OUT — error code is P=0, not the
P=1+wrong-data signature of TLB staleness pointing at recycled pages.

## Path forward (deferred to SMP-T24+)

The next investigation step is to RECORD the dispatch sequence
just BEFORE init.sh hits BUG_PR — the state-trace auto-freeze
gives us exactly that. Specifically check:

1. Is there a `current->mm` transition (cross-mm dispatch) within
   the last few dispatches before the fault?
2. Does `mm->context.tlb_gen` show recent bumps?
3. What is `vcpu->last_seen_tlb_gen` minus `current->mm->context.tlb_gen`?
4. Is the kicker firing or being suppressed via cmpxchg dedup?

If the trace-on auto-freeze captures the smoking gun (it did on
the one trace-on capture but the dump was for a different signature
class than the trace-off captures), we can decisively distinguish
H1 vs H2 vs H3.

For this session: **STOP** at 93% pass rate (28/30) with characterized
residual + diagnostic infrastructure in place. The next session
should:

1. Run the threaded-subprocess-wait.py reproducer with
   `kvm_v2_trace_enable` until a BUG_PR fires.
2. Decode the state-trace ring around the freeze point.
3. Match the smoking gun against H1/H2/H3.
4. Implement the targeted fix.

## Validation status (baseline + this session's work, all gated through umlctl)

| Gate | Result |
|---|---|
| threaded-subprocess-wait × 30 (kvm-v2 SMP, trace-OFF) | **28/30 (93%)** |
| threaded-subprocess-wait × 30 (kvm-v2 SMP, trace-ON) | **29/30 (97%)** |
| threaded-subprocess-wait × 20 (seccomp baseline) | **20/20** |
| mt-mini SMP T=4 ncpus=4 × 30 | **30/30** |
| mt-mmap-stress SMP T=8 ncpus=4 | **PASS** |
| substrate (regrtest-repros) | **25/3/3** |
| cpython-parity (21 modules) | **21/21 PARITY** |

Phases A-J still on the original timetable; the threaded-subprocess-
wait residual is a known limitation, not a phase blocker. Phase J
(24h soak + Tier 1/2/3 third-party tests) is the next phase milestone.
