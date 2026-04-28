# UML Redesign — Status Tracker

Last updated: 2026-04-28

This document is the single source of truth for "where are we, what's
broken, what's next." Updated whenever priorities or blockers change.

If something contradicts a memo in `02-workstreams/` or
`04-risks/decisions-log.md`, this file wins until the underlying memo
catches up.

---

## v1 ARCHIVED (2026-04-28): KVM backend restart underway

Per memos 24-27, the v1 KVM backend reached **mean 19.4/21** on the
cpython-parity gate post-A.4i but Bug B (just-past-physmem stale RIP,
memo 22 §"Update — Bug B is NOT a use-after-munmap") and the
structural fragility of shadow PT (memo 24's 10 clean-slate items)
made further incremental fixes unproductive. Memo 25 Part 1 lands
the mechanical restart:

- A.4i committed (`cbc6d9fbffbd`); 5 C reproducers committed
  (`ed4158203fd2`); investigation memos 21-27 committed
  (`8c2221d29711`).
- v1 source moved to `arch/um/backend/kvm-v1-archive/`
  (`17a3e87bab75`); kept in-tree as v2-implementer reference, not
  built (`CONFIG_UM_BACKEND_KVM_V1_ARCHIVE depends on BROKEN`).
- v1's hooks stripped from ARCH=um core (`b19444243944`):
  pgtable.h's `kvm_shadow_sync_pte`, tlbflush.h's
  `kvm_shadow_sync_va/range_atomic`, the `arch_thread.kvm` fpu/vcpu
  fields, mm_id's `kvm_shadow` pointer, the kick-signal handler,
  and the ops-table declarations all gone. Build verified
  seccomp-only on x86_64 defconfig.
- v2 stub directory at `arch/um/backend/kvm-v2/`
  (`046375fd50ed`) — Kconfig + Makefile + README + arch_initcall
  banner. Not yet plumbed into dispatch.
- Memo 25 R10 — `arch/um/backend/kvm-v1-archive/harness.c`
  deleted (`928bbbfebcb8`). 1526 LoC of D-04b.1b bring-up
  diagnostic, dead since Step 3 stripped its Kconfig gate.
- Memo 25 R11 — ptrace backend removed entirely
  (`06c88545ae2c`, Option B). `arch/um/backend/ptrace/` gone
  (~600 LoC); skas/process.c and skas/mem.c stripped of their
  ptrace branches; `wait_stub_done`, `check_ptrace`,
  `check_sysemu`, `start_ptraced_child`, `stop_ptraced_child`,
  `ptrace_child` deleted from os-Linux/start_up.c. Net ~1000
  LoC deletion. `embedded.config` redirected to SECCOMP_ONLY.
  Hosts genuinely without seccomp-filter pin to v6.16 or earlier.

- Memo 25 R1 — host-VA / kernel-VA Kconfig-gated abstraction
  (`3086bf6d8bd8`). Per user direction, R1 lands as a rename
  rather than a runtime relocation. Today both anchors equal
  `__binary_start & PAGE_MASK`; v2's Phase B Kconfig gate flips
  `uml_physmem` to a high constant (PML4[256+]) for the guest pgd
  while `__binary_start_hva` stays low. 5 host-VA-meaning sites
  audited (main.c kfree-vs-vfree, virtio_uml/vfio_user offsets,
  mem.h header).
- Memo 25 R2 — backend ops abstraction cleanup (`764eac6d79de`).
  `run_userspace` → `vcpu_run`; `mm_attach`/`mm_detach` →
  `mm_create`/`mm_destroy` taking `struct mm_struct *`;
  `mm_map`/`mm_unmap` → `mm_region_added`/`mm_region_removed`;
  new optional `mm_region_protected`. Keystone — unblocks
  R3/R5/R7/R8/R9.
- Memo 25 R3 — TLB-sync decoupled from backends (`3aa0af5d04e6`).
  Documentation-only: the substantive strip happened in Step 3 +
  R2; this commit formalizes the contract in tlbflush.h.
- Memo 25 R5 — generic `struct um_memory_region` (`d3b2035f4079`).
  All region ops take `const struct um_memory_region *`; backends
  may stash per-region state in `region->backend_data` (today
  reserved for v2's memslot ID).
- Memo 25 R7 — backend tracepoint subsystem (`1a5dc6d061fa`).
  `um_backend:{mm_create,mm_destroy,mm_region_added,mm_region_removed}`
  ftrace events, free under CONFIG_TRACING=n, full visibility under
  CONFIG_FUNCTION_TRACER=y. New header
  `arch/um/include/asm/trace/um_backend.h` + new TU
  `arch/um/kernel/trace.c`.
- Memo 25 R8 — um_tlb_sync as the post-R5 generic drainer
  (`72a35bbe7b80`). Documentation: contract in tlb.c header
  formalizes "no backend-specific knowledge in mm-arbiter; backends
  see struct um_memory_region only".
- Memo 25 R9 — syscall-class table deferred to v2 Phase D
  (`9dc0d005feb0`). Premise was "handle_syscall is monolithic";
  post-archive that's no longer true. The class infrastructure
  lands with its first consumer (vmcall hypercall path) rather
  than as dead pre-v2 scaffolding.
- Memo 25 R12 — documentation refresh (this commit). backend-
  contract.rst rewritten for the post-R2/R5 ops; backends.rst
  ptrace section archived; ftrace.rst's "UML backend tracepoints"
  section lives. MAINTAINERS already covers arch/um/ broadly.

- Memo 28 — R4 design lock (`654d8581fb56`, `c2cf8a99ec25`).
  Captures the surface-mapping subagent's findings, locks four
  design decisions (UNIX socket IPC; SIGCHLD reaping; SIGUSR2
  cross-mm migration; one-thread-per-worker dispatcher pool),
  defines the 128-byte IPC wire format, and lays out the 6-commit
  E.1-E.6 implementation sequence. Part I.5 added 2026-04-28
  evening locks the kernel-state-ownership question:
  spawner-owns-everything (gVisor sentry pattern), worker is a
  thin trap relay with no task_struct / mm_struct of its own.
- R4 E.1 — scaffolding (`4eb34edab3f7`). New
  `arch/um/include/shared/worker_api.h` (spawner ↔ worker API)
  and `arch/um/backend/seccomp/worker_ipc.h` (128-byte wire
  format). New `CONFIG_UM_WORKER_PROCESS` Kconfig toggle (default
  n). `struct um_worker *worker` field in mm_context (always
  present; NULL today). Stub static-inlines for =n; runtime
  no-op for =y until E.3 spawns workers.
- R4 E.2 — spawner skeleton (`9547b9c40c31`). New TU
  `arch/um/kernel/spawner.c` (gated on
  CONFIG_UM_WORKER_PROCESS). struct um_worker definition (list
  head, mm back-ref, pid, ipc_sock). spawner_init / shutdown
  wired into arch_initcall + late_initcall panic notifier;
  spawn_worker_for_mm / reap_worker_for_mm are stubs that keep
  mm->context.worker NULL so seccomp's existing stub-child path
  still runs. Boot smoke (WORKER_PROCESS=y) emits "um: worker
  model: spawner ready" before the seccomp banner.
- R4 E.3a — worker process spawn/reap USER-side machinery
  (`23b4de4380a3`). New USER TU
  `arch/um/os-Linux/worker_user.c` with `spawn_worker_process`
  (socketpair + clone without CLONE_VM/VFORK/FILES) and
  `reap_worker_process` (close socket → SIGTERM → waitpid).
  Worker entry point installs SIGTERM handler and PR_SET_PDEATHSIG,
  enters echo-only main loop (E.3b replaces with stub-child
  manager). worker_ipc.h moved from
  arch/um/backend/seccomp/ to arch/um/include/shared/ for USER
  TU access. spawn_worker_for_mm wired to actually create
  workers. Reachable code today but not invoked from any
  production path; integration with seccomp_mm_create lands in
  E.3d.

**Tag:** `kvm-v1-archive-20260428`. **Branch:** `kvm-v1-final`.
**Tip of `uml-redesign-plan`:** post-R4-E.3a.

**Next: R4 E.3b-E.6 + memo 26.** R4's substantive remaining work
is the worker stub-child manager (E.3b), spawner-side IPC
dispatcher (E.3c), seccomp integration (E.3d), per-task pthread
inside worker (E.4), cross-mm migration via SIGUSR2 (E.5), and
the defconfig toggle flip (E.6). Memo 28's Part J has the LoC +
risk breakdown per commit. Total remaining: ~750 LoC. Best
executed as a focused multi-day session. After R4 E.6 lands,
memo 26 Phase A (v2 init.c + per-VM context + vcpu0 placeholder)
starts.

The Stage A foundation work (per-task vCPU + KVM_SET_SIGNAL_MASK)
informed the v2 design but does not directly survive: v2 uses a
per-CPU vCPU pool (memo 26 Phase C) instead of v1's per-task model,
because the per-CPU model is what KVM mmu_notifier and TDP expect.

---

## Stage A LANDED (2026-04-27): per-task vCPU foundation

The architecture review at `03-architecture-review-2026-04-27/` identified
the singleton `vcpu0_fd` shared across all UML tasks (via cooperative
`switch_threads`/longjmp on the same host thread) as the root architectural
defect — every UML task ran on one KVM vCPU, violating KVM's "1 host
thread = 1 vCPU for life" contract. This drove the 5-class race
playbook documented in memo-19 and the ~75% per-trial gate flakiness.

**Stage A delivered the per-task vCPU foundation:**

- `struct kvm_vcpu_handle` per task, allocated lazily on first
  `kvm_run_userspace` via `kvm_vcpu_for_current()`, pinned to
  `current->thread.arch.kvm.vcpu` for the task's lifetime, freed at
  `exit_thread`.
- `KVM_SET_SIGNAL_MASK` blocks every host signal except SIGALRM (UML's
  scheduler tick — required for CPU-bound preemption) and
  `KVM_UM_KICK_SIGNAL` (future SMP eviction). Without the mask, UML's
  signal handler can longjmp into unrelated kernel code mid-`KVM_RUN`.
- `vcpu0_fd` deleted under `CONFIG_UM_BACKEND_KVM_INTEGRATED`. Snapshot
  / record / KUnit diagnostic paths migrated to `current->thread.arch.kvm
  .vcpu` (return -ENODEV when no current vCPU on early-boot).
- `kvm_run_userspace`'s "snapshot before unblock_signals" workaround
  (commit `b516bee62eb2`) deleted — moot under per-task vCPU since
  `vcpu->run` is single-writer.
- FPU/events save/restore on context-switch turned into a no-op. Per-
  task vCPU naturally retains FPU state across schedule-out. Fork:
  `arch_copy_thread` calls `kvm_fpu_capture_for_fork` to KVM_GET_FPU
  on the parent's vCPU into the child's `arch_thread.kvm.fpu`; the
  child's first `kvm_run_userspace` restores via
  `kvm_fpu_install_on_first_run`.
- Per-mm `shadow->dirty` boolean kept (telemetry); `atomic64_t tlb_gen`
  scaffolding added (per-shadow gen + per-vCPU `last_flushed_tlb_gen`)
  for the future per-vCPU TLB-flush tracker. Consumer-side activation
  deferred (task A.4d) until a CLONE_VM-shared-mm test exposes the
  cross-task TLB staleness deterministically.
- Vestigial singleton state in `struct kvm_um` deleted: `cpuid_done /
  msrs_primed / sregs_primed / cached_cr3_gpa / cached_fs_base /
  cached_gs_base / kernel_gs_base_primed / shadow_pgd*`. All migrated
  to per-task `struct kvm_vcpu_handle` or per-mm `struct kvm_shadow_mm`.

**Empirical state post-Stage A:** cpython-parity gate 18/21 single-pass
(measured 2026-04-27 via the canonical `tools/testing/selftests/um/
cpython-parity/cpython-parity.sh`). The 3 diverging modules (test_struct,
test_math, test_decimal) all crash mid-test (k=?/?) — heaviest-memory /
heaviest-compute modules where the latent shadow-PT staleness fires
most reliably. Per-trial variance 60-75% across the 3-module subset
trials. The remaining flake is pre-existing latent shadow-PT
staleness (`test_pylong_roundtrip_huge` in test_int is the most
deterministic repro, ~10 MB contiguous digit-array working set with
per-byte verification on round-trip — see agent research summary in
the Stage A commit message). Stage B's TDP+memslots replacement is
the structural cure.

**Stage B design memo:** `02-workstreams/D-kvm-backend/20-stage-b-design.md`.
Per-mm memslots via `KVM_SET_USER_MEMORY_REGION`, kernel-half PGD shared
across mms, KVM TDP/EPT walks `mm->pgd` directly. ~3-4k LOC delete net.
Implementation tracked at tasks B.1-B.13.

---

## TL;DR — what works, what doesn't

| Workload                                    | Backend  | Reliable? |
|---------------------------------------------|----------|-----------|
| `init=/bin/echo`                            | kvm      | yes       |
| `init=/usr/bin/python3 -V`                  | kvm      | yes       |
| `python3 -c "import hashlib; sha256(...)"`  | kvm      | 5/5       |
| `python3 -c "import _bisect / _datetime / _ssl / _hashlib / _struct"` | kvm | yes (single C-extension import, no test framework) |
| `subprocess.run(['/bin/echo', 'x'])`        | kvm      | ~8/10     |
| `python3 -c "import re"`                    | kvm      | **YES** (post-2026-04-26 keystone fix `901213a8d2d1`) |
| `python3 -c "import unittest"`              | kvm      | 15-20/20 (variance; SECCOMP baseline 20/20) — see "Residual flake" below |
| `read_test5` byte-integrity (8-file mmap harness) | kvm | 8/8 matches seccomp baseline (was 0/8) |
| CPython parity gate (21 stdlib modules)     | kvm      | **16-18/21 PARITY** (run-to-run variance; was 0/21 pre-keystone) |
| Same workloads under `backend=force=seccomp` | seccomp | yes (20/20 deterministic) |

## Memo 18 follow-up (2026-04-26 evening session)

Three commits landed on top of memo 17 Phase A-K:

  - `1bb6c7b7637a` Phase 1.1+1.2 — CPL-aware marshal gate (don't clobber
    user CPL=3 regs on CPL=0 vmexits, fixes the Phase K class without K's
    breakage)
  - `6f2dc2a14e2e` Phase 2 — per-mm IRETQ-frame storage + signal-block
    window around KVM_RUN (eliminates cross-mm IRETQ frame collision)
  - `8de84913b847` Phase 3-fix — install per-mm IRETQ frame AFTER
    kvm_shadow_fill_from_uml_pgd's install pass (was: before; the
    fill's install pass walks PGD slot 384 unguarded and could
    overwrite the IRETQ frame's shadow leaf via the kernel direct-map
    alias of the same physical page → wild-jump-into-ring-3)

**Phase 3 PARTIAL ATTEMPT REVERTED**: tried only step 3.3 of the plan
(per-mm SREGS-skip cache) without 3.1/3.2/3.4 (per-mm vCPU/run/bootstrap).
Result REGRESSED parity from 17/21 to 14/21. Root cause: per-mm cache
+ singleton vCPU produces false hits — predicate matches "what THIS mm
last programmed" but vCPU SREGS were clobbered by another mm. Cache
must stay singleton until per-mm vCPU lands. Phase 3 must be done as
a coordinated unit (vCPU+run+cache+bootstrap) — see memo 18.

**Phase 3-fix empirical impact**: parity=17/21 (same as Phase 2 baseline
median over 3 trials). Structurally cleaner — fill cannot clobber
post-fill installs — but doesn't measurably move parity.

**Empirically ruled out** (additional to memo 17 Phase A-K list):
  - SREGS-skip cache as TLB-flush gate. Disabled the skip → CR4.PGE
    flush every entry. Test_decimal import 13/15 reliability before,
    13/15 after. The dirty-flag bool race exists in principle but is
    NOT the dominant failure cause.
  - Shadow-PT staleness from missed direct-sync writes. Forced full
    PGD-walk every entry → 13/15. Same rate. The direct-sync recovery
    via needs_full_resync IS catching missed leaves.
  - Per-mm IRETQ frame leaf clobber by fill (Phase 3-fix). Moved
    install AFTER fill → ~17/21 parity (within noise of 17-18/21
    baseline). The IRETQ frame leaf is now structurally protected
    but doesn't measurably reduce flake rate.

**Failure modes characterized** (test_decimal import reproducer,
init=python3 PID 1, single mm, single user task):
  - Mode A (most reproducible across runs): NULL deref at va=0x470
    in ld-linux-x86-64.so.2 offset 0x265c3, instruction
    `cmpq $0x0,0x470(%r15)` immediately after a call to
    `_dl_fatal_printf` inside `_dl_mcount`/`_dl_relocate_object`.
    The call SHOULD NOT return (fatal_printf calls _exit), so
    either it's returning incorrectly OR registers (r15 in particular)
    are getting clobbered. Either path implicates KVM-side state
    handling that doesn't manifest under SECCOMP.
  - Mode B (rare): wild jump rip=cr2=small value (0x1, 0x361, 0x3d0,
    0x80000000 etc.) with rdi=0xaaaaaaaaaaaaaaab. Pre-Phase-3-fix
    likely caused by IRETQ frame clobber; post-fix this should be
    closed but a low residual rate persists, suggesting another
    register-corruption path.
  - Mode C (rare): hang / timeout (gate reports k=?/?).

**Residual gap to 21/21**: SECCOMP 100% reliable on every module;
KVM consistent 17-18/21 with a rotating set of 3-4 flaky modules
(test_struct, test_decimal, test_int, test_float, test_array,
test_typing, test_list, test_math, test_hashlib, test_bytes,
test_set, test_abc, test_itertools all have failed at least once
across 4 trials). The bug appears to be a single-task single-mm
register/memory corruption that is NOT TLB, NOT shadow-staleness,
NOT cross-mm/cross-task IRETQ-frame, and NOT obvious singleton-state
contamination. Per-mm vCPU (Phase 3 full structural) addresses
cross-mm contamination, which doesn't exist in this single-mm
scenario, so won't help.

**Next investigation directions** (deferred to follow-up session):
  1. Disassemble what's happening at ld-linux+0x265c3 with the
     pre-call register state restored (need GDB-style attach to
     UML — use the multiple-process bootstrap docs in 16-architecture-
     review/scripts).
  2. Audit ALL singleton state on `struct kvm_um` for race-on-write
     (LSTAR? FMASK? STAR? EFER? KERNEL_GS_BASE?). Phase H caught
     FPU + VCPU_EVENTS but not these MSRs.
  3. Investigate whether the kernel's signal-delivery path
     (`setup_signal_stack_si`, `do_signal`) writes to user memory
     via the host VA in a way that races with guest reads through
     shadow PT. Phase 4 (KVM-native uaccess) addresses this class.

## User-recommended items evaluated 2026-04-26 evening

User provided 9 prioritized items; my honest evaluation:

| # | Item | Status | Empirical |
|---|------|--------|-----------|
| 1 | KVM_EXIT_INTR CPL gate | ✅ DONE | Phase 1.2 commit `1bb6c7b7637a` |
| 2 | Eliminate singleton IRETQ frame | ⚠️ PARTIAL | Per-MM in Phase 2 (`6f2dc2a14e2e`); per-TASK deferred |
| 3 | Run-window serialization (turnstile) | ⚠️ PARTIAL | Phase 2.4 added block_signals; no per-mm mutex |
| 4 | Fork/clone FPU inheritance | ✅ DONE | Phase J commit `9fb1a82e2357` |
| 5 | Disable KVM_SYNC_X86_REGS diagnostic | ✅ EVALUATED | parity 15/21 (no improvement); reverted |
| 6 | Direct PTE sync vs full fill race | ⚠️ EVALUATED + REVERTED | Tried cmpxchg(0→new) install-if-absent: v1 (all slots) 16/21, v2 (user-half only) 15/21 — both regressed slightly. Race is theoretically real but fix didn't measurably help; variance-dominated |
| 7 | Per-mm host worker | ❌ DEFERRED | Multi-week structural |
| 8 | CR3/TLB dirty consumption | ✅ EVALUATED | Disabled SREGS-skip cache → CR4.PGE flush every entry → 13/15 (no improvement). Rules out missed producer as dominant cause |
| 9 | Hot-path diagnostics off by default | ❌ NOT EVALUATED | Out-of-scope cleanup |

**Conclusion**: Items 1, 4, 8 are confirmed DONE/EVALUATED. Items 2, 3
are PARTIAL (would need per-task scope, deferred). Items 5, 6 surgical
fixes do NOT measurably improve parity beyond 17/21 baseline median —
variance (15-18/21 per single-trial) dominates any small effect.

**Root cause hypothesis after extensive testing**: the residual
4-6/21 flakes are NOT explained by any single-cell race in the KVM
backend code paths I've audited. The most reproducible failure
mode (ld-linux NULL deref at va=0x470, rip=0x400265c3 after a call
to a private helper at 0x14560) suggests either:
  (a) genuine glibc behavior under timing-sensitive paths that KVM
      vCPU emulation perturbs differently than seccomp, or
  (b) a memory-content corruption mechanism we haven't found —
      possibly in kernel→user data flow paths (signal delivery,
      hostfs reads, or memory the kernel writes via host VA that
      the guest reads via shadow PT).

**Recommended next paths** (all multi-day or larger):
  - Per-TASK vCPU (Phase 5 from memo 18 plan): 5-10 days
  - Per-mm host worker (Phase 4 Option B): multi-week
  - GDB-style attach to UML at the failure point to capture
    pre-call register state
  - Statistically powered N=20+ trials per config to detect
    sub-noise improvements

**Current committed state**: Phase 2 baseline 17/21 (median over
4 trials) with Phase 3-fix (`8de84913b847`) for IRETQ frame
post-fill ordering. Architecturally sound, no regressions.

## Failure-mode dissection 2026-04-26 evening: ld-linux NULL deref is glibc exception handling

N=50 baseline of `import test.test_decimal` reproducer: 43/50 = 86%
reliability. Failure dissection (N=30 with full pf_mini_regs capture):

- **5/8 failures (62.5%)**: IDENTICAL signature
  ```
  rip=0x400265c3 cr2=0x470 ec=0x4 rax=0x0 rbx=0x4003cac0 rcx=0x400299eb
  rdx=0x1 rsi=0x4000 rdi=0x403d3000 r8=0x0 r12=<varies> r13=0x0 r14=0x0 r15=0x0
  ```
  Only r12 varies across trials (stack-allocated counter). r13/r14/r15
  are CONSISTENTLY zero. Instruction at 0x265c3 is
  `cmpq $0x0,0x470(%r15)` which faults at va=NULL+0x470.

- **3/8 failures (37.5%)**: distinct rip/cr2 patterns (Python internals,
  libc internals, etc.)

**Root cause of the dominant 5/8 mode**: the r13/r14/r15 = 0 pattern
matches `ELF_PLAT_INIT` (arch/x86/um/asm/elf.h:118-120) which zeroes
ALL registers at execve. The code at 0x265c3 is reached via
setjmp/longjmp exception unwinding — the helper at 0x14560 (called
from 0x265be) is likely `_dl_catch_exception`. Under exception:

  1. Earlier glibc/ld-linux state inconsistency triggers an exception
  2. Helper at 0x14560 catches it (longjmp)
  3. longjmp restores callee-save regs from the saved jmpbuf —
     captured at process startup when r13/r14/r15 were 0 per ELF_PLAT_INIT
  4. Resume at 0x265c3 with r15=0 (the "exception caught" path)
  5. Optimized code assumes r15 is valid → NULL deref → SIGSEGV → exit

The actual bug is NOT at 0x265c3. The bug is **whatever causes glibc
to raise the exception in step 1** — a memory-content discrepancy
or invariant violation triggered by KVM-specific behavior that
seccomp doesn't reproduce.

This shifts the investigation from "find the corrupted register" to
"find the inconsistency glibc detects". Without deep runtime
instrumentation (capturing the exception's reason field, or stepping
through ld-linux at the moment exception is raised), this is hard
to root-cause from this layer.

**Bottom-line empirical state**: 86% per-import reliability under KVM,
17/21 parity gate median. No surgical fix beyond Phase 1+2+3-fix has
moved the needle. The remaining flake is tied to a glibc exception
handler being invoked under KVM but not seccomp — root cause is
upstream of the SIGSEGV symptom and needs runtime trace
instrumentation or per-mm host worker (eliminating the host-VA
aliasing class entirely).

---

---

## Residual flake state (post memo 17 Phase A-K — 2026-04-26)

11 commits landed this session moved the cpython parity gate from
**0/21** (deterministic FAIL) to **16-18/21** (run-to-run variance).
import unittest under KVM moved from 0/10 to 27-29/30 (~93%
deterministic; SECCOMP baseline 20/20 — the residual ~7% under
KVM is KVM-only SIGSEGV at varying RIPs, not Python nondeterminism).

### What's been fixed (commits)

  Phase A `dc58a7ed3ada`: 6 lifecycle.c sites missing the producer-
                          side WRITE_ONCE+smp_wmb for shadow->dirty
                          (Race-I keystone parallel)
  Phase B `(in-Phase-A)`: CR4.PGE-toggle replaces CR3-XOR hack as
                          the TLB-flush mechanism
  Phase D `(in-Phase-A)`: FPU hash → embedded in arch_thread.kvm
                          (eliminates B-FPU-HASH-UAF)
  Phase E `4b908bd16e4f`: VCPU_EVENTS save/restore on context switch
  Phase H `c512077454cd`: 4 task-isolation bugs from external
                          review (dirty-flag cmpxchg, fresh-task
                          VCPU_EVENTS reset, events.flags preserve,
                          fresh-task FPU init)
  Phase I `a71d44db0235`: mmap_read_lock around all 3 source-pgd
                          fill walks (finding 5)
  Phase J `9fb1a82e2357`: needs_full_resync race fix +
                          fork FPU inheritance (findings 1b + 6)
  Phase K `0dc8b091b747`: REVERTED. The KVM_EXIT_INTR marshal-skip
                          (commit 644ebea88419) was unsound — KVM_EXIT_
                          INTR with rc=0 reaches the dispatcher and
                          out_read_regs's marshal is gated only on
                          rc==-EINTR, so skipping the bulk marshal
                          dropped the user's CPL=3 RIP/RSP/RFLAGS
                          updates between entries. A CPL-based gate
                          (skip-on-CPL=0) breaks SYSCALL/PF/MMIO paths
                          which legitimately exit at CPL=0 and need
                          the marshalled regs (kvm_decode_syscall
                          reads RAX/RCX/R11). An IST-based recovery
                          (re-read user state from the iretq frame on
                          CPL=0 INTR exit) tested at 25-26/30 — slight
                          regression from Phase J's 27-29/30. Net:
                          revert. The structural fix needs the per-
                          task IRETQ-frame storage approach, deferred
                          to memo Phase 4 Option B.

### What's been ruled out

  - vCPU memory ordering on shadow->dirty (Phase A)
  - TLB-flush mechanism (Phase B / keystone)
  - FPU hash UAF / cross-task FPU leak (Phase D / H)
  - VCPU_EVENTS leak (Phase E + H)
  - dirty-flag consume race (Phase H finding 1)
  - needs_full_resync consume race (Phase J finding 1b)
  - VCPU_EVENTS UAPI flag discard (Phase H finding 3)
  - mmap_read_lock around source-pgd walks (Phase I finding 5)
  - fork FPU inheritance (Phase J finding 6)
  - KVM_EXIT_INTR ring-0 marshal leak (Phase K external finding 1)

### What remains (deferred to architectural follow-up)

  - **Finding 4 / external Finding 2 + 3** — singleton vCPU + singleton
    IRETQ-frame buffer + no per-mm turnstile. Seccomp avoids these
    entirely via the per-mm stub-child host process. Under KVM, the
    bootstrap_page_stack is shared across all tasks; signal-driven
    preemption between frame-write and KVM_RUN can let task B's
    frame clobber task A's. Tested block_signals around the
    critical section (Phase F) — no measurable improvement under
    UML's signal infrastructure. The structural fix is per-mm host
    worker (memo Phase 4 Option B) — multi-week refactor.
  - **External Finding 5** — direct sync writes shadow leaves
    without fill_lock; if a full-fill snapshot+install cycle is in
    progress, the fill's stale snapshot can overwrite a fresh
    sync_pte write. Mitigated by mmap_read_lock (Phase I) which
    bounds the race window, but full lockless audit is deferred.

The session-end equilibrium: Phase A-K is correct, lands solid
keystone correctness, and pushes the cpython parity gate from
0/21 to 16-18/21 with import unittest at 27-29/30 (was 0/10).
Closing to 21/21 requires either the per-mm host worker (memo
Phase 4 Option B, multi-week) or per-task IRETQ-frame storage
+ per-mm turnstile around the trap iteration — both meaningful
restructuring beyond this session's scope.

## P0 keystone fix landed 2026-04-26 (`901213a8d2d1`)

**Root cause**: `KVM_SET_SREGS` with the same CR3 value as the vCPU's
current CR3 does NOT flush the guest TLB even when shadow PT contents
changed (e.g. munmap cleared a leaf, then user re-mmap a different
file at the same VA). KVM's `kvm_set_cr3` calls `invalidate_pcid` only
conditionally, and the VMCS-level CR3 reload is also gated on a value
change. Result: stale guest TLB → user reads OLD PFN's bytes after a
clear+remap cycle, never faulting because the cached translation is
"valid".

**Empirical proof** (`read_test5` C harness, 8 file mmaps at the same
VA): without the fix, only 1/8 user accesses to a re-mmap'd VA generate
a host #PF and only 3/8 set_ptes installs fire — the other 5 cycles
silently read stale TLB entries, returning the prior file's content
(the page at PFN 0x689 contained "glibc-ld..." from the dynamic
linker's earlier ld.so.cache mapping; functools.pyc's actual page at
PFN 0x10ac was correctly populated but never reached the user because
the shadow leaf write didn't invalidate the cached TLB).

**Fix**: in `kvm_enter_guest`, when CR3 is unchanged but shadow is
dirty, write a sentinel CR3 (XOR bit 12 — guaranteed-different,
guaranteed-valid GPA in our 512MiB physmem) before the real CR3 so
KVM observes a real change → forces VMCS reload + full TLB flush.
Cost: one extra `KVM_SET_SREGS` ioctl on dirty-shadow same-CR3
entries; the cached-skip predicate above still elides BOTH ioctls
when nothing changed.

**Impact**: cpython parity gate `parity=0 → parity=17` (of 21 modules).

**Open follow-on (task #77)**: 5 modules still diverge under KVM, all
hanging at multi-task / threading / subinterpreter / deep-recursion
paths. Distinct bug class — multi-mm shadow PT issue, not the same
TLB keystone. Per-mm CR3 changes already flush TLB via the value
change; the residual issue is likely in how clone(CLONE_VM) tasks
share but each need their own TLS / per-task vCPU state.

---

## Pre-keystone notes (kept for historical context)

**The headline fact (corrected 2026-04-26 by `kvm_shadow_audit_va`
diagnostic, commit `44abfd6e6657`): under integrated KVM, a single
Python `import unittest` faults inside `python3.14`'s text or
`ld-linux-x86-64.so.2`'s text — depending on ASLR layout — NOT
inside ld-linux processing `_lzma.cpython-*.so` relocations as
previously claimed.** With ASLR off (`randomize_va_space=0`) the
crash is deterministic: `cr2=0x300`, `rip=0x4002625f` in
`ld-linux-x86-64.so.2[2625f]`, which addr2line resolves to
`dl_main` at `elf/rtld.c:1953`. The faulting instruction is
`mov 0x300(%rbx),%edi`; the immediately-preceding instruction at
`0x26236` is `mov -0x260(%rbp),%rbx`, so dl_main loads rbx from
the stack slot `[rbp-0x260]` which is supposed to hold the main
executable's `struct link_map *` (arg1 to the upcoming call to
`_dl_map_object_deps`). The slot reads as 0. Several other
callee-saved regs (r12/r13/r15) are also 0 — consistent with the
initial register state at process start, suggesting dl_main's
earlier code never wrote to them.

Same script under `backend=force=seccomp randomize_va_space=0`
succeeds (UT_PRE → UT_DONE → exit 0). So the bug is specifically
in the KVM backend's interaction with dl_main's early
initialization, not in glibc/python.

Diagnostics ruled out so far (commits `44abfd6e6657`,
`2fdfd77f7264`, `97c73735c47a`, `b458b8605c8f`):
  - Shadow PT divergence at the fault VA (audit at !touched: EQUAL
    on every run)
  - Stale-true cache on the cached-skip path (lockstep audit:
    DIV=0 even at 2253 leaves)
  - Syscall round-trip clobbering callee-saved regs
    (CALLEE-SAVE-CLOBBER never fires across the boot)

Real bugs fixed this session (each verified against hashlib smoke
+ merge gate; none individually moved parity from 0/21, but
together they make hashlib reliably pass and the dl_main reproducer
moved FAIL→PASS):

  - `1e49886e` T1: `set_ptes()` PFN advancement bug. The mainline
    UML implementation advances PTE PFN by `nr_remaining` pages
    instead of one — under integrated KVM the wrong PFNs propagate
    into the shadow PT. This is the bug that was causing the
    deterministic dl_main rtld.c:1953 fault.
  - `55345b11` T7: always mark `shadow->dirty=true` in
    `kvm_shadow_invalidate_va_range`, not only when cleared > 0.
    Stale-TLB risk when invalidate runs on a range that hadn't
    been lazy-filled yet.
  - `664df08f` T15: `update_pte_range` (and pmd/pud/p4d variants)
    in `arch/um/kernel/tlb.c` marked PTEs uptodate even when the
    backend `ops->mmap`/`ops->unmap` returned an error — leaving
    the host VA / shadow permanently divergent from the pgd. Fixed
    to gate `*_mkuptodate` on rc==0.
  - `171330af` T13: `init_new_context`'s
    `mm_unmap(new_id, 0, STUB_START)` call was passing
    `len=0x7fffffffc000` (~128 TB) to `kvm_mm_unmap`, which
    invalidated the parent's shadow over a huge range and
    potentially destructively munmap'd host VA. Short-circuited
    under KVM_INTEGRATED.
  - `1ba17edf` T17: `kvm_ensure_cpuid_done` failure was silently
    dropped via `(void)` cast.
  - `aa3fedaa` T16: `kvm_shadow_invalidate_va_range` errors from
    `kvm_mm_map`/`unmap` were silently dropped.
  - `062c3f98` T8: freeing a `kvm_shadow_mm` whose `pgd_gpa`
    matched `cached_cr3_gpa` left the SREGS-skip cache pointing
    at freed memory.
  - `05d17152` T20: FSGSBASE exposed in CPUID without CR4.FSGSBASE
    set — guaranteed #UD on first `WRFSBASE`.
  - `e326c75a` T5: `kvm_context_switch` synced `prev->mm` not
    `prev->active_mm`, dropping kernel-thread-borrowed-mm pgd
    mutations on the floor.

What still remains (the real bug that keeps moving downstream
and now manifests as `PyList_Append` or `_PyObject_MakeTpCall`
faults with `self->ob_type == NULL` — i.e. ob_type field of a
live PyObject zeroed at offset 0x8): some path is corrupting
heap memory in early Python startup. Bisect by import:

  - sys, os, collections, pickle, threading: SUCCEED
  - subprocess, json, importlib, unittest: FAIL (different IPs
    and crash sites each time, but always the same pattern of
    NULL pointer dereference at small offset)

Failure correlates with workload size — more memory ops, more
chance to hit the corruption. Suggests a per-N-operations bug
that the audits-on-cached-skip path can't catch. Remaining
tasks worth investigating: full-pgd mm_id passthrough (T2);
FPU/XSTATE save/restore around context switches (T3);
bidirectional shadow audit (T6); fill_lock missing (T11);
turnstile missing (T12).

Important meta-finding from `b458b8605c8f`: the underlying race
this bug is rooted in is **timing-sensitive enough that adding
work on the hot path measurably changes its trigger rate**. The
audit_pgd lockstep walk on every cached-skip dropped hashlib
smoke from ~95% to ~30%; even just adding the dead-code regs-dump
bytes to the !touched branch dropped it to ~80%. Diagnostics that
run in the steady-state path must be gated behind the
`kvm_diag_*` kernel command-line knobs (default off) and only
enabled for specific debugging boots — otherwise they will
themselves trigger the bug they are trying to characterise. Knobs:

  kvm_diag_pf_dump_regs=1       enable GP-reg dump on !touched #PF
  kvm_diag_audit_pgd_skip=1     enable full pgd vs shadow lockstep
                                audit on every cached-skip

Remaining hypotheses to investigate:
  - Shadow PT has stale leaves the pgd doesn't (shadow→pgd
    direction not yet audited)
  - Guest TLB caches stale mappings even though shadow PT is
    correct (CR3 reload / TLB flush ordering)
  - Non-syscall paths (signal injection, page fault recovery,
    initial process setup via execve) corrupt user state
  - dl_main takes a different code path under our backend due to
    different return values from early syscalls (e.g. brk, mmap,
    arch_prctl, set_tid_address) — would need per-syscall return-
    value diff vs seccomp baseline

That blocks the entire CPython test suite, which is the merge gate
for closing #274 ("Python is table stakes; if UML can't run a normal
Python script that millions of people use, what is it good for?").

---

## The merge gate

`tools/testing/selftests/um/cpython-parity/cpython-parity.sh` runs
21 curated stdlib test modules under both backends and reports
module-by-module divergence. Statuses: `PARITY`,
`DIVERGE_KVM_REGRESSION`, `DIVERGE_KVM_BETTER`, `SKIP_NO_RESULT`,
`BOTH_FAIL`.

Today's reading:

```
TOTAL: parity=0 diverge=21 skip=0
```

Every module passes under seccomp, every module crashes under kvm.
**Until this reads `parity=21 diverge=0`, no other phase-1/2/3 work
proceeds.**

When parity reaches zero diverges, the gate widens:
1. Tier 1: pytest-driven third-party libraries (`requests`, `cryptography`, `numpy`)
2. Tier 2: `pip install <pkg> && pytest <pkg>` — full network + tarball + subprocess
3. Tier 3: small Django/FastAPI server actually serving requests over loopback (needs `CONFIG_UML_NET_VECTOR=y` rebuild + tap setup)

Each tier is an explicit kselftest. Width before depth.

---

## Open in_progress

### #274 — KVM SIGSEGVs on Python C-extension import (table stakes)

The umbrella for all Python-runs-under-kvm work. Stays open until
`cpython-parity` reads zero diverges. Currently in_progress.

**Subordinate work:**

- #275 — per-mm shadow PGD. **Completed** (commit `b9d78a7eeb51`).
  Each UML mm now owns its own `struct kvm_shadow_mm`. Eliminated
  the cross-process leaf-leak that was the dominant failure mode
  for `subprocess.run`. Reliability went from 1/5 to 8/10.

- #276 — skip `os_map_memory` in `kvm_mm_map`. **Pending (reverted).**
  Hypothesis was that under integrated KVM the host VA mapping
  wasn't load-bearing because the guest CPU goes through shadow PT
  → memslot → uml_physmem linear mapping. Empirically wrong:
  `import unittest` crashed in libc when os_map_memory was skipped.
  Restored. Real fix needs per-mm host-VA isolation (child host
  process per UML mm, matching seccomp's stub-child model).
  Substantial refactor; defer until the within-mm cumulative-imports
  staleness is fixed first because that may turn out to be the
  dominant remaining bug regardless.

- #277 — full CPython test suite as merge gate. **Pending.** The
  gate exists (cpython-parity.sh). Closing #277 requires #274 to
  close first.

### #250 — KVM-aware `um_snapshot_ready`

Snapshot/forkserver primitives shipped (commit `040bdb2f6b04`); the
hook into `um_snapshot_ready` so `snapshot-smoke` PASSES under kvm
is the remaining work. Deferred until #274 closes — there's no
point gating snapshot-replay on a backend that can't run real user
programs.

### #253 — first-class time-travel + record/replay (KVM determinism)

Per-NR record matrix covers `getrandom / read / pread64 / recvfrom
(sockaddr) / readv (iov)`. Strict-replay mode is default. Round-
trip KUnit cases pass. Open ladder rungs: `recvmsg` (msghdr +
optional msg_control), PMU-driven instruction-boundary recording,
MMIO recording. Deferred until #274 closes.

---

## The actual debugging plan for #274

Goal: `cpython-parity` flips to `parity=21 diverge=0`.

Method: every fix must show a parity delta. "Looks correct" without
a delta is rejected. Three phases:

### Phase 1 — make divergence visible

Until I can SEE pgd-vs-shadow-PT divergence in a log, every "fix" is
a guess. Add three diagnostics:

1. **Divergence audit at every guest #PF entry.** Walk UML's pgd for
   `cr2`'s VA, walk the shadow PT for the same VA, log
   `(cr2, pgd_pte, shadow_pte, equal?)`. The first faulting access
   where shadow != pgd identifies the missed sync point.

   **Status (2026-04-26):** First instance landed as
   `kvm_shadow_audit_va` wired into the !touched branch of the
   UM_KVM_PF_PORT handler (commit `44abfd6e6657`). Empirically the
   SIGSEGV-bound fault is `EQUAL` on every run — the keystone
   hypothesis ("first divergence is at cr2") is wrong for this bug
   class. Search must shift to non-faulting VAs (item #2 below) and
   to per-mutation tracing (item #3 below).

2. **Divergence audit at every kvm_enter_guest.** Sample 20 random
   user VAs from `current->mm`'s vmas, walk both trees, count
   divergences. Log a histogram each entry. If divergences accumulate
   over time, the rate identifies the leaky operation class.

3. **Tracepoint every `set_pte_at` / `kvm_shadow_map_page` /
   `kvm_shadow_invalidate_va_range`** with `(mm, va, old_pfn, new_pfn,
   flags, caller_function)`. Then I can grep: every `set_pte_at` on a
   user VA should have a paired shadow update. The unpaired one is
   the bug.

4. **Stress-test reproducer.** A C program (not Python) that does N
   rounds of `mmap(PROT_RW)` → write → `mprotect(PROT_RO)` → read →
   `munmap`. Tunable N. Find the smallest N at which divergence first
   appears. Deterministic non-Python reproducer the diagnostics can
   attach to.

Phase 1 is done when one boot of `python3 -c "import unittest"`
produces a single log line saying exactly which (VA, mm, syscall,
timestamp) the first divergence occurred.

### Phase 2 — match each divergence to a code path

Each divergence event from phase 1 has a calling stack. Group by
stack. The groups are bug families.

Likely candidates I haven't ruled out:

- **`flush_tlb_range` not draining before guest entry.** UML's
  flush_tlb_range marks ranges via `um_tlb_mark_sync`. `um_tlb_sync`
  drains. If flush is called and sync isn't drained before the next
  guest entry, divergence.

- **mprotect / madvise / munmap.** Each can change PTEs. Verify each
  path triggers our shadow invalidation.

- **handle_mm_fault from inside copy_to_user.** UML's `maybe_map`
  calls `handle_page_fault` mid-syscall. The resulting `set_pte_at`
  on the active mm doesn't trigger a fresh `current_mm_sync` until
  the next iteration of `kvm_run_userspace`. Within-syscall stale
  shadow.

- **Implicit anonymous fault-in.** Generic Linux's `do_anonymous_page`
  allocates a zero page on first access. If the next iteration's
  `current_mm_sync` doesn't drain THIS specific range, divergence.

- **Exec mm replacement timing.** When `execve`'s `exec_mmap`
  installs a new mm, the OLD `mm_id` (with its `kvm_shadow`) gets
  freed. If KVM_RUN's saved register state references the old
  shadow's CR3 GPA, and we re-enter without rebuilding sregs,
  freed memory.

- **mmap_lock vs shadow update ordering.** UML's pgd mutations
  happen under `mmap_lock`. `kvm_shadow_invalidate_va_range`
  doesn't take any lock. If the GUEST is reading the shadow tree
  while the kernel mutates pgd → marks sync → drains via
  `kvm_mm_map` → invalidates shadow, racy reads can see torn state.

The phase-1 traces tell me which apply.

### Phase 3 — fix one root cause, prove it with the parity gate

For each identified divergence:
1. Add a regression test triggering the exact pattern (the C
   reproducer + a Python repro).
2. Fix it.
3. Run `cpython-parity`. Metric: how many module rows flipped from
   DIVERGE to PARITY.
4. If zero modules flipped, the fix isn't load-bearing — revert and
   look harder.
5. Commit only fixes where parity moves.

### Phase 4 — structural endgame: collapse the sync chain

Today's chain:
```
set_pte_at → um_tlb_mark_sync → (later) um_tlb_sync →
ops.mmap → kvm_shadow_invalidate_va_range
```

Every step is a place where the chain can break. The structural
fix: under `CONFIG_UM_BACKEND_KVM_INTEGRATED`, have `set_pte_at`
directly call into the active mm's shadow update under one
critical section. No "mark + drain later." pgd write and shadow
write happen transactionally.

May not be necessary if phases 1-3 close enough divergences. But
the moment three "missed sync point" bugs show up in three
different places, that pattern becomes evidence for needing the
structural fix.

---

## Discipline rules in effect

These are the habits adopted after the 2026-04-25 retrospective
("how did you get this far without paying attention to how broken
this was?").

1. **`cpython-parity` is the only metric I report.** Parity number,
   nothing else. Latency, commit count, line count, kselftest count
   are not answers to "does this work."

2. **No new feature work until #274 closes.** Phase 5 LKML series,
   perf optimizations, record/replay extensions, snapshot-forkserver
   integration — all paused.

3. **No test downgrades.** If a kselftest harness sees a fatal
   signal, segfault, panic, or non-zero exit, it must FAIL. Skipping
   is allowed only for "the prerequisite isn't installed on this
   host" — never for "the thing crashed but we'll call it OK." All
   existing harnesses get audited for swallow-failures patterns
   before any new work.

4. **Width before depth.** Adding KUnit cases for the dispatcher is
   depth — measures the same surface more carefully. Adding
   `test_subprocess`, real third-party library tests, real package
   installs is width — measures whether the thing is useful at all.
   Width catches "your foundation is broken." Depth doesn't.

5. **Smell test before every "done" claim.** Would I bet $100 that
   a user can do <the use case> on this build right now? If no,
   it's not done.

6. **Disagreement with the user is a signal to slow down, not push
   through.** When the user says "are you sure?" — re-test. When
   they say "fix the real problem" — stop adding small fixes and
   look at the structural problem. When they say the same thing
   twice in the same tone, drop everything else immediately.

7. **Periodically widen the test surface.** At every phase boundary,
   ask: does real software run on this? Not "did the green
   checkmarks light up?"

---

## Audit todo: kselftest harness honesty

Before resuming feature work, audit every
`tools/testing/selftests/um/*/run-*.sh` for swallow-failure
patterns. Specifically look for:

- `|| true` on a UML invocation followed by parsing OUT for "PASS"
  patterns instead of failing on non-zero exit
- `echo "FOO: PASS runtime skipped (...)"` on a fatal signal
  detection — `kvm-record-smoke/run-kvm-record-smoke.sh:154-157`
  has this pattern today
- Any harness that downgrades a SIGSEGV into "best-effort"
- Any harness whose passing condition is just "boots and exits"
  rather than "exercises the thing under test"

Each downgrade gets removed, and if the underlying behavior is
broken, that's a new task in the tracker.

---

## Snapshot of the task tracker (as of 2026-04-26)

### in_progress (P0 — gating)

- **#274** — KVM SIGSEGVs on Python C-extension import (table stakes)
- **#275** — per-mm shadow PGD (completed; tagged in_progress
  pending closure of #274)
- **#250** — KVM-aware um_snapshot_ready (was in_progress; deprioritized)

### pending (P0 — gating)

- **#276** — per-mm host VA isolation
- **#277** — full CPython test suite as merge gate

### pending (Phase 1 follow-on)

- **#244** — huge-page (2 MiB) shadow PT for UML physmem (perf
  lever #5; deferred)

### pending (Phase 2)

- **#246** — per-handler size-aware G1 caps
- **#247** — A3 expansion: move using_seccomp probe

### pending (Phase 3)

- **#252** — vm/uml syzkaller backend
- **#253** — first-class time-travel + record/replay extensions

### pending (Phase 4)

- **#256** — ARM64 KVM backend port
- **#257** — RISC-V KVM backend port
- **#258** — cross-host CI matrix expansion

### pending (Phase 5 — LKML submissions, ALL PARKED until #274 closes)

- **#259** — refresh Series 7 cover letter post-Phase-1 numbers
- **#260** — submit Series 1 (bpf-hygiene-v1)
- **#261** — submit Series 2 (kmsan-arch-callback-rfc)
- **#262** — submit Series 3 (ftrace-notrace-generic-v1)
- **#263** — write + submit Series 4 (backend-ops-abstraction-rfc)
- **#264** — write + submit Series 5 (static-key-hot-paths-series)
- **#265** — write + submit Series 6 (profiles series)
- **#266** — submit Series 7 (kvm-backend-series)

### completed (selected highlights, this session and recent)

- **#275** — per-mm shadow PGD landed (`b9d78a7eeb51`)
- **#274 partial** — `current_mm_sync()` before KVM_RUN
  (`96bfe20b3616`), IRETQ-frame RSP/RIP/RFLAGS restore on ring-3 #PF
  (`401c231766ac`), CPUID XSAVE/AVX masking (`1a62c7d27e0d`)
- **#273** — KVM_SET_CPUID2 passthrough so guest libc loads
- **#272** — dyn-loader 0xc680 wild-jump fix
- **#271** — kvm_bootstrap_gva separated from kvm_bootstrap_va
- **#269** — IDT[8] #DF handler
- **#268** — perf bench statistical hygiene
- **#267** — /dev/kvm ACL self-heal
- **#251** — snapshot kselftest
- **#255** — PMU passthrough via KVM_CAP_PMU_EVENTS
- **#254** — runtime-flippable hooks via static keys

---

## How to use this document

When starting a session: read the TL;DR, check the merge gate
state, pick the highest-priority `in_progress` task. Do not pick a
non-P0 task while a P0 is open.

When ending a session: update the TL;DR, the merge gate state, and
move tasks between `in_progress` / `pending` / `completed`. Commit
this file with the change.

When in doubt about whether to work on something: if it's not P0
and the merge gate is red, the answer is no.
