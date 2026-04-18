# A-03: Wrap merged 6.16 seccomp work as seccomp backend

**Status:** **API wired across PTRACE_ONLY/SECCOMP_ONLY/DYNAMIC builds (2026-04-18); some ops stubbed pending C-11**

Caveats matching the ptrace status:

- `probe`, `init`, `shutdown` — stubs; the real seccomp probe still
  lives in `os-Linux/start_up.c::init_seccomp` invoked from
  `os_early_checks`.
- `read_guest_regs`, `write_guest_regs` — return `-EOPNOTSUPP`
  pending KGDB / C-11. (The seccomp backend has the underlying
  primitives — `set_stub_state`/`get_stub_state` — but plumbing them
  up to the ops contract for arbitrary tasks needs the same
  turnstile work as the ptrace impl.)

The arbiter (REVIEW.2 fix landed 2026-04-18) is now the single
source of truth for backend selection: `init_backend()` runs from
`linux_main()` immediately after `os_early_checks`, picks the
backend based on Kconfig + the host probe's `using_seccomp` result,
panics on missing HOT ops, and sets `using_seccomp` to match the
chosen backend so all downstream code sees a consistent view.
**Effort:** 4 weeks
**Dependencies:** A-01, A-02 (need ptrace as reference impl)
**Blocks:** A-05 (conformance), C profiles that prefer seccomp

## Status detail (S1)

| Sub | Outcome |
|---|---|
| S1.0 plan | Sliced into S1 (lift seccomp impls, single-backend works) and S2 (DYNAMIC arbiter) |
| S1.1 Kconfig | `CONFIG_UM_BACKEND_{PTRACE,SECCOMP,PTRACE_ONLY,SECCOMP_ONLY,DYNAMIC}` choice block; both backends `default y` |
| S1.2 scaffolding | `arch/um/backend/seccomp/{Makefile,seccomp_backend.{c,h}}` with `um_backend_seccomp_ops` singleton |
| S1.3 run_userspace | extracted `seccomp_userspace_iter` from `os-Linux/skas/process.c` → `arch/um/backend/seccomp/trap_user.c::seccomp_run_userspace` (USER TU); `userspace()` router uses `IS_ENABLED(CONFIG_UM_BACKEND_SECCOMP)` guard |
| S1.4 seccomp-specific ops | `lifecycle.c` (probe/init/shutdown stubs, like ptrace's), `mm.c` (attach + detach incl. socket close + map/unmap wrappers) |
| S1.5 shared-impl wrappers | `thread.c` (context_switch, thread_create, thread_start_idle, ipi_send), `time.c`, `debug_user.c` (read/write_guest_regs stubbed pending KGDB / C-11) |
| S1.6 wiring | all 18 seccomp ops set in `um_backend_seccomp_ops`; prototypes in `<backend.h>` |
| S1.7 boot verify | `SECCOMP_ONLY` builds (348 s cold) and boots (`um: backend = seccomp (contract v1)`); `PTRACE_ONLY` regression-tested still works |
| S1.8 tools/tracking | checkpatch on full S1 diff: **0 errors, 0 warnings**; smatch clean on new code; `init_backend()` arbiter extended to handle SECCOMP_ONLY + DYNAMIC modes (S2 will refine) |

S1 deliverable: SECCOMP_ONLY and PTRACE_ONLY both buildable+bootable
from the same source tree. The arbiter is currently Kconfig-driven;
S2 adds runtime selection via boot param + probe priority.

S2 work remaining (~2 wk):

- Real DYNAMIC build: both backends compiled in; arbiter picks based
  on boot param `backend=auto|ptrace|seccomp` and probe results.
- `force=` panic semantics (per A-01.8 README.Q2).
- Cross-backend equivalence test (workstream A-05 dependency).
- Document migration in `Documentation/virt/uml/backend-contract.rst`.

## Goal

Take Benjamin Berg's merged seccomp-mode code and wrap it as
`um_backend_seccomp` implementing the ops table. After this task,
selecting `backend=seccomp` at boot uses the seccomp trap path
instead of ptrace.

## Approach

1. Read the merged seccomp series (linux-um list, 6.16-rc1 pull).
   Map every function to an ops-table op.
2. Move seccomp-specific code from wherever it landed (likely
   mixed in `arch/um/os-Linux/`) into `arch/um/backend/seccomp/`.
3. Implement each ops-table op as a wrapper around the existing
   seccomp impl.
4. Where seccomp lacks an op the ops table requires (e.g., maybe
   some debug op), add it. This is real engineering work.
5. Pass the conformance suite (A-05).

## Deliverable

- `arch/um/backend/seccomp/` directory
- `um_backend_seccomp` ops-table instance
- Conformance suite passes
- Performance: faster syscall path than ptrace (target: 2× or
  better)

## Validation

- All A-05 conformance tests pass on seccomp backend
- Microbenchmark: `getpid()` syscall cycles ≤ 50% of ptrace
  backend
- LTP, kselftest pass identically to ptrace backend (invariant
  I4)

## Open questions

- **Q1**: Berg's seccomp work currently requires `seccomp=on` boot
  param. Do we wire it under `backend=seccomp` instead? (Plan: yes;
  deprecate `seccomp=on` after one release.)
- **Q2**: Seccomp mode has had open security review concerns.
  How do we close them out? (Plan: list them in the conformance
  suite; each becomes a CI test.)
- **Q3**: Does seccomp backend interact differently with SMP than
  ptrace does? (Plan: probably yes for IPI delivery via signals;
  per-CPU stub state may differ. Verify.)

## Risk: seccomp backend was 4 years to land

Berg's seccomp work took years of review. Wrapping it should be
much faster (the hard architectural work is done) but the security
review concerns may resurface.

**Mitigation:**
- Engage Berg directly during this task; he knows the gotchas.
- Document any behavioral difference between ptrace and seccomp
  paths in `arch/um/Documentation/backends.rst`.
