# Post-2026-05-19 Next Sprint — Roadmap

**Date:** 2026-05-19
**Reference cutoff:** `umlctl-deploy` @ `9ccbf5e7bb58` (post Round 14 + memo 52)
**Predecessor:** [`post-2026-05-14-next-sprint.md`](../post-2026-05-14-next-sprint.md)
**Successor of:** [`PLAN-2026-05-14.md`](../PLAN-2026-05-14.md)

## Why this sprint exists

The post-2026-05-14 sprint shipped:

1. Round 14 closure of the 5-week Django cache-flake (SMP-T73 YMM-upper
   XSAVE leak + T74/T75/T76 latent-bug follow-ups + T77 future-feature
   checklist).
2. Snapshot Phase 1–6 and record/replay Phase 1–7 ports
   (`arch/um/backend/kvm-v2/{snapshot,record}.c`, KUnit 3/3 + 7/7).
3. Memo 52 (host resource controls): SMP-T78–T84 levers covering
   pinning / KSM-disable / THP control / OOM-score-adjust / hugepages /
   CPU affinity / cgroup v2 / preflight verification, plus the umlctl
   `[host_resources]` TOML schema.
4. `umlctl mission` first-class acceptance gate
   (`tools/uml/uml-launcher/src/bin/umlctl/mission.rs`).
5. Comprehensive report + slides refresh (28-page report,
   39-slide deck).

The remaining gates from that sprint are operational (the 24-hour
long-tail soak) or external (Series 7 LKML cover-letter refresh +
submission). They run on wall-clock, not on engineering hours, so
this sprint plans the *code* work that ought to happen in parallel.

## What this sprint targets

After investigating the rest of `arch/um` and `drivers/{block,net,char}`
for under-modernised paths (sub-agent report 2026-05-19), the headline
finding is that **the recently-completed network and snapshot work
have left UML's *block* and *filesystem* paths in a much older state
than the rest of the tree**. UBD still uses a single helper thread
with synchronous `pread`/`pwrite`; hostfs writeback is one `pwrite`
per folio and resolves paths without `openat2`. These are higher-
leverage targets than another network-driver tweak.

Sequencing principle: **finish vector2 (the network workstream's
default-flip), then shift attention to block + filesystem I/O.**
Lower-priority items (console batching, virtio-rng, common I/O
completion thread, vCPU CPU pinning) are scheduled afterwards or
deferred to the sprint after this one.

## Sequenced work

| # | Memo | Priority | Effort | Depends on | Status (2026-05-19) |
|---|------|----------|--------|-----------|--------|
| 1 | [vector2 default flip + stress](01-vector2-default-flip.md) | HIGH | small flip (10 LoC) + 30/30 + 7200s soak + perf-gate work | none — start here | Steps 1 + 3 + 4a DONE; Step 2 partial (TSO patch 0.126 → 0.721 ratio, gap to gate); 4b + 5 HELD |
| 2 | [UBD io_uring port](02-ubd-io-uring.md) | HIGH | 400–700 LoC + new `os_*` shims | host kernel ≥ 5.6 | Phases 1 + 2a + 3 DONE (substrate + within-req parallel + vectored submission); 2b + 4 + 5 planned |
| 3 | [hostfs `openat2` + io_uring writeback](03-hostfs-io-uring-openat2.md) | HIGH | 200–400 LoC | host kernel ≥ 5.6; can parallel with #2 | Phases 1 + 2 + 3 DONE (openat2 strict, writepages ring, fsync ring) |
| 4 | [Time-travel ↔ record/replay wiring](04-time-travel-record-replay.md) | MEDIUM-HIGH | 200–400 LoC | record/replay Phase 1–7 (done) | Phase 1 hook DONE; Phases 2–4 deferred to a backend-neutral RR follow-on sprint |
| 5 | [Common epoll I/O completion thread](05-common-io-completion-epoll.md) | MEDIUM | 300–500 LoC | #2 and #3 | planned — both prerequisites largely landed, but consolidation can wait for the next sprint |
| 6 | [vCPU host-thread CPU pinning](06-vcpu-thread-cpu-pinning.md) | MEDIUM | 200 LoC | memo 52 (done) | design refinement needed (UML lacks dedicated vCPU threads) — planned for next sprint |
| 7 | [Console batched I/O (`writev` / `vmsplice`)](07-console-batched-io.md) | LOW-MEDIUM | ~100 LoC | none | investigated 2026-05-19, win smaller than agent estimate — held |
| 8 | [virtio-rng modernisation](08-virtio-rng.md) | LOW | 50–150 LoC | none | Phase 1 DONE (`76c428c95d8e`, `os_getrandom()` direct); Phase 2 (Rust host backend) planned |

## Sequencing rationale

- **#1 vector2 flip** is "start here" because the prior sprint
  finished the in-code work; the remaining steps are a kvm-v2 30/30
  re-run (almost certainly clean now that R14 is fixed), a natural-
  completion long soak, and bounding the guest→host TCP regression
  before flipping the umlctl default. It also unblocks the
  legacy-`vector_*` retirement clock, which simplifies the driver
  matrix for #2 / #3 / #5.
- **#2 UBD io_uring** and **#3 hostfs io_uring** are the two
  highest-leverage subsystem modernisations. They share an
  architectural pattern (per-fd io_uring submission + epoll
  harvest) so the per-system work overlaps. Either can lead;
  #5 consolidates them onto one completion thread once both land.
- **#4 time-travel** is the natural multiplier on the snapshot +
  record/replay work that just landed. Without it, record/replay
  can replay the syscall + RDTSC + SIGALRM streams but cannot
  guarantee deterministic timer interrupts.
- **#6 CPU pinning** is the deferred Tier 3 lever from memo 52 §3.2.
  It belongs in this sprint when a NUMA host appears in the bench
  matrix; otherwise defer.
- **#7 console** and **#8 virtio-rng** are cheap modernisations
  with no architectural blockers; pick them up as filler.

## Critical path

```
    vector2 flip (#1)
        |
        |  unblocks legacy-vector retirement,
        |  simplifies driver matrix
        v
    UBD io_uring (#2) ----- hostfs io_uring (#3)
        \                       /
         \                     /
          v                   v
        common epoll completion (#5)
                 |
                 v
    time-travel ↔ record/replay (#4) [can also branch off #1]
                 |
                 v
        CPU pinning (#6), console (#7), rng (#8)
```

## Out of scope (explicitly NOT in this sprint)

- **AF_XDP for vector2.** Sendmmsg + TSO offloads already cover
  ~90% of the perf gap. Multi-month diversion.
- **Further TLB / page-table-walker work.** The vCPU pool +
  `MAP_POPULATE` already captured the easy wins.
- **More THP work** beyond memo 52.
- **A fork-server / userland snapshot replay tool.** That belongs
  in a separate fuzzer / record-replay productisation sprint.
- **Tier 3 host resources (T85 SCHED_FIFO RT, T86 NUMA `mbind`,
  T87 `O_DIRECT` for hostfs).** Memo 52 §3 keeps the design;
  implementation opens only when a concrete operator question
  demands it. #6 in this roadmap is the closest follow-on; the
  others wait.

## Acceptance criteria per memo

Every sub-memo in this folder lists:

1. **Why it matters** — perf number, security gap, mainstream-
   readiness step, or correctness gap.
2. **Current state** — actual file paths + line numbers + a quote
   of the current implementation.
3. **Proposed change** — concrete API or syscall switch, with a
   sketch of the patch shape.
4. **Effort estimate** — small / medium / large with LoC range.
5. **Dependencies** — both code (other memos) and runtime (host
   kernel version, capabilities).
6. **Acceptance criteria** — at least one measurable gate per
   memo.
7. **Risk notes** — what can go wrong, what the rollback looks
   like.

## Related memos

- [memo 52](../../08-future-phases/52-uml-host-resource-controls.md):
  the predecessor planning artefact that scoped the host-resource-
  control sprint.
- [memo 50](../../08-future-phases/50-kvm-v2-django-flake-investigation-summary.md):
  the Round-14 narrative that ended the previous sprint.
- [`STATUS.md`](../../STATUS.md): live state.
- [`umlctl mission`](../../../../../../tools/uml/uml-launcher/src/bin/umlctl/mission.rs):
  the acceptance gate every memo's "acceptance criteria" section
  ultimately rolls up into.
