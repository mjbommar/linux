# Historical Branch Disposition — 2026-06-13

Closes the "Required Historical Branch Disposition" item from
`2026-06-11-next-completion-execution-plan.md`. Each historical branch is
source material and a regression reference, never an integration base. The
question answered here is narrow: **does any intended UML v2 functionality,
selftest, or fix live ONLY on a historical branch and is missing from `next`?**

Method: for each branch, `git log --oneline next..<branch>`,
`git diff --stat next...<branch>`, and content spot-checks with `git show` /
`git grep`. The branches form a single descent chain off merge-base `ba314ed1`
(each a superset of the previous), so the large three-dot diffs are mostly
upstream churn `next` already carries via the torvalds/master merges; verdicts
are based on actual file/symbol presence on `next`, not commit-message presence.

| Branch | Unique headline | Verdict |
| --- | --- | --- |
| `kvm-v2-snapshot-elf64` | kvm-v2 snapshot ELF64 ET_CORE export; kvm-v1 archive; diag rounds | ALREADY-ON-NEXT (`arch/um/backend/kvm-v2/snapshot_elf.c`); v1 archive + diag tooling RETIRE |
| `fork-server-phase1c` | `umlctl pool serve` daemon; template-pause fork hook | ALREADY-ON-NEXT (`pool_serve.rs`, `pool_client.rs`, `template_pause.c`) |
| `memo09-phase2` | in-kernel template-pause identity apply | ALREADY-ON-NEXT (`template_pause_identity.c` + `_test.c`) |
| `memo09-phase3-pool-bench` | pool-bench acceptance gates; LTP suite lists | ALREADY-ON-NEXT (`selftests/um/pool-bench/`, `soak/ltp-*`) |
| `memo09-phase4` | pool exec/port-forward verbs; syzkaller `vm/uml` shim | ALREADY-ON-NEXT (`pool-exec-smoke`, `pool-port-forward-smoke`, `syzkaller-vm-shim/uml.go`) |
| `experiment-path-c` | kvm-v2 utime accounting; per-member mconsole/TAP; path pivot tests | ALREADY-ON-NEXT (accounting in `vcpu.c`, identity swap in `template_pause_identity.c`); pivot exploration docs RETIRE |
| `umlctl-deploy` | cpython postmortem fixes incl. `_Py_Dealloc` trap.c intercept | RETIRE the intercept (see below); all other fixes ALREADY-ON-NEXT |

## The one substantive finding: `_Py_Dealloc` trap.c intercept — RETIRED

`arch/um/kernel/trap.c::intercept_cpython_dealloc_tstate_null()` (commits
`40203d69dff1` / `525e0ebce4cd`) exists only on `umlctl-deploy`;
`git merge-base --is-ancestor 40203d69dff1 next` is false and `next`'s
`trap.c` (462 lines) has no intercept code.

**Disposition: retire, do not port.** The intercept is a CPython-3.14-specific,
opcode-byte-matching SIGSEGV interceptor that converts the spawn-worker fault
into `do_exit(1)`. Per the diagnosis preserved in session memory, the fault is
an upstream CPython free-threading teardown ordering bug (the per-thread
`tstate` is cleared before the `stderrprinter` dealloc runs), **not** a UML bug.
A hardcoded version-specific opcode match in `arch/um/kernel/trap.c` is exactly
the kind of research band-aid the completion standard keeps out of
upstream-facing source.

**User-facing surface that remains:** none. On `next` the spawn race is handled
as a documented expected failure in
`tools/testing/selftests/um/cpython-full/expected_failures.txt`. The archived
`2026-05-24-cpython-full-suite-postmortem.md` carries a banner clarifying that
its "shipped kernel workaround" describes the `umlctl-deploy` experiment, not
`next`.

**Future work to revive (if ever wanted):** an upstream CPython teardown fix
obsoletes the need entirely; absent that, a *generic* (not opcode-matched)
post-clear NULL-tstate fault policy with a Kconfig/static-key gate and a smoke
test would be the upstream-appropriate shape. Tracked under W1 (KVM v2 / cpython
workload breadth), not as a completion blocker.

## Net

Six of seven branches are fully ALREADY-ON-NEXT for functional and selftest
content; their genuinely-unique files are archive, one-off diagnostics, or
superseded exploration docs (RETIRE). The single code-level gap — the cpython
trap.c intercept — is deliberately retired rather than ported. No PORT actions
are required for the completion claim.
