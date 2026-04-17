# C-09: Snapshot/forkserver host launcher

**Status:** planned
**Effort:** 6 weeks
**Dependencies:** A, B (snapshot hooks gated by static keys)
**Blocks:** fuzz profile achieving <50 ms restart

## Goal

A host-side launcher that snapshots a UML guest at a known-good
state and restores from snapshot in <50 ms. This is the fuzz
profile's killer feature — eliminates boot time as the bottleneck.

## Approach

1. **Snapshot points**: kernel emits "snapshot ready" at well-
   defined moments (post-userspace-init, post-test-setup). Trigger
   via mconsole or static-key gate.
2. **State capture**:
   - Backend ops table provides `read_guest_regs`,
     `read_guest_memory` (uses existing process memory + stub state).
   - Snapshot file format: header + memory regions + register
     state + open FDs + filesystem state (or hostfs assumed
     stable).
3. **Restore**: re-fork the UML process, mmap memory regions,
   restore registers, resume.
4. **Forkserver model**: the UML kernel runs once to ready
   point; forks per fuzz iteration; child exits after its
   testcase; parent maintains pristine state.

This is conceptually similar to AFL's forkserver but operating
on the UML kernel rather than a userspace target.

## Deliverable

- `tools/uml/snapshot/` — host-side tools
  - `uml-snapshot save <pid> <file>`
  - `uml-snapshot restore <file>`
  - `uml-forkserver <kernel> <ready-point>` — long-running parent
- `arch/um/kernel/snapshot.c` — kernel-side hooks
- Documentation

## Validation

- Snapshot+restore <50 ms on a typical fuzz workload
- syzkaller using forkserver achieves >1000 iter/s
- Snapshot file is portable across runs (deterministic state)

## Open questions

- **Q1**: How do we handle host-side state (open files, sockets)?
  (Plan: snapshot includes open FDs and their content; sockets
  are harder. Document limitations — testcases that open
  external sockets aren't snapshottable.)
- **Q2**: Does snapshot work across backends? (Plan: per
  invariant I9 — yes, within a backend; cross-backend snapshot
  is not promised.)
- **Q3**: How does this interact with KASAN's shadow region?
  (Plan: snapshot includes shadow. Larger files; acceptable.)

## Risk

CRIU-style state capture is notoriously hard. Snapshot/restore
is a separate engineering project that AFL's forkserver
sidesteps by being simpler.

**Mitigation:**
- Start with the forkserver model (simpler than full
  snapshot/restore).
- Full snapshot/restore as a v2 if forkserver isn't enough.
