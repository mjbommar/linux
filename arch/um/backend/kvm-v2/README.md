# UML KVM backend v2

This directory holds the v2 reimplementation of the UML KVM backend.
It is currently a stub. Real implementation lands phase-by-phase per
the plan in
[`26-v2-implementation-plan.md`](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-v2-implementation-plan.md).

## Read first (in this order)

1. [Memo 24](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md)
   — strategic context: why v1 is being replaced and the 10 things a
   clean-slate would do differently.
2. [Memo 25](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/25-v2-restart-guide.md)
   — the operational playbook: mechanical restart and the 12 ARCH=um
   core refactors that must land before v2 implementation begins.
3. [Memo 26](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-v2-implementation-plan.md)
   — the v2 build itself, 10 phases A-J, ~12 weeks, ~1500 LoC target.
4. [Memo 22](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/22-dlopen-repro.md)
   — v1 bug diagnosis (especially Bug B) so v2 doesn't reintroduce it.
5. [Memo 21](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/21-tlb-shootdown-gap.md)
   — v1 failed-fix log (so v2 doesn't repeat the cross-vCPU SIGKICK
   shootdown attempt that v1 burned three rounds on).
6. [Memo 27](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/27-execution-prompt.md)
   — the per-session execution prompt for whoever picks up the work.

## v1 archive

The v1 implementation lives at
[`../kvm-v1-archive/`](../kvm-v1-archive/) — kept in-tree for
reference but not built (`CONFIG_UM_BACKEND_KVM_V1_ARCHIVE` depends
on `BROKEN`). v2 implementers should grep, `git log`, IDE-search v1
freely for "how did v1 handle this case" but should not import code
verbatim — the structural shape changes (no shadow PT, per-CPU vCPU
pool, per-mm host worker process, vmcall hypercalls) make line-level
borrow-and-paste a category error.

## Reproducers

The deterministic reproducers under
[`tools/testing/selftests/um/cpython-parity/repros/`](../../../../tools/testing/selftests/um/cpython-parity/repros/)
remain backend-agnostic. v2 must pass `single_dlopen.c × 100` cleanly
and the cpython-parity gate at 21/21 reliably.
