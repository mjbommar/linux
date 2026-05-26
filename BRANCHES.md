# mjbommar/linux — UML Development Tree

Maintainer tree for User-Mode Linux (UML) development.  Tracks
`torvalds/master` with UML-specific patch series queued for upstream
submission.

## Branches

### `master`

Tracks `torvalds/master` exactly.  Never diverges.  Updated after
each -rc release or Linus merge window close.

**Current base:** v7.1-rc5+ (`e8c2f9fdadee`, May 2026)

**Use this branch to:** verify that upstream builds/boots without
our patches, or as a clean base for bisection.

---

### `next`

Development branch.  Contains all UML patch series queued for the
next merge window, committed in submission order on top of `master`.

**Current state:** 7 series-level commits on v7.1-rc5+

| # | Commit | Series | Scope |
|---|--------|--------|-------|
| 1 | `c1084ea` | bpf-hygiene | BPF JIT stub + x86 hygiene |
| 2 | `be9dbd1` | kmsan-arch-callback | KMSAN early-shadow hook for UML physmem |
| 3 | `8301623` | ftrace-notrace | notrace on kthread/smpboot signal-adjacent fns |
| 4 | `86b74b8` | backend-ops-abstraction | `struct um_backend_ops`, SMP, TLB sync, PTE rework, hostfs io_uring, snapshot infra |
| 5 | `12b19d0` | kvm-v2-backend | Full KVM-based execution backend (EPT/VT-x) |
| 6 | `dba5784` | umlctl + selftests | Rust launcher tool + comprehensive kselftests |
| 7 | `4bfa5d7` | documentation | Operator docs, redesign plans, submission queue |

**Build/boot verified:** seccomp and kvm-v2 backends both compile
clean and boot on this branch.

**Use this branch to:** build and test the latest UML with all
features, or as the base for `git format-patch` when preparing
LKML submissions.

---

### `fixes`

Bugfix branch for the current -rc cycle.  Based on the latest
release candidate tag.  Only contains fixes for code already in
Linus's tree.

**Current base:** `v7.1-rc5`

**Currently empty** (no UML-specific fixes needed for v7.1-rc5).

**Use this branch to:** cherry-pick urgent fixes that need to land
before the next merge window.

---

### `umlctl-deploy`

The original development branch.  1,236 commits of iterative
development including experimental work, diagnostic commits,
hypothesis-testing, and the full investigation archaeology.

**Preserved as-is** via tag `umlctl-deploy-pre-rebase-20260526`.
This branch is NOT rebased and will NOT be force-pushed.  It
serves as the permanent record of how the code was developed.

**Use this branch to:** trace the history of any specific fix or
feature back to its original investigation, or to recover
experimental code that was excluded from the upstream series.

---

## Upstream Submission Plan

Patches are sent to `linux-um@lists.infradead.org` from the `next`
branch, in the order documented in
`Documentation/virt/uml/redesign/upstream-patches/SUBMISSION-QUEUE.md`.

Series 1-3 are small, independent, and ready to send.  Series 4
(backend ops abstraction) is the tentpole RFC.  Series 5-7 depend
on Series 4 landing.

See `Documentation/virt/uml/redesign/upstream-patches/REBASE-PLAN-20260526.md`
for the full rebase plan and execution log.

## Related Repositories

- **CPython fork:** [mjbommar/cpython](https://github.com/mjbommar/cpython)
  Branch `fix/stdprinter-immortal` — fixes a `Py_FinalizeEx`
  teardown crash exposed by UML's slower execution timing.
  PR: [mjbommar/cpython#2](https://github.com/mjbommar/cpython/pull/2)

## Contact

Michael Bommarito <michael.bommarito@gmail.com>
