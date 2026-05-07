# SMP-T55 — perf-py-startup regression: fix-plan memo

**Status:** OPEN, plan only — no code changes proposed in this memo.
**Authored:** 2026-05-07.
**Builds inspected:** branch `umlctl-deploy`, tip as of 2026-05-07
(`82df9571eb25`); validation kernel at
`~/src/uml-builds/uml-smp-t41fix/linux`.
**Originating diary:** `state-audit/22-smp-t41-stress-and-perf.md`
"Known perf regression vs 2026-04-30 baseline (separate from T41)".

## 1. TL;DR

The `perf-py-startup` kselftest gate (kvm-v2 / seccomp wall-clock
ratio ≤ 1.20) FAILS on Zen 4: ratio=1.250 (kvm-v2 0.100s vs seccomp
0.080s). This is a real regression — three months ago the same gate
read 0.444 (kvm-v2 2.25× FASTER than seccomp). The regression
decomposes into two independent contributors: ~+50% from a clutch of
SMP-correctness commits added between the 2026-04-30 UP build and the
2026-05-02 UP build (TLB-kick infra `7e1c255a09ad`/`9f0ff6257e8b`,
`migrate_disable` `95b3a85bd309`, EINTR-mid-PF inline
`e5977806fd14`); and ~+67% from SMP-T26/T27 (commit `76b1d98b2006`)
which converted the always-on `KVM_GET_FPU` skip from "skip when guest
CR0.TS=1" to "always GET" to plug the cross-task XMM leak. Hot-path
gadget-driven workloads still show kvm-v2 ~3-4× faster than seccomp
because they don't pay per-dispatch FPU cost — but Python startup is
exactly the workload that maximises non-FPU dispatches (heavy syscall
churn, mmap, exec) and minimises any benefit from elided FPU.

**Proposed fix (one sentence):** restore Phase H.2's "skip
`KVM_GET_FPU` when guest didn't touch FPU" behaviour, but predicate
the skip on a **per-vCPU FPU-dirty epoch** that flips when ANY task
touches FPU on this vCPU — so cross-task leak is impossible by
construction while the same-task FPU-not-used common case is free
again.

## 2. Confirmation

This section restates what I read in memo 22 (full read), in the four
+50% UP-hop commits, and in `arch/um/backend/kvm-v2/vcpu.c` around the
FPU sites — in my own words.

### 2.1 Memo `22-smp-t41-stress-and-perf.md` — what it says

Memo 22 is primarily a T41-fix validation report: byte[0]=0
STRICT_MEMSET_FAIL is closed at root cause (0/24000 over N=1000
boots), substrate parity is preserved, and the T41 fix introduces no
measurable wall-clock overhead (T39-vs-T41-fix table reads identical
to the 10 ms printk-time granularity). Then the memo's last section
("Known perf regression vs 2026-04-30 baseline") tabulates a
three-way comparison from `scoreboard.jsonl` plus on-host re-runs:

| Build | Commit | seccomp_med | kvm_v2_med | Ratio | Era |
|---|---|---|---|---|---|
| 04-30 baseline (UP) | `f1e3130a69af` | 0.090 s | **0.040 s** | **0.444** | pre-H.2 (peak v2 perf) |
| 05-02 UP build | `ad18db7c3768` | 0.090 s | 0.060 s | 0.667 | post-tlb-kick-activate, pre-T26/T27 |
| 05-03 SMP build | `cf98c8d21e99` + T41 | 0.120 s | 0.100 s | 0.833 | post-T26/T27/T33 + SMP flavor |

Memo 22's decomposition:

  1. **UP→UP, 04-30→05-02 (kvm-v2 0.04→0.06, +50%)** — same build
     flavor, same seccomp wall-clock. ~99 commits between; memo names
     four candidate hot-path additions: `7e1c255a09ad`,
     `9f0ff6257e8b`, `95b3a85bd309`, `e5977806fd14`.
  2. **UP→SMP + 05-02→05-03 (kvm-v2 0.06→0.10, +67%)** — both build
     flavor (CONFIG_SMP=y, NR_CPUS=4 → +33% on seccomp wall-clock too)
     and SMP-T26/T27 (`76b1d98b2006`) which "REVERTED the H.2 lazy-FPU
     optimization (`ba9c83331f30`) by forcing `KVM_GET_FPU` after
     EVERY `KVM_RUN`".

The memo names this **SMP-T55** and proposes: bisect the worst single
commit in the UP window, investigate whether T26/T27's always-GET_FPU
can be cheaper while remaining correct, restore <0.5 ratio if
possible.

The 2026-05-07 reproduction run (kernel
`~/src/uml-builds/uml-smp-t41fix/linux`, current HEAD as of 18:41)
reads ratio=1.250 — i.e. on the Zen 4 host the regression has crept
~50% further than memo 22's 0.833 number (unsurprising: 0.833 was
already against a 0.120 s SMP seccomp baseline, and run-to-run
printk-time bucket noise at 10 ms granularity easily moves ratio by
±15 pp). What matters is that the gate ceiling is 1.20× and we are
above it.

STATUS.md line 13 ("All gates clean post-gadget") is therefore stale
— SMP-T55 has been a known open item since memo 22 (2026-05-03) and
the gate now exceeds the ceiling.

### 2.2 The four UP-hop commits (`git show <hash>` for each)

`7e1c255a09ad` — *um: kvm-v2: implement tlb_kick_others; unblock
IPI_SIGNAL during KVM_RUN* (Phase G.2 commit B):
  - Adds `kvm_v2_tlb_kick_others(mm)` that walks `for_each_online_cpu`
    and `os_send_ipi(cpu, UML_IPI_RES)` for each remote CPU (stub on
    UP).
  - Modifies `kvm_v2_install_signal_mask` to `sigdelset` `IPI_SIGNAL`
    from `KVM_SET_SIGNAL_MASK` under SMP.
  - Commit message states "doesn't change runtime behavior on its own
    (no caller invokes tlb_kick_others)" — the cost is install-time
    (one `KVM_SET_SIGNAL_MASK`-shaped change) and the unconditional
    `for_each_online_cpu` path that is still a stub on UP.
  - **Hot-path impact UP-build:** small. The visible cost is the
    `signal_mask` adjustment which fires once at vCPU create.

`9f0ff6257e8b` — *um: kvm-v2: cmpxchg-dedup'd tlb_kick_others
(activation still deferred)*:
  - Adds `atomic_t kick_pending` to `struct kvm_v2_vcpu` for IPI
    dedup via `cmpxchg(0,1)`.
  - Adds an ack site in `kvm_v2_load_user_sregs` (one
    `atomic_set(&vcpu->kick_pending, 0)` per dispatch, on every
    dispatch — the activation point in `arch/um/kernel/tlb.c` is
    commented out, but the ack is in the dispatch hot path).
  - **Hot-path impact UP-build:** one atomic store per dispatch.
    Cheap on Zen 4 (sub-ns); not the headline cost.

`95b3a85bd309` — *um: kvm-v2: use migrate_disable() instead of
preempt_disable() in vcpu_run*:
  - Replaces `preempt_disable()`/`preempt_enable()` (NO-OPs under
    `CONFIG_PREEMPT_VOLUNTARY` without `CONFIG_PREEMPT_COUNT`) with
    `migrate_disable()`/`migrate_enable()` at three sites in
    `kvm_v2_vcpu_run`.
  - **Real correctness fix:** without this, `schedule()` could
    migrate the task mid-dispatch and `vcpu`/`run` ptrs got stale.
    Mt-yieldonly went from ~10% to 100% PASS.
  - **Hot-path impact UP-build:** `migrate_disable()` increments
    `current->migration_disabled` (a normal store, but with full
    barrier semantics on x86 it's effectively a single memory write
    plus a function call wrapper). Per dispatch we now pay 3 × pair
    (`migrate_disable` + `migrate_enable`) where before we paid 3 ×
    pair NO-OP. Cost: ~5-15 cycles each → ~50-100 cycles per
    dispatch.

`e5977806fd14` — *um: kvm-v2: handle EINTR-mid-stub PF inline (SMP
fix)*:
  - Adds `kvm_v2_handle_pf_eintr_inline()` that processes a #PF
    inline at EINTR time (when the EINTR caught the vCPU
    mid-PF-stub).
  - Adds the EINTR-detection/dispatch logic to `vcpu_run` itself
    (vcpu.c diff +48 lines).
  - **Hot-path impact UP-build:** the *path* is inactive when EINTR
    didn't catch a PF stub (the common case). T41 added 1 IST-top-56
    read + 1 conditional branch in the EINTR-mid-PF-stub path; not
    the +50% cost. The cost is the EINTR detection itself, plus a
    conditional that runs every dispatch.

**Net UP-hop cost per dispatch:** my read is the dominant per-
dispatch additions are `migrate_disable`/`migrate_enable` (3 pairs in
`vcpu_run`) and the new EINTR detection branch. None of these alone
should be +50%, which suggests that the cumulative effect across ~99
commits in this window includes secondary additions — state-trace
ring instrumentation, several debug pr_warn paths, the mmap-return
diagnostic at `d9f6787672ab`, and the SMP-T16/T22/T23/T25 cr2 and
nm_ts_bypass paths. A bisect is the only way to localise the worst
single offender; memo 22 explicitly defers that.

### 2.3 The T26/T27 fix (`76b1d98b2006`) — does the commit message mention reverting H.2?

**Yes.** The commit message (verified via `git show
76b1d98b2006`) explicitly says:

> "Phase H.2 (CR0.TS lazy-FPU optimization) is effectively REVERTED
> for the GET-FPU side; the SET-FPU side at vcpu.c:1772 retains the
> 'only install if valid' check (which is now always true after this
> fix's GET)."

Diary memo `state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md`
§"Phase H.2 status" says the same in different words: "Phase H.2 is
now effectively HALF-REVERTED. The GET-FPU side (this fix) always
runs after every KVM_RUN."

The commit message also gives the cost: "~1µs/dispatch overhead for
the always-on KVM_GET_FPU. For syscall-heavy workloads, this offsets
the perf gain of Phase H.2."

### 2.4 The actual code at the always-`KVM_GET_FPU` site

Read directly from `arch/um/backend/kvm-v2/vcpu.c` (HEAD as of this
memo):

  - **Pre-KVM_RUN install (SET):** vcpu.c:2006-2010, inside
    `kvm_v2_vcpu_run`. Gated on `iotrap_fpu_valid`; after T26/T27
    that flag is true on every dispatch, so the SET runs every
    dispatch.
  - **Post-KVM_RUN capture (GET):** vcpu.c:2083-2085. Unconditional —
    a bare `os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_FPU, …)` plus
    `iotrap_fpu_valid = (rc == 0)`. No `if (TS == 1) skip` gate
    remains. The 60+ lines of comment around vcpu.c:2030-2082 narrate
    the H.1b smoking-gun fix and the T26/T27 always-GET reasoning.
  - **TS-arming on SET side:** vcpu.c:1477-1482, inside
    `kvm_v2_load_user_sregs`. Unconditional `sregs->cr0 |= TS` on
    every dispatch (except for the one-shot `nm_ts_bypass` path
    after a host-side `kvm_v2_handle_io_nm` clear). The in-guest
    IDT[7] #NM stub (`clts; iretq`, no vmexit) is still there. So
    the TS-arming machinery is intact and detectable post-vmexit —
    we just don't act on it.
  - **`fpu_capture_for_fork`:** vcpu.c:2437-2476. Snapshots a parent
    task's FPU at fork into `to->thread.arch.kvm_v2.fpu` so the
    child's first dispatch installs from the snapshot via
    `kvm_v2_fpu_install_on_first_run`. Hot-path-irrelevant (only on
    fork).
  - **`fpu_capture_for_switch_out`:** vcpu.c:2507-2588. Hooked from
    `kvm_v2_context_switch` (vcpu.c:2606-2633), runs at every UML
    context switch under v2. Has a SMP-T29 gate: `if (vcpu->last_task
    != from) return` — only captures when the per-host-CPU vCPU's
    FPU still belongs to `from`. So after T29 this site is
    one-`KVM_GET_FPU` per same-task switch-out, zero per cross-task.

**STATUS line 130's count "5 sites including 3 in vcpu_run" — wrong.**
There are 4 FPU ioctl sites in this file: 1 SET in `vcpu_run`
(line 2007), 1 GET in `vcpu_run` (line 2083), 1 GET in
`fpu_capture_for_fork` (line 2460), 1 GET in
`fpu_capture_for_switch_out` (line 2572). The "5/3-in-vcpu_run"
count appears to be a stale reference to a defunct shape (perhaps
the pre-T26/T27 version that had a third site for the H.1b
restore + a separate H.2 GET-skip test). Reviewers should rely on
the line numbers above, not on STATUS.

The struct `arch_thread.kvm_v2` (in `arch/x86/um/asm/processor_64.h`)
holds the per-task fields that drive this state machine:

  - `kvm_v2.fpu` + `fpu_valid` — fork/switch-out snapshot, restored
    via `kvm_v2_fpu_install_on_first_run`.
  - `kvm_v2.iotrap_fpu` + `iotrap_fpu_valid` — *post-KVM_RUN* capture
    (the H.1b/T26-T27 path), restored at the *next* dispatch's pre-run
    SET at vcpu.c:2006-2010.
  - `kvm_v2.nm_ts_bypass` — one-shot bypass of the TS arming after a
    host-side `kvm_v2_handle_io_nm` (so the user's FP retry doesn't
    immediately #NM-loop).

So the fix space has TWO orthogonal correctness levers: (a) when to
GET the FPU (T26/T27 said: every dispatch), and (b) when to SET it
(today: every dispatch where `iotrap_fpu_valid`, which after T26/T27
is every dispatch). Both reverted halves of H.2 contribute cost on
syscall-heavy workloads.

## 3. Root cause hypothesis

I split the regression into the two contributors memo 22 names.

### 3.1 The +50% UP→UP hop (commits in the 04-30 → 05-02 window)

  - **Code path:** the dispatch loop body of `kvm_v2_vcpu_run` and the
    function `kvm_v2_load_user_sregs` it calls every dispatch. Net
    additions across the four named commits + ~95 unnamed others:
    `migrate_disable`/`migrate_enable` ×3, atomic
    `kick_pending` ack (one store/dispatch),
    state-trace ring update (one store/dispatch on
    instrumented builds), the SMP-T16/T22/T23 cr2-gate logic, the
    nm_ts_bypass conditional, the EINTR-mid-PF-stub detection
    branch.
  - **Why startup more than steady-state:** Python startup is
    syscall-heavy (mmap, brk, openat, read, mprotect) — so it spends
    more wall-clock per second in `kvm_v2_vcpu_run` *boundary* code
    than in actual guest execution. Every per-dispatch cycle scales
    linearly with dispatch count, and startup hits ~30k-50k
    dispatches in <100 ms. The bench-py "trap" subsection
    (`os.getpid()` × 20000) is normally insulated from this because
    it runs through the gadget — gadget-resolved syscalls do NOT do
    a full `KVM_RUN` round-trip and don't pay the per-dispatch FPU
    or TS-arming cost. Bare process startup uses pre-gadget syscalls
    until the gadget is mapped. So bench-py reads 4× faster while
    perf-py-startup reads 1.25× slower — same kernel, different
    workload mix.
  - **Confidence:** medium. I haven't bisected. My read is that the
    +50% is *cumulative* across many small additions, no single
    +30% offender — which makes a bisect productive but slow. The
    memo's hypothesis to bisect is the right call.

### 3.2 The +67% UP→SMP / always-`KVM_GET_FPU` hop

  - **Code path:** `arch/um/backend/kvm-v2/vcpu.c:2083-2085`. One
    `os_ioctl_generic(vcpu_fd, KVM_GET_FPU, &iotrap_fpu)` per
    dispatch. KVM's `kvm_arch_vcpu_ioctl_get_fpu` does an FPU save
    (XSAVE/FXSAVE depending on host CPU) of the vCPU's `guest_fpu`
    struct and copies into userspace. Plus the matching `KVM_SET_FPU`
    at vcpu.c:2007 every dispatch — also reactivated by T26/T27
    because `iotrap_fpu_valid` is now true every dispatch.
  - **Why startup more than steady-state:** at startup, the FPU
    rarely runs (the early init shell, libc startup, dynamic linker,
    Python's `_PyRuntime_Initialize` only touch FPU during specific
    XMM memcpy intrinsics in libc). Most of the dispatches during
    the 100 ms startup window are FPU-cold, exactly the case where
    pre-T26/T27 H.2 skipped 95% of GETs. Now we pay a full GET +
    full SET on every cold dispatch. At ~1 µs/ioctl × ~30-50k
    dispatches = 30-50 ms additional wall-clock — which lines up
    with the 0.04→0.06 → 0.10 progression on UP and the parallel
    +67% on SMP.
  - **Confidence:** high. Both the commit message of `76b1d98b2006`
    and memo 15 explicitly call out the cost and explicitly call out
    that it offsets H.2's gain on syscall-heavy workloads. The
    perf-py-startup workload IS the canonical syscall-heavy mix.

### 3.3 The CONFIG_SMP=y build-flavor cost

Memo 22 notes seccomp wall-clock also went 0.090 → 0.120 (+33%) when
CONFIG_SMP=y was enabled. That's the SMP build cost (extra IPI
plumbing, RCU, locking — NOT v2-specific). It is part of the 1.250
ratio numerator and denominator both, so it cancels in the ratio (we
compare kvm-v2 vs seccomp on the same kernel). Important context but
not actionable from the v2 backend.

## 4. Fix options

Three options listed in the task brief, plus a fourth (option d) that
came out of reading the code.

### 4.1 Option (a) — per-vCPU FPU-dirty epoch flag

**Sketch.** Add to `struct kvm_v2_vcpu` a `bool fpu_dirty` field,
plus we keep using the existing per-task `iotrap_fpu_valid`. The
state machine becomes:

```c
/* in load_user_sregs (entry side, SET) */
if (current->thread.arch.kvm_v2.iotrap_fpu_valid) {
    KVM_SET_FPU(iotrap_fpu);
    /* iotrap_fpu_valid stays true — we cleared TS in nm_ts_bypass-handled
       paths, but the cached snapshot is still authoritative for THIS task
       on THIS vCPU until something else dirties it */
}
vcpu->fpu_dirty = false;     /* we just synced; clean as of pre-run */

/* The in-guest IDT[7] #NM stub still does clts;iretq.
   The post-vmexit handler (kvm_v2_handle_io_nm) ALSO sets
   vcpu->fpu_dirty = true.  And: any path that we KNOW touches FPU on
   this vCPU (e.g. KVM_RUN exits with sregs.cr0.TS==0, meaning the
   guest issued an FP/SSE instruction) also sets vcpu->fpu_dirty = true. */

/* post-KVM_RUN (GET) */
bool fpu_was_used = !(run->s.regs.sregs.cr0 & X86_CR0_TS);
if (fpu_was_used)
    vcpu->fpu_dirty = true;
if (vcpu->fpu_dirty) {
    KVM_GET_FPU(&iotrap_fpu);
    iotrap_fpu_valid = true;
}
/* else: vCPU FPU is still bit-identical to what we SET pre-run (which
   was current's iotrap_fpu), so iotrap_fpu remains the authoritative
   per-task snapshot. */
```

**State-machine change.** Adds one bool to `kvm_v2_vcpu`. Replaces
the unconditional GET with a guard. Adds a small bookkeeping step:
mark dirty on (a) post-vmexit FPU-used (TS=0 in sregs), (b) any
context-switch arrival on this vCPU from a different task, (c) any
out-of-band KVM internal that might touch guest_fpu.

**Correctness argument vs T26/T27.** T26/T27's bug was: task X
re-dispatches on a vCPU where task Y has run between, X's
`iotrap_fpu_valid=false`, install-on-first-run no-ops, X resumes
with Y's XMM. The dirty epoch fixes this directly: when task Y ran,
*its* dispatch's post-vmexit GET ran (because either Y touched FPU
→ dirty=true, or `vcpu->fpu_dirty` was already true from a prior
unsynced state). Y's GET captured Y's FPU into Y's
`iotrap_fpu`. Critically, the cross-task case is captured by adding
a "task switched on this vCPU" hook: when `vcpu->last_task != current`
in `load_user_sregs`, we MUST do a GET-before-SET (or assume dirty)
because the vCPU's FPU is whatever the *previous* owning task left.

The minimum-correct rule is: at the moment `load_user_sregs` notices
`vcpu->last_task != current`, the OUTGOING task's FPU still lives in
the vCPU. We must ensure that outgoing task's `iotrap_fpu` is either
captured already (its previous dispatch did a GET) OR we capture it
now into the previous task's slot. The simplest version: piggyback
on the existing `kvm_v2_fpu_capture_for_switch_out` path (already
hooked from `kvm_v2_context_switch`). If we GUARANTEE that
capture-for-switch-out always captures `from`'s FPU into `from`'s
snapshot at every UML-level context switch, then the `iotrap_fpu`
slot is no longer the authoritative store — the per-task `fpu`
snapshot is — and we can drop `iotrap_fpu` entirely. (Option (d)
below explores this.)

For the conservative version of (a), keep `iotrap_fpu` as today, but
at every dispatch entry where the dirty-epoch is set OR the task
mismatches, do the GET. The skip then fires only on
**same-task back-to-back FPU-cold dispatches** — which is exactly
the H.2 win.

**Risk class.** MEDIUM. The reasoning relies on enumerating every
path that can dirty `vcpu->arch.guest_fpu`. KVM's internals can
touch guest_fpu in surprising places (SMM emulation, kernel/user
FPU swap on host preempt — KVM is supposed to defend its
`guest_fpu` against host preemption but the contract is subtle).
Need a thorough audit of every KVM ioctl path we use to confirm
nothing else dirties.

### 4.2 Option (b) — gate `KVM_GET_FPU` on `vcpu->last_task != current`

**Sketch.**

```c
/* post-KVM_RUN (GET) */
bool cross_task = (vcpu->last_task != current);
bool fpu_was_used = !(run->s.regs.sregs.cr0 & X86_CR0_TS);
if (cross_task || fpu_was_used) {
    KVM_GET_FPU(&iotrap_fpu);
    iotrap_fpu_valid = true;
}
```

**State-machine change.** Reuses the existing `vcpu->last_task` field
already maintained by `kvm_v2_load_user_sregs` (vcpu.c:1342, set at
every dispatch). No new fields. Mirrors the existing T16/T23/T29
`last_task != current` pattern.

**Correctness argument vs T26/T27.** PARTIAL — and this is a
problem. Re-read memo 15 §"smoking gun" carefully:

  > "Between dispatch 4210668 (consume valid) and 4210723 (finally
  > capture fresh), task ran 4 dispatches — and during this window,
  > the shared per-host-CPU vCPU could have run other tasks…"

The leak is *between* dispatches, but the gate `last_task != current`
only fires AT the cross-task transition. Consider:

  - Dispatch K (task X): cold. `last_task=X` (no transition). Skip GET.
  - Some KVM internal/host preempt swaps guest_fpu (rare but possible).
  - Dispatch K+1 (task X again): `last_task==X`, no cross-task,
    skip GET. iotrap_fpu_valid still consumed.
  - Task Y dispatches; cross_task=true; we GET (good). But Y's GET
    captures Y's FPU into Y's iotrap_fpu, not X's. X's slot remains
    stale.
  - Task X dispatches again: `last_task=Y → X`, cross_task=true. We
    GET — but this captures *Y's leftover* (or whatever the vCPU has
    now) and writes it into X's iotrap_fpu_valid. **WRONG.** We've
    re-introduced the T26/T27 contamination, just with a different
    edge.

So option (b) is **NOT safe without coupling it to a write at the
outgoing edge**. To make (b) safe we'd also need to GET-on-same-task
at minimum once per "task ran something we didn't capture". The dirty
epoch in (a) is exactly that "did anyone modify the vCPU's FPU since
last GET" predicate, which is the right primitive.

**Risk class.** HIGH. As written, this IS the T26/T27 bug
re-introduced. Don't ship without (a)'s dirty bit or equivalent.

### 4.3 Option (c) — lazy capture at switch-out time only

**Sketch.** Drop the post-KVM_RUN GET entirely. Move authoritative
capture to `kvm_v2_fpu_capture_for_switch_out`, which already runs
at every UML context switch. Per-task `iotrap_fpu` becomes redundant
with the per-task `fpu` snapshot.

```c
/* post-KVM_RUN: do NOTHING for FPU (just clear iotrap_fpu_valid). */

/* kvm_v2_context_switch (already runs on every from→to switch): */
kvm_v2_fpu_capture_for_switch_out(from);
/* This already does: if (vcpu->last_task == from) KVM_GET_FPU(&from->fpu).
   The 'last_task == from' gate works because if it doesn't, the vCPU
   FPU is from someone else and from's snapshot is already authoritative
   via fork-snapshot or its own prior switch-out. */

/* kvm_v2_fpu_install_on_first_run: install snapshot via KVM_SET_FPU
   on every first-dispatch-after-context-switch. */
```

**State-machine change.** Removes the
`iotrap_fpu`/`iotrap_fpu_valid` slot entirely. Makes the only FPU
storage the per-task `fpu` slot, captured at switch-out, restored at
first-dispatch-after-switch-in.

**Correctness argument vs T26/T27.** This is closest to v1 archive's
shape, and v1 didn't have the T26/T27 bug. The reason it works in v1
is that v1 had per-task vCPUs (so "the vCPU's FPU == this task's
FPU" was tautological) — this option achieves the same invariant in
v2 by ensuring at every context switch the outgoing task's FPU is
captured before the next task gets the vCPU.

**But** there's a subtle gap: the T22 nm_ts_bypass path. If task X
takes an #NM, the host clears TS but does NOT install X's FPU back
— it relies on the next pre-KVM_RUN SET. Today, `iotrap_fpu_valid`
is true after T26/T27's GET, so the SET runs. If we remove the GET,
then on a back-to-back same-task FP-using burst,
`iotrap_fpu_valid=false` at dispatch K+1, install-on-first-run is a
no-op, vCPU's `guest_fpu` is whatever-it-was — for *same-task
back-to-back* this is fine (the vCPU's FPU IS X's FPU, no other task
touched it). For *cross-task* the switch-out capture (now mandatory)
saves X's state.

So (c) is correct ONLY IF every cross-task transition goes through
`kvm_v2_context_switch`. This is true for normal UML scheduling but
NOT for the per-host-CPU vCPU pool's task migration via
`migrate_disable`/`migrate_enable`. After SMP-T13 the task can't
migrate mid-dispatch — but it CAN migrate between dispatches. And
when it does: task X runs on vCPU A, schedule(), task X runs on
vCPU B (the migrate happened during a host-side wait).
`kvm_v2_context_switch` fires at the schedule() boundary AND
captures X's FPU from vCPU A into X's snapshot. Next dispatch on B,
`kvm_v2_fpu_install_on_first_run` SETs the snapshot to vCPU B. So
this is fine.

But: what about task X dispatching on vCPU A, releasing migrate, NOT
context-switching, then dispatching again on vCPU A — but in the
window between the two dispatches some other task *did* dispatch on
vCPU A (this is exactly the T26/T27 setup). Has the other task's
schedule-out fired `kvm_v2_context_switch`? Yes — every `schedule()`
that picks a different `to` task fires `kvm_v2_context_switch`. So
the other task got captured at switch-out. The vCPU's FPU is whatever
the *most recent* task on it left, which was the other task. When X
dispatches again, **X's `fpu_valid` is false** (it was consumed when
X first dispatched). install_on_first_run no-ops. **vCPU has wrong
task's FPU.** This is the T26/T27 bug again.

So we'd need to make `kvm_v2_fpu_install_on_first_run` install
EVERY time we cross task boundaries on a vCPU — i.e. when
`vcpu->last_task != current`. That means we'd need to keep
`fpu_valid` true after install, OR re-capture at switch-IN as
well. The simpler shape: make every dispatch where
`vcpu->last_task != current` install the per-task `fpu` snapshot
via SET, and after every dispatch where the vCPU's
`last_task` was someone else than `current`, GET into our per-task
snapshot (or rely on switch-out having done so).

This is getting complicated. (c) by itself is incomplete. (c) needs
to be combined with "always re-install on cross-task" and "always
capture at switch-out" to be sufficient.

**Risk class.** HIGH. The state machine has more edge cases than
(a). Cross-task dispatch sequences without a UML schedule boundary
in between (does this exist? probably not under v2's pinned vCPU,
but I haven't proven it) could leak.

### 4.4 Option (d) — collapse `iotrap_fpu` into `fpu`, gate dispatch on dirty epoch

This is what (a)+(c) would actually become if combined. Don't
re-frame yet — recommend (a) plain.

## 5. Recommendation

**Recommend option (a), per-vCPU FPU-dirty epoch flag.**

Justification:

  1. **Smallest delta to known-good.** It keeps the T26/T27 fix's
     post-vmexit GET site, just guards it. The SET side is unchanged.
     The per-task `iotrap_fpu` slot stays. Most of the existing
     proof-of-correctness from memo 15 still applies.
  2. **Restores H.2's gain in the right shape.** H.2 originally tried
     to skip on `sregs.cr0.TS==1`. That's broken because TS==1 is a
     property of *this task's* dispatch, not "the vCPU FPU is this
     task's". Option (a) replaces the per-dispatch TS predicate with
     a per-vCPU dirty predicate, which is the correct semantic.
  3. **Cheap.** Adds one bool to `struct kvm_v2_vcpu`, ~3 reads/
     writes per dispatch. Cost ~5-10 cycles. Save ~1 µs per skipped
     GET → ~95% of dispatches → ~30-50 ms wall-clock at startup.
     Should restore ratio to ~0.5 (the pre-T26/T27 era).
  4. **The cross-task case is captured by an explicit hook,** not by
     accidental ordering. We add: at `load_user_sregs`'s
     `last_task != current` site (already detected, vcpu.c:1223,
     :1342), set `vcpu->fpu_dirty = true`. That's the moment
     ownership transfers; the next post-vmexit GET will capture into
     the new task's slot.
  5. **The "what dirties the vCPU FPU outside our control" worry is
     bounded.** Under v2, the only sites that touch
     `vcpu->arch.guest_fpu` are KVM-internal — and KVM is required
     to keep `guest_fpu` consistent across host-side preemption (it
     uses kernel_fpu_begin/end internally). Audit the KVM ioctls v2
     uses; mark dirty conservatively if any could write guest_fpu
     out-of-band.

Gotchas to watch for:

  - **`kvm_v2_handle_io_nm` clears TS without restoring FPU.** Memo
    15 §"Why prior ablations didn't help" notes the handler clears
    TS and sets `nm_ts_bypass=true` so the next dispatch re-enters
    with TS=0. The next dispatch's pre-run SET runs from
    `iotrap_fpu_valid` which is true (post-T26/T27). Under (a) this
    still holds: when the user retries the FP instruction, vmexit's
    `sregs.cr0.TS==0`, we set `vcpu->fpu_dirty = true`, and the GET
    runs. Good.
  - **The very first dispatch of a freshly-fork'd task.** Today the
    capture-for-fork copies parent's FPU into child's
    `kvm_v2.fpu`/`fpu_valid=true`. install_on_first_run consumes
    that. After install, `iotrap_fpu_valid=false`. Under (a), if the
    child's first dispatch is FPU-cold AND the vCPU isn't dirty,
    we'd skip the GET — leaving `iotrap_fpu_valid=false`. On the
    next dispatch, install_on_first_run no-ops (both flags false).
    BUG class T26/T27 again, narrowly.
    **Mitigation:** at install_on_first_run's `if fpu_valid` branch,
    also do `vcpu->fpu_dirty = true` so the post-vmexit GET fires.
    That captures the freshly-installed snapshot back into
    `iotrap_fpu`. Marginal cost (one GET per fork-first-dispatch).
  - **Cross-vCPU migration.** A task can move between per-host-CPU
    vCPUs via UML's scheduler. At the destination,
    `vcpu->last_task != current` triggers (a)'s dirty mark, and the
    destination vCPU's FPU is *some other task's* — install via
    `iotrap_fpu_valid` (likely true if the source dispatch ended
    with a GET, OR via `fpu_valid` from `capture_for_switch_out` if
    a context-switch happened in between). Either path installs the
    correct task FPU. Watch for the case where source dispatch
    skipped its GET (because cold + clean) AND no context switch
    fired — the source's `iotrap_fpu` might be stale. The fix here
    is the same as the freshly-fork'd-task gotcha: ANY skip-of-GET
    must guarantee that `iotrap_fpu` is bit-identical to what the
    vCPU has. That's true by induction if (a)'s invariant
    "fpu_dirty=false ⇒ vCPU's guest_fpu == iotrap_fpu" holds, which
    it does as long as we mark dirty on ALL paths that change the
    vCPU FPU — including the SET we just did at pre-run.

    Concretely: at vcpu.c:2007's `KVM_SET_FPU` site, do NOT set
    `vcpu->fpu_dirty = false` because we just made it match the
    snapshot. Set `vcpu->fpu_owner_task = current` (or equivalent).
    Then at post-vmexit, only skip if `fpu_dirty=false AND
    fpu_owner_task == current`. The latter rules out the cross-task
    leak even if we miss a dirtying event.

Net: option (a) is ~30 lines of code in `vcpu.c` plus one bool
field. The work is in the **gating predicate audit**, not the
implementation.

## 6. Experimental gate plan

### 6.1 Pre-fix repro (confirms the regression)

**Reproducer:**

```bash
cd /home/mjbommar/projects/personal/linux
UML_BINARY=$HOME/src/uml-builds/uml-smp-t41fix/linux \
  SAMPLES=10 MAX_V2_RATIO=1.2 \
  bash tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh
```

**Expected output:** exit 1 (FAIL), `ratio_v2_over_seccomp ≥ 1.20`.
The 2026-05-07 reading is 1.250 (kvm-v2 0.100s vs seccomp 0.080s).
For statistical robustness, run 3 invocations of the script and
record the median ratio — printk's 10 ms time grain means a single
script's ratio can bucket-jump by ±0.10.

### 6.2 Post-fix repro (gate must pass)

Same command. **Targets:**

  - **Hard requirement (gate ceiling):** ratio ≤ 1.20.
  - **Stretch target (matches pre-T26/T27 era):** ratio ≤ 0.55.
    The 2026-04-30 baseline read 0.444; we won't fully recover that
    because the +50% UP-hop is a separate pile, but the +67% T26/T27
    hop should fully reverse. Expected: 0.55-0.70.

If the post-fix ratio lands ≥ 0.70 the dirty-epoch optimization is
not catching enough skips — add a counter (kvm_v2 `fpu_skip_count` /
`fpu_get_count`) and inspect via debugfs/trace to see the actual
skip rate. Memo 15 expected ~95% skip on syscall-heavy workloads;
anything <50% on perf-py-startup is suspicious.

### 6.3 Regression-guard battery

These must ALL pass post-fix at the same level they pass today.
Reproducers are kselftests; the gate-runner harness lives in
`tools/testing/selftests/um/gates/`.

| Gate | Reproducer | Pre-fix today | Post-fix target |
|---|---|---|---|
| substrate (kvm-v2) | `tools/testing/selftests/um/gates/regrtest-substrate.toml` (force=kvm-v2) | PASS=25/FAIL=3/XFAIL=3 | unchanged |
| substrate (seccomp) | same, force=seccomp | PASS=25/FAIL=3/XFAIL=3 | unchanged |
| cpython-tier0 (kvm-v2) | `tools/testing/selftests/um/gates/cpython-tier0.toml` | PASS | PASS |
| cpython-parity (kvm-v2) | `tools/testing/selftests/um/cpython-parity/cpython-parity.sh` | 21/21 | 21/21 |
| mt-mini SMP T=8 ncpus=4 N=30 | `tools/testing/selftests/um/mt-mmap-stress/mt-mini` (ITERS=50; N=30 boots) | 30/30 | 30/30 (≥99% PASS rate, Wilson 95% ≥ 88%) |
| bench-py (kvm-v2 vs seccomp) | `tools/testing/selftests/um/bench/bench-py.toml` | kvm-v2 ~3-4× faster than seccomp; absolute ~125 ms on Zen 4 | unchanged ratio; absolute may improve ~5-10% (post-fix kernel slightly cheaper per dispatch) |

**Specific commands:**

```bash
# substrate gate, both backends
KVM_V2_BIN=$HOME/src/uml-builds/uml-smp-t41fix-fpu-fix/linux  # post-fix
for backend in kvm-v2 seccomp; do
  python3 tools/testing/selftests/um/gates/run-gate.py \
    --gate tools/testing/selftests/um/gates/regrtest-substrate.toml \
    --kernel "$KVM_V2_BIN" --backend "$backend"
done

# cpython-parity (21 modules)
KERNEL="$KVM_V2_BIN" tools/testing/selftests/um/cpython-parity/cpython-parity.sh

# mt-mini SMP — N=30 boots
for i in $(seq 1 30); do
  timeout 30 "$KVM_V2_BIN" backend=force=kvm-v2 mem=512M ncpus=4 \
    rootfstype=hostfs root=/dev/root rw \
    init=tools/testing/selftests/um/mt-mmap-stress/mt-mini-init.sh \
    con=null con0=fd:0,fd:1 panic=-1 \
    | tee /tmp/mt-mini-$i.log
done
grep -c "PASS" /tmp/mt-mini-*.log

# bench-py
python3 tools/testing/selftests/um/gates/run-gate.py \
  --gate tools/testing/selftests/um/bench/bench-py.toml \
  --kernel "$KVM_V2_BIN" --backend kvm-v2
```

### 6.4 Critical correctness probe — does the fix preserve T26/T27's guarantee?

This is the most important part of the gate plan. T26/T27 was caught
by `threaded-fork-malloc` (see
`tools/testing/selftests/um/fork-tree-3level/repros/threaded-fork-malloc.c`
and memo 15 §Validation). The reproducer:

```bash
# Spawn 8 worker pthreads, each loops 500 iterations of:
#   fork() → exec(self) → child does no-op exit
# Total = 4000 forks per boot. Pre-T26/T27: 0.15%/fork CHILD_FAIL
# (= ~6 fails per 4000 forks, accumulating to 36/24000 across 6 boots).
# Post-T26/T27: 0/24000 (Wilson 95% upper bound 0.015%).

cd tools/testing/selftests/um/fork-tree-3level/repros
make threaded-fork-malloc
# Boot UML with init=this-binary 8 500
# Repeat 6 boots.
# PASS criterion: total_fails == 0 across all boots.
```

To make this airtight as a probe for the proposed fix:

  - **N=6 boots minimum** (the original T26/T27 ablation size).
    Counts: 8 workers × 500 iters × 6 boots = 24000 forks. With the
    pre-fix rate of 0.15%/fork → expected 36 fails; observed 0.
    Wilson 95% upper bound is ~0.015%/fork. If the proposed fix
    re-introduces *any* contamination, we expect the failure rate to
    return to 0.15%/fork → 36 fails / 24000. The detection power at
    N=6 is essentially perfect for that effect size.
  - **Stretch N=12 boots (48000 forks)** to detect a 4× weakened
    leak (e.g. only fires in 0.04%/fork). At α=0.05 we can detect
    rates ≥ ~0.005%/fork, an order of magnitude below the original.
  - **Companion probe — `mt-xmmprobe`.** This is a targeted XMM
    contamination detector at
    `tools/testing/selftests/um/mt-mmap-stress/mt-xmmprobe.c`. Run
    N=15 trials at T=4 ncpus=4. Pre-T26/T27 had 1-2 failures per 15;
    post-T26/T27 reads 15/15. If the proposed fix re-introduces any
    XMM bit-leak, we'll see it here too.

**Pass criterion for the correctness probe (combined):**
  - threaded-fork-malloc: 0/24000 across 6 boots (or 0/48000 at
    N=12). **Wilson 95% upper bound on fail rate ≤ 0.015%/fork.**
  - mt-xmmprobe: 15/15 at T=4 ncpus=4.

**If the proposed fix fails this probe, do NOT ship.** The H.2
optimization restoration is a perf win; it is not worth even a
0.05%/fork CHILD_FAIL regression.

### 6.5 Optional: bisect the +50% UP-hop

Memo 22 calls this out as a separate item. Out of scope for the
T26/T27 perf restoration but adjacent. Suggested:

```bash
# build at ad18db7c3768 (post-UP-hop, pre-T26/T27)
# build at f1e3130a69af (pre-UP-hop)
# run perf-py-startup against each — should reproduce 0.444 vs 0.667.
# Then bisect across the ~99-commit range with the script as the test.
git bisect start ad18db7c3768 f1e3130a69af
# git bisect run with a script that builds and runs perf-py-startup
# with MAX_V2_RATIO set to discriminate (e.g. fail if ratio > 0.55).
```

This is independent of (a) and can land in either order. Combined
target: ratio < 0.50 (recover both halves).

## 7. Open questions

  - **What's the actual cost of `KVM_GET_FPU` on Zen 4?** Memo 15
    says ~1 µs/ioctl based on prior measurements. I haven't confirmed
    on this specific host. A `bpftrace` of
    `kvm_arch_vcpu_ioctl_get_fpu` entry/return latency for 1k samples
    would give a hard number. If it's <0.5 µs the +67% can't be ALL
    from the GET — there's something else (KVM-side dispatch
    overhead, the SET-FPU side which is also always-on after T26/T27).
  - **Does `KVM_SET_FPU` on every dispatch contribute equally?** The
    SET runs every dispatch via `iotrap_fpu_valid=true`. Symmetric
    cost to GET. Option (a)'s skip predicate handles the GET, but a
    matching skip on SET is also needed: skip the SET when
    `iotrap_fpu` is bit-identical to what we last installed AND no
    one else's task has touched the vCPU since. The same dirty-epoch
    primitive serves both. The fix should address both halves.
  - **Is the +50% UP-hop dominated by `migrate_disable` or by
    cumulative small additions?** I couldn't determine this without
    bisect. My read of the diffs says `migrate_disable` is small (<10
    cycles per pair), so the +50% must be cumulative. A bisect is
    the only definitive answer.
  - **Does CONFIG_PREEMPT_DYNAMIC change anything?** UML builds with
    `CONFIG_PREEMPT_VOLUNTARY` (per memo `state-audit/08`). If a
    future config flip activates preempt-count, the cost of
    `migrate_disable` changes (it becomes a real preempt-count
    increment) — relevant for a hypothetical second perf hop later
    but not for SMP-T55 today.
  - **Why is bench-py 4× faster on kvm-v2 while perf-py-startup is
    1.25× slower?** My hypothesis (§3.1) is that bench-py's "trap"
    subsection routes through the gadget and skips the dispatch
    boundary entirely. I didn't trace this directly. Worth a
    `trace-cmd -e um_backend_kvm_v2:vcpu_enter` on each workload and
    a count of dispatches per second — if bench-py's hot loop has
    ~100× fewer dispatches/s than startup's mmap loop, that explains
    the asymmetry. Confirm before claiming the fix improves bench-py
    too.
  - **Is there a simpler fix I'm missing?** Specifically: could the
    H.2 TS-skip be made safe by writing a sentinel into
    `vcpu->arch.guest_fpu` at every cross-task install — say, the
    architectural reset values — so that "TS==1 AND it's still our
    sentinel" provably means our state? That would let the original
    `if (TS==1) skip GET` survive. I think the answer is no, because
    we'd still need to *detect* the cross-task case to write the
    sentinel, at which point we're already on (a)'s state machine.
    But it's worth a paragraph of consideration before coding (a).
  - **Does Phase J's soak rig exercise the fork-FPU correctness path
    enough?** Memo 22 doesn't list `threaded-fork-malloc` as part of
    the J-pilot. We should add it explicitly to the post-fix gate
    battery (see §6.4).

---

**Cross-references:**
  - `state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md` — the
    fix being partially-restored.
  - `state-audit/16-smp-t29-fork-snapshot-clobber-FIXED.md` — the
    `vcpu->last_task` gate pattern this memo's option (a) reuses.
  - `state-audit/22-smp-t41-stress-and-perf.md` — the empirical
    decomposition this memo confirms.
  - `26-v2-implementation-plan.md` §H.2 (lines 2752-2789) — the
    original lazy-FPU design.
  - `tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh`
    — the gate harness.
  - `arch/um/backend/kvm-v2/vcpu.c:2006-2010` (SET site),
    `:2083-2085` (GET site), `:1477-1482` (TS arming),
    `:2507-2588` (`fpu_capture_for_switch_out`),
    `:2657-2691` (`fpu_install_on_first_run`).
  - Commits: `ba9c83331f30` (H.2 ship), `76b1d98b2006` (T26/T27
    revert of H.2 GET-skip), `7e1c255a09ad`, `9f0ff6257e8b`,
    `95b3a85bd309`, `e5977806fd14` (UP-hop window).
