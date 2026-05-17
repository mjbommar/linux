# UML kvm-v2 record/replay port — design memo (2026-05-16)

Strategic Time-machine lift per
`Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`
§4.1 (#169). Sibling to `26-snapshot-v2-port.md` (#168, snapshot
port, landed alongside commit `aa4cd328102c`). Track B's next
strategic lift now that snapshot Phase 1 is in tree: port v1's
1597 LoC record/replay infrastructure
(`arch/um/backend/kvm-v1-archive/record.c`) onto v2's vCPU-pool +
LSTAR-gadget + signal-queue shape.

This memo is the design contract for the #169 ladder. Phase 1
(skeleton + alloc/destroy + static_key) lands as a separate
commit after this memo. Phases 2-7 follow the sub-sequencing in
§5. No code lands with the memo itself.

## TL;DR

The record/replay container is exactly the v1 shape: a
`struct kvm_v2_record` wraps a `struct kvm_v2_snapshot`
checkpoint plus a variable-length `struct kvm_v2_replay_entry`
log, gated by `DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)`
for zero hot-path cost when off. The container surface
(`alloc/start/stop/replay/destroy` plus `observe_dispatch` /
`consume_syscall`) carries over verbatim; the deltas are all
mechanical adaptations to v2's runtime topology.

The v1 → v2 deltas the port has to handle:

  1. **Observation hook site.** v1 hooked `kvm_decode_syscall`
     (per-arm dispatcher in `syscall_class.c`); v2's equivalent
     is `kvm_v2_handle_io_trap` in `syscall_trap.c` (KVM_EXIT_IO
     port `UM_KVM_TRAP_SYSCALL`). One static-key branch, post-
     `handle_syscall`, mirrors v1's call site.
  2. **Gadget-shadowed syscalls.** v2's LSTAR gadget services
     11 hot-path NRs (getpid family + clock_gettime) in-guest
     without a vmexit. Record mode forces gadget-disable so
     those NRs trap and become observable.
  3. **RDTSC / vvar determinism.** v2 lets RDTSC pass through;
     record mode programs the vCPU to vmexit on RDTSC and
     captures the vvar seqlock-protected values on each
     refresh.
  4. **SIGALRM timing.** v2's per-vCPU signal mask delivers
     SIGALRM at host timer ticks. Record captures the
     instruction-retired count at delivery; replay uses
     `KVM_REQ_PMI` style injection at the same count.
  5. **Per-pool vCPU state.** Cross-task vCPU transitions
     (`vcpu->last_task != current`) are themselves nondetermin-
     istic events; record logs the dispatch-on-vCPU-N event.
  6. **XSAVE epoch invalidation.** SMP-T55's lazy `fpu_dirty`
     skip is incompatible with always-recording XSAVE after
     each syscall; record-mode forces always-capture.
  7. **Memslot deltas.** Phase 1 mirrors v1 — full-memslot
     restore before each replay iteration. Dirty-bitmap
     incremental capture is a Phase 2 optimization tracked at
     `28-record-replay-dirty-bitmap.md` (future memo).

Sub-sequencing (§5): Phase 1 = skeleton + static_key + alloc/
destroy; Phase 2 = `observe_syscall` hook in `handle_io_trap`;
Phase 3 = `consume_syscall` replay primitive; Phase 4 = gadget-
disable in record mode; Phase 5 = RDTSC + vvar determinism;
Phase 6 = SIGALRM determinism via PMU; Phase 7 = KUnit + self-
test re-plumb.

## 1. Why a port (vs reimplementation)

Memo 13 (`13-record-replay-determinism.md`) is the strategic
pitch — record at ~2× cost, replay byte-identical across kernel
versions, deliver Phase 3's "time-travel determinism" vision.
The v1 archive proved the design: `kvm-v1-archive/record.c`
shipped through Step 1+2+3+3.5+5 (see memo 13 §Status) with a
working round-trip KUnit, a debugfs control surface, a per-NR
dispatcher router covering 5 NRs (`getrandom`, `read`, `pread64`,
`recvfrom`, `readv`), and end-to-end strict + loose replay
semantics. Snapshot port memo (memo 26) just landed the v2 base
(`arch/um/backend/kvm-v2/snapshot.c` Phase 1). Record/replay
strictly builds on snapshot — the checkpoint primitive IS the
snapshot — so #169 follows the same port-then-extend pattern.

Lifting v1's 1597 LoC with the deltas below is cheaper and
lower-risk than redesigning from scratch. The data structures
(`struct kvm_record` / `struct kvm_replay_entry`), state
machine (recording / replaying / strict_replay), memory model
(per-entry payload + metadata side buffers, kvmalloc lifecycle),
and contract (FIFO consume, -EILSEQ on divergence, -ENODATA
on end-of-log strict) all carry over.

## 2. v1 architecture recap

Compressed restatement of `kvm-v1-archive/record.c`'s design so
the port deltas in §3 land against a shared mental model. Full
detail in the source.

**Container.** `struct kvm_record` wraps a
`struct kvm_snapshot *checkpoint`, a variable-length
`struct kvm_replay_entry *log` (initial 256 entries, grown by
doubling), a `(recording, replaying, replay_cursor,
strict_replay)` state tuple, and payload-cap accounting
(`payload_bytes / payload_drops`).

**Public surface.** `kvm_record_alloc/_destroy/_start/_stop/
_replay` plus `_observe_syscall/_observe_syscall_buf/_observe_
syscall_buf_meta/_observe_dispatch` (record side) and
`_consume_syscall/_consume_syscall_meta/_strict_replay/_set_
strict_replay` (replay side). All `EXPORT_SYMBOL_GPL`.

**Hot-path gate.** `DEFINE_STATIC_KEY_FALSE(um_kvm_record_
enabled)` plus a single-slot `um_kvm_active_record` pointer
under a spinlock. The unlikely-key branch keeps non-record
runtime at zero per-syscall cost. Single-active discipline:
only one container holds the slot at a time.

**Replay-entry schema.** Five kinds — `KVM_REPLAY_TIME` (RDTSC
/ clock_gettime / vvar), `_RAND` (getrandom / RDRAND),
`_INTERRUPT` (preemption injection point), `_SYSCALL` (class-A
passthrough syscall return), `_MMIO_READ` (device read).
Inline `data[4]` plus two optional kvmalloc'd side buffers
(`payload` for data bytes, `metadata` for sockaddr / iovec
descriptors under `META_NONE / _SOCKADDR / _IOV` tags). Caps
at 1 MiB per entry, 64 MiB total log.

**Observation flow.** `kvm_decode_syscall`'s tail gates
`kvm_record_observe_dispatch(nr, ret, regs)` through the
static key. The dispatch routes per-NR — getrandom / read /
pread64 / recvfrom / readv take the side-buffer path
(capture user-out bytes into the payload, capture
addrlen+sockaddr or iovec descriptor into the metadata),
default-NRs take the inline-only path
(`data[0]=NR, data[1]=ret, data[2]=arg0, data[3]=arg1`).

**Replay flow.** `kvm_decode_syscall`'s prelude gates
`kvm_record_consume_syscall_meta` through the same key. On
return > 0, the dispatcher copies payload + metadata back to
user, sets `regs->gp[HOST_AX]` from the entry, and skips
`handle_syscall`. On -EILSEQ (NR divergence) or -ENODATA
(end-of-log), strict mode kills the task; loose falls through
to live `handle_syscall`. The `kvmemdup` on payload/metadata
out-pointers fixes BUG.3 (UAF when destroy raced consume).

**Debugfs surface.** `/sys/kernel/debug/um/kvm_record_
{ctl,state,log}` for `start/stop/replay/destroy/strict/loose`
control, state inspection, and a bounded log dump. The v1
kselftest `kvm-record-smoke` is the operator of this surface;
the port preserves it.

**State-machine invariants.** FIFO consume (cursor only
advances), NR divergence → -EILSEQ, end-of-log → -ENODATA
(strict) or 0 (loose).

## 3. v1 → v2 deltas the port must handle

The big section. Each subsection has the deltas + the design
decision the port takes.

### 3.1 Syscall observation hook site

**v1:** the hook lived in `kvm_decode_syscall` (per-arm
dispatcher in `kvm-v1-archive/syscall_class.c`). v1's
dispatcher consumed the vmexit, called `handle_syscall`, then
gated the post-call observation through
`static_branch_unlikely(&um_kvm_record_enabled)`.

**v2:** the analogous site is `kvm_v2_handle_io_trap` in
`arch/um/backend/kvm-v2/syscall_trap.c` (line 2110), specifically
the `UM_KVM_TRAP_SYSCALL` arm. After `handle_syscall(regs)`
returns (line 2195), but BEFORE the marshal-out (line 2353),
the dispatcher has `syscall_nr` (captured at line 2153),
`regs->gp[HOST_AX]` (the return), and `regs` itself — which is
all `observe_dispatch` needs.

**The hook the port adds (Phase 2):**

```c
/* After handle_syscall returns, before marshal-out. */
if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
    kvm_v2_record_observe_dispatch(syscall_nr,
                                   (long)regs->gp[HOST_AX],
                                   regs);
}
```

Slot order:

  1. `syscall_nr = regs->gp[HOST_AX];` (line 2153, already
     there)
  2. `handle_syscall(regs);` (line 2195, already there)
  3. `interrupt_end()` gate (line 2255-2260, already there)
  4. **NEW HOOK** (post-handle_syscall, pre-marshal-out)
  5. `PT_SYSCALL_NR(regs->gp) = -1;` (line 2300, already there)
  6. `kvm_v2_marshal_to_kvm_regs(...);` (line 2353)

The hook reads `regs->gp[HOST_AX]` to get the return value
(handle_syscall stashed it there per the bug-fix comment block
at line 2199-2253). `regs->gp[HOST_DI]` / `[HOST_SI]` /
`[HOST_DX]` / `[HOST_R10]` / `[HOST_R8]` / `[HOST_R9]` hold the
syscall args (already populated by C.3's marshal-from-kvm-regs
at vcpu.c:kvm_v2_vcpu_run). v1's per-NR dispatcher uses these
same `regs->gp[]` slots, so the port reuses them verbatim.

**Zero hot-path cost when off.** The static-key branch
(`static_branch_unlikely`) compiles to a `jnz` that's never
taken when the key is false; the kernel patches the branch
out at boot. Non-record runtime pays one `jnz` skipped per
vmexit — same shape as v1.

### 3.2 Replay-side hook site

The replay-side `consume_syscall` hook needs to fire BEFORE
`handle_syscall(regs)` (line 2195), not after:

```c
/* Before handle_syscall — if replay served the call, skip the
 * live syscall and jump straight to marshal-out. */
if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
    long replay_ret;
    u64 user_buf_va = 0;
    const void *payload = NULL, *metadata = NULL;
    size_t payload_len = 0, metadata_len = 0;
    u32 metadata_kind = 0;
    int rc = kvm_v2_record_consume_syscall_meta(
        syscall_nr, &replay_ret, &user_buf_va,
        &payload, &payload_len,
        &metadata, &metadata_len, &metadata_kind);

    if (rc > 0) {
        regs->gp[HOST_AX] = (unsigned long)replay_ret;
        kvm_v2_record_restore_user_payload(user_buf_va,
                                           payload, payload_len,
                                           metadata, metadata_len,
                                           metadata_kind);
        kvfree((void *)payload);
        kvfree((void *)metadata);
        goto skip_dispatch;
    } else if (rc < 0 && kvm_v2_record_strict_replay()) {
        force_sig(SIGSEGV);
        return rc;
    }
    /* rc == 0 or loose replay: fall through to handle_syscall. */
}

handle_syscall(regs);
...
skip_dispatch:
    /* same marshal-out path as the live-syscall arm. */
```

`kvm_v2_record_restore_user_payload` is the port's analog of
v1's inline `copy_to_user` calls in the dispatcher; it walks
the per-NR routing table to scatter `payload` + `metadata` to
the user VAs.

### 3.3 Gadget-shadowed syscalls

**The problem.** v2's LSTAR gadget body
(`arch/um/backend/kvm-v2/lstar_gadget.S`) services 11 hot-path
NRs in-guest with NO vmexit:

  - `getpid` / `gettid` / `getppid`
  - `getuid` / `geteuid` / `getgid` / `getegid`
  - `getcpu`
  - `time(2)` / `clock_gettime(CLOCK_MONOTONIC)`
  - (`sched_yield` is DELIBERATELY NOT gadgeted per the
    comment at `lstar_gadget.S:149-151`.)

The gadget reads from the per-vCPU `gadget_state` page (offset
`KVM_V2_GADGET_OFF_TGID` etc., updated by the kernel pre-
dispatch) and from the vvar page (for clock_gettime's seqlock-
protected fast path). It writes the return value directly to
the user RAX in the trampoline frame, then `sysretq`s back to
user mode WITHOUT a `KVM_EXIT_IO` event reaching the host.

**Consequence for record/replay.** Gadget-handled syscalls are
invisible to `observe_dispatch`. If a recorded session relied
on the gadget's `clock_gettime` returning a specific time,
replay couldn't reproduce it: there's no log entry.

**Per memo 13 §"gadget interaction":** record mode forces
gadget-disable. The gadget body trampolines to the fallback
path (`lstar_gadget.S:107-122`, 5 bytes: `outb $UM_KVM_TRAP_
SYSCALL; sysretq`), which vmexits unconditionally. Then
`handle_io_trap` dispatches `handle_syscall` normally and the
observation hook fires.

**Mechanism — two options:**

  - **Option A (per-vCPU).** A flag byte in
    `vcpu->gadget_state_kva` at a new offset
    `KVM_V2_GADGET_OFF_RECORD`; gadget body reads it
    (`cmpb $0, KVM_V2_GADGET_OFF_RECORD(%gs); jne fallback`)
    at `gadget_entry`. Set by `record_start` (all-pool walk),
    cleared by `record_stop`. Cost: 1 byte / vCPU / VM, one
    untaken `cmp+jne` per gadget entry.
  - **Option B (per-VM).** Call a new
    `kvm_v2_trampoline_downgrade_to_fallback` (symmetric to
    `kvm_v2_trampoline_upgrade_to_gadget` at exception.c:1161)
    from `record_start`. Cost: one host-VA memcpy + a TLB
    flush per record arm/disarm.

**Decision: Option A.** Per-vCPU flag is cheaper to flip,
doesn't require TLB shootdown across the pool, and matches
the existing per-vmexit static_key gate granularity. Phase 4
lands the gadget assembly change + the per-vCPU setter.

**Caveat: the 11 NRs become slow under record.** Gadget
(~50 cyc) → vmexit + handle_syscall (~1500 cyc) is a 30×
slowdown on these specific NRs. Acceptable for record-mode
(record is the slow side; replay is the fast side). Memo 13
acknowledges the ~2× overall slowdown.

### 3.4 RDTSC / RDTSCP

**v1 (memo 13 §"Time"):** v1 set `MSR_TSC` at restore-time to
match record-time and disabled RDTSC pass-through via
`KVM_CAP_SET_TSC_KHZ` at a fixed virtual frequency. Each RDTSC
then resolved to a deterministic value computed from the
guest's virtualised TSC base.

**v2 today:** RDTSC passes through to the host TSC. No
exit-on-rdtsc plumbing. RDTSC results are nondeterministic
across runs.

**Two implementation choices for the port:**

  **(a) Trap-on-RDTSC.** Set `KVM_TSC_CONTROL` or the
  `KVM_X86_*_RDTSC_EXIT` cap on the per-vCPU config so RDTSC
  raises `KVM_EXIT_INTERNAL_ERROR` / `KVM_EXIT_HYPERCALL`
  (TBD — kernel API differs across versions; see Q1 in §6).
  Capture each value in a `KVM_REPLAY_TIME` entry; on replay,
  serve from the log.

  **(b) Virtualise TSC offset.** Set a per-vCPU TSC offset
  via `KVM_SET_MSRS` of `MSR_IA32_TSC` such that the guest's
  RDTSC reads a deterministic value computed from the offset
  + a per-vCPU "virtual clock" counter the host advances at
  known points (syscall entries, signal deliveries). No exit
  per RDTSC — fast — but the determinism budget shrinks to
  "RDTSC values at vmexit boundaries are deterministic."

**Decision: (a) for Phase 5.** Memo 13 picked (a) implicitly
(its §"Time" describes a TSC-offset model that's effectively
capturing each RDTSC result, just with a coarser hook).
Phase 5 implements the trap-on-rdtsc path; (b) is a follow-up
optimization if (a)'s overhead is unacceptable. The expected
RDTSC frequency in user code is dominated by glibc's
`clock_gettime` fast path (which v2 already gadgets — see
§3.3); after gadget-disable in record mode, RDTSC exits are
on the order of the syscall count, not orders-of-magnitude
higher.

**Replay side.** A new `kvm_v2_record_consume_time()` is
called from a new `KVM_EXIT_*_RDTSC` arm in `vcpu.c`'s exit-
reason switch; it pops the next `KVM_REPLAY_TIME` entry and
writes the recorded value to `regs->rax:rdx` before
re-entering the guest. Strict-mode divergence (wrong kind at
cursor, or end-of-log) follows the same strict_replay policy.

### 3.5 vvar / vDSO clock_gettime

**v2 today:** the gadget's `h_clock_gettime` body
(`lstar_gadget.S:344+`) reads from the vvar page under a
seqlock retry loop:

```
1. read seq (vvar+0)                      ; vvar.seq
2. if seq odd → racy, fallback (vmexit)   ; seq & 1
3. read sec/nsec from vvar.basetime
4. re-read seq, compare
5. if changed → racy, fallback (vmexit)
```

**The problem.** Even with gadget-disable (§3.3), if the
guest's userspace dynamically links against a vDSO
clock_gettime that reads vvar directly (which it does for
recent glibc), record mode still doesn't see those reads —
they're memory accesses to a page the host owns.

**Two choices:** (a) trap on vvar read — remove the vvar
memslot, let each read fault to `KVM_EXIT_MMIO`. Slow (every
vDSO clock_gettime vmexits) but correct. (b) freeze the vvar
buffer — at record time, capture the seqlock-protected fields
on each refresh (`vdso_update_begin/_end` boundaries in
`kernel/time/vsyscall.c`); on replay, restore the vvar buffer
to the captured values before each guest entry and bump seq
twice (skip past the seqlock retry). Fast, but requires a
hook into the vvar update path.

**Decision: (b) for Phase 5.** Memo 13 §"vvar page" picks (b);
the port follows. Implementation: record-side hook in the
vvar update tail captures `{seq, basetime_sec, basetime_nsec,
...}` into a `KVM_REPLAY_TIME` entry with `data[0]` discrim-
inating TIME_RDTSC vs TIME_VVAR; replay-side writes the bytes
back to the vvar page (Policy A — one singleton memslot, host-
VA known) before each guest entry. vvar refresh rate is ~1 Hz
under normal load, so 1 entry/sec × replay duration. The
cross-subsystem touch (a single function call into `record.c`
from the vvar update tail) is the load-bearing risk.

### 3.6 SIGALRM and signal-arrival determinism

**v2 today.** `vcpu.c:install_signal_mask` (line 864) keeps
SIGALRM unmasked during KVM_RUN; the host kernel's HZ=100
timer interrupts the vCPU's host thread, KVM_RUN returns
-EINTR, the vmexit-loop sees the signal, processes it
(`do_signal` etc.), then re-enters. The signal lands at an
unpredictable instruction boundary inside the guest's user-
mode execution.

**The problem.** Determinism requires the signal land at the
SAME boundary on replay. Two routes:

  **(a) PMU instruction-retired boundary recording.** Memo 13
  §"Interrupts" picks this: program `INST_RETIRED.ANY`
  (Intel) / equivalent (AMD: `PMCx0C0`; ARM64:
  `INST_RETIRED`) to fire an overflow at `N` retired
  instructions, where `N` is the count captured at record
  time. Replay programs the same overflow + injects the
  IRQ in the overflow handler. This is the rr algorithm.

  **(b) KVM_REQ-driven replay with synthesised SIGALRM at
  the captured tick count.** Record stores
  `(syscall_count_at_signal, signal_no)`; replay tracks
  syscall count via the observation hook (we already see
  every vmexit-dispatched syscall in record mode) and
  triggers the signal at the same syscall count. Coarser
  than (a) — sub-syscall-instruction precision is lost.
  But cheaper: no PMU plumbing, no perf-event API touch.

Memo 13 preferred (a); the port has to pick now because
Phase 6 lands it. **Decision: (b) for Phase 6.** Reasoning:

  1. Most UML signals land on a syscall boundary anyway (the
     syscall gave the host scheduler the delivery window).
     Sub-instruction precision is rarely required.
  2. (a) requires arch-specific code (Intel/AMD/ARM64 differ;
     memo 13 Q3 flags this) AND host KVM user-space PMU
     support on the target silicon. Zen 4 PMU under KVM is
     shakier than Intel; v2 dev runs on AMD.
  3. (b) reuses the §3.1 observation hook; the only new state
     is a per-record `syscall_count` counter.
  4. (b)'s precision — "signal arrives between syscalls,
     ±100 syscalls" (see Q5) — is sufficient for
     deterministic-syscall-replay (the #170 syzkaller use
     case in Q4).

If (b) proves inadequate, Phase 6.5/7 can layer (a) on top;
the static_key gate makes either route additive.

**Implementation sketch for (b):**

  - `struct kvm_v2_record` gains `u64 syscall_count` (record
    side) / `u64 replay_syscall_cursor` (replay side).
  - On every `observe_dispatch`, increment `syscall_count`
    BEFORE appending the entry; the entry carries the count
    in `data[3]` (Phase 6 repurposes the inline-only-default
    slot since payload_len isn't relevant for inline-only
    entries).
  - On SIGALRM during recording, append an INTERRUPT entry
    with `data[0] = SIGALRM`, `data[1] = syscall_count_at_
    signal`. (Hook in `vcpu.c`'s SIGALRM-EINTR path at
    `kvm_v2_vcpu_run`'s post-KVM_RUN error handling.)
  - On replay, the dispatcher's `observe_dispatch` analog
    (`replay_advance`, fired AFTER `consume_syscall`)
    increments `replay_syscall_cursor`. When the next
    INTERRUPT entry's `data[1] == replay_syscall_cursor`,
    inject the SIGALRM (via `KVM_SET_VCPU_EVENTS` pending-
    IRQ slot or a synthetic `do_signal` call before the next
    KVM_RUN).

### 3.7 Cross-task vCPU pool transitions

**v1.** Each task had its own vCPU; cross-task transitions
didn't exist — a record was a single-task stream of syscalls
+ time + signals.

**v2.** Tasks share vCPUs from the per-host-CPU pool. A
recording task may have its observe_dispatch called from a
different pool member across its lifetime (if the host
scheduler migrated it). The cross-task arrival path
(`vcpu->last_task != current` in `load_user_sregs`,
exception.c) re-installs SREGS + FPU from the task's
saved state.

**Question.** Is the dispatch-on-vCPU-N event itself
nondeterministic relative to the record?

**Answer.** No, for the record/replay determinism contract:
the snapshot (memo 26) captures the full register state of
the vCPU current most recently dispatched on, and restore
re-installs it onto whichever pool member current next
dispatches on. So replay can land on a different physical
vCPU than record — the snapshot bytes are the same.

**But:** the cross-task arrival branch does some side-
effecting work that record/replay needs to be aware of
(specifically, it triggers a SREGS re-install, which under
record mode may itself produce observable events if any
host-side syscalls are needed). For Phase 1-7 we assume the
cross-task transition is observationally identical to a
same-task continuation; the SREGS re-install is a host-only
operation with no record-relevant side effects.

**Phase 4 + Phase 7 caveat.** If a cross-task arrival
encounters a record-time-vs-replay-time vCPU pool layout
divergence (e.g. record ran on a 4-vCPU pool, replay on a
2-vCPU pool — see Q3 in §6), the SREGS re-install fires at
a different syscall count. This breaks the (b)-style
SIGALRM injection (§3.6) because the syscall count under
replay no longer maps to the same instruction boundary.

**Mitigation.** Record the host CPU index (`vcpu->cpu`) and
the pool size at record_start; refuse replay if the replay-
time pool differs. Q3 tracks this.

### 3.8 XSAVE state recording

**The problem.** Snapshot (memo 26) captures XSAVE at the
checkpoint. Each subsequent syscall that touches FPU/SSE/AVX
state diverges the vCPU's XSAVE from the snapshot. Replay's
restore-then-execute only works if either:

  (i) Replay re-executes the same syscalls (live) and gets
      the same XSAVE state via deterministic intra-guest
      computation; OR
  (ii) Each XSAVE-touching syscall's effect is captured in
      the log so replay can restore the post-syscall XSAVE
      from the log.

(i) requires that the syscalls themselves be deterministic
under replay — which they are, because the observation/
consume hooks replace the host syscall with canned results.
So (i) is the path. **The XSAVE divergence resolves itself
because the deterministic-replay contract ensures the
guest computation reaches the same XSAVE state.**

**BUT:** SMP-T55's lazy `fpu_dirty` skip
(`kvm_v2_backend.h:391-423`) breaks this. The pre-dispatch
`KVM_SET_FPU` is skipped when the vCPU's guest FPU is bit-
identical to the task's `iotrap_fpu`. Record mode can't
trust this optimization because:

  - Under record, the per-task `iotrap_fpu` lifecycle
    interacts with the snapshot/replay state machine in ways
    that the SMP-T55 design (which assumed no replay) didn't
    contemplate.
  - Specifically: if replay re-installs an XSAVE state and
    then dispatches a syscall, the pre-syscall `KVM_SET_FPU`
    might skip (fpu_dirty == false from a previous epoch),
    landing on the vCPU's PRE-REPLAY FPU instead of the
    intended snapshot-restored FPU.

**Three options:** (i) record-mode forces always-GET_XSAVE
after every vmexit (defeats SMP-T55 but only when record is
on); (ii) record only deltas at observation points (requires
detecting "touched FPU" — XSAVE-INUSE bitmap walk or per-NR
whitelist); (iii) only record dispatches that change the
dirty epoch (cleanest if SMP-T55 exposes the epoch-advance
event as a hook; else touches lazy-FPU bookkeeping).

**Decision: (i) for Phase 2.** Simplest; perf cost is
record-side only; (ii)/(iii) are optimizations to revisit if
record-mode workloads need them. Implementation: gate the
SMP-T55 skip on
`!static_branch_unlikely(&um_kvm_v2_record_enabled)`. The
post-restore hygiene in `kvm_v2_snapshot_restore_full`
(snapshot.c:390) already sets `vcpu->fpu_dirty = true`;
record mode extends this to "fpu_dirty is always true under
record."

Phase 2 acceptance: record/replay round-trip of an AVX-using
code path (glibc `memcpy` >16-byte buffer uses `vmovdqu` from
`vpxor`-cleared `ymm0`) produces bit-identical XSAVE.

### 3.9 Memslot deltas

**v1's design.** `kvm_record_replay` calls
`kvm_snapshot_restore_full`, which memcpy's the entire
memslot back from the captured backing buffer. No
incremental capture — every replay iteration starts from a
full restore.

**v2 Phase 1 (memo 26).** `kvm_v2_snapshot_capture` is a
stub (`-EOPNOTSUPP`); memslot capture lands in snapshot
Phase 3. Until then, the record/replay port can't do full-
state replay — only log-only mode (memo 13 §start has the
pattern: log-only mode arms even when snapshot capture
fails). For Phase 1-2 of #169, log-only is acceptable: the
KUnit and the smoke test exercise the observe / consume
log machinery independent of snapshot state restore.

**Snapshot Phase 3 dependency.** Record/replay Phase 5+
(RDTSC/vvar/SIGALRM determinism) wants a real checkpoint to
restore the vCPU state across record→replay. So
record/replay Phase 5 depends on snapshot Phase 3 having
landed. Sequencing:

  - Snapshot Phase 1 (DONE, commit `aa4cd328102c`).
  - Record/replay Phase 1-2-3 (skeleton + observe + consume)
    runs against log-only mode.
  - Snapshot Phase 3 (memslot + IDT/GDT + IST capture).
  - Record/replay Phase 4-5-6 (gadget-disable + RDTSC/vvar +
    SIGALRM) builds on the now-real snapshot.

**Dirty bitmap optimization.** v1's full-memslot restore is
~physmem_size MB per replay iteration. Under v2, with the
Phase 3 snapshot's memslot copy at ~64 MB+, that's a
non-trivial cost per replay iteration. KVM exposes
`KVM_GET_DIRTY_LOG` for dirty bitmap retrieval; record mode
could capture only the dirty pages, replay only restore
those pages. Deferred to a follow-up memo
(`28-record-replay-dirty-bitmap.md` placeholder); Phase 1
of #169 matches v1's full-memslot pattern.

### 3.10 Header + symbol changes

  - `arch/um/backend/kvm-v2/kvm_v2_backend.h` gains:
      * `struct kvm_v2_record`
      * `struct kvm_v2_replay_entry`
      * `enum kvm_v2_replay_kind`
      * `enum kvm_v2_replay_meta_kind`
      * `extern struct static_key_false um_kvm_v2_record_enabled;`
      * `KVM_V2_GADGET_OFF_RECORD` (Phase 4 — the gadget-
        state-page record-force-fallback byte offset).
      * Prototypes: `kvm_v2_record_alloc/_destroy/_start/_stop/
        _replay/_observe_syscall/_observe_syscall_buf/_observe_
        syscall_buf_meta/_observe_dispatch/_consume_syscall/
        _consume_syscall_meta/_strict_replay/_set_strict_
        replay/_restore_user_payload`.
  - `arch/um/backend/kvm-v2/Makefile` gains `record.o` under
    `obj-$(CONFIG_UM_BACKEND_KVM_V2)`.
  - `arch/um/backend/kvm-v2/record.c` (NEW, Phase 1-N).
  - `arch/um/backend/kvm-v2/test_record.c` (NEW, Phase 7 —
    KUnit round-trip + side-buffer + strict/loose tests).
  - `arch/um/backend/kvm-v2/lstar_gadget.S` (Phase 4 — gadget
    body adds the `KVM_V2_GADGET_OFF_RECORD` check at
    `gadget_entry`).

## 4. Phase 1 entry-point (what lands in the next commit)

Phase 1 is the smallest landable slice: skeleton + static_key
+ alloc/destroy + start/stop/replay state machine. NO
observation hook yet (Phase 2 adds the `handle_io_trap` hook).
Build-time KUnit asserts the state-machine transitions
without depending on a live vCPU.

**Files:**

  - `arch/um/backend/kvm-v2/record.c` (~350 LoC): the structs
    + enums + `DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_
    enabled)` + `alloc/destroy/free/start/stop/replay/strict_
    replay/set_strict_replay`. `observe_syscall` and
    `consume_syscall` are stubs (no-op / returns 0).
  - `kvm_v2_backend.h`: public symbols + struct definitions
    (per §3.10).
  - `Makefile`: `record.o` under
    `obj-$(CONFIG_UM_BACKEND_KVM_V2)`.
  - `test_byteshape.c`: `test_kvm_v2_record_basic` (alloc /
    start / stop / destroy transitions, no observation hook
    needed). Skip-aware.

**Recommended entry-point.** The smallest landable piece is
`kvm_v2_record_alloc/_destroy` + the `DEFINE_STATIC_KEY_FALSE`
+ the `struct kvm_v2_record` header definition + a no-op
`kvm_v2_record_start` / `_stop`. That's the v1 pattern (commit
`56274adfe16b` per memo 13 §Status step 1). The KUnit asserts
the state-machine transitions without depending on a snapshot
capture (snapshot Phase 3 isn't ready); the static_key gate is
in place from day 1 so Phase 2's hook in `syscall_trap.c` can
land as a pure addition without touching the data structures.

The file mirrors `kvm-v1-archive/record.c` lines 41-280 (the
alloc/destroy/start/stop/replay shell) with the symbol prefix
swapped `kvm_` → `kvm_v2_` and `kvm_snapshot_*` references
swapped for `kvm_v2_snapshot_*`.

## 5. Sub-sequencing (Phases 1-7)

Mirrors snapshot port memo §6 with the deltas appropriate to
record/replay. Each phase is one commit (possibly two when a
KUnit lands alongside the implementation).

### Phase 1 — skeleton + static_key + alloc/destroy

Per §4 above. ~350 LoC. KUnit asserts alloc/start/stop/destroy
state transitions.

**Acceptance:**
  - Build clean (`make ARCH=um O=…`).
  - checkpatch clean.
  - KUnit `test_kvm_v2_record_basic` passes (alloc / state-
    machine transitions / no UAF on destroy).
  - `static_branch_unlikely(&um_kvm_v2_record_enabled)`
    branch verified to be patched-out when off (objdump on
    a sample hot path).

### Phase 2 — observe_syscall hook in `handle_io_trap`

Add the post-`handle_syscall` hook per §3.1; wire
`kvm_v2_record_observe_dispatch` with the inline-only path
first (no per-NR side-buffer routing yet — that's Phase 2.5).
Also: gate the SMP-T55 lazy-FPU skip on
`!static_branch_unlikely(&um_kvm_v2_record_enabled)` per
§3.8(i).

**Acceptance:**
  - `kvm-v2-record-smoke` selftest (Phase 7's stub, run early
    here): `echo start > kvm_record_ctl; cat /etc/passwd >
    /dev/null; echo stop > kvm_record_ctl; cat
    kvm_record_state` shows `log_count > 0` matching the
    number of vmexit-dispatched syscalls.
  - Hot-path benchmark (Python startup) under record-OFF
    shows zero regression vs pre-Phase-2 (the static_key gate
    is patched-out).
  - Hot-path benchmark under record-ON shows the expected
    record overhead (~10-20% per memo 13 §Status's hot-path
    ratio band).

### Phase 2.5 — per-NR side-buffer routing

Port v1's `kvm_record_observe_dispatch` switch (getrandom,
read, pread64, recvfrom, readv) verbatim. Same payload +
metadata cap discipline (1 MiB per entry, 64 MiB total).

**Acceptance:**
  - KUnit `test_kvm_v2_record_sidebuf`: observe a fake
    getrandom entry with deterministic 8-byte payload,
    consume + memcmp the payload bytes back.
  - Selftest extension: `cat /dev/urandom | head -c 4096 >
    /dev/null` under record produces a `__NR_read` log entry
    with `payload_len == 4096` + non-NULL payload.

### Phase 3 — replay primitive (consume_syscall)

Add the pre-`handle_syscall` hook per §3.2; wire
`kvm_v2_record_consume_syscall` + `_consume_syscall_meta` +
`kvm_v2_record_restore_user_payload`. Strict and loose modes
both implemented.

**Acceptance:**
  - Round-trip KUnit (mirror v1 commit `915299f2a9d6`):
    observe three syscalls, replay, consume in FIFO order,
    verify cursor exhaustion + NR-mismatch divergence
    returns -EILSEQ.
  - **The big one:** record a 100-syscall stream
    (`/bin/echo` × N), replay it, every state transition
    bit-identical. Pre-replay XSAVE matches post-replay
    XSAVE (per §3.8(i)); pre-replay regs match.

### Phase 4 — gadget-disable in record mode

Per §3.3. Add `KVM_V2_GADGET_OFF_RECORD` byte to the per-
vCPU gadget state page. Wire `kvm_v2_gadget_set_record_
fallback(vcpu, bool)` called from `record_start` (all-pool
walk) and `record_stop`. Modify `lstar_gadget.S` to read the
byte at `gadget_entry` and branch to `fallback` when set.

**Acceptance:**
  - Under record mode, `clock_gettime(CLOCK_MONOTONIC, &ts)`
    produces a `__NR_clock_gettime` log entry (gadget
    bypassed → vmexit → handle_syscall → observe).
  - Under record-OFF, gadget hot-path benchmark is unchanged
    (the new check is one untaken `cmp+jne` per gadget
    entry; budget is "negligible").

### Phase 5 — RDTSC + vvar determinism

Per §3.4 + §3.5. Trap-on-RDTSC via `KVM_X86_*_RDTSC_EXIT`
(API decision deferred to Q1 in §6). vvar hook in the host's
vdso_update path.

**Acceptance:**
  - Record + replay of a tight `for(;;) rdtsc()` loop: 1000
    iterations, replay produces bit-identical RDTSC values
    from the log.
  - Record + replay of `clock_gettime(CLOCK_REALTIME)` × N:
    replay produces bit-identical timestamp values from the
    vvar log entries (one log entry per vvar refresh,
    consumed at each gadget-disabled trap-on-clock_gettime
    vmexit).

### Phase 6 — SIGALRM determinism

Per §3.6(b). Syscall-count-driven SIGALRM injection on
replay. Record + replay of stress-ng IPC test.

**Acceptance:**
  - **Stress-ng IPC under record/replay = same scoreboard
    verdict.** I.e. if record-run completes 1234 IPCs in
    10 s, replay-run completes 1234 IPCs in (replay-time)
    deterministically. Tier 3 Phase J soak harness gets a
    `record-replay` test case.
  - INTERRUPT entry log shape: `data[0] = SIGALRM` (or
    other signal NR), `data[1] = syscall_count_at_signal`.

### Phase 7 — KUnit + selftest re-plumb

  - `tools/testing/selftests/um/kvm-record-smoke` (rename
    from v1's `kvm-record-smoke`, port to v2 debugfs node
    names `kvm_v2_record_*`).
  - `arch/um/backend/kvm-v2/test_record.c` — full KUnit
    suite (basic-shape + side-buffer + round-trip + strict/
    loose + gadget-bypass + RDTSC-determinism + SIGALRM-
    determinism). Gated by `CONFIG_UM_BACKEND_KVM_V2_KUNIT`.

**Acceptance:**
  - KUnit suite passes under `kunit_um.py` v2 boot env.
  - Kselftest passes in the in-tree CI matrix.

## 6. Open questions

### Q1: KVM API for RDTSC exit configuration

Memo 13 §"Time" assumes `KVM_CAP_SET_TSC_KHZ` + per-vCPU TSC
offset suffice for record/replay's "RDTSC is deterministic"
contract. v1 used the offset model (no explicit exit-on-rdtsc).
v2 today doesn't trap RDTSC at all; the port wants to either
(a) explicitly trap RDTSC for capture/replay, or (b) use the
offset model.

What's the right KVM ioctl for (a)?

  - Older KVM API: `KVM_TSC_CONTROL` capability check + per-
    vCPU `KVM_SET_TSC_KHZ`. Doesn't directly enable a per-
    instruction RDTSC vmexit; it virtualises the TSC base.
  - Newer KVM API: `KVM_CAP_X86_DISABLE_EXITS` has a
    `KVM_X86_DISABLE_EXITS_*` bitmask but it's about disabling
    optional exits (HLT, MWAIT), not enabling RDTSC exits.
  - `KVM_EXIT_RDPMC` exists for `RDPMC` instructions but the
    parallel for RDTSC is `KVM_EXIT_IO_INSTRUCTION`'s
    `KVM_EXIT_REASON_RDTSCP` (Intel SDM section on VMCS exit
    reasons). Whether the upstream KVM exposes a per-vCPU
    "exit on RDTSC" toggle is unclear.

Phase 5 begins with a one-day spike: read
`arch/x86/kvm/vmx/vmx.c` for the RDTSC-exit handling and
identify the cleanest API surface for record mode. If no
explicit per-vCPU exit toggle exists, fall back to the offset
model (b) and document the precision tradeoff.

### Q2: PMU instruction-retired counter availability under Zen 4 vs Intel

If Phase 6 (b) (syscall-count-driven SIGALRM) proves
inadequate and the port needs Phase 6.5 (PMU-driven), the
arch-specific shim has to handle:

  - Intel: `INST_RETIRED.ANY` (event 0xC0, umask 0x01).
  - AMD: `PMCx0C0` "Retired Instructions" (Zen 4: yes;
    upstream KVM PMU passthrough on AMD: less tested than
    Intel).
  - ARM64: `INST_RETIRED` (PMU event 0x08).
  - The user-space API for programming this under KVM is
    `KVM_CAP_PMU_CAPABILITY` (from task #255 in memo 13's
    cross-reference, kernel docs:
    Documentation/virt/kvm/api.rst).

Dev rig is AMD; CI rig is mixed. Phase 6.5 requires both work,
which depends on KVM's AMD PMU passthrough maturity. Q2 stays
open until Phase 6 lands and we know whether (b) suffices.

### Q3: Replay across kernel-version updates with per-pool vCPU layout drift

Memo 13 promised "deterministic across kernel versions." v2's
per-pool vCPU layout drifts commit-to-commit:

  - SMP-T16 added `vcpu->last_task`.
  - SMP-T55 added `vcpu->fpu_dirty` + `vcpu->fpu_owner_task`.
  - SMP-T57 added the XSAVE / XCRS path.
  - Future SMP-T## work may add more fields.

If record runs against kernel V1 and replay against V2, the
vCPU pool layout may have new fields that the V1-recorded
snapshot doesn't populate. The snapshot port memo (memo 26)
Q3 flags this for snapshot; record/replay inherits the same
concern, amplified because record/replay relies on the
snapshot AND on the runtime behavior of the dispatcher (which
SMP-T## work also changes).

**Mitigation candidates:**

  1. Record the kernel git rev + Kconfig hash in the record's
     header; refuse cross-version replay v1. This is memo
     13's stated approach; it's correct but conservative.
  2. Decouple the recorded log from the snapshot: record the
     log against a per-record interface-version contract that
     stays stable across SMP-T## work; restore via the
     contract (a translation layer) when replay hits a
     different kernel rev. Heavyweight; Phase 7+ work.
  3. Use a serialization format (memo 13 Q1 — snapshot to
     disk, the 08-future-phases/02-snapshot-to-disk.md
     pitch) with explicit field-presence flags, so older
     records replayed on newer kernels just zero-fill the
     new fields. This is what container-format design (CRIU
     style) does for vCPU state.

Q3 ties to Q3 in memo 26 (snapshot's analogous question).

### Q4: syzkaller (#170) fork-server minimum shape

memo 14 (`14-syzkaller-vm-uml-backend.md`) sketches a
syzkaller fork-server using snapshot + replay: each testcase
forks from a clean checkpoint, runs N syscalls, the harness
captures the syscall trace, the parent restores and forks
the next testcase from the same checkpoint.

What's the minimum record/replay shape needed before #170
prototyping unblocks?

  - Snapshot Phase 3 (memslot capture/restore): MUST. Without
    full-state restore, fork-server can't reset between
    testcases.
  - Record/replay Phase 2 (observe_syscall): MUST. The harness
    needs to capture the syscall trace.
  - Record/replay Phase 3 (consume_syscall): NICE-TO-HAVE
    initially. The first prototype can just record and not
    replay; replay-into-deterministic-fork comes later.
  - Record/replay Phase 4-6 (gadget-disable, RDTSC, SIGALRM):
    Phase 4 is required for clock_gettime visibility; 5-6
    can wait.

So #170 prototyping unblocks after Phase 4 of #169 + Phase 3
of #168. Q4 documents the gate; #170 work picks up after.

### Q5: Sub-instruction-precision signal arrival

§3.6's decision to take (b) (syscall-count-driven) over (a)
(PMU-driven) is conservative; for some workloads (compute-
bound code with no syscalls between SIGALRMs), (b) is
inadequate.

What's the smallest precision unit (b) achieves? "Between
syscalls" — but how often are SIGALRMs delivered relative to
syscalls?

UML HZ=100 → SIGALRM every 10 ms. Typical UML workload
syscall rate: ~10k/s under normal load → 100 syscalls per
SIGALRM. So (b)'s precision is "±100 syscalls" on replay
relative to record. For most fuzzing / debugging workloads
this is fine; for tight benchmarks (which we shouldn't
record/replay anyway) it isn't.

Q5 documents the precision tradeoff for the operator's
mental model.

## 7. Acceptance criteria summary (per phase)

| Phase | Acceptance |
|-------|-----------|
| 1 | Build + checkpatch clean. KUnit basic-shape passes. Static-key gate verified patched-out when off. |
| 2 | Hot-path benchmark unchanged when off. `log_count` advances per vmexit-dispatched syscall when on. |
| 2.5 | Side-buffer KUnit passes. Selftest captures `__NR_read` 4 KiB payload. |
| 3 | Round-trip KUnit passes. **100-syscall record → replay = bit-identical state.** |
| 4 | Gadget-disable verified: `clock_gettime` under record produces log entry. Gadget hot-path unchanged when record OFF. |
| 5 | RDTSC + clock_gettime values bit-identical record vs replay. |
| 6 | **Stress-ng IPC under record/replay = same scoreboard verdict.** |
| 7 | KUnit suite passes under v2 boot env. Selftest passes in CI matrix. |

The Phase 3 and Phase 6 acceptance criteria are the load-
bearing ones; Phase 3 proves the syscall replay primitive
works, Phase 6 proves the determinism budget closes for a
real workload.

## 8. References

  - `arch/um/backend/kvm-v1-archive/record.c` — 1597 LoC, the
    lift target.
  - `arch/um/backend/kvm-v1-archive/kvm_backend.h` — v1
    record/replay public surface (lines 1067-1130).
  - `arch/um/backend/kvm-v2/snapshot.c` — the v2 snapshot
    foundation (Phase 1 just landed, commit
    `aa4cd328102c`).
  - `arch/um/backend/kvm-v2/syscall_trap.c::kvm_v2_handle_io_
    trap` — the dispatch site (line 2110) the observe hook
    slots into.
  - `arch/um/backend/kvm-v2/lstar_gadget.S` — the gadget body
    that record-mode disables (§3.3).
  - `arch/um/backend/kvm-v2/vcpu.c::install_signal_mask` —
    the SIGALRM mask path Phase 6 hooks into (line 864).
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-
    backend/13-record-replay-determinism.md` — the strategic
    memo this port implements.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-
    backend/26-snapshot-v2-port.md` — sibling port memo,
    structural pattern this memo follows.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-
    backend/12-snapshot-forkserver-kvm.md` — the snapshot
    design memo record/replay layers on.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-
    backend/14-syzkaller-vm-uml-backend.md` — the #170
    fork-server consumer.
  - `Documentation/virt/uml/redesign/06-sequencing/PLAN-
    2026-05-14.md` §4.1 — the seven-step sub-sequencing
    this memo elaborates for #169.
  - `Documentation/virt/uml/redesign/04-risks/decisions-
    log.md` D101-D105 (v1 record/replay landings),
    D119/D121 (SMP-T55/T57 — the FPU-dirty + XSAVE state
    record/replay has to coexist with), D123 (snapshot
    port).
  - Commit `aa4cd328102c` — snapshot v2 port Phase 1.

## 9. Acceptance for this memo

  - [x] Memo written (this file).
  - [ ] checkpatch clean (`./scripts/checkpatch.pl --no-tree
        -f Documentation/virt/uml/redesign/02-workstreams/
        D-kvm-backend/27-record-replay-v2-port.md`).
  - [ ] Diary entry `plan-2026-05-14-execution/07-record-
        replay-design.md`.
  - [ ] Commit `-s` with kernel discipline (lowercase
        `Co-authored-by:`, no `--no-verify`).
  - [ ] Push to `origin/umlctl-deploy`.

Phase 1 (code) lands as a separate commit; this memo is the
design contract that precedes it.
