# Record/replay v2 port — Phase 1 landed (2026-05-16)

Track B / `#169` record/replay port — Phase 1 lifts the v1
`kvm-v1-archive/record.c` state machine (alloc / start / stop /
replay / destroy + the `DEFINE_STATIC_KEY_FALSE` hot-path gate)
onto v2's symbol prefix and per-Phase-1 fixed-buffer shape.
Observation (`observe_syscall`) and consume (`consume_syscall`)
land as no-op stubs so the Phase 2 hook in
`arch/um/backend/kvm-v2/syscall_trap.c` and the Phase 3 replay
primitive can land as pure additions without touching the data
structures.

Design contract: `02-workstreams/D-kvm-backend/27-record-replay-
v2-port.md` §Phase 1 + §4 entry-point recommendation.

## What landed

  - **`arch/um/backend/kvm-v2/record.c`** (NEW, 547 lines):
    - `DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)` — the
      hot-path gate that the Phase 2 hook in syscall_trap.c will
      consult via `static_branch_unlikely(...)`. EXPORT_SYMBOL_GPL.
    - File-scope `um_kvm_v2_active_record` + `um_kvm_v2_record_lock`
      single-active-record discipline. Mirror of v1
      (`kvm-v1-archive/record.c:76-77`).
    - `kvm_v2_record_alloc(size_t buffer_size)` — kzalloc the
      container + kvmalloc the entry buffer. `buffer_size = 0`
      selects a 64 KiB default; capped at 64 MiB. Initial state =
      `KVM_V2_RECORD_INIT`, `strict_replay = true` (v1 default).
    - `kvm_v2_record_destroy(rec)` / `kvm_v2_record_free(rec)`
      (alias; v1 exposed both names) — drops the static-key gate
      defensively if the caller forgot to stop, then `kvfree` the
      buffer + `kfree` the container.
    - `kvm_v2_record_start(rec)` — `INIT → RECORDING` /
      `STOPPED → RECORDING` (re-arm). Resets per-session counters.
      Calls `static_branch_enable(&um_kvm_v2_record_enabled)`.
      Returns `-EBUSY` when another container already holds the
      slot, `-EINVAL` for `RECORDING`/`REPLAYING` source state.
    - `kvm_v2_record_stop(rec)` — `RECORDING → STOPPED` /
      `REPLAYING → STOPPED`. Calls
      `static_branch_disable(&um_kvm_v2_record_enabled)`.
      Returns `-EINVAL` for `INIT`/`STOPPED` source state. (Note:
      stricter than v1, which was idempotent — the explicit error
      matches memo 27 §Phase 1's KUnit-asserts-invalid-transitions
      criterion.)
    - `kvm_v2_record_replay(rec)` — `STOPPED → REPLAYING`. Re-arms
      the static-key gate (slot may have been released by `_stop`).
      Returns `-EBUSY` / `-EINVAL` on the corresponding bad cases.
    - `kvm_v2_record_set_strict_replay(rec, strict)` /
      `kvm_v2_record_strict_replay(rec)` — getter/setter for the
      replay strictness flag. v1's analog read from the global
      active record; the v2 port takes an explicit `@rec` so the
      KUnit harness can flip the flag without going through
      debugfs (memo 27 §3.10 "no debugfs in Phase 1").
    - `kvm_v2_record_observe_syscall(rec, nr, ret, regs)` —
      **Phase 2 stub**. Currently a no-op
      (`(void)` casts on the parameters). The symbol + the
      EXPORT_SYMBOL_GPL ship now so Phase 2 lands as a pure
      reimplementation of the function body without touching
      syscall_trap.c's `static_branch_unlikely` call site.
    - `kvm_v2_record_consume_syscall(rec, nr, *ret)` —
      **Phase 3 stub**. Returns 0 ("entry not served — fall
      through to live syscall"). Symbol + EXPORT_SYMBOL_GPL ship
      now for the same reason as observe.

  - **`arch/um/backend/kvm-v2/test_record.c`** (NEW, 224 lines):
    - `test_kvm_v2_record_basic` — happy-path traversal:
      alloc → start → stop → replay → stop → destroy. After every
      transition asserts both `rec->state` AND
      `static_branch_unlikely(&um_kvm_v2_record_enabled)` to lock
      the gate-flip contract from day 1 (v1's Review-01 P0 was
      exactly the "replay forgot to re-arm the gate" bug; the KUnit
      catches that regression on the v2 port).
    - `test_kvm_v2_record_state_transitions` — walks the 7
      invalid edges of the state graph:
        - `stop` from `INIT`           → `-EINVAL`
        - `replay` from `INIT`         → `-EINVAL`
        - `start` from `RECORDING`     → `-EINVAL` (double-start)
        - `replay` from `RECORDING`    → `-EINVAL`
        - `stop` from `STOPPED`        → `-EINVAL`
        - `start` from `REPLAYING`     → `-EINVAL`
        - `replay` from `REPLAYING`    → `-EINVAL`
      Plus NULL-pointer cases on every public entry point.

  - **`arch/um/backend/kvm-v2/kvm_v2_backend.h`** (926 → 1150 lines,
    +224):
    - `enum kvm_v2_record_state` — `INIT/RECORDING/STOPPED/REPLAYING`.
    - `enum kvm_v2_replay_kind` — the discriminator for Phase 2-6
      observation entries (`SYSCALL`, `SIGALRM`, `RDTSC`,
      `VVAR_READ`, `INTERRUPT`, `MMIO_READ`). Phase 1 declares the
      full enum so Phase 2's append helper can land additively;
      none of these values are referenced from Phase 1 code.
    - `enum kvm_v2_replay_meta_kind` — `NONE/SOCKADDR/IOV` for
      Phase 2.5's per-NR metadata routing. Same "declare now, use
      later" pattern.
    - `struct kvm_v2_replay_entry` — 16-byte TLV header
      (`kind/size/sequence`) + variable-length payload. Phase 1
      never reads this; it's wire-shape locked-in for Phase 2's
      buffer appender.
    - `struct kvm_v2_record` — Phase 1 shape. Fields: `state`,
      `strict_replay`, `buffer` (kvmalloc'd entry stream),
      `buffer_size`, `buffer_used`, `sequence`, `entries_recorded`,
      `entries_replayed`, `lock`. (Phase 1 keeps `buffer_used` at
      zero — the field exists so Phase 2 grows usage without
      revisiting the struct.)
    - `DECLARE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)` —
      header-side declaration so the Phase 2 hook in
      syscall_trap.c can resolve the symbol.
    - Public prototypes for the 7 lifecycle entry points + the
      observe/consume stubs.
    - Added `#include <linux/jump_label.h>` for
      `DECLARE_STATIC_KEY_FALSE` and `#include <linux/mutex.h>`
      for `struct mutex`.

  - **`arch/um/backend/kvm-v2/Makefile`** — `record.o` added to
    `obj-$(CONFIG_UM_BACKEND_KVM_V2)` and `test_record.o` to
    `obj-$(CONFIG_UM_BACKEND_KVM_V2_KUNIT)`.

  - **`Documentation/virt/uml/redesign/04-risks/decisions-log.md`** —
    new D131 entry recording the Phase 1 surface choice (fixed-size
    buffer for Phase 1 vs v1's growable log array). See the
    decisions-log diff for the rationale.

## KUnit verification

Build dir: `/home/mjbommar/src/uml-builds/uml-smp-t41fix`.

Build incantation:

```
make ARCH=um O=/home/mjbommar/src/uml-builds/uml-smp-t41fix \
    -j$(nproc)
```

Build clean — no new warnings (the pre-existing
`label 'err_unwind_pt' defined but not used` in exception.c and
the `ignoring return value of 'set_memory_rw'` in syscall_trap.c
are pre-Phase-1 baseline, unrelated to this commit).

Boot incantation (same Phase 3 snapshot recipe per
`06c-snapshot-port-phase3.md` §KUnit verification):

```
timeout 60 .../linux backend=force=kvm-v2 mem=512M ncpus=1 \
    init=/bin/echo rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 panic=-1 </dev/null
```

Result (filtered to KUnit lines):

```
    # Subtest: kvm_v2_marshal
    # kvm_v2_marshal: pass:8 fail:0 skip:0 total:8
ok 1 kvm_v2_marshal
    # Subtest: kvm_v2_byteshape
    # kvm_v2_byteshape: pass:9 fail:0 skip:0 total:9
ok 2 kvm_v2_byteshape
    # Subtest: kvm_v2_snapshot
    ok 1 test_kvm_v2_snapshot_basic
    ok 2 test_kvm_v2_snapshot_full
# kvm_v2_snapshot: pass:2 fail:0 skip:0 total:2
ok 3 kvm_v2_snapshot
    # Subtest: kvm_v2_record
    ok 1 test_kvm_v2_record_basic
    ok 2 test_kvm_v2_record_state_transitions
# kvm_v2_record: pass:2 fail:0 skip:0 total:2
ok 4 kvm_v2_record
```

All 4 suites PASS: `kvm_v2_marshal` 8/8, `kvm_v2_byteshape` 9/9,
`kvm_v2_snapshot` 2/2, `kvm_v2_record` **2/2 (new)**.

The pr_info lines from the state-machine transitions tell the
KUnit story by hand too:

```
um: kvm-v2 record_start: armed (buffer_size=65536, strict_replay=1)
um: kvm-v2 record_stop: disarmed (0 entries recorded, 0 replayed)
um: kvm-v2 record_replay: armed (buffer_used=0, strict_replay=1)
um: kvm-v2 record_stop: disarmed (0 entries recorded, 0 replayed)
um: kvm-v2 record_stop: container never started
um: kvm-v2 record_replay: container has no recorded log
um: kvm-v2 record_start: armed (buffer_size=65536, strict_replay=1)
um: kvm-v2 record_start: container already recording
um: kvm-v2 record_replay: container is recording (stop first)
um: kvm-v2 record_stop: disarmed (0 entries recorded, 0 replayed)
um: kvm-v2 record_stop: container already stopped
um: kvm-v2 record_replay: armed (buffer_used=0, strict_replay=1)
um: kvm-v2 record_start: container is replaying (stop first)
um: kvm-v2 record_replay: container already replaying
```

Each line matches the corresponding KUnit assertion: the four
"armed" / "disarmed" lines come from the basic-path case; the
seven `container ...` warnings come from the invalid-edge case
walking the state graph.

## The hardest part of the state-machine port

**Designing the state-machine error policy to be stricter than
v1's idempotent stop/replay while keeping it KUnit-testable.**
v1's `kvm_record_stop` (`kvm-v1-archive/record.c:380-409`) is
idempotent — calling it on an unarmed container is a no-op. v1's
`kvm_record_replay` (`kvm-v1-archive/record.c:1301-1353`) is also
idempotent on the re-replay edge. Both behaviours made sense for
v1's debugfs-driven operator surface (where a user might
`echo stop > kvm_record_ctl` twice in a row by accident), but the
KUnit spec in memo 27 §Phase 1 explicitly calls out
"assert invalid transitions return error" — which means the
state machine has to reject those edges.

The decision (recorded in D131) is to make Phase 1 stricter than
v1 on the error policy, with the rationale that the v2 surface
is callable directly from KUnit (no debugfs operator yet — Phase 7
adds it) so the error returns are observed and assertable, and
when Phase 7's debugfs surface lands it can adapt the policy
(swallow `-EINVAL` to preserve v1's idempotent-from-shell shape,
or expose the strict semantics as the contract).

A close second: the `static_branch_unlikely` test in the KUnit.
The `DECLARE_STATIC_KEY_FALSE` macro expands to a struct with the
field set so the key reads as false until `static_branch_enable`
flips it; the `static_branch_unlikely` expression is the runtime
predicate. KUnit's `KUNIT_EXPECT_TRUE` / `KUNIT_EXPECT_FALSE`
take a C expression so passing the static-key predicate directly
"just works" — but only after I confirmed the predicate doesn't
have to be in a hot path to be readable (it doesn't; it's a
boolean expression with no side effects).

## What's deferred (Phase 2-7)

  - **Phase 2** — observe hook in `syscall_trap.c::
    kvm_v2_handle_io_trap`. The hook slots in post-handle_syscall
    (line 2195) and pre-marshal-out (line 2353), gated by
    `static_branch_unlikely(&um_kvm_v2_record_enabled)`. The
    Phase 2 commit re-implements `kvm_v2_record_observe_syscall`
    to actually append a KVM_V2_REPLAY_SYSCALL entry to
    `rec->buffer` when `rec->state == KVM_V2_RECORD_RECORDING`.
    No touch to record.c's public surface; the hook insertion in
    syscall_trap.c is a pure addition.

  - **Phase 2.5** — per-NR side-buffer routing (`getrandom`,
    `read`, `pread64`, `recvfrom`, `readv`). Port of
    `kvm-v1-archive/record.c:758-1080`. Mostly mechanical lift.

  - **Phase 3** — replay primitive. `kvm_v2_record_consume_syscall`
    fills in with a FIFO walk over `rec->buffer`, returning
    `-EILSEQ` on NR-mismatch + `-ENODATA` on end-of-log. Strict-
    mode delivers SIGSEGV; loose mode falls through to live
    handle_syscall.

  - **Phase 4** — gadget-disable on record-enable
    (`KVM_V2_GADGET_OFF_RECORD` byte in the per-vCPU gadget state
    page; lstar_gadget.S reads it at gadget_entry; record_start
    writes 1 across the pool).

  - **Phase 5** — RDTSC trap + vvar refresh capture.

  - **Phase 6** — SIGALRM-on-syscall-count determinism (memo
    13's option (b); §3.6 in memo 27).

  - **Phase 7** — kselftest plumb + full KUnit round-trip suite.

## Recommended Phase 2 entry point

The Phase 2 hook site is `arch/um/backend/kvm-v2/syscall_trap.c::
kvm_v2_handle_io_trap` — specifically the `UM_KVM_TRAP_SYSCALL`
arm. Per memo 27 §3.1, the slot is between `handle_syscall(regs)`
(line 2195) and the marshal-out (line 2353), inside the existing
`interrupt_end()` gate (lines 2255-2260). The hook is a pure
addition:

```c
if (static_branch_unlikely(&um_kvm_v2_record_enabled))
    kvm_v2_record_observe_syscall(um_kvm_v2_active_record,
                                  syscall_nr,
                                  (long)regs->gp[HOST_AX],
                                  regs);
```

The `um_kvm_v2_active_record` global has to either become exported
from record.c (currently file-scope) OR Phase 2 introduces a
`kvm_v2_record_active(void)` accessor that returns the locked
pointer under the spinlock. The accessor is cleaner; Phase 2
should pick that path.

## LoC delta

  - `arch/um/backend/kvm-v2/record.c`: +547 (new file)
  - `arch/um/backend/kvm-v2/test_record.c`: +224 (new file)
  - `arch/um/backend/kvm-v2/kvm_v2_backend.h`: +228
    (struct/enum/prototype additions + 2 new includes)
  - `arch/um/backend/kvm-v2/Makefile`: +14
  - Diary (this file): +N
  - decisions-log D131: +~30

Net: ~1013 additions, 0 modifications to existing files outside
of the strictly additive surface in `kvm_v2_backend.h` and
`Makefile`.

## checkpatch

```
./scripts/checkpatch.pl --no-tree -f \
    arch/um/backend/kvm-v2/record.c \
    arch/um/backend/kvm-v2/test_record.c \
    arch/um/backend/kvm-v2/kvm_v2_backend.h \
    arch/um/backend/kvm-v2/Makefile
```

Result: 0 errors, 1 warning. The warning ("EXPORT_SYMBOL(foo);
should immediately follow its function/variable") is a false
positive — `DEFINE_STATIC_KEY_FALSE(um_kvm_v2_record_enabled)`
defines the symbol on the preceding line, and the
`EXPORT_SYMBOL_GPL` follows directly. v1's record.c at
`kvm-v1-archive/record.c:73-74` has the same pattern and the
same checkpatch warning; matches the baseline.

## Acceptance criteria (memo 27 §7 Phase 1)

  - [x] Build clean (`make ARCH=um O=…`).
  - [x] checkpatch clean (1 known-false-positive warning matching
        v1's baseline).
  - [x] KUnit `test_kvm_v2_record_basic` passes.
  - [x] KUnit `test_kvm_v2_record_state_transitions` passes.
  - [x] `static_branch_unlikely(&um_kvm_v2_record_enabled)`
        verified to flip on `_start` / `_replay` and off on
        `_stop` / `_destroy` (asserted in the basic KUnit case).

Phase 1 closes. Phase 2 (observe hook in syscall_trap.c) is the
next commit; the static-key gate + record.c symbol surface are
in tree for it to land additively.
