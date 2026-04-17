# B-06: First end-to-end demo — KCOV flippable mid-run

**Status:** planned
**Effort:** 2 weeks
**Dependencies:** B-02..B-05 (all gates and infra in place)
**Blocks:** workstream B "done" milestone; informs C-08
            (syzkaller backend)

## Goal

Demonstrate the killer property end-to-end: boot prod-with-hooks
UML, run a workload, flip KCOV on mid-flight, observe coverage
data flowing without restart.

## Approach

1. Build prod-with-hooks defconfig (uses C-01 work; OK if not
   merged yet — manually configure).
2. Boot UML.
3. Run a syscall-heavy workload (e.g., `dd if=/dev/urandom |
   wc`).
4. Open `/sys/kernel/debug/um/hooks/kcov_enabled`; verify it's 0.
5. Mmap a coverage buffer in a guest userspace process.
6. `echo 1 > /sys/kernel/debug/um/hooks/kcov_enabled`.
7. Observe coverage data appearing in the buffer.
8. `echo 0` — coverage stops, kernel keeps running.
9. Capture `perf stat` before and after — verify ~10 ns
   per-syscall added when on, ~0 when off.

## Deliverable

- `tools/testing/selftests/uml/demo/kcov-flip/` with the script
- A short demo video / asciicast for upstream advocacy
- Numbers in `06-sequencing/milestones.md` confirming B's bookend

## Validation

- Demo reproduces on three backends
- No kernel oops, no missed events, no buffer corruption when
  flipping under load

## Open questions

- **Q1**: Should this demo also exercise time-travel and
  record-replay? (Plan: no, scope creep. KCOV is the most
  syzkaller-relevant; that's enough.)

## Risk

Low; this is the validation lap.
