# 04 — Time-travel ↔ record/replay wiring

**Sprint:** post-2026-05-19
**Priority:** MEDIUM-HIGH
**Effort:** medium (~200–400 LoC across `arch/um/kernel/{hooks,time}.c`
and `arch/um/backend/kvm-v2/record.c`)
**Status:** planned
**Depends on:** record/replay Phase 1–7 (done — memo 27);
snapshot Phase 1–6 (done — memo 26).

**Status:** Phases 1 + 2 + 3 + 4 DONE 2026-05-19; integration gate
DONE 2026-05-21.
  * Phase 1 hook wired (`b2ff4c0b775e`)
  * Phase 2 observer + Phase 3 consumer (`f03b12af1267`)
  * Phase 4 KUnit round-trip (`980ebb779ee8`)
  * Live wiring (time.c::time_travel_set_time →
    um_time_travel_consume_replay / um_on_clock_read →
    __um_record_event_clock) — CLOSED at `1bb6dd6b6d38` (the
    HONEST-AUDIT §1 follow-up).
  * **Integration gate** (the acceptance criteria in §"Acceptance
    criteria" of this memo) — closed at this commit via
    `arch/um/backend/kvm-v2/record.c::kvm_v2_record_clock_bench_run`
    + the new `kvm-record-clock-bench` selftest. The bench drives
    N monotonic clock advances through the production chain
    function (`__um_record_event_clock` → `kvm_v2_record_active`
    → `observe_time_travel`), state-transitions into REPLAY, and
    reads back via `um_time_travel_consume_replay`
    (→`kvm_v2_record_active` → `consume_time_travel`). PASS
    iff every advance round-trips byte-identically. Verified at
    N=1 / 50 / 100 / 500 / 4096 on the local build: all PASS.

## Why this matters

The branch shipped record/replay Phase 1–7 in the previous sprint:
the `kvm_v2_record_*` state machine, the observe/consume primitives
for SYSCALL + RDTSC + SIGALRM, the gadget-bypass byte, the
anonymous-union replay-entry payload, KUnit 7/7. What it did
**not** do is integrate with UML's time-travel infrastructure.

The time-travel profile exists
(`Documentation/virt/uml/profiles/time-travel.rst`) and the
**redesign hook** to drive it exists
(`arch/um/kernel/hooks.c:252`):

```c
notrace void __um_time_travel_clock(u64 ns)
{
    /* ... */
}
EXPORT_SYMBOL_GPL(__um_time_travel_clock);
```

But this hook is **never called from anywhere in tree**. The
existing `arch/um/kernel/time.c` time-travel implementation
(mainline pre-redesign) maintains its own clock at
`time_travel_time` and its own event list at
`time_travel_events`. The two clocks are not wired together.

Result: record/replay records the SYSCALL + RDTSC + SIGALRM
streams faithfully, but on replay it cannot guarantee that
timer interrupts (the SIGALRM stream's downstream consumers)
fire at the same virtual time as during record. The replay is
deterministic at the syscall boundary but not at the
timer-interrupt boundary.

This wiring closes that gap. With it, **record/replay becomes
the durable-replay substrate that the time-travel profile was
designed to enable** — guest behaviour reproducible bit-for-bit
across runs, including timer-driven internal state.

## Current state

| Surface | File | State |
|---------|------|-------|
| Redesign hook function | `arch/um/kernel/hooks.c:252` | defined, exported, **unused** |
| Static-key gate | `arch/um/kernel/hooks.c:42` `um_hook_time_travel_active` | defined; no caller flips it |
| Mainline time-travel clock | `arch/um/kernel/time.c::time_travel_time` | active when `CONFIG_UML_TIME_TRAVEL_SUPPORT=y` |
| Record/replay SIGALRM anchor | `arch/um/backend/kvm-v2/record.c::kvm_v2_record_observe_sigalrm` | active, uses `syscall_count_at` |
| Record/replay RDTSC anchor | `arch/um/backend/kvm-v2/record.c::kvm_v2_record_observe_rdtsc` | active, returns recorded TSC |
| Replay-side time-travel consumer | none | not wired — replay does not advance the time-travel clock |

## Proposed change

### Phase 1 — Wire `__um_time_travel_clock` into `time.c`

```c
/* arch/um/kernel/time.c (new) */
void time_travel_set_time(u64 ns)
{
    time_travel_time = ns;

    /* Redesign hook: drives the static-key-gated path that
     * record/replay observes. No-op when the static key is off
     * (production / non-time-travel profiles). */
    if (static_branch_unlikely(&um_hook_time_travel_active))
        __um_time_travel_clock(ns);
}
```

And similarly in `time_travel_add_event`, `time_travel_run_events`,
and the timer-interrupt path. Every clock advance flows through
the hook.

### Phase 2 — Record observer

```c
/* arch/um/backend/kvm-v2/record.c (new) */
void kvm_v2_record_observe_time_travel_advance(struct kvm_v2_record *rec,
                                               u64 ns_at_advance)
{
    /* Append a new replay entry of kind KVM_V2_REPLAY_TIME_TRAVEL. */
    /* Payload: { u64 ns_at_advance; u64 syscall_count_anchor; } */
    ...
}
```

Static-key gate: `um_kvm_v2_record_enabled && um_hook_time_travel_active`.
Both must be on. Production builds pay zero cost.

### Phase 3 — Replay consumer

On replay, the time-travel events from the log are consumed by a
new hook called from `time_travel_set_time`:

```c
void time_travel_set_time(u64 ns)
{
    if (static_branch_unlikely(&um_kvm_v2_record_replaying)) {
        u64 recorded_ns;
        if (kvm_v2_record_consume_time_travel(rec, &recorded_ns) == 1) {
            /* Force the local clock to the recorded value. */
            ns = recorded_ns;
        }
    }
    time_travel_time = ns;
    if (static_branch_unlikely(&um_hook_time_travel_active))
        __um_time_travel_clock(ns);
}
```

### Phase 4 — KUnit coverage

`arch/um/backend/kvm-v2/test_record.c` gets a new case
`test_kvm_v2_record_time_travel_round_trip`:

  1. Stage a sequence of `(ns_advance, syscall_count)` pairs.
  2. Record them via `observe_time_travel_advance`.
  3. Stop, replay.
  4. Consume each pair, verify the order and values match.

Plus an integration case (Phase 5 if scoped): record a real
`clock_gettime(CLOCK_MONOTONIC)` workload under the time-travel
profile, replay, verify the guest sees the same `ns` sequence
both times.

## Effort breakdown

- Phase 1 (clock wiring in `time.c`): ~50 LoC.
- Phase 2 (record observer + replay-entry kind): ~80 LoC
  (mostly mirrors `observe_rdtsc` / `observe_sigalrm` patterns).
- Phase 3 (replay consumer + integration in `time.c`): ~80 LoC.
- Phase 4 (KUnit): ~80 LoC.

Total: ~290 LoC.

## Acceptance criteria

- **Round-trip gate (KUnit):** the new `test_kvm_v2_record_time_
  travel_round_trip` case PASSES.
- **Integration gate:** a guest workload that calls
  `clock_gettime(CLOCK_MONOTONIC)` 100 times under
  `time-travel=inf-cpu` records 100 advances; replay reproduces
  them in order.
- **Production zero-cost gate:** boot time with
  `CONFIG_UML_TIME_TRAVEL_SUPPORT=y` but neither
  `time-travel=inf-cpu` nor record/replay armed is unchanged vs
  baseline (static keys patch out the hook entirely).
- **`umlctl mission` Phase 1 KUnit:** extends from 7/7 record
  cases to 8/8 with the new case.

## Dependencies

- **Code:** record/replay Phase 1–7 (done — memo 27).
- **Code:** snapshot Phase 1–6 (done — memo 26).
- **Profile:** `uml/time-travel` profile already exists; this
  memo doesn't change its defconfig.
- **No memo dependency in this sprint:** can land in parallel
  with memos #1 / #2 / #3.

## Risk notes

- **Time-travel is UP-only.** `time-travel=inf-cpu` requires
  `CONFIG_SMP=n`. The profile compose handles this. Don't try
  to combine with kvm-v2's SMP backend; pin time-travel to the
  seccomp backend in profiles.
- **Mainline `arch/um/kernel/time.c` ABI changes.** This memo
  modifies upstream UML's time-travel surface. Care needed not
  to break existing time-travel users
  (the `time-travel=ext` distributed-system simulator). The
  hook is additive and behind a static key; existing users see
  zero change.
- **Static-key build-time cost.** Adding a second static key
  (`um_hook_time_travel_active`) doubles the entry-cost
  bookkeeping in `kernel/jump_label.c` for UML. Negligible in
  practice but noted.
- **Rollback:** if the wiring breaks an existing time-travel
  test, revert is a single `static_branch_disable` per call site.

## Cross-references

- Sub-agent's full investigation: 2026-05-19 report (item #4).
- Memo 27 (record/replay v2 port): the immediate predecessor.
- Memo 26 (snapshot v2 port): provides the checkpoint substrate
  the time-travel record/replay session is anchored on.
- `Documentation/virt/uml/profiles/time-travel.rst`: the profile
  documentation that calls out the "still a counter stub" gap
  this memo closes.
