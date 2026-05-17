# Record/replay v2 port — Phase 3 landed (2026-05-16)

Track B / `#169` record/replay port — Phase 3 closes the loop: the
replay primitive `kvm_v2_record_consume_syscall` gets a real body, the
matching pre-`handle_syscall` hook lands in
`syscall_trap.c::kvm_v2_handle_io_trap`, and the
`test_kvm_v2_record_strict_replay` KUnit case exercises the
record → replay round trip end-to-end (4 KUnit cases in the record
suite, up from 3 at Phase 2).

Design contract: `02-workstreams/D-kvm-backend/27-record-replay-
v2-port.md` §Phase 3 + §3.2 (replay-side hook site).

Phase 1 (commit `358c4d3c83ab`) landed the state-machine skeleton +
`DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)` gate. Phase 2
(commit `41e0de91afb8`) wired the `observe_syscall` hook AFTER
`handle_syscall` in `kvm_v2_handle_io_trap`. Phase 3 is symmetric:
the `consume_syscall` hook fires BEFORE `handle_syscall`, gated by
the same static key, and on a served entry (`rc > 0`) jumps past
the live syscall to the post-call marshal-out path. Strict-mode
divergence (`rc < 0 && rec->strict_replay`) delivers `force_sig
(SIGSEGV)` to current — replay's whole contract is fail-stop on
the first byte of divergence.

## What landed

  - **`arch/um/backend/kvm-v2/record.c`** (+~125 LoC):
    - `kvm_v2_record_consume_syscall` body. Phase 1's stub
      (returned 0 unconditionally) becomes a real FIFO walker:
      take `rec->lock`, validate `rec->state == REPLAYING`, read
      the next entry at `rec->buffer + rec->buffer_replayed`,
      validate `entry->kind == KVM_V2_REPLAY_SYSCALL` AND
      `entry->syscall.nr == syscall_nr`, populate `*ret_value`
      from `entry->syscall.retval`, advance the cursor by
      `entry->size`, bump `entries_replayed`, return 1.
    - Return code table:
        * **1**         entry served (the only side-effect path).
        * **0**         NULL @rec OR @rec->state != REPLAYING
                        (quiet no-op; caller falls through to
                        live).
        * **-ENODATA**  end-of-log (`buffer_replayed >=
                        buffer_used`); strict mode SIGSEGVs.
        * **-EILSEQ**   sequence error: kind != SYSCALL (buffer
                        corruption — Phase 2's observe only writes
                        SYSCALL entries), nr mismatch (replay
                        diverged from record on the syscall
                        stream), partial entry at tail (size
                        header lies), or size < `sizeof(entry)`
                        (defensive). Strict mode SIGSEGVs.
    - Cursor invariant: `buffer_replayed <= buffer_used` always
      holds; the consume walker advances the read cursor only on
      a fully-validated entry, so a divergent NR or end-of-log
      leaves the cursor positioned for the operator to dump the
      buffer post-mortem (`rec->buffer + rec->buffer_replayed`
      points at the entry the replayer rejected, or one byte
      past the last valid entry).
    - `kvm_v2_record_start` now also resets `rec->buffer_replayed`
      to 0 (Phase 2's reset was buffer_used + sequence +
      entries_recorded + entries_replayed only).
    - `kvm_v2_record_replay` now resets `rec->buffer_replayed`
      to 0 too — symmetric to `entries_replayed` reset. A replay
      always starts from the head of the log.

  - **`arch/um/backend/kvm-v2/syscall_trap.c`** (+~85 LoC):
    - Adds `#include <linux/sched/signal.h>` for `force_sig`.
    - In `kvm_v2_handle_io_trap`'s `UM_KVM_TRAP_SYSCALL` arm,
      BEFORE `handle_syscall(regs)` (the existing call at line
      ~2196), a new gated block:

      ```c
      if (static_branch_unlikely(&um_kvm_v2_record_enabled)) {
          struct kvm_v2_record *rec = kvm_v2_record_active();

          if (rec && rec->state == KVM_V2_RECORD_REPLAYING) {
              long served_ret = 0;
              int rc = kvm_v2_record_consume_syscall(rec, syscall_nr,
                                                     &served_ret);
              if (rc > 0) {
                  regs->gp[HOST_AX] = (unsigned long)served_ret;
                  goto skip_handle_syscall;
              }
              if (rc < 0 && rec->strict_replay) {
                  pr_info_ratelimited(
                      "kvm-v2 record: strict replay divergence nr=%lu rc=%d entries_replayed=%llu\n",
                      syscall_nr, rc, rec->entries_replayed);
                  force_sig(SIGSEGV);
                  goto skip_handle_syscall;
              }
              /* rc == 0 or loose miss: fall through to live. */
          }
      }
      ```

    - The new label `skip_handle_syscall:` lands right BEFORE the
      `PT_SYSCALL_NR(regs->gp) = -1;` clear at line ~2351, so the
      replay-served path skips:
        * the live `handle_syscall`,
        * the `KVMV2_OP_HANDLE_SYSCALL_POST` trace,
        * the `interrupt_end()` `-ERESTART*`-drain block,
        * the Phase 2 observe hook (we'd be re-observing the same
          retval we just consumed — pointless and would inflate
          `entries_recorded` mid-replay),
      and rejoins the common post-call path at the `PT_SYSCALL_NR`
      clear + marshal-out. SYSRETQ still pops RIP/RFLAGS correctly
      because the entry-side stash of `HOST_CX→HOST_IP` and
      `HOST_R11→HOST_EFLAGS` ran before our hook.

  - **`arch/um/backend/kvm-v2/kvm_v2_backend.h`** (+~33 LoC):
    - `struct kvm_v2_record` grows `size_t buffer_replayed` (read
      cursor, distinct from `buffer_used` write cursor). Placement
      between `buffer_used` and `sequence` keeps the field-by-
      field record-side cluster contiguous (write cursor →
      read cursor → sequence number → counters).
    - Updated kerneldoc for `@buffer_used` ("write cursor") and
      added `@buffer_replayed` documenting the invariant.
    - `kvm_v2_record_consume_syscall` prototype: Phase 1's "stub"
      comment becomes a real return-code table mirroring the
      function-level kerneldoc.

  - **`arch/um/backend/kvm-v2/test_record.c`** (+~164 LoC):
    - New `test_kvm_v2_record_strict_replay` KUnit case. No live
      vCPU dependency — same shape as the prior 3 record cases:
      drive `kvm_v2_record_observe_syscall` directly to build a
      log, then drive `kvm_v2_record_consume_syscall` directly to
      consume it back, asserting the rc + cursor + retval at each
      step.
    - Three sub-scenarios in one case:
        1. **Happy path** — alloc(4096) → start → observe 5
           entries with NR=__NR_getpid (39) + distinct retvals
           4242..4246 → stop → replay. FIFO consume × 5:
           each rc=1, retval matches, cursor +=
           `sizeof(entry)`. 6th consume → -ENODATA; cursor does
           NOT advance.
        2. **NR-mismatch divergence** — fresh alloc → start →
           observe 1 entry with NR=39 → stop → replay → consume
           with NR=110 → -EILSEQ; cursor preserved.
           Sanity follow-up: consume with NR=39 (the recorded
           value) STILL serves (rc=1, retval matches) because
           the divergent consume didn't advance the cursor.
        3. **Not-REPLAYING state** — stop the container, consume
           returns 0 quietly; @ret_value left untouched.
           NULL @rec consume returns 0 quietly too.

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
    ok 4 test_kvm_v2_record_strict_replay
# kvm_v2_record: pass:4 fail:0 skip:0 total:4
ok 4 kvm_v2_record
```

All 4 suites pass. Record suite is now 4/4 (Phase 2: 3/3).

Beyond KUnit: the kernel boots `/bin/echo` to the same VFS-mount
panic as before with no record container ever instantiated outside
the KUnit cases. The static_branch gate guarantees the new
pre-`handle_syscall` block compiles to a 5-byte NOP that the kernel
patches out at boot; no non-record dispatch ever touches
`kvm_v2_record_active()` or `kvm_v2_record_consume_syscall()`.

## Hardest part

The replay-cursor / strict-divergence integration's hardest part
was deciding where the `skip_handle_syscall:` label belongs in the
`kvm_v2_handle_io_trap` flow. The replay-served path needs to skip:

  - `handle_syscall(regs)` itself — that's the whole point.
  - The `interrupt_end()` `-ERESTART*` drain — the recorded entry's
    retval was either non-`-ERESTART*` (in which case interrupt_end
    is a no-op) or the recording captured a `-ERESTART*` post-
    interrupt_end translation, meaning the retval we just stuffed
    into HOST_AX is already the post-interrupt-end value and
    re-running interrupt_end would double-translate.
  - The Phase 2 observe hook — re-observing a replayed entry would
    silently grow the log mid-replay, breaking the
    `entries_recorded` invariant the operator's debugfs surface
    relies on.

But the replay-served path must NOT skip:

  - The `PT_SYSCALL_NR(regs->gp) = -1;` clear — this is the
    cross-path orig_ax-leakage protection (commit a478952b8da0).
    A subsequent #PF / #GP dispatcher's `interrupt_end()` would
    misinterpret a stale `orig_ax + -ERESTARTSYS in HOST_AX` as
    an in-progress syscall needing RIP-rewind, → SIGILL.
  - The marshal-out — SYSRETQ has to pop the right RIP/RFLAGS
    from RCX/R11, and the marshal-out helper is what writes them
    into `kvm_run->s.regs.regs`.

Decision: place `skip_handle_syscall:` right BEFORE the
`PT_SYSCALL_NR` clear (and the marshal-out further below).
This is the only seam that gets all three "skip" requirements
right + both "preserve" requirements right. The seam is documented
in the surrounding comment block so the inevitable Phase 5/6 hook
sites that may want to rejoin at the same label have an explicit
invariant to honor.

The strict-mode `force_sig(SIGSEGV)` path took the same seam:
after `force_sig` we still `goto skip_handle_syscall` so the
marshal-out runs and the kernel re-enters the guest cleanly; the
queued SIGSEGV is delivered on the next return-to-userspace check
(`get_signal` → `do_signal`). The alternative — return early from
`kvm_v2_handle_io_trap` with a non-zero rc — was ruled out because
the call site in vcpu.c doesn't have a stable error-propagation
contract; the in-tree behaviour is "any non-zero return halts the
vmexit loop and panics," which is the wrong outcome for a per-
task divergence.

Second-hardest: the cursor non-advance on divergence. The first
version of consume_syscall advanced `buffer_replayed` before the
kind / NR validation, on the theory that a divergent entry is
"consumed" in the sense that we don't want to re-serve it on a
subsequent consume. But that broke the operator-facing diagnostic:
a strict-mode SIGSEGV kills the task, but if a debugfs dump of
`rec->buffer + rec->buffer_replayed` shows ONE PAST the divergent
entry, the operator can't read out the kind / NR / retval that
caused the divergence. Fixed by moving the cursor advance to AFTER
the rc=1 success branch — the test case's "sanity: matching NR
after the failed divergence still serves" sub-scenario directly
asserts this invariant.

## What's deferred (Phase 4-7)

  - **Phase 4** — gadget-disable on record-enable
    (`KVM_V2_GADGET_OFF_RECORD` byte in the per-vCPU gadget state
    page; `lstar_gadget.S` reads it at gadget_entry and branches
    to the fallback). Phase 3's replay primitive works for
    non-gadgeted syscalls today; Phase 4 makes the gadgeted NRs
    (getpid, gettid, getuid, clock_gettime, ...) observable to
    the observe hook so they can be replayed too. Without Phase 4,
    a recorded workload that calls `getpid()` produces NO log
    entry (the gadget services it in-guest with no vmexit), so
    replay sees the live `getpid()` go through to handle_syscall
    and the strict_replay contract fails when the syscall stream
    "diverges" on the first gadgeted call.

  - **Phase 5** — RDTSC trap + vvar refresh capture. Without
    Phase 5, recorded workloads that call `clock_gettime(2)` or
    raw RDTSC get nondeterministic time values on replay; the
    strict-replay contract still holds for the syscall stream
    but the user-visible "time" the replayed program sees
    diverges from record.

  - **Phase 6** — SIGALRM-on-syscall-count determinism.

  - **Phase 7** — kselftest plumb + full KUnit round-trip suite
    against a live vCPU + overflow handler + debugfs control
    surface.

  - **Phase 2.5** (deferred earlier; still pending) — per-NR
    side-buffer routing for `read` / `getrandom` / `recvfrom` /
    `readv` / `pread64`. Without Phase 2.5 the replay primitive
    can't restore the user-visible payload bytes for those NRs;
    only the retval is replayed. Phase 3's KUnit case uses
    `__NR_getpid` (retval-only, no user payload) to sidestep the
    Phase 2.5 dependency.

## LoC delta

  - `arch/um/backend/kvm-v2/record.c`: +124 (consume_syscall body
    + the two buffer_replayed resets in _start / _replay).
  - `arch/um/backend/kvm-v2/syscall_trap.c`: +85 (hook block +
    skip_handle_syscall label + the new #include).
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h`: +32 (struct field +
    kerneldoc on the new field + consume_syscall prototype
    rewrite).
  - `arch/um/backend/kvm-v2/test_record.c`: +164 (new KUnit case).
  - Diary (this file): +N.
  - decisions-log D133: +N.

Net: 4 files modified, ~405 lines added, 21 lines removed (the
Phase 1 / 2 stub bodies + the stub comment block). 0 files added;
0 files removed; 0 lines of behavioral change to existing call
sites (the new `skip_handle_syscall:` label is a jump target the
new block uses; existing flow falls through it unchanged).

## checkpatch

```
git diff arch/um/backend/kvm-v2/ |
    ./scripts/checkpatch.pl --no-tree -
```

Result: **0 errors, 7 warnings.** All 7 warnings are
"Prefer 'fallthrough;' over fallthrough comment" prose mentions
of "fall through" / "fall(s) through" in kerneldoc and inline
comments. No switch-case fallthrough is involved; the warning is
a known false positive (same one Phase 1 hit; documented in D131).

## Acceptance criteria (memo 27 §Phase 3)

  - [x] Build clean (`make ARCH=um O=…`).
  - [x] checkpatch clean (0 errors on the diff; 7 false-positive
        prose-mentions-of-"fall through" warnings).
  - [x] Round-trip KUnit (`test_kvm_v2_record_strict_replay`)
        passes (4/4 cases in record suite).
  - [x] `/bin/echo` boots to the same VFS-mount panic with
        record OFF (the static key never armed outside KUnit
        cases; the new hot-path gate is patched-out NOPs).
  - [x] Strict-mode divergence path implemented: `force_sig
        (SIGSEGV)` on rc < 0 + rec->strict_replay.
  - [x] FIFO consume semantics validated: cursor advances on
        rc=1 only; -ENODATA / -EILSEQ leave the cursor intact
        for operator post-mortem.
  - [ ] **100-syscall record → replay = bit-identical state**
        (memo 27 §Phase 3 "the big one"). Deferred to Phase 7's
        end-to-end test; Phase 3's KUnit covers the consume
        primitive in isolation. Phase 7's selftest will drive a
        real workload through the syscall_trap.c hook to close
        this.

Phase 3 closes. Phase 4 (gadget-disable in record mode) is the
next sub-phase per memo 27 §Phase 4 + §3.3.

## Recommended Phase 4 entry point

Phase 4 wires gadget-disable into the record arm/disarm path so
the 11 gadgeted NRs become observable to the Phase 2 observe hook
+ replayable by the Phase 3 consume hook. Per memo 27 §3.3 the
decision is Option A: a per-vCPU `KVM_V2_GADGET_OFF_RECORD` byte
in the gadget state page; `lstar_gadget.S:gadget_entry` reads it
and branches to the fallback path (`outb $UM_KVM_TRAP_SYSCALL;
sysretq`) when set.

Implementation surface:

  1. `arch/um/backend/kvm-v2/kvm_v2_backend.h` — add the byte
     offset `KVM_V2_GADGET_OFF_RECORD` to the gadget state page
     layout (next to `KVM_V2_GADGET_OFF_TGID` etc.).
  2. `arch/um/backend/kvm-v2/lstar_gadget.S` — add the check at
     `gadget_entry` (5 bytes: `cmpb $0,
     KVM_V2_GADGET_OFF_RECORD(%gs); jne fallback`).
  3. `arch/um/backend/kvm-v2/record.c` — new
     `kvm_v2_record_set_gadget_fallback(bool)` static that walks
     every vCPU in the pool and writes the byte; call from
     `_start` (set 1), `_stop` (set 0), `_replay` (set 1),
     `_destroy` (set 0 if still active).
  4. `arch/um/backend/kvm-v2/test_record.c` — new KUnit case
     `test_kvm_v2_record_gadget_disable` that asserts the byte
     flips correctly across _start / _stop. (The full
     gadget-bypass round-trip is Phase 7's selftest; KUnit can
     only verify the byte-write contract.)

Phase 4 acceptance (memo 27 §Phase 4):
  - Under record mode, `clock_gettime(CLOCK_MONOTONIC, &ts)`
    produces a `__NR_clock_gettime` log entry (gadget bypassed →
    vmexit → handle_syscall → observe).
  - Under record-OFF, gadget hot-path benchmark is unchanged
    (the new check is one untaken `cmp+jne` per gadget entry).

## Refs

  - `02-workstreams/D-kvm-backend/27-record-replay-v2-port.md`
    §Phase 3 + §3.2.
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/11-record-port-phase2.md`
    (Phase 2 predecessor + "Recommended Phase 3 entry point").
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/08-record-port-phase1.md`
    (Phase 1 predecessor).
  - D133 (record/replay Phase 3 surface decision — this landing).
  - D132 (record/replay Phase 2; predecessor).
  - D131 (record/replay Phase 1; predecessor).
  - D130 (memo 27 design contract).
  - Commit `41e0de91afb8` (Phase 2 landing).
  - Commit `358c4d3c83ab` (Phase 1 landing).
