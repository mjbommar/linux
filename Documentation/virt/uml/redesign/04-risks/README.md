# Risks

What could derail the plan, ranked by likelihood × impact.

## The risk register

| # | Risk | Likelihood | Impact | Doc |
|---|---|---|---|---|
| R1 | LKML rejects the architecture (Tazaki redux) | Medium | High | [political-lkml-acceptance.md](political-lkml-acceptance.md) |
| R2 | RWX `.text` patches conflict with KVM RO mappings | Low | High | [technical-rwx-vs-readonly.md](technical-rwx-vs-readonly.md) |
| R3 | Adding hooks regresses prod-fast | Medium | Critical | [perf-regressions.md](perf-regressions.md) |
| R4 | CI resources can't run all profiles × backends × tests | Medium | Medium | [ci-resource-cost.md](ci-resource-cost.md) |
| R5 | KMSAN port is intractable | Medium | Medium | (covered in C-07) |
| R6 | Single-engineer bus factor on a workstream | High | Medium | (covered in 02-workstreams/README) |
| R7 | KVM backend doesn't reach <100 ns | Medium | Low | (covered in D-06) |
| R8 | Snapshot/forkserver harder than CRIU-lite | Medium | Medium | (covered in C-09) |

## The decisions log

Every architectural choice with rationale, date, and trigger
for revisitation:

[decisions-log.md](decisions-log.md)

## Risk classes by mitigation strategy

### Avoidable through engineering

- R2 (section split): standard kernel pattern; copy x86_64.
- R3 (perf regression): CI gate; invariant I2 enforcement.
- R7 (KVM perf): acceptable fallback to seccomp.

### Avoidable through process

- R6 (bus factor): document everything; write ADRs in
  decisions-log.md so a successor can pick up.
- R4 (CI cost): tier the test suite; full matrix nightly,
  smoke on every commit.

### Avoidable only through political work

- R1 (LKML acceptance): incremental landing strategy; engage
  maintainers throughout; learn from Tazaki's failed v8.

### Acceptable if it materializes

- R5 (KMSAN intractable): ship without; not all profiles need
  it.
- R8 (snapshot complexity): forkserver alone is the v1; full
  CRIU-style snapshot is v2 if needed.
