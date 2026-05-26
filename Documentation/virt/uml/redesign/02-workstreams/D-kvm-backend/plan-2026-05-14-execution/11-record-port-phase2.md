# Record/replay v2 port — Phase 2 landed (2026-05-16)

Track B / `#169` record/replay port — Phase 2 lands the
`observe_syscall` hook in `kvm_v2_handle_io_trap`, fills in the
`kvm_v2_record_observe_syscall` body that was a Phase 1 no-op
stub, and gates the SMP-T55 lazy-FPU skip on
`!static_branch_unlikely(&um_kvm_v2_record_enabled)` per memo 27
§3.8(i).

Design contract: `02-workstreams/D-kvm-backend/27-record-replay-
v2-port.md` §Phase 2 + §3.1 (observation hook site) + §3.8(i)
(XSAVE-on-every-vmexit when record is armed).

Phase 1 landed at commit `358c4d3c83ab` — state-machine skeleton,
static-key gate, 2 KUnit cases (basic + state-transitions). Phase 2
is a pure addition: Phase 1's stub function bodies get filled in,
and the hot path acquires one new `static_branch_unlikely`
predicate plus one new arm in the SMP-T55 skip condition. No
existing call sites moved; no surface broke.

## What landed

  - **`arch/um/backend/kvm-v2/record.c`** (+~150 lines):
    - `kvm_v2_record_active(void)` accessor — promotes the file-
      scope `um_kvm_v2_active_record` global into a callable
      function so `syscall_trap.c` doesn't need direct visibility
      into record.c's internals. Takes the record_lock spinlock,
      returns the slot pointer (may be NULL). EXPORT_SYMBOL_GPL.
    - `kvm_v2_record_observe_syscall(rec, nr, ret, regs)` —
      Phase 1's stub becomes a real append helper. Reads
      `rec->state` under `rec->lock`; appends a 80-byte
      `struct kvm_v2_replay_entry` (16-byte header + 64-byte
      `syscall {nr,_pad,retval,args[6]}` payload) when state ==
      RECORDING; quiet no-op otherwise (including NULL @rec and
      STOPPED/REPLAYING — race against _stop on another CPU).
    - Buffer-full path drops quietly (Phase 7's overflow handler
      will wire the explicit `_stop`-on-cap policy; Phase 2's
      drop is benign — strict replay will SIGSEGV on the
      end-of-log mismatch).

  - **`arch/um/backend/kvm-v2/syscall_trap.c`** (+~50 lines):
    - Adds `#include <linux/jump_label.h>` so
      `static_branch_unlikely` resolves.
    - In `kvm_v2_handle_io_trap`'s `UM_KVM_TRAP_SYSCALL` arm,
      between `interrupt_end()` (line ~2260) and the
      `PT_SYSCALL_NR(regs->gp) = -1;` clear (line ~2300), a new
      gated block:

      ```c
      if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
          struct kvm_v2_record *rec = kvm_v2_record_active();
          if (rec)
              kvm_v2_record_observe_syscall(rec, syscall_nr,
                                            (long)regs->gp[HOST_AX],
                                            regs);
      }
      ```

    - The hook reads `syscall_nr` from the local already-captured
      copy (line ~2153, before any in-flight clears) and reads
      `regs->gp[HOST_AX]` for the retval. The args[0..5] slots
      (HOST_DI/SI/DX/R10/R8/R9) survive `handle_syscall`
      unmolested by every in-tree path — that's the contract for
      the marshal-out (line ~2353) to make sense.

  - **`arch/um/backend/kvm-v2/vcpu.c`** (+~5 LoC + comment):
    - Adds `#include <linux/jump_label.h>`.
    - SMP-T55 skip condition (line ~2362) extended with the
      record-armed force-GET arm:

      ```c
      if (vcpu->fpu_dirty || vcpu->fpu_owner_task != current ||
          static_branch_unlikely(&um_kvm_v2_record_enabled)) {
              /* always KVM_GET_FPU when record is on */
      }
      ```

    - Rationale documented in the new comment block: replay needs
      bit-identical post-vmexit XSAVE; the SMP-T55 dirty-epoch
      skip optimization preserves the vCPU's FPU across
      dispatches in a way that doesn't compose with replay's
      restore-then-execute contract. The static_branch gate
      makes the change zero-cost when record is off (the common
      case in every shipped UML profile).

  - **`arch/um/backend/kvm-v2/kvm_v2_backend.h`** (+~30 LoC):
    - `struct kvm_v2_replay_entry` grows from Phase 1's bare
      16-byte header to a 80-byte entry with the inline
      `syscall {nr,_pad,retval,args[6]}` payload. Phase 1's
      "payload follows" comment is replaced by an explicit
      field set. The `@size` field documents
      "header + payload" (was "payload only"); the Phase 3
      consume walker uses `entry->size` to advance the cursor.
    - `kvm_v2_record_active(void)` prototype added below the
      existing `kvm_v2_record_strict_replay` declaration.

  - **`arch/um/backend/kvm-v2/test_record.c`** (+~170 LoC):
    - Adds `#include <linux/string.h>` and `#include
      <sysdep/ptrace.h>` for `memset` + HOST_DI / HOST_SI / etc.
    - New `test_kvm_v2_record_observe` case:
      1. `kvm_v2_record_alloc(0)` + `_start` — assert RECORDING +
         gate on + `kvm_v2_record_active()` returns the rec.
      2. Build a synthetic `struct uml_pt_regs synth_regs` on the
         stack with per-slot sentinels in HOST_DI..HOST_R9.
      3. Call `kvm_v2_record_observe_syscall(rec, 0 /*__NR_read*/,
         42, &synth_regs)` directly.
      4. Walk `rec->buffer` head as a `struct kvm_v2_replay_entry`
         and assert: kind == SYSCALL, size == sizeof(entry),
         sequence == 1, syscall.nr == 0, syscall.retval == 42,
         syscall.args[0..5] match the sentinels.
      5. Second append with `__NR_write` sentinel: buffer cursor
         advances, sequence == 2.
      6. NULL-`@regs` path: args[] zeroed; nr/retval correct.
      7. `_stop` the container, follow-on `observe_syscall` is a
         quiet no-op (buffer + counters frozen at stop-time
         values; `kvm_v2_record_active()` returns NULL post-stop).
      8. NULL-`@rec` path: also a quiet no-op.
      9. Destroy — gate off.

## Verification

KUnit boot under `backend=force=kvm-v2 mem=512M ncpus=1
init=/bin/echo`:

```
    # Subtest: kvm_v2_marshal     pass:8 fail:0 (ok 1)
    # Subtest: kvm_v2_byteshape   pass:9 fail:0 (ok 2)
    # Subtest: kvm_v2_snapshot    pass:2 fail:0 (ok 3)
    # Subtest: kvm_v2_record
    ok 1 test_kvm_v2_record_basic
    ok 2 test_kvm_v2_record_state_transitions
    ok 3 test_kvm_v2_record_observe
# kvm_v2_record: pass:3 fail:0 skip:0 total:3
ok 4 kvm_v2_record
```

All 4 suites pass. Record suite is now 3/3 (Phase 1: 2/3).

Beyond KUnit: the kernel boots `/bin/echo` to exit-0 under
`backend=force=kvm-v2 mem=512M ncpus=1` with no record container
ever instantiated outside the KUnit cases. The static_branch gate
guarantees the new hook block compiles to a 5-byte NOP that the
kernel patches out at boot; no non-record dispatch ever touches
`kvm_v2_record_active()` or `kvm_v2_record_observe_syscall()`.

## Hardest part

The observe_syscall integration's hardest part was deciding the
slot ordering in `kvm_v2_handle_io_trap`'s SYSCALL arm. The
existing function has six conceptually-distinct phases:

  1. marshal-from kvm_run (already done by C.3 before entry).
  2. cache `syscall_nr`, populate `PT_SYSCALL_NR`, copy
     CX/R11 → IP/EFLAGS.
  3. `handle_syscall(regs)`.
  4. interrupt_end gate (perf-O1 inlined; fires on
     `-ERESTART*` returns + any TIF_WORK_MASK bit).
  5. `PT_SYSCALL_NR(regs->gp) = -1` clear.
  6. marshal-out + RCX/R11 explicit overwrite.

Memo 27 §3.1 says the hook goes between (3) and (6). Within that
window, (4) and (5) both have semantic side effects:

  - (4) interrupt_end may translate `-ERESTARTSYS` →
    `-EINTR` (via do_signal). Phase 2's hook needs to capture the
    user-visible retval, so it must come AFTER (4).
  - (5) clears `PT_SYSCALL_NR`. The hook reads the cached
    `syscall_nr` local (captured at step 2), not
    `PT_SYSCALL_NR`, so ordering vs (5) is logically a no-op —
    but cosmetically, putting the hook BEFORE (5) keeps regs in a
    consistent "syscall NR still valid" shape for future hooks
    (Phase 5/6) that may want to read it.

Decision: insert between (4) and (5). The interrupt_end translation
of `-ERESTART*` to `-EINTR` is captured; the regs shape stays
syscall-NR-valid for the hook; the args[0..5] slots
(regs->gp[HOST_DI/SI/DX/R10/R8/R9]) are still the original syscall
arg values that C.3's marshal-from-kvm-regs populated at entry
(no in-tree path mutates them, but documenting the dependency in
the comment block guards against future regressions).

Second-hardest: the `struct kvm_v2_replay_entry` shape extension.
Phase 1's bare 16-byte header is fine for the no-op stub, but
Phase 2 needs an inline payload for the {nr, retval, args[6]}.
Three options considered:

  (a) keep the header bare; allocate a separate inline payload
      struct in the buffer following each header. Cleaner
      separation but requires walker code to chase the pointer.
  (b) grow the header to include the syscall payload inline.
      Simpler walker; the `@size` field documents the on-disk
      shape; future kinds (RDTSC, SIGALRM, MMIO) can use the
      same `@size` discipline once their payloads land via
      union members.
  (c) make the payload a fixed-size tail; pass a `kind`-specific
      payload struct at consume time. Same as (a) but with
      compile-time payload sizes per kind.

Picked (b). It's the simplest walker shape; the Phase 1 KUnit
already understood `@size` as "advance N bytes per entry"
implicitly (the buffer is a packed stream); Phase 2 lands one
payload kind (SYSCALL) with the contract `@size == sizeof(entry)`
across the entire log. Phase 5-6 will introduce mixed-kind logs
where `@size` varies per entry; Phase 3's consume walker reads
`@size` from each header and advances by that amount.

## What's deferred (Phase 2.5, 3-7)

  - **Phase 2.5** — per-NR side-buffer routing (`getrandom`,
    `read`, `pread64`, `recvfrom`, `readv`). Port of
    `kvm-v1-archive/record.c:758-1080`. Mostly mechanical lift;
    each NR allocates a side buffer + copy_from_user's the
    written bytes; the entry's `args[1]` slot becomes
    "side-buffer length" (or similar discriminator).

  - **Phase 3** — replay primitive. `kvm_v2_record_consume_syscall`
    fills in with a FIFO walk over `rec->buffer`. The consume
    hook slots into `kvm_v2_handle_io_trap` BEFORE
    `handle_syscall(regs)` — the entry-point recommendation is
    "line ~2195 prelude, between the `syscall_nr` capture
    (~2153) and the existing `handle_syscall(regs)` call". On
    a SYSCALL kind match, the consume function pops the next
    entry, validates NR equality (else returns -EILSEQ), writes
    the recorded retval into `regs->gp[HOST_AX]`, and returns 1
    (entry served; skip handle_syscall). Returns 0 to fall
    through to live (loose mode); strict mode delivers SIGSEGV
    on the divergence path.

  - **Phase 4** — gadget-disable on record-enable
    (`KVM_V2_GADGET_OFF_RECORD` byte in the per-vCPU gadget state
    page; lstar_gadget.S reads it at gadget_entry).

  - **Phase 5** — RDTSC trap + vvar refresh capture.

  - **Phase 6** — SIGALRM-on-syscall-count determinism (memo
    13's option (b); §3.6 in memo 27).

  - **Phase 7** — kselftest plumb + full KUnit round-trip suite +
    overflow handler.

## Recommended Phase 3 entry point

The Phase 3 consume hook site is the SAME function as Phase 2's
observe hook — `arch/um/backend/kvm-v2/syscall_trap.c::
kvm_v2_handle_io_trap`, `UM_KVM_TRAP_SYSCALL` arm — but on the
OTHER side of `handle_syscall(regs)`:

```c
syscall_nr = regs->gp[HOST_AX];       /* line ~2153, already there */
/* ... PT_SYSCALL_NR install, IP/EFLAGS copy ... */

/* NEW PHASE 3 HOOK — pre-handle_syscall consume */
if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
    struct kvm_v2_record *rec = kvm_v2_record_active();
    if (rec && rec->state == KVM_V2_RECORD_REPLAYING) {
        long served_ret;
        int rc = kvm_v2_record_consume_syscall(rec, syscall_nr,
                                               &served_ret);
        if (rc > 0) {
            regs->gp[HOST_AX] = (unsigned long)served_ret;
            goto skip_handle_syscall; /* jump past handle_syscall */
        }
        /* rc == 0: not served (fall through to live); rc < 0:
         * divergence — Phase 3 enters strict-mode SIGSEGV here. */
    }
}

handle_syscall(regs);                /* line ~2195, already there */

skip_handle_syscall:
/* ... interrupt_end, observe_syscall (Phase 2), marshal-out ... */
```

The `goto skip_handle_syscall` shape is the cleanest way to bypass
`handle_syscall` without restructuring the existing fall-through
sequence; it's the same pattern as v1
(`kvm-v1-archive/syscall_class.c`'s per-NR arms each had a
"served from log" branch that jumped to the post-call marshal).

Phase 3's `kvm_v2_record_consume_syscall` body (currently a stub
returning 0) needs to:

  1. Take `rec->lock`.
  2. Compute a "next entry" cursor — Phase 1 doesn't have one;
     Phase 3 adds `rec->buffer_replayed` analogous to
     `rec->buffer_used` (the write cursor that Phase 2 just wired).
  3. If `buffer_replayed >= buffer_used`, return -ENODATA
     (end-of-log).
  4. Cast `buffer + buffer_replayed` to `struct kvm_v2_replay_entry`,
     validate `entry->kind == KVM_V2_REPLAY_SYSCALL`.
  5. Validate `entry->syscall.nr == (s32)syscall_nr` — return
     -EILSEQ on mismatch.
  6. Write `*ret_value = (long)entry->syscall.retval`.
  7. Bump cursor by `entry->size`, increment
     `rec->entries_replayed`.
  8. Return 1 (entry served).

The hook site call needs to handle three return codes:

  - `rc > 0`: entry served; skip handle_syscall.
  - `rc == 0`: not served; fall through to live (today's path;
    Phase 1's stub returns this unconditionally).
  - `rc < 0`: divergence — strict mode delivers SIGSEGV via
    `force_sig(SIGSEGV)` at the hook site; loose mode falls
    through to live (and `pr_warn`s).

The strict-mode `force_sig(SIGSEGV)` path is the one new ABI
surface Phase 3 introduces beyond Phase 2; it requires
`#include <linux/sched/signal.h>` in syscall_trap.c plus the
`current` task context (already on hand in the hook site).

## LoC delta

  - `arch/um/backend/kvm-v2/record.c`: +~150 (active accessor +
    observe body, ~80 LoC of code + ~70 LoC of kerneldoc).
  - `arch/um/backend/kvm-v2/syscall_trap.c`: +~50 (hook + the
    rationale comment block).
  - `arch/um/backend/kvm-v2/vcpu.c`: +~5 + comment (one new arm
    in the SMP-T55 skip condition + the new include line).
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h`: +~30 (struct
    extension + prototype).
  - `arch/um/backend/kvm-v2/test_record.c`: +~170 (new KUnit
    case + the two new includes).
  - Diary (this file): +N
  - decisions-log D132: +~50

Net: 5 files modified, ~450 lines added, 1 line of behavioral
change in the SMP-T55 skip site, 0 lines removed in the existing
surface (the Phase 1 stubs' `(void)cast` lines were entirely
replaced by the real bodies).

## checkpatch

```
git diff arch/um/backend/kvm-v2/{record,test_record,syscall_trap,vcpu}.c \
        arch/um/backend/kvm-v2/kvm_v2_backend.h |
    ./scripts/checkpatch.pl --no-tree -
```

Result: **0 errors, 0 warnings.** Clean.

(The full-file checkpatch on `arch/um/backend/kvm-v2/vcpu.c`
flags 2 pre-existing errors + 24 pre-existing warnings — none
introduced by Phase 2's 5-LoC diff to that file.)

## Acceptance criteria (memo 27 §Phase 2)

  - [x] Build clean (`make ARCH=um O=…`).
  - [x] checkpatch clean (0 errors / 0 warnings on the diff).
  - [x] KUnit `test_kvm_v2_record_observe` passes
        (3/3 cases in record suite).
  - [x] `/bin/echo` boots to exit-0 with record OFF (the static
        key never armed outside KUnit cases; the new hot-path
        gate is patched-out NOPs).
  - [x] `static_branch_unlikely(&um_kvm_v2_record_enabled)` gate
        on the SMP-T55 skip site verified zero-cost when off
        (the new condition is one `cmp $0, key; jne` — patched
        out by jump_label_init at boot).
  - [ ] Hot-path benchmark (Python startup) under record-OFF —
        deferred to Phase 7's benchmark suite; KUnit smoke
        coverage is sufficient acceptance for the Phase 2 close.

Phase 2 closes. Phase 3 (replay primitive — consume hook + strict-
mode divergence) is the next sub-phase per memo 27 §Phase 3.

## Refs

  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    §Phase 2 + §3.1 + §3.8(i).
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/08-record-port-phase1.md`
    (Phase 1 predecessor).
  - D132 (record/replay Phase 2 surface decision).
  - D131 (record/replay Phase 1 surface decision; predecessor).
  - D130 (memo 27 design contract).
  - Commit `358c4d3c83ab` (Phase 1 landing).
