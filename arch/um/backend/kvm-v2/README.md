# UML KVM backend v2

User-Mode Linux's KVM backend reimplemented from scratch. Replaces v1
(archived at `arch/um/backend/kvm-v1-archive/`) which suffered from
shadow-PT-induced corruption that ~6 weeks of fixes failed to root-
cause.

## Design at a glance

| Aspect              | v2                                     | v1 (archived)                  |
|---------------------|----------------------------------------|--------------------------------|
| Address translation | TDP / EPT (KVM mmu_notifier)           | Custom shadow page tables      |
| vCPU lifecycle      | Per-host-CPU pool                      | Per-task vCPU                  |
| Syscall path        | `out %al, $0xf4` → `KVM_EXIT_IO`       | LSTAR trampoline + gadgets     |
| Exception delivery  | IDT in PML4[508] kernel-half           | IDT/GDT/TSS in PML4[0] (Bug A) |
| Worker model        | Per-mm host worker (memo 25 R4)        | Stub child + ptrace            |
| First entry         | Direct CPL=3 SREGS install             | Bootstrap IRETQ stub           |
| Cross-vCPU TLB      | KVM mmu_notifier (free)                | Custom SIGKICK (broken)        |
| Bootstrap pages     | None                                   | IDT/GDT/TSS/LSTAR/IST + gadget |

Net effect: v2 is ~1500 LoC of new code that structurally eliminates
v1's bug families. See memo 24 §"Risk classes that v2 ELIMINATES".

## Current status (2026-04-30)

| Phase | Status                                     |
|-------|--------------------------------------------|
| A — KVM context + ops registration         | done |
| B — TDP + memslots                         | done |
| C — Per-CPU vCPU pool + sync-regs          | done |
| D — IO-port syscall trap                   | done |
| E — Exception handling (IDT/TSS, no boot pages) | done |
| F — Signal/preemption                      | done |
| G — SMP                                    | open |
| H — Performance                            | mostly done (2× faster than seccomp on minimal Python startup) |
| I — Polish (KUnit, ftrace, docs, EXPERT lift) | in progress |
| J — Validation (24h, Tier 1/2/3, soak)     | open |

**Substrate gate**: `PASS=25 / FAIL=3 / EXPECTED_FAIL=3` under
`backend=force=kvm-v2` — bit-for-bit match with seccomp baseline.
Stable across 3 consecutive runs.

**Performance**: ~2× faster than seccomp on `python3 -c "import math;
print('done')"` minimal startup (40 ms vs 90 ms median, sample n=7,
bit-identical). The advantage comes from KVM-direct syscall path
(one `KVM_EXIT_IO` per syscall) versus seccomp's stub-child + ptrace
round-trip overhead.

**Known residual**: `InterpreterPoolExecutor` (5 worker threads each
running its own PEP-684 subinterpreter) flakes ~50% under v2 with
SIGSEGV at near-NULL writes or glibc heap corruption. 100% PASS
under seccomp. Single-threaded subinterpreters and multi-threaded
single-interpreter workloads pass cleanly. Deferred to a focused
investigation session — see memo 26 §H.1b.

## File layout

| File              | What's in it                                    |
|-------------------|-------------------------------------------------|
| `init.c`          | Backend probe (`/dev/kvm` capability), ops registration |
| `context.c`       | Per-VM context lifecycle (`kvm_v2_vm_create/destroy`)   |
| `vcpu.c`          | Per-host-CPU vCPU pool, dispatch (`kvm_v2_vcpu_run`), FPU capture/install, context-switch hook |
| `region.c`        | Memslot allocator (single slot covers `uml_physmem`)    |
| `memslot.c`       | `KVM_SET_USER_MEMORY_REGION` wiring                     |
| `exception.c`     | IDT/GDT/TSS layout, handler stubs, populate-then-install |
| `syscall_trap.c`  | LSTAR trampoline + IO-trap dispatcher (syscall + #PF + #GP + #UD + #DE + #OF) |
| `kvm_v2_backend.h`| Internal types + helper declarations                    |
| `ops.c`           | Backend ops table — `.vcpu_run = kvm_v2_vcpu_run` post-D.5 |
| `snapshot.c`      | Snapshot / forkserver primitives (Time-machine #168 Phase 1) — capture/restore vCPU scalar state via KVM_GET/SET_{REGS,SREGS,XSAVE,XCRS,VCPU_EVENTS,MSRS} |
| `test_marshal.c`  | KUnit suite for marshal-shape correctness               |

## Read first (in order)

1. [Memo 24](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md)
   — strategic context: why v1 is being replaced and the 10 things a
   clean-slate would do differently.
2. [Memo 25](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/25-v2-restart-guide.md)
   — the operational playbook: mechanical restart and the 12 ARCH=um
   core refactors that must land before v2 implementation begins.
3. [Memo 26](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-v2-implementation-plan.md)
   — the v2 build itself, 10 phases A-J, ~12 weeks, ~1500 LoC target.
   Has the most up-to-date "what's done / what's next" tracking.
4. [Memo 22](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/22-dlopen-repro.md)
   — v1 bug diagnosis (especially Bug B) so v2 doesn't reintroduce it.
5. [Memo 21](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/21-tlb-shootdown-gap.md)
   — v1 failed-fix log (so v2 doesn't repeat the cross-vCPU SIGKICK
   shootdown attempt that v1 burned three rounds on).
6. [Memo 27](../../../../Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/27-execution-prompt.md)
   — the per-session execution prompt for whoever picks up the work.

## Build and run

```bash
# defconfig already enables UM_BACKEND_KVM_V2 alongside SECCOMP via DYNAMIC
make ARCH=um O=$HOME/src/uml-builds/uml-clean -j$(nproc)

# Force v2 (default is seccomp)
$HOME/src/uml-builds/uml-clean/linux backend=force=kvm-v2 mem=256M \
    rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 init=/bin/true

# Substrate gate (every reproducer that seccomp PASSes)
UML_BINARY=$HOME/src/uml-builds/uml-clean/linux \
  BACKEND=kvm-v2 TIMEOUT=180 \
  bash tools/testing/selftests/um/regrtest-repros/run-regrtest-repros.sh
# Expect: PASS=25 / FAIL=3 / EXPECTED_FAIL=3 (matching seccomp)

# Cpython tier-0 (hashlib + numerics + containers + regex + strings + typing)
UML_BINARY=$HOME/src/uml-builds/uml-clean/linux \
  CPYTHON_TIER0_BACKEND=kvm-v2 \
  bash tools/testing/selftests/um/cpython-tier0/run-cpython-tier0.sh
# Expect: TOTAL PASS, sha256 matches reference

# Cpython parity (21 modules; current v2 best-trial is 18/21 PARITY)
UML_BINARY=$HOME/src/uml-builds/uml-clean/linux \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh
```

## v1 archive

The v1 implementation lives at
[`../kvm-v1-archive/`](../kvm-v1-archive/) — kept in-tree for
reference but not built (`CONFIG_UM_BACKEND_KVM_V1_ARCHIVE` depends
on `BROKEN`). v2 implementers should grep, `git log`, IDE-search v1
freely for "how did v1 handle this case" but should not import code
verbatim — the structural shape changes (no shadow PT, per-CPU vCPU
pool, per-mm host worker process, IO-port hypercalls) make
line-level borrow-and-paste a category error.

## Reproducers

The deterministic reproducers under
[`tools/testing/selftests/um/cpython-parity/repros/`](../../../../tools/testing/selftests/um/cpython-parity/repros/)
and
[`tools/testing/selftests/um/regrtest-repros/`](../../../../tools/testing/selftests/um/regrtest-repros/)
are backend-agnostic. v2 passes the substrate gate at full seccomp
parity; the cpython-parity gate has the InterpreterPool residual
documented above.

## Tracing

```bash
trace-cmd record -e 'um_backend_kvm_v2:*' \
  $HOME/src/uml-builds/uml-clean/linux backend=force=kvm-v2 ...
```

Tracepoints in `arch/um/include/trace/events/um_backend.h`:
- `um_backend_kvm_v2_vcpu_enter` / `_exit` / `_eintr` — KVM_RUN cycle
- `um_backend_kvm_v2_iotrap_*` — port + decoded operand per exit
- `um_backend_kvm_v2_fpu_capture` — per fork / context-switch FPU snapshot
