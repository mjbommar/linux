# 09 — Fork-server snapshot restore (Firecracker-class fast spawn)

**Sprint:** post-2026-05-19 (next-next)
**Priority:** HIGH (strategic)
**Effort:** large (~600–1000 LoC across umlctl + arch/um/backend/kvm-v2)
**Status:** designed 2026-05-19, awaiting implementation
**Depends on:** memo 26 (snapshot capture/restore — DONE),
fast_boot knob (`e4ab828f1555` — DONE)

## Why this matters

Firecracker's headline metric is **125 ms from `--snapshot` to
guest userspace**.  Our current cold boot is ~207 ms (fast_boot
mode); a snapshot-restore path could collapse that to **~100 µs**
because we'd skip:

  * UML host-side preflight (~36 ms)
  * Kernel `start_kernel()` (~100 ms)
  * Driver `subsys_initcall` work (~70 ms)
  * VFS pivot + initramfs unpack (~varies)

UML's snapshot infrastructure (memo 26) already covers the
per-task / per-vCPU / per-mm state in **35 µs capture / 13 µs
restore**.  What's missing is the **fork-server**: a long-lived
UML supervisor process that holds the post-boot state and clones
new instances from it on demand.

## Current state

| Piece | Path | State |
|---|---|---|
| Snapshot capture (regs + vCPU + task) | `arch/um/backend/kvm-v2/snapshot.c::kvm_v2_snapshot_capture_full` | DONE (memo 26) |
| Snapshot restore | `arch/um/backend/kvm-v2/snapshot.c::kvm_v2_snapshot_restore_full*` | DONE |
| Snapshot KUnit | `test_snapshot.c` | DONE — 35µs capture / 13µs restore bench |
| **Save to file** | — | MISSING |
| **Load from file at boot** | — | MISSING |
| **Fork-server supervisor** | — | MISSING |
| **umlctl pool / restore CLI** | — | MISSING |

## Proposed change — three-phase build

### Phase 1 — Snapshot file format + save/load

```rust
// New: tools/uml/uml-launcher/src/snap.rs
struct UmlSnapshot {
    magic: [u8; 4],             // "UMLS"
    version: u32,                // 1
    kernel_sha256: [u8; 32],     // must match at restore
    physmem_size: u64,
    physmem_data: Vec<u8>,       // CoW-able with mmap MAP_PRIVATE + COW
    vcpu_state: Vec<u8>,         // serialized kvm_v2_snapshot
    task_state: Vec<u8>,
    timestamp_ns: u64,
}
```

  * **Save**: at the post-boot "ready" moment (init phase
    completes, or operator-triggered via debugfs), pause the
    guest, capture state via existing `kvm_v2_snapshot_capture_*`,
    serialize physmem to a file.  Physmem dump is the long pole
    (~512 MiB at ~5 GB/s = 100 ms).
  * **Load**: at supervisor startup, mmap the file's physmem
    section as `MAP_PRIVATE | MAP_POPULATE`, deserialize vCPU /
    task state, restore via existing `restore_full_vcpu`.

### Phase 2 — Fork-server supervisor

```
umlctl pool create <Umlfile> [--size N] [--snapshot OUT.snap]
    Pre-spawn N instances all booted to the same Umlfile's init.
    Each one captures a snapshot at "ready" (or all share one
    snapshot file from a master boot).

umlctl pool take <pool> --as <name>
    Restore a fresh instance from the pool's snapshot.  Skips
    cold-boot entirely; resumes the captured state under a new
    name.  Target: <100 µs setup, <1 ms total to "guest userspace
    responding."

umlctl pool replenish <pool>
    Add more pre-warmed instances when the pool drains below
    threshold.
```

The fork-server is a long-lived host process that:

  1. mmaps the snapshot file's physmem section MAP_PRIVATE.
  2. fork()s on each `pool take` request — child inherits the
     physmem via COW (host pagetable shared until first write).
  3. Child remaps shared regions, sets up a new tap/seccomp
     context, then resumes the guest via `kvm_v2_snapshot_
     restore_full`.

### Phase 3 — CoW physmem optimization

For the headline 100µs number, physmem must NOT be copied at
fork time.  Linux's CoW gives that for free: parent + child share
the same physmem pages until one writes.  Most "first request"
guests touch <1 MiB of pages, so the per-fork memory + time cost
is bounded.

  * Use `MADV_DONTFORK` on regions we explicitly want copied
    (vCPU mmap'd KVM_RUN buffer).
  * Use `MAP_SHARED` for read-only kernel text / data.
  * `MAP_PRIVATE` for guest physmem (CoW on write).

## Effort breakdown

  * Phase 1 (snapshot file format + serializer): ~200 LoC kernel
    + ~150 LoC Rust supervisor.
  * Phase 2 (umlctl pool / take / replenish): ~300 LoC Rust
    across deploy.rs + new pool.rs module.
  * Phase 3 (CoW physmem tuning): ~50 LoC kernel
    (`MADV_DONTFORK` placement) + careful seccomp filter audit.

Total: ~700 LoC.

## Acceptance criteria

  * **Functional gate**: `umlctl pool take` produces a running
    instance that responds to a sentinel ping (e.g., curl
    against an in-guest HTTP server) within 1 ms wall clock.
  * **Headline metric**: median `take` latency ≤ 200 µs over
    1000 takes.
  * **Memory amplification**: 1000 forks of a 64 MiB guest
    consume ≤ 100 MiB total host memory (CoW sharing of 99 %+ of
    physmem).

## Cross-references

  * memo 26: snapshot capture/restore — the substrate this
    builds on.
  * `b305e1ae5516`: A/B knob shape that the pool feature
    should mirror (one binary, runtime opt-in).
  * Firecracker's `LoadSnapshot` API:
    https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md
