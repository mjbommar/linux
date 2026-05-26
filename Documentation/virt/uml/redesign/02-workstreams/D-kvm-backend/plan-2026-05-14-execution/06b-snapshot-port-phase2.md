# Snapshot v2 port — Phase 2 landed (2026-05-16)

Track B / `#168` snapshot port — Phase 2 makes the Phase 1 KUnit
case actually RUN (not skip) at boot time. Sub-sequencing source:
`02-workstreams/D-kvm-backend/26-snapshot-v2-port.md` §6 "Phase 2 —
make the KUnit case actually run."

Numbering note: `06b` rather than `06` because `06` is reserved
for the upstream Series 7 cover letter rewrite being driven by a
parallel sub-agent.

## What

Phase 1 (commit `aa4cd328102c`) landed `kvm_v2_snapshot_capture_
regs_only` + `kvm_v2_snapshot_restore_full` + `test_kvm_v2_snapshot
_basic`, but the test `kunit_skip`'d at boot because no UML task
had dispatched a vCPU through the lazy first-KVM_RUN arming
sequence (CPUID + CR4.OSXSAVE + XCR0=FP|SSE|YMM) by KUnit run-time
(`do_basic_setup()` in `init/main.c` — before `kernel_init` execs
userspace). Without that priming, `KVM_GET_XSAVE` / `KVM_SET_XCRS`
reject because guest_supported_xcr0 is empty and CR4.OSXSAVE is
clear.

Phase 2 stands up a suite_init fixture that drives the same
priming sequence explicitly, so the test runs end-to-end without
waiting for a user task to dispatch.

## What landed

  - **`arch/um/backend/kvm-v2/vcpu.c`** — new function
    `kvm_v2_vcpu_prime_for_kunit(struct kvm_v2_vcpu *v)` under
    `#if IS_ENABLED(CONFIG_UM_BACKEND_KVM_V2_KUNIT)`. Extracts the
    `if (!vcpu->cpuid_primed)` block from `kvm_v2_vcpu_run()` so
    the KUnit fixture can call it from any context (no need for a
    task with `current->mm`, no `KVM_RUN`, no `migrate_disable`).
    ~80 LoC, zero text in production builds.
  - **`arch/um/backend/kvm-v2/kvm_v2_backend.h`** — prototype for
    the new helper under the same KUnit gate. ~17 LoC.
  - **`arch/um/backend/kvm-v2/test_snapshot.c`** (NEW, ~155 LoC).
    Holds the `kvm_v2_snapshot` KUnit suite with a `.suite_init =
    kvm_v2_snapshot_suite_init` hook that primes `vcpus[0]` once
    before any case runs and stashes the pointer in a TU-local
    static for the cases to read. `test_kvm_v2_snapshot_basic`
    moved here from `test_byteshape.c`; the `kunit_skip` came out.
  - **`arch/um/backend/kvm-v2/test_byteshape.c`** — the snapshot
    case dropped (moved to `test_snapshot.c`). Byte-shape suite
    stays pure-data and runs without a vCPU. Includes trimmed
    (`<linux/kvm.h>` + `<os.h>` no longer needed).
  - **`arch/um/backend/kvm-v2/Makefile`** — `test_snapshot.o`
    under `CONFIG_UM_BACKEND_KVM_V2_KUNIT`. Same Kconfig as the
    byte-shape + marshal suites; no new symbol needed.

## KUnit verification

Build dir: `/home/mjbommar/src/uml-builds/uml-smp-t41fix`.

Boot incantation (Phase 2 acceptance gate):

```
timeout 30 .../linux backend=force=kvm-v2 mem=256M ncpus=1 \
    init=/bin/echo rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 panic=-1 </dev/null
```

Result:

```
KTAP version 1
    # Subtest: kvm_v2_marshal
    ...
ok 1 kvm_v2_marshal
    # Subtest: kvm_v2_byteshape
    ...
ok 2 kvm_v2_byteshape
    # Subtest: kvm_v2_snapshot
    ok 1 test_kvm_v2_snapshot_basic
ok 3 kvm_v2_snapshot
```

The snapshot case runs (NO `# SKIP`) and PASSES. Verbatim prime
+ capture + restore log:

```
um: kvm-v2 cpuid_install: CPUID installed (67 entries; ...)
um: kvm-v2 install_xcrs: vcpu_fd=5 set=0x7 get_rc=0 nr=1 xcr0=0x7
um: kvm-v2 kunit prime: vcpu_fd=5 primed (CPUID+OSXSAVE+XCR0)
um: kvm-v2 snapshot kunit: vcpus[0] primed (vcpu_fd=5)
    # Subtest: kvm_v2_snapshot
um: kvm-v2 snapshot: captured regs+sregs+xsave+xcrs+events+7 msrs (regs-only)
um: kvm-v2 snapshot: restored vCPU (regs-only path)
    ok 1 test_kvm_v2_snapshot_basic
ok 3 kvm_v2_snapshot
```

Boot proceeds normally through the test; `/bin/echo` runs, exits
0, kernel panics on `Attempted to kill init` (expected — `panic=-1`
on the cmdline). The pre-existing high-cr2 BUG_PR diagnostics fire
both with and without Phase 2, confirming the prime path doesn't
disturb production user dispatch.

## Lifecycle subtlety

The fixture has to short-circuit cleanly if `v->cpuid_primed` is
already set — otherwise re-running the suite (KUnit module-load
or future `debugfs/kunit/...` re-trigger) would attempt to re-
install CPUID, which `KVM_SET_CPUID2` rejects after first
`KVM_RUN` ([`kvm_vcpu_after_set_cpuid` enforces "cannot change
once vCPU has run"]).

Mitigation: `kvm_v2_vcpu_prime_for_kunit()` returns 0 immediately
when `cpuid_primed == true`. Production callers of `kvm_v2_vcpu_
run()` also short-circuit on the same flag — same idempotency
contract.

## v1 → v2 invariant the prime path documents

The lazy-first-dispatch arming sequence in `vcpu_run()` is the
ONLY production path that lands the curated CPUID + CR4.OSXSAVE
+ XCR0 mask. Snapshot, future record/replay (#169), and any
boot-time introspection that needs a "ready-to-snapshot" vCPU
state without driving guest user code through a KVM_RUN must
either (a) ride that lazy path (production task context) or (b)
call `kvm_v2_vcpu_prime_for_kunit` (test context, no
`current->mm` required).

Splitting the priming into a callable helper was a structural
clean-up Phase 2 happened to need; it's now reusable by any
Phase 3+ work that has the same "stand up a vCPU for
introspection" need.

## What Phase 2 does NOT do

  - **No full capture** — that's Phase 3 (memslot + IDT/GDT/IST
    pages). The KUnit case still uses `_capture_regs_only`.
  - **No cross-task snapshot semantics** — Phase 4 (record/replay
    deterministic-driver work) needs that; Phase 2's fixture runs
    in a single kthread context.
  - **No bench harness** — Phase 5.
  - **No selftest re-plumbing** — Phase 6.

## Acceptance for Phase 2

  - [x] suite_init fixture wired (`kvm_v2_snapshot_suite_init`).
  - [x] `kvm_v2_vcpu_prime_for_kunit` exposed under
        `CONFIG_UM_BACKEND_KVM_V2_KUNIT`.
  - [x] `test_kvm_v2_snapshot_basic` moved to `test_snapshot.c`,
        `kunit_skip` dropped, fixture vCPU pointer wired.
  - [x] Build clean under `make ARCH=um O=…`.
  - [x] Boot under `backend=force=kvm-v2 ncpus=1`: case PASSES
        (not SKIP).
  - [x] checkpatch clean (`scripts/checkpatch.pl`) on the diff —
        only MAINTAINERS heads-up for the new file (false positive
        — `arch/um/` is covered by an existing F: line).
  - [x] Diary entry (this file).

## LoC delta vs Phase 1

  - `vcpu.c`            : +84
  - `kvm_v2_backend.h`  : +17
  - `test_byteshape.c`  : -54 (test moved out, includes trimmed)
  - `test_snapshot.c`   : +155 (NEW)
  - `Makefile`          : +7

Net: +209 LoC, of which +84 lives in production text behind a
KUnit Kconfig gate and +155 is the test TU itself.

## References

  - `02-workstreams/D-kvm-backend/26-snapshot-v2-port.md` §Phase 2
  - `02-workstreams/D-kvm-backend/plan-2026-05-14-execution/
    02-snapshot-port-phase1.md` (the Phase 1 diary)
  - Phase 1 commit `aa4cd328102c`
