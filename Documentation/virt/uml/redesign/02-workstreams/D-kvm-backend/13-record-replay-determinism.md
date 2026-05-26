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

## Per-NR record/replay coverage matrix

The dispatcher in `kvm_record_observe_dispatch()` routes specific
syscall numbers through the side-buffer path so replay restores
their output bytes. The default for unspecial-cased NRs is
inline-only (NR + return value, no payload). Strict replay (the
default) refuses to dispatch on any NR with a partial-record
contract — operators that need to record those NRs flip the
record container to loose mode (`echo loose >
/sys/kernel/debug/um/kvm_record_ctl`) and accept that replay
correctness is best-effort.

| NR              | side-buffer | sockaddr | iovec | strict-replay safe? | notes                                      |
|-----------------|-------------|----------|-------|---------------------|--------------------------------------------|
| `getrandom`     | yes         | n/a      | n/a   | **yes**             | rdi=buf, rsi=len; ret=bytes filled         |
| `read`          | yes         | n/a      | n/a   | **yes**             | rdi=fd, rsi=buf, rdx=count                 |
| `pread64`       | yes         | n/a      | n/a   | **yes**             | same shape as read; offset arg unused      |
| `recvfrom`      | yes         | **yes**  | n/a   | **yes**             | data + sockaddr metadata (META_SOCKADDR)   |
| `recvmsg`       | not yet     | not yet  | not yet | no                | scatter-gather msghdr — ladder rung        |
| `readv`         | yes         | n/a      | **yes** | **yes** (≤8 iovs) | iov array + concatenated data (META_IOV)   |
| `ioctl`         | not yet     | n/a      | n/a   | no                  | per-driver knowledge — case by case        |
| getpid family   | inline-only | n/a      | n/a   | yes (return only)   | gadget-handled; only fallback path records |
| brk/mmap/munmap | inline-only | n/a      | n/a   | yes (return only)   | mm-mutating; no user-output-buffer payload |
| anything else   | inline-only | n/a      | n/a   | yes (return only)   | NR + return; replay restores ret           |

**Open questions on partial-record NRs:**

- For `recvfrom` with src_addr capture: **landed** in commit
  73c2d1fdc993 (review-01 P2 #13). `struct kvm_replay_entry`
  gained a second side-buffer slot ("metadata buffer") and a
  kind tag (`KVM_REPLAY_META_NONE` / `_SOCKADDR` / `_IOV`).
  The recvfrom dispatcher case captures `{u32 addrlen; u8 sa[]}`
  into the metadata slot under tag `META_SOCKADDR`; the replay
  dispatcher restores both the data buffer and the sockaddr +
  addrlen out-pointers. Lifecycle ownership matches the data
  payload (kvmalloc + free at destroy).
- For `readv`: **landed** as a subsequent commit on the same
  metadata-buffer extension. `kvm_record_observe_dispatch`
  walks the user iovec, snapshots up to 8 entries (UIO_FASTIOV)
  into the metadata slot under tag `META_IOV`, and stages a
  single concatenated data buffer of the actually-written
  bytes. Replay walks the recorded iov array and scatters the
  payload back into the user iov targets, mirroring the kernel.
  Iovcnt > 8 falls back to data-only capture (still replayable
  for inline NR/return divergence detection, just doesn't
  restore the user buffers).
- For `recvmsg`: still a ladder rung — needs the same IOV
  metadata wrapper but inside an `msghdr` whose `msg_control`
  also carries scatter-gather data. Reuses the META_IOV slot;
  msg_control may need a third tag (META_CMSG) if the
  ancillary data turns out to be a routine source of replay
  divergence.
- For `ioctl`: per-driver. Most ioctls have either no output
  buffer (set-only commands) or a fixed-size out-arg the
  dispatcher can capture if it knows the cmd code. A
  whitelist mapping `cmd → output_size` is the pragmatic
  approach; populated incrementally as driver-specific record/
  replay needs emerge.

Until those land, the operator's options are (a) avoid those
NRs in record-mode workloads, (b) use loose-replay mode and
accept best-effort correctness, or (c) extend the dispatcher
with a per-driver case.

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
- 2026-04-25 (later) — **steps 1+2+3+3.5+5(partial) LANDED.**
  - Step 1: `struct kvm_record` skeleton + alloc/start/stop/
    replay/destroy + KUnit basic-shape test + `kvm-record-smoke`
    kselftest. Commits `56274adfe16b` + `c2ead30c12e4`.
  - Step 2: `DEFINE_STATIC_KEY_FALSE(um_kvm_record_enabled)` +
    active-record registry + record-side hook in
    kvm_decode_syscall (gated, zero hot-path cost when off) +
    `/sys/kernel/debug/um/kvm_record_{ctl,state}` debugfs
    surface. Commits `33fd1fff3c1e` + `6b1357d96ad5` +
    `988e2e08afb0`.
  - Step 3: variable-length side buffer on `struct kvm_replay_
    entry` (`payload` + `payload_len`) + `kvm_record_observe_
    syscall_buf()` API. Free per-entry payloads on destroy.
    Commit `a085afc07448`.
  - Step 3.5 (replay-side dispatcher):
    `kvm_record_consume_syscall()` consumed before
    `handle_syscall` in kvm_decode_syscall when in replay mode —
    serves the recorded return + copy_to_user's the side buffer
    back. Commit `918ccaf828cc`.
  - Step 5 (per-NR routing): `kvm_record_observe_dispatch()`
    per-NR router. Today covers `__NR_getrandom`, `__NR_read`,
    `__NR_pread64`, `__NR_recvfrom`, `__NR_readv` through the
    side-buffer path so replay restores the user-buffer payload
    byte-identically. recvfrom carries an additional metadata
    slot (`META_SOCKADDR`) for the src_addr/addrlen out-pointers;
    readv carries a `META_IOV` slot snapshotting the iov array
    so replay can scatter the concatenated payload across the
    same user buffers the kernel filled. Other NRs default to
    the inline-only path. Commits `1992d6315f92` + `d669522cc964`
    + `915299f2a9d6` + `73c2d1fdc993` (regs * extension +
    read/pread64 + recvfrom + meta extension/recvfrom-sockaddr).
  - Round-trip KUnit (`kvm_record_roundtrip_test`): drives the
    record/replay log directly via the C API — observe three
    syscalls, replay, consume in FIFO order, verify cursor
    exhaustion + NR-mismatch divergence returns -EILSEQ.
    Side-buffer round-trip: observe a fake-getrandom entry
    with deterministic 8-byte payload, consume + memcmp the
    payload bytes back. Skip-on-early-boot pattern matches the
    snapshot/record basic-shape tests. Commits `915299f2a9d6`
    + `14414378fde9`.
  - Debugfs log dump
    (`/sys/kernel/debug/um/kvm_record_log`): bounded prefix of
    captured entries with kind / instruction_count / inline
    data + payload-presence indicator. Commit `9d62f71540e2`.
  - Per-entry timestamp via `ktime_get_ns()` populates the
    `instruction_count` field — true PMU-derived count is
    memo-13 step 4 territory; the timestamp gives a useful
    diagnostic + ordering signal until that lands. Commit
    `e70bf0b6bbd5`.
- Step 4 (PMU interrupt boundary recording) deferred —
  architectural piece requiring perf-event API plumbing.
- Step 6 (MMIO recording) deferred — small but bounded; pattern
  matches step 2's record-side hook in the MMIO case of
  kvm_decode_mmio.
- `__NR_readv` per-NR routing **landed** on top of the metadata
  extension. Walks the user iovec (capped at UIO_FASTIOV=8 for
  v1), snapshots {base, len} pairs into the META_IOV metadata
  slot, and records a single concatenated payload of the
  actually-written bytes. Replay walks the same iov array and
  scatters the payload back. Iovcnts above the cap fall back
  to data-only capture.
- Additional NR special-cases (`__NR_recvmsg`, `__NR_ioctl`)
  deferred — recvmsg adds an msghdr around the iov array (and
  optional msg_control ancillary data); ioctl needs per-driver
  output-size knowledge. Architecture supports them; pattern is
  the same as readv (recvmsg) or recvfrom (ioctl-with-known-cmd).

  All hot-path ratios held in the 1.06-1.19× kvm/seccomp band
  post-hook — the static-key gate keeps the cost zero when off.
