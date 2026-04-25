# Memo 13: First-class record/replay determinism via KVM (task #253)

**Status:** design / not yet implemented. Sits on top of memo 12
(snapshot/forkserver primitives).

**Why now.** Phase 1 closed at 1.15× kvm/seccomp; the perf story
is done. Phase 3's vision §"Time-travel determinism — clean
record/replay, not best-effort" is the remaining big lift. The
existing UML record/replay parking-lot doc
(`08-future-phases/10-uml-record-replay.md`) sketches a host-side
ptrace-driven flow; that approach gets ~10–100× slowdown and
can't replay across kernel versions. Doing record/replay through
KVM gets us deterministic execution at ~2× cost, replayable
across builds. Memo 12's capture/restore primitives are the
foundation; this memo specs what additional state has to be
recorded to make replay byte-identical, and how the replay loop
plumbs it back.

## Determinism budget

The guest's perceived state is a function of:

1. **Initial state** — captured at checkpoint (memo 12).
2. **Intra-guest computation** — fully deterministic given (1).
3. **Sources of nondeterminism the guest reads from the host:**
   a. **Time** — RDTSC, RDTSCP, MSR_TSC, TSC_DEADLINE, vvar
      clock page, syscall return values from clock_gettime /
      gettimeofday.
   b. **Random** — CPUID bits for RDRAND/RDSEED, host /dev/random
      bytes returned via getrandom(2).
   c. **Interrupts** — guest-observable timing of host signals
      (timer ticks, I/O completion).
   d. **Syscall results that include host state** — getpid (UML
      already remaps), gettid, getuid (idem), but also
      stat/fstat sizes/mtimes, host's actual /proc/self
      content, network packet arrival, etc.
   e. **MMIO reads** — devices the guest probes; under UML's
      typical config there are very few.

Record/replay determinism = capture every output of (3) at
record time, and feed those captured outputs back at replay time
in lieu of consulting the host.

## Architecture

```
                 RECORD                         REPLAY
                 ──────                         ──────
   memo 12 capture()          ┐         ┌── memo 12 restore_full()
                              │         │
   record_log = []            │         │   for entry in record_log:
   for each VMEXIT/syscall:   │         │     KVM_RUN
     KVM_RUN                  │         │     compare exit shape
     record(entry)            │   ───► │     entry.replay()
       — entry kind            │         │       (deliver canned data)
       — entry timing          │         │
     deliver real result      │         │
                              │         │
   memo 12 free()              ┘         └── memo 12 free()
```

Two new primitives on top of memo 12:

```c
struct kvm_replay_entry {
    enum {
        KVM_REPLAY_TIME,      /* RDTSC / clock syscall result */
        KVM_REPLAY_RAND,      /* getrandom / rdrand */
        KVM_REPLAY_INTERRUPT, /* preemption injection point */
        KVM_REPLAY_SYSCALL,   /* class-A passthrough syscall return */
        KVM_REPLAY_MMIO_READ, /* device read */
    } kind;
    u64 instruction_count;    /* TSC at point of capture */
    u64 data[4];              /* kind-specific payload */
};

struct kvm_record {
    struct kvm_snapshot *checkpoint;
    struct kvm_replay_entry *log;
    size_t                  log_capacity;
    size_t                  log_count;
    bool                    recording;        /* RECORD mode */
    bool                    replaying;        /* REPLAY mode */
    size_t                  replay_cursor;
};

int kvm_record_start(struct kvm_record *rec);
int kvm_record_stop(struct kvm_record *rec);
int kvm_record_replay(struct kvm_record *rec);
```

The `recording` and `replaying` flags gate the kvm_run_userspace
dispatcher's capture/replay hooks (zero-cost when both are off,
which is always in non-record builds).

## Sources of nondeterminism — concrete coverage

### Time (3a)

**RDTSC / RDTSCP.** KVM exposes them via the guest TSC. We can
make the guest's TSC deterministic by setting MSR_TSC at restore
time to match the record-time value, plus disabling RDTSC pass-
through via `KVM_CAP_SET_TSC_KHZ` to a fixed virtual frequency.
Cost: each RDTSC becomes a memory load from the synced sregs +
an offset add — same hot path the gadget already uses for the
clock_gettime body's `mov %gs:..., %edx` pattern.

**Clock syscalls.** Already class-B in our taxonomy
(arch_prctl etc. propagate FS_BASE/GS_BASE; clock_gettime is
class-E gadget). The class-A passthrough path's clock-related
syscalls (clock_nanosleep, gettimeofday) already round-trip
through `handle_syscall`; we record their return into the log
and serve from the log on replay.

**vvar page.** The G5 gadget reads from the vvar page for
clock_gettime's fast path. We freeze the vvar page values at
record time (write a snapshot copy back at restore + on each
gadget refresh during replay).

### Randomness (3b)

**RDRAND / RDSEED.** CPUID bits are masked off in
`kvm_ensure_cpuid_done` for record/replay builds. The guest then
uses the software-RNG fallback (drbg) which is deterministic
given the seed. Capture the drbg seed at checkpoint.

**getrandom(2)** is a syscall; class-A passthrough; record
each return value.

**/dev/random / /dev/urandom reads.** Are read() syscalls;
class-A passthrough; recorded.

### Interrupts (3c)

This is the hardest case. Linux's host signal-driven preemption
delivers SIGALRM to UML at unpredictable instruction boundaries.
Replay needs to inject the signal at the *same* boundary.

Two-tier approach:

1. **Coarse-grained:** record `(instruction_count_at_signal,
   signal_kind)` and replay by instructing KVM to single-step
   to that instruction count, then inject the equivalent VM-IRQ.
   `KVM_CAP_VCPU_EVENTS` lets us inject a pending exception/IRQ
   on the next KVM_RUN.
2. **Fine-grained:** for sub-instruction-level signals (rare —
   only matters for instructions that touch memory and could
   trip a fault mid-write), use KVM's branch-counter PMU to
   re-key timing precisely.

`instruction_count` derives from a hardware retired-instruction
counter (Intel: `INST_RETIRED.ANY`, configured via the existing
KVM_CAP_PMU_CAPABILITY plumbing from task #255). We program the
counter to fire an overflow at a record-time-known boundary,
making replay deliver the IRQ at the same boundary.

This is the same approach that's worked in rr (the upstream
record-replay tool); we steal the algorithm wholesale.

### Syscall results (3d)

Class-A passthrough syscalls (the bulk of all calls) flow
through `handle_syscall` and produce a `regs->gp[HOST_AX]`
return + zero or more user-memory writes (for syscalls that
take output buffers). We hook the post-handle_syscall path:

```c
if (rec->recording) {
    record_syscall_result(rec, syscall_nr,
                          regs->gp[HOST_AX], output_buffers);
}
if (rec->replaying) {
    replay_syscall_result(rec, syscall_nr,
                          &regs->gp[HOST_AX], output_buffers);
    /* skip handle_syscall — replay supplies the answer. */
    goto skip_dispatch;
}
```

Class-B (vCPU-state) and class-C (sigframe) get the same
treatment — they're rarer but still nondeterministic if the
host's actual state differs.

### MMIO (3e)

UML's typical config has very few MMIO devices. Today MMIO
reads go through `kvm_decode_mmio` in the integrated path. The
record/replay hook is in the same place: capture the byte(s)
returned by the host, replay from log on replay.

## Sequencing

| Step | Effort | Output |
|------|--------|--------|
| 1. `struct kvm_record` skeleton + start/stop/replay shell | half day | builds, no recording yet |
| 2. Time recording (TSC + clock syscalls + vvar) | 1 day | tight loop replays bit-identically |
| 3. Syscall recording (class-A return + buffers) | 2 days | `/bin/echo` recorded + replayed |
| 4. Interrupt recording via PMU instruction-count overflow | 3 days | preempt-heavy workload deterministic |
| 5. Random recording (getrandom + /dev/random) | half day | `python3 -c 'import random;...'` deterministic |
| 6. MMIO recording | half day | full coverage of nondeterminism budget |
| 7. `record-replay-smoke` kselftest | half day | regression guard |
| 8. Documentation + measurements | half day | this memo's status flipped to "implemented" |

Total: ~7-8 engineer-days. None of it blocks Phase 1 closure
(already done). Can run in parallel with the rest of the Phase 3
ladder (#250 v2 step 3+4, #252 syzkaller backend).

## Open questions

- **Snapshot file format vs in-memory log.** Today memo 12's
  capture is an in-memory `struct kvm_snapshot`; the record log
  is similarly in-memory. For cross-process replay (record on
  host A, replay on host B), we need a serialization format.
  The 08-future-phases/02-snapshot-to-disk.md memo sketches an
  ELF64-core layout for v2; this is the natural extension point.
- **Replay determinism across kernel versions.** Recording
  against kernel V1 and replaying on V2 requires that the V2
  KVM_RUN-loop produces byte-identical syscall dispatch. We
  should record the kernel git rev + Kconfig hash in the log
  and refuse cross-version replay v1.
- **PMU compatibility across silicon.** `INST_RETIRED.ANY` is
  Intel-specific; AMD has a different umask. ARM64 has its own
  PMU API. The interrupt-recording path needs an arch-specific
  shim that picks the right counter. Memo 12's per-arch port
  (#256/#257) is a sister project; align signatures.

## Status

- 2026-04-25 (initial) — memo written.
- 2026-04-25 (later) — **steps 1+2 LANDED.**
  - Step 1: `struct kvm_record` skeleton + alloc/start/stop/
    replay/destroy + KUnit basic-shape test + `kvm-record-smoke`
    kselftest. Commits `56274adfe16b` + `c2ead30c12e4`.
  - Step 2: `DEFINE_STATIC_KEY_FALSE(um_kvm_record_enabled)` +
    active-record registry + dispatcher hook in
    kvm_decode_syscall (gated, zero hot-path cost when off) +
    `/sys/kernel/debug/um/kvm_record_{ctl,state}` debugfs
    surface. Commits `33fd1fff3c1e` + `6b1357d96ad5` +
    `988e2e08afb0`.
- Step 3 (full output-buffer capture for read/write-style
  syscalls), step 4 (PMU interrupt boundary recording), step 5
  (getrandom/drbg seeding), step 6 (MMIO recording) deferred to
  focused sessions; the v1 dispatcher hook above already
  validates the static-key + active-record-registry pattern
  scales without perf regression (perf-fallback held at
  ratio_kvm/seccomp ≈ 1.13× post-hook).
