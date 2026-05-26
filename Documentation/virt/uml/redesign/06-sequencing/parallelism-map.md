# Parallelism map

What can run concurrently. Dictates team size sweet spots.

## Team-size sweet spots

| Team | Wall-clock | Engineers idle? |
|---|---|---|
| 1 EM (just one) | ~24 mo | n/a |
| 2 EM | ~14 mo | minimal |
| 3 EM | ~9 mo | minimal |
| 4 EM | ~7 mo | KVM eng partly idle in early Q1 |
| 5 EM | ~7 mo | additional eng waits for parallelizable work |
| 6+ EM | ~7 mo | diminishing returns; coord overhead grows |

The critical path is 7 months (see `critical-path.md`).
Beyond 4 engineers, returns diminish.

## Parallelism by phase

### Phase 1 (months 1-3): foundation

Critical: A-01 → A-02 → A-04 → C-01

Parallel:
- A-05 (conformance skeleton) once A-01 done (week 2)
- A-06 (docs) once A-01 done
- B-01 (audit) once A-01 done
- D-01 (KVM design) can start any time after A-01

**Sweet spot**: 3 engineers (A driver, parallel helper, B-or-D).

### Phase 2 (months 4-6): backend complete + first gates

Critical: A-02 finish → A-03 → B-02 → B-04

Parallel:
- A-07 (perf CI)
- B-03 (debugfs)
- C-01 wraps up
- D-02 (KVM trap path) can run independently

**Sweet spot**: 4 engineers (A driver wraps refactor, B driver,
C profile lead, D engineer).

### Phase 3 (months 7-9): observability + first user

Critical: B-04 → B-06 (demo)

Parallel:
- All of C-02..C-07 (sanitizer + tracer ports) can run in
  parallel
- C-08 (syzkaller backend) starts
- D-03 (page tables) ongoing
- C-10 (launcher) starts

**Sweet spot**: 5-6 engineers. The widest workstream.

### Phase 4 (months 10-12): research profile ships

Critical: C-04 + C-05 → research profile usable

Parallel:
- All of D-* ongoing
- C-09 (snapshot) starts
- C-06 (BPF JIT)

**Sweet spot**: 4-5 engineers.

### Phase 5 (months 13-15): fuzz profile ships

Critical: C-09 → fuzz profile

Parallel:
- C-03 (KCSAN)
- C-06 (BPF JIT)
- D-* ongoing

**Sweet spot**: 4 engineers.

### Phase 6 (months 16-18): KVM convergence

Critical: D-02 → D-03 → D-04

Parallel:
- C-07 (KMSAN) start
- C-10 (launcher) finish

**Sweet spot**: 3-4 engineers.

### Phase 7 (months 19-24): polish + ship

Critical: D-06 (KVM bookend)

Parallel:
- C-07 (KMSAN) finish
- All-profile validation
- Documentation
- Upstream advocacy

**Sweet spot**: 2-3 engineers (most are wrapping up).

## Single-engineer scaling

If only one engineer:

- 1-3: A only (foundation)
- 4-6: A finishing, start B
- 7-9: B finishing, start C-01..C-02
- 10-12: more C ports
- 13-18: C-08, C-09, ramp D-01
- 19-24: D-02..D-06, KMSAN, polish

24-month plan still works at 1 EM/month, just slower. Critical
path doesn't change because there's no parallel work to skip.

## When to add a 5th engineer

Phase 3 (months 7-9). The C workstream's 8+ port tasks
parallelize freely. Above 5 engineers, you run out of
parallel work; adding a 6th just helps with coord and
docs.

## When to add a specialized hire

- **Hypervisor expert**: month 12 (D-01 design starts).
- **syzkaller specialist**: month 9 (C-08 starts).
- **Security/sandboxing expert**: month 13 (C-10 starts).
- **Performance engineer**: month 4 (B-01 audit benefits from
  one full-time).

Outside these, generalist kernel engineers are fine for any task.

## Coordination overhead by team size

- 2 engineers: weekly sync (30 min)
- 3-4 engineers: weekly sync (60 min) + decisions log
- 5-6 engineers: weekly sync (60 min) + workstream-specific
  syncs (30 min/week per workstream)
- 7+ engineers: as above + a coord lead role half-time

The plan tops out around 6 EM/month total throughput. Beyond
that, you're paying more in coord than in delivery.
