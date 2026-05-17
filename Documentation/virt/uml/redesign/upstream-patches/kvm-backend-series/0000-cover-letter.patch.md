# 0000-cover-letter.patch — draft (kvm-v2 refresh, 2026-05-14)

Draft form of the cover letter that will head the v2-shaped
patch series emitted from `git format-patch`. Not yet a `.patch`
file because the patches themselves haven't been squashed from
the `umlctl-deploy` branch. Final `.patch` generation and
diffstat fill-in happens after Phase J reaches DONE and the
squash audit converges.

## Pre-letter preamble — what changed since the 2026-04-27 draft

This draft replaces the 2026-04-27 Stage-A draft (414 lines)
in place. The 2026-04-27 draft described a per-task vCPU +
shadow page table architecture (commit `7f94922a356f`). That
architecture is **no longer the proposed upstream shape**.

What happened between 2026-04-27 and 2026-05-14:

  - The Stage-A code (and v1 broadly) was archived at
    `arch/um/backend/kvm-v1-archive/` (gated on `BROKEN`), tag
    `kvm-v1-archive-20260428`. Stage A taught the project the
    set of bugs the per-task / shadow-PT design made
    structurally inevitable; memos 24-25 captured the
    rationale.
  - v2 was built from scratch at `arch/um/backend/kvm-v2/` per
    memo 26's 10-phase implementation plan. Net code is ~1500
    LoC (v1 was ~6000 by the time it was archived).
  - The architecture is now: **per-CPU vCPU pool** (one vCPU
    per host CPU, UML scheduler picks tasks onto them) +
    **TDP / EPT** (KVM walks `mm->pgd` directly via per-region
    `KVM_SET_USER_MEMORY_REGION` memslots) + a **5-byte
    kernel-half IO-port trampoline** at PML4[448] for the
    syscall trap, plus a revived in-guest **systrap gadget**
    that handles 11 hot-path syscalls without VMEXIT.
  - 14 SMP-T## fix cycles closed since the v2 substrate first
    reached cpython-parity 21/21 (T13, T16, T17, T19, T20, T22,
    T23, T25, T26, T27, T29, T33, T36, T37, T41, T47, T54, T55,
    T56, T57 — decision-log D106..D122).
  - SMP-T57 Phase A (commit `ab68bf077de3`) enabled CR4.OSXSAVE
    + KVM_SET_XCRS(0x7) + CPUID un-mask of AVX / AVX2 / FMA /
    F16C / XSAVE / OSXSAVE. AVX-512 (Phase B) is a known
    follow-up; 12 of 14 sampled stress-ng vm-methods pass on
    kvm-v2 post-Phase-A (was 0 of 14).
  - cpython-parity is 21/21 stdlib modules under both UP and
    SMP. mt-mini SMP T=8 ncpus=4 N=400 = 400/400 = 100 %.
    threaded-fork-malloc 8 workers × 500 iters × 6 boots =
    24 000 forks ⇒ 0 CHILD_FAIL. The first 2 h Phase J
    daemon-driven soak (memcheck / iocheck / stress-ng IPC /
    tier1-pylibs × kvm-v2 × 200 iters, 2026-05-14) returned
    200 / 200 = 100 %.
  - Performance, post-Phase-A, on AMD Ryzen 7 7840HS (Zen 4):
    bench-py 4.00× faster than seccomp, bench-micro getpid
    ~1000× faster via the LSTAR gadget (89-105 cyc on kvm-v2
    vs ~109 000 cyc on seccomp). perf-py-startup ratio 1.10-
    1.17 vs the 1.20 ceiling (gate passes; the prior 0.44
    "kvm-v2 twice as fast as seccomp" reading is the SMP-T55
    stretch target that remains a deferred polish item).

**This series is gated on Phase J DONE.** Phase J DONE means
24 h continuous soak ≥ 99.5 % PASS per workload (Wilson 95 %
lower bound ≥ 99.0 %), Tier 1/2/3 + LTP wired with at least
one passing cycle each, headline ratio recorded. As of
2026-05-14 the daemon driver is live, Tier 1 (third-party Python
libs) has a passing cycle in the 2 h soak, Tier 2 (`uv run
--with`) templates are designed, Tier 3 + LTP templates are
landed (operator pre-flight pending). The 24 h continuous run
has not yet been kicked off.

The cover-letter content below is finalised in shape but not
in numbers: the final perf table will be re-captured post Phase
J DONE on the validation fleet (the 8-host bench from
`02-workstreams/D-kvm-backend/measurements.md` plus one fresh
24 h-validated host).

---

    Subject: [PATCH RFC 00/19] um: add KVM backend (v2) with TDP, per-CPU vCPU pool, and in-guest systrap gadget

    From: Michael Bommarito <michael.bommarito@gmail.com>

    This series adds a third execution backend to User Mode Linux
    alongside the existing ptrace and seccomp backends.

    The backend ("kvm-v2") executes the UML guest in a hardware-
    enforced ring-3 KVM VM whose paging is driven directly by
    `mm->pgd` via two-dimensional paging (TDP/EPT). Syscalls trap
    via a 5-byte LSTAR trampoline at PML4[448] (kernel-half;
    canonical-sign-extended past the user/kernel boundary)
    emitting `out %al, $0xf4` → `KVM_EXIT_IO`, after which the
    host dispatcher runs UML's standard `handle_syscall` and
    re-enters via `KVM_RUN`. An optional in-guest systrap gadget
    services 11 hot-path syscalls (getpid family + sched_yield +
    clock_gettime + time + getcpu) entirely in ring-3 without
    VMEXIT, gated on `CONFIG_UM_BACKEND_KVM_V2_GADGET` (default y).

    Architecturally, v2 differs from a hypothetical Stage-A
    "1 task = 1 vCPU + shadow page table" design (which the
    project tried, archived, and rejected — see Documentation/
    virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-
    clean-slate.md for the post-mortem) in three load-bearing
    ways:

      - **Per-CPU vCPU pool** rather than per-task. N vCPUs equal
        to host CPU count; the UML scheduler picks tasks onto
        vCPUs the same way Linux picks tasks onto host CPUs. KVM's
        "1 host-thread = 1 vCPU for life" contract maps cleanly
        to "1 UML per-CPU pthread = 1 vCPU for life."
      - **TDP / per-region memslots** rather than shadow page
        tables. KVM walks `mm->pgd` directly via per-mapping
        `KVM_SET_USER_MEMORY_REGION` entries; cross-vCPU
        coherence is KVM's mmu_notifier's job, not ours. Net
        delete of ~3-4 kLoC versus v1 / Stage-A.
      - **Kernel-half-only trampoline** at PML4[448] rather than
        a user-half bootstrap page. The trampoline is never
        installed as a US=1 leaf in any user task's page table,
        so the "user-half / kernel-half PML4 alias on the
        bootstrap page" bug class (Stage A's Bug B) is
        structurally impossible.

    TL;DR (AMD Ryzen 7 7840HS / Zen 4, 2026-05-07, post Phase A
    AVX/XSAVE enable; placeholder numbers to be re-captured on
    Phase-J-validated fleet):

      bench-micro getpid (cyc/call, median):
        seccomp                         ~109 000
        kvm-v2 (gadget enabled)          ~95     (≈1050× speedup)
        kvm-v2 (gadget disabled, fall-
          back through KVM_EXIT_IO)     ~9 000-15 000 (varies by host)

      bench-py (Python startup proxy, ms/iter, median):
        seccomp                         501.20
        kvm-v2                          125.13       (4.00× faster)

      perf-py-startup ratio (kvm-v2 / seccomp ceiling 1.20):
        2026-05-07 reading              1.10-1.17    (gate passes)

      mt-mini SMP T=8 ncpus=4 N=400 (post-T54):
        kvm-v2                          400 / 400 = 100.0 %
        seccomp                         400 / 400 = 100.0 %

      cpython-parity 21 stdlib modules (UP + SMP):
        kvm-v2 vs seccomp               21 / 21 PARITY (both flavours)

      threaded-fork-malloc 8w × 500i × 6 boots (24 000 forks):
        kvm-v2                          0 CHILD_FAIL
        seccomp                         0 CHILD_FAIL

      Phase J pilot soak (W=2 × M=20 × 3 short workloads ×
      2 backends, 2026-05-05):                240 / 240 = 100.0 %
      Phase J first 2 h daemon soak (kvm-v2, 4 workloads,
      memcheck/iocheck/stress-ng/tier1-pylibs, 2026-05-14):
                                              200 / 200 = 100.0 %

    The gadget hot path measurements remain consistent with the
    pre-Stage-A fleet bench (the LSTAR gadget bytes themselves
    are byte-shape-compatible with v1's after the gadget revival
    at commit `7ebcd8aac347`, with full RDX/R8/R10 ABI
    preservation per x86_64 ABI), so the gadget:fallback and
    gadget:seccomp ratios published in the earlier 2026-04-24
    fleet bench (~0.001 gadget:fallback, ~0.002 gadget:seccomp)
    are expected to hold within noise on the rebench.

    Patch organization (target: 19 patches, possibly 17-18 after
    the squash audit fuses small fixups):

      Patches 1-3:   Kconfig + lifecycle + per-VM context
                     (memo 26 Phase A — backend probe, register
                     ops table, KVM_CREATE_VM, KVM_SET_TSS_ADDR /
                     IDENTITY_MAP_ADDR).
      Patches 4-6:   per-CPU vCPU pool + TDP memslot
                     registration + KVM_RUN loop skeleton
                     (memo 26 Phases B + C — KVM_CREATE_VCPU
                     per host CPU, SYNC_REGS fast path,
                     memslot allocator, mm_region_added /
                     removed ops, CR3 = __pa(mm->pgd)).
      Patches 7-9:   lazy CPUID install + LSTAR trampoline +
                     kernel-half PML4[448] install + EFER.SCE
                     + MSR programming (memo 26 Phase D —
                     D.0a CPUID, D.1 trampoline, D.4 MSRs,
                     D.4b-pre physmem memslot, D.4b PML4[448]).
      Patches 10-12: IDT/GDT/IST/TSS + exception handling +
                     signal delivery + EINTR-mid-KVM_RUN +
                     per-task FPU swap-out (memo 26 Phases E
                     + F).
      Patches 13-14: SMP — per-CPU IDT/IST/TSS install +
                     cross-vCPU TLB kick + migrate_disable on
                     dispatch + cross-task KVM_SET_SREGS to
                     drop KVM's prev_roots[] (memo 26 Phase G
                     + SMP-T13/T26/T27/T33/T41 fix cycles).
      Patch 15:      lazy FPU + per-vCPU FPU-dirty epoch flag
                     (memo 26 Phase H + SMP-T55 — restores
                     the lazy-FPU perf win while preserving
                     T26/T27 cross-task XMM guarantee).
      Patches 16-17: AVX / XSAVE enable: CR4.OSXSAVE,
                     KVM_SET_XCRS(0x7), CPUID un-mask of
                     XSAVE/OSXSAVE/AVX/AVX2/FMA/F16C
                     (SMP-T57 Phase A).
      Patch 18:      systrap gadget revival — `lstar_gadget.S`,
                     per-vCPU state/vvar pages, swapgs-toggled
                     %gs: field reads, 11 CLASS_E handlers,
                     KUnit byteshape suite,
                     `CONFIG_UM_BACKEND_KVM_V2_GADGET` Kconfig
                     (gadget revival Phases 1-7 +
                     mainstream-readiness items #1-5).
      Patch 19:      Documentation/virt/uml/backends.rst +
                     MAINTAINERS entry.

    Patch count is a starting estimate; the squash audit will
    shape the final count. Anywhere a fix landed on the redesign
    branch as a focused one-shot commit, the upstream patch
    folds the fix into the patch that introduced the surface —
    e.g. SMP-T13's `migrate_disable` belongs in Patch 6 (the
    dispatch loop), not its own patch; SMP-T41's
    `recover_user_rax_from_ist_top56` belongs in Patch 10 (the
    PF handler), not its own patch. Reviewers see the coherent
    final shape, not the redesign walkthrough.

    All patches are gated `CONFIG_UM_BACKEND_KVM_V2`; building
    without it produces a kernel identical to today's. With it
    enabled, the backend auto-selects at boot if `/dev/kvm` is
    present, else falls back to seccomp (or ptrace, per the
    existing backend-selection logic).

    === Why a third backend ===

    UML's existing backends each have tradeoffs the project has
    lived with for years:

      - **ptrace** is portable to any Linux host but every
        syscall costs a tracer round-trip (~14 µs on modern
        silicon). Effectively single-CPU at the host level. No
        fast path. Decades of carry-on.

      - **seccomp** (the 2022 addition) replaces the tracer with
        SECCOMP_RET_USER_NOTIF dispatch via syscall-user-dispatch,
        dropping per-syscall cost into the ~few-microsecond
        range. Requires the guest to cooperate with the seccomp
        filter shape and pins the dispatch path through the
        seccomp infrastructure.

      - **kvm-v2** (this series) uses `/dev/kvm` to run the
        guest in a hardware-enforced ring-3 KVM VM whose CR3
        points at `__pa(mm->pgd)` directly. SYSCALL traps via
        the LSTAR trampoline → `out %al, $0xf4` →
        `KVM_EXIT_IO`. Most syscalls still vmexit to UML's
        `handle_syscall` (the "fallback" path, class A in
        memo 10's taxonomy), but 11 hot-path syscalls are
        serviced entirely in-guest via the gadget (class E).

    The kvm-v2 backend's fallback cost on stock KVM is in the
    same ballpark as seccomp once the SYNC_REGS / KVM_RUN
    pipeline is tight. The value proposition is two-fold:

      1. **Headroom**. Real hardware paging (TDP), real
         hardware ring boundary, real KVM mmu_notifier
         coherence — UML's existing backends pay a syscall-
         interception tax on every guest userspace syscall;
         KVM lets the hot path stay in-guest. The gadget
         monetises this: 27-32 ns / getpid on the validation
         fleet, ~400× faster than ptrace and ~440× faster than
         seccomp on the same workload.

      2. **Time-machine alignment**. UML's strategic
         differentiator (vs. Docker / chroot / gVisor) is
         being a *kernel* with snapshot, record/replay, and
         syzkaller integration. KVM's vCPU state model is
         small, standard, and snapshot-friendly. The v2
         backend was shaped from the start so the
         time-machine work (`02-workstreams/D-kvm-backend/
         12-snapshot-forkserver-kvm.md`, /13-record-replay-
         determinism.md, /14-syzkaller-vm-uml-backend.md)
         ports cleanly. v1's shadow-PT made snapshot
         intractable; v2 makes it tractable.

    === The v2 architecture ===

    Three load-bearing structural choices, each with a
    paragraph of rationale, plus the in-guest gadget for
    completeness.

    **(1) Per-CPU vCPU pool.** v1's per-task vCPU model
    violated KVM's "1 host-thread = 1 vCPU" contract because
    UML's `switch_threads` longjmp'd between tasks on the same
    host thread, so the same vCPU fd was driven by serial
    "virtual" threads of execution. The contract violation
    produced a 5-class bug playbook (memo 19) that ~75 %
    per-trial gate flakiness sat on. v2 reverses the polarity:
    one vCPU per **host CPU**, lifetime-bound to the
    per-host-CPU UML pthread, with the UML scheduler picking
    tasks onto vCPUs. This is the KVM-canonical model.

    The cost is that cross-task transitions on a shared vCPU
    have to be careful about state that's per-task: FPU,
    `prev_roots[]` TLB caches, GS_BASE for the gadget. Each of
    these costs surfaced as a closed SMP-T## fix (T26/T27 FPU
    leak → always `KVM_GET_FPU` then per-vCPU dirty epoch flag;
    T29 fork-snapshot clobber → gate on `last_task`; T33/T47
    prev_roots[] cache → cross-task full `KVM_SET_SREGS` ioctl
    with `WARN_ON_ONCE` on failure; T13 task migration →
    `migrate_disable` around dispatch; T41 user-RAX recovery
    from IST top-56 in EINTR-mid-PF-stub). All known transitions
    are now structurally handled. Decisions-log entries
    D106..D122 catalogue the May 2026 fix cycle.

    **(2) TDP / per-region memslots.** Each guest mapping is
    registered as a `KVM_SET_USER_MEMORY_REGION` entry against
    the per-VM context (one VM per UML invocation under v2's
    current shape; memo 25 R4 outlines a per-mm host worker
    model that v2 doesn't yet adopt — see "known limitations"
    below). KVM walks `mm->pgd` directly; cross-vCPU coherence
    is mmu_notifier's job. There is no shadow page table, no
    shadow PTE encoder, no shadow PGD allocator, no
    eager-shadow-fill, no shadow-#PF-recovery. The substrate
    is 3-4 kLoC smaller than v1.

    The cost is one CR3 swap per cross-mm dispatch — handled
    by writing `__pa(mm->pgd)` into `kvm_run->s.regs.sregs.cr3`
    and OR'ing `KVM_SYNC_X86_SREGS` into `kvm_dirty_regs`. KVM
    consumes on next `KVM_RUN` entry. Same-task re-entries hit
    the SYNC_REGS fast path; cross-task transitions take the
    heavy `__set_sregs2 → kvm_mmu_reset_context` path (closing
    the prev_roots[] LRU cache window per SMP-T33).

    **(3) Kernel-half-only trampoline at PML4[448].** The 5-byte
    trampoline (`out %al, $0xf4; sysretq`) lives at GVA
    `0xffffe00000000040` (PML4 index 448). The PML4[448] entry
    is installed once into `swapper_pg_dir` + `init_mm.pgd`
    explicitly (the original draft assumed an mm_list iteration
    but `arch/um/kernel/skas/mmu.c`'s mm_list isn't exported
    to backend code). Pointing through the kernel-half PT chain
    (`_KERNPG_TABLE` non-leaf flags — `_PAGE_TABLE` has US=1,
    wrong for kernel-half), the chain pages are VM-lifetime
    (never freed until vm_destroy).

    The trampoline is never installed at any user-half VA as a
    US=1 leaf. The user task page tables don't have it. The
    user/kernel canonical-sign-extended boundary at bit 47
    means CPL=3 walks can't reach PML4[448]. Bug B (memo 22)
    — v1's user-half / kernel-half PML4 alias on the bootstrap
    page — is structurally impossible.

    **(4) Systrap gadget.** Opt-in, gated on
    `CONFIG_UM_BACKEND_KVM_V2_GADGET` (default y). With it
    disabled, every syscall takes the slow KVM_EXIT_IO path
    structurally identical to "pre-gadget v2". With it enabled,
    the LSTAR points at a 449-byte assembled gadget (built from
    `arch/um/backend/kvm-v2/lstar_gadget.S`, pattern lifted
    from `arch/x86/kernel/ftrace_64.S`) that:

      - Saves user RDX / R8 / R10 into per-vCPU SAVE_*
        slots on entry; restores on every exit (tail or
        fallback) before SYSRETQ. Full x86_64 syscall ABI
        compliance — the saved registers aren't strictly
        clobbered by the pid-family handlers but the
        restore is uniform for cross-handler hygiene.
      - Dispatches on RAX low byte against a curated set:
        __NR_getpid, __NR_gettid, __NR_getppid, __NR_getuid,
        __NR_geteuid, __NR_getgid, __NR_getegid,
        __NR_sched_yield, __NR_clock_gettime
        (CLOCK_MONOTONIC), __NR_time, __NR_getcpu. Upper-NR
        guard (`cmp $0x135, %eax` plus
        `test $0xffffff00, %eax`) prevents low-byte alias
        hijack.
      - Reads per-vCPU state via swapgs-toggled
        `%gs:disp32` field loads. State page mapped P
        (no US, no RW) at `bootstrap_va + PAGE_SIZE`;
        refresh in `kvm_v2_load_user_sregs` from
        current->tgid/tid/pid/uid/etc. Vvar clock page
        mapped P|RW (no US); seqlock writer in
        `kvm_v2_vvar_refresh`. Call-budget counter bounds
        vvar staleness to ≤ 10 000 calls
        (~300 µs) before forcing a fallback that refreshes.
      - Fallthrough path on bad pointer / unsupported
        clockid / out-of-budget / unrecognised NR: emits
        `out %al, $0xf4` — the exact same path a non-gadget
        syscall takes. POSIX -EFAULT semantics preserved on
        bad user pointers by routing the gadget-mid-store
        #PF back through `handle_syscall` (SMP-T56 / D114
        extended the LSTAR-EINTR carve-out to cover the full
        gadget body; pre-revival the carve-out was a 2-byte
        window).
      - KUnit `kvm_v2_byteshape` suite locks the assembled
        gadget entry preamble (swapgs + 3× `movq %reg,
        %gs:SAVE_*` with expected disp32 values) and a
        page-fit upper bound. Static_asserts in
        `syscall_trap.h` for gadget state-page slot
        alignment + non-overlap. Boot-time memcmp panic on
        LSTAR install/upgrade mismatch.

    Performance: bench-micro getpid 89-105 cyc / call on
    Zen 4 7840HS (vs ~109 000 cyc on seccomp on the same host),
    matching the v1-era fleet bench within noise. ABI test
    suite (11 gadget paths × 3 saved registers) passes 33/33.

    === Security posture ===

    The gadget runs in ring-0 inside the guest VM's own CR3. It
    cannot escape to the host without an exploitable KVM CVE —
    same trust model as any KVM guest.

    Page-table mapping:
      - Trampoline page (LSTAR code, PML4[448] leaf): P only.
        Ring-0 reads/exec work; ring-3 walks can't reach it
        (canonical-sign-extend past bit 47).
      - Gadget state page: P only. Ring-0 reads work via
        `%gs:disp32`; ring-3 faults if it ever tries.
      - Gadget vvar page: P | RW. Same access model as
        trampoline; US dropped so ring-3 can't scrape the
        vvar for side-channel timing.

    Syscall classifier class-D trap list for NRs whose
    successful execution would be semantically dangerous for a
    bare-userspace UML guest — ptrace, reboot, init_module /
    finit_module / delete_module, kexec_load / kexec_file_load,
    bpf, modify_ldt, set_thread_area. Class-D short-circuits
    to -EPERM before `handle_syscall` runs. (Open review
    question: -EPERM vs falling through to -ENOSYS via UML's
    logical syscall_table lookup. Currently -EPERM; see
    "anticipated review questions" below.)

    User RFLAGS preservation across SYSCALL and recoverable
    #PF: DF, CF, PF, AF, ZF, SF, TF, OF all round-trip cleanly
    (audit F2 / D75 — the prior revision clobbered them by
    hardcoding R11 = 0x3202). KUnit covers four input cases
    including `rep movs` after `std` (DF preservation).

    Cross-task FPU isolation under the per-CPU vCPU pool: a
    per-vCPU `fpu_dirty` flag + `fpu_owner_task` pointer gates
    the post-vmexit `KVM_GET_FPU` (SMP-T55 / D119). Skip the
    GET only when both `fpu_dirty=false` AND
    `fpu_owner_task == current`. Cross-task arrival in
    `kvm_v2_load_user_sregs` marks dirty. Validates against
    24 000 cross-task fork transitions ⇒ 0 cross-task XMM
    leak.

    Cross-task TLB / paging coherence: cross-task transitions
    issue a full `KVM_SET_SREGS` ioctl (SMP-T33 / D108 with
    `WARN_ON_ONCE` SMP-T47 / D112), which takes KVM's heavy
    `__set_sregs2 → kvm_mmu_reset_context` path and drops the
    `prev_roots[]` LRU. Same-task re-entries take the
    SYNC_REGS fast path.

    === Testing methodology + results ===

    The reliability gate is **cpython-parity** (`tools/testing/
    selftests/um/cpython-parity/cpython-parity.sh`): 21 curated
    stdlib test modules run under both kvm-v2 and seccomp,
    module-by-module divergence reported. Statuses: PARITY,
    DIVERGE_KVM_REGRESSION, DIVERGE_KVM_BETTER, SKIP_NO_RESULT,
    BOTH_FAIL.

    Current reading (UP and SMP, 2026-05-07 → 2026-05-14):

      TOTAL: parity=21 diverge=0 skip=0

    A wide cpython-parity sweep (134 curated modules,
    `02-workstreams/D-kvm-backend/wide-parity/2026-05-03-
    wide-parity-135-modules.log`) reports 134 PARITY /
    0 REGRESSION / 1 KVM_BETTER / 5 BOTH_FAIL.

    Substrate gate (the regrtest substrate, 25 + 3 + 3):
    kvm-v2 PASS=25 FAIL=3 XFAIL=3, bit-identical to the
    seccomp baseline. The 3 expected fails are upstream-
    regrtest known-fails; the 3 expected-fail items are
    UML-specific paths gated XFAIL upstream.

    SMP stress reproducers under `tools/testing/selftests/um/`:

      mt-mini SMP T=8 ncpus=4 N=400 (post-T54 SOCK_CLOEXEC
      fix):                            400 / 400 = 100.0 %
      mt-yieldonly T=8 N=15:           100.0 %
      mt-rawmmap T=8 N=20:             100.0 %
      mt-mmap-stress T=8:              PASS (post-T22)
      threaded-subprocess-wait × 20:   17-18/20 pre-T22;
                                        10/10 post-T29
      threaded-fork-malloc 8w × 500i × 6 boots:
                                        0 / 24 000 forks fail
                                        (0 CHILD_FAIL)
      threaded-fork-malloc 30 LONG boots × 4000 iters:
                                        29 / 30 boots; 0 / 116 000
                                        forks (1 RCU-stall outlier,
                                        no CHILD_FAIL)
      `make -j4` inside guest (Phase G.3): PASS

    Performance regression gates:

      perf-py-startup ratio_v2_over_seccomp ≤ 1.20 ceiling:
        2026-05-07 reading 1.10-1.17 PASS (SMP-T55 closed
        via per-vCPU FPU-dirty epoch flag at `fd2f9639b0ce`).

      bench-py (Python startup proxy) on Zen 4 7840HS:
        kvm-v2 125.13 ms / seccomp 501.20 ms = 4.00× faster.

      bench-micro getpid post-gadget, post Phase A:
        kvm-v2 89-105 cyc/call / seccomp 109 532 cyc/call
        ≈ 1050× speedup on Zen 4 7840HS.

    Phase J pilot soak (2026-05-05, memo `phase-J-pilot-
    2026-05-05.md`):

      memcheck × 40 each:    kvm-v2 40/40   seccomp 40/40
      iocheck × 40 each:     kvm-v2 40/40   seccomp 40/40
      stress-ng × 40 each:   kvm-v2 40/40   seccomp 40/40
      Total: 240 / 240 = 100.0 %  Wilson 95 % [98.5 %, 100 %]

    First post-Phase-A 2 h Phase J daemon soak (2026-05-14,
    diary `phase-J-soak-2h-2026-05-14.md`):

      memcheck × 50 (kvm-v2):           50/50 = 100 %
      iocheck × 50 (kvm-v2):            50/50 = 100 %
      stress-ng × 50 (kvm-v2):          50/50 = 100 %
      tier1-pylibs × 50 (kvm-v2):       50/50 = 100 %
      Total kvm-v2: 200 / 200 = 100.0 %
        across mmap-heavy, file-IO, IPC, and C-extension
        workloads; 0 panics, 0 throttle pauses, 0 corruption.

    AVX/XSAVE matrix (stress-ng `--vm --verify`, sampled vm-
    methods, Zen 4 7840HS, post-Phase-A):

      Pre-Phase-A:       0 / 14 PASS
      Post-Phase-A:     12 / 14 PASS (mscan + prime-incdec
                        still fail; both use AVX-512
                        `vmovdqa64 %zmm` — Phase B territory)

    Per-instruction validation:
      `vpxor %xmm6, %xmm6, %xmm6` (VEX/AVX-128):  PASS
      `vpxor %ymm0, %ymm0, %ymm0` (VEX/AVX-256):  PASS
      Manual AVX_TEST in-guest: CPUID reports
        avx_bit=1 xsave_bit=1 osxsave_bit=1;
        xgetbv returns 0x7 (FP | SSE | YMM).

    Userspace ABI test (gadget RDX/R8/R10 preservation across
    11 syscalls × 3 saved registers, `tools/testing/selftests/
    um/abi-rdx-r8-r10/`): 33 / 33 PASS.

    KUnit suites (under `arch/um/backend/kvm-v2/`):

      kvm_v2_marshal       8 / 8 PASS
      kvm_v2_byteshape     9 / 9 PASS (incl. 2 gadget cases)

    === Known limitations + follow-ups ===

    Items NOT fixed by this series and acknowledged as
    follow-on work:

      - **AVX-512 (Phase B of SMP-T57)**. `stress-ng --vm
        --vm-method=mscan` and `prime-incdec` emit
        `vmovdqa64 %zmm6, %zmm7` in their tail-merge
        verification routine; those two vm-methods still
        SIGILL under kvm-v2 post-Phase-A. Phase B un-masks
        leaf-7 AVX-512 EBX/ECX/EDX bits and bumps XCR0 to
        include OPMASK / ZMM_Hi256 / Hi16_ZMM (XCR0 bits
        5/6/7), plus a `struct kvm_fpu` (512 B FXSAVE-shape)
        → `struct kvm_xsave` marshal switch for the wider
        register file. Memo `state-audit/25` §3.2 + §5.1
        catalogues the work. Not gating: 12-of-14 vm-methods
        is the substantial improvement; AVX-512 isn't on the
        Phase J critical path. Will land as a focused
        follow-up series.

      - **SMP-T55 stretch — the +50 % UP→UP perf hop in the
        2026-04-30 → 2026-05-02 build window**. perf-py-
        startup historically read 0.44 (kvm-v2 ~2.25× faster
        than seccomp), now reads 1.10-1.17 (gate ceiling
        1.20). The closed-gate fix (per-vCPU FPU-dirty epoch)
        recovered most of the FPU-side regression; the
        residual is a clutch of SMP-correctness commits
        (TLB-kick infra `7e1c255a09ad` / `9f0ff6257e8b`,
        `migrate_disable` `95b3a85bd309`, EINTR-mid-PF inline
        `e5977806fd14`). Bisecting it is deferred until a
        gate failure justifies the focused investigation
        budget. Tracked at memo `state-audit/23` §6.2.

      - **Per-mm host worker model (memo 25 R4)**. v2 currently
        runs all UML tasks under a single per-VM context.
        Memo 25's "per-mm host worker process" refactor — each
        guest mm becomes its own host process with its own VM
        context — was deferred during the Phase A→I sprint
        because the substrate didn't need it to reach
        cpython-parity. With Phase J validating the substrate
        on realistic workloads, the question reopens: is the
        per-mm worker model a v2-incremental change, or does
        it warrant a v3 series? Decision deferred until Phase
        J DONE.

      - **x86_64 only**. The trampoline is 5 bytes of x86_64
        asm, the LSTAR gadget is hand-assembled x86_64, and
        the memslot installer uses `__pa(pgd)` semantics
        specific to x86's 4-level paging. Porting to arm64
        (`02-workstreams/D-kvm-backend/14-syzkaller-vm-uml-
        backend.md` §"arm64") or RISC-V is a separate series
        per vision items V.5 and V.6. The dispatch / vCPU
        pool / TDP shape is arch-agnostic and translates;
        the asm tail is the arch-specific bit.

      - **No KSM / KSAN guest-side**. KVM's host-side memory
        compression / KASAN integration applies normally to
        the UML guest, but the guest itself doesn't yet wire
        through the host shadow infrastructure. Deferred
        cleanup, not a regression.

      - **Snapshot / record-replay TUs are stubs at v2**.
        `arch/um/backend/kvm-v2/snapshot.c` Phase 1 landed
        2026-05-16 (commit `aa4cd328102c`); the full capture
        path is stubbed (`-EOPNOTSUPP`) pending Phase 2-7.
        See PLAN-2026-05-14 §4 for the time-machine track.

    Closed-since-2026-04-27 fix-cycle catalogue (for reviewer
    credibility — issues we ALREADY fixed since this draft's
    earlier revision, with decisions-log refs in
    `Documentation/virt/uml/redesign/04-risks/decisions-log.md`):

      D106  SMP-T26/T27   FPU cross-task leak — always
                          KVM_GET_FPU after KVM_RUN.
      D107  SMP-T29       fork-snapshot clobber — gate
                          capture_for_switch_out on last_task.
      D108  SMP-T33       cross-task KVM_SET_SREGS to drop
                          KVM prev_roots[] (later
                          superseded by T41 as the canonical
                          fix; T33 left as defensive
                          hygiene).
      D109  SMP-T36       mmu_gather batch-free walk every
                          batch under CONFIG_UML.
      D110  SMP-T37       pin per-CPU UML host pthread CPU
                          affinity (defense-in-depth).
      D111  SMP-T41       recover user RAX from IST top-56
                          in EINTR-mid-PF-stub (the true
                          mt-mini residual closure).
      D112  SMP-T47       WARN_ON_ONCE on cross-task
                          KVM_SET_SREGS failure
                          (mainstream-readiness #6).
      D113  Gadget revival Phases 1-7 + items #1-5 —
                          `lstar_gadget.S`, ABI preservation,
                          KUnit byteshape, Kconfig.
      D114  SMP-T56       extend LSTAR-EINTR carve-out to
                          cover gadget body (post-revival).
      D115  SMP-T54       SOCK_CLOEXEC on worker socketpair
                          + umlctl sysrq halt verb (mt-mini
                          init.sh-hang closure).
      D116  perf-O1       gate post-syscall interrupt_end()
                          on -ERESTART or _TIF_WORK_MASK
                          (~120 cyc/syscall save).
      D117  Phase J pilot soak rig.
      D118  SMP-T55       perf-py-startup gate failure surfaced
                          + policy decision: do NOT loosen
                          the ceiling.
      D119  SMP-T55       closed via per-vCPU FPU-dirty epoch
                          flag.
      D120  Phase J #167  daemon-mode driver landed.
      D121  SMP-T57 A     AVX/XSAVE enabled.
      D122  Phase J 2h    first 200 / 200 sustained soak on
                          kvm-v2.

    === MAINTAINERS routing ===

      Primary list: linux-um@lists.infradead.org
      Cc:           kvm@vger.kernel.org
                    Paolo Bonzini <pbonzini@redhat.com>
                    Sean Christopherson <seanjc@google.com>
                    Richard Weinberger <richard@nod.at>
                    Johannes Berg <johannes@sipsolutions.net>
                    Anton Ivanov <anton.ivanov@cambridgegreys.com>
                    Benjamin Berg <benjamin@sipsolutions.net>
                    Tiwei Bie <tiwei.btw@antgroup.com>
                    linux-arch@vger.kernel.org

    `scripts/get_maintainer.pl` should be re-run against the
    rebased + squashed series before emission. MAINTAINERS
    entry to be added under "USER MODE LINUX" with a sub-
    section for `arch/um/backend/kvm-v2/` listing the same
    UML maintainers as primary plus a KVM contact (Paolo or
    Sean) for the TDP / SREGS / MSR / XCR0 surface.

    === Dependencies on Series 1-6 ===

    Series 7 (this one) depends on:

      Series 4 (backend-ops-abstraction-rfc) — landed
      upstream first.
        Series 7 patches 1-3 wire kvm-v2 into the typed
        `struct um_backend_ops` Series 4 introduces.
        Sending 7 before 4 forces reviewers to reconstruct
        the dispatch shape from scratch.

    Series 7 is independent of:

      Series 1 (bpf-hygiene-v1)            — pure x86 BPF
                                              hygiene.
      Series 2 (kmsan-arch-callback-rfc)   — mm callback
                                              shape.
      Series 3 (ftrace-notrace-generic-v1) — three notrace
                                              annotations on
                                              signal-adjacent
                                              kernel helpers.
      Series 5 (static-key-hot-paths)      — orthogonal to
                                              the kvm-v2
                                              dispatch path;
                                              v2 compiles
                                              with or without.
      Series 6 (per-profile C-series)      — gates on
                                              Series 7 in the
                                              other direction
                                              (some profiles
                                              want kvm-v2 to
                                              be available
                                              before their
                                              feature audit
                                              makes sense).

    The local submission queue at
    `Documentation/virt/uml/redesign/upstream-patches/
    SUBMISSION-QUEUE.md` is authoritative for the
    inter-series dependency graph.

    === Outstanding review questions ===

    Items where reviewer input would particularly help:

      (a) **Per-CPU vCPU pool vs. per-task vCPU.** v2 picks
          per-CPU; linux-um maintainers may push back wanting
          per-task, citing simpler reasoning about
          per-task state. The trade-off is laid out in memo 26
          §C and decision D106..D112. Short answer: per-task
          violates KVM's 1-host-thread-1-vCPU contract under
          UML's `switch_threads` longjmp model and we tried
          it (v1 archive), per-CPU is what KVM expects and
          what TDP / mmu_notifier are shaped for.

      (b) **Class-D classifier — -EPERM vs -ENOSYS.** The
          class-D trap list (ptrace, reboot, init_module,
          finit_module, delete_module, kexec_load,
          kexec_file_load, bpf, modify_ldt, set_thread_area)
          short-circuits to -EPERM before `handle_syscall`
          runs. -EPERM is more precise ("refused here, not
          missing") but -ENOSYS might match what some
          programs expect (and is what UML's logical
          syscall_table lookup would return for unimplemented
          NRs). Currently -EPERM; happy to change.

      (c) **Per-mm host worker model deferral.** Memo 25's R4
          refactor (each guest mm becomes its own host
          process with its own VM context) would close a
          theoretical cross-mm fault-routing class but isn't
          on the path to today's gates passing. Should this
          series land first under the single-VM model (with
          per-mm worker as a v3 series follow-up) or should
          the per-mm worker land as part of the initial v2
          drop? The single-VM model is what's tested and
          validated; the per-mm worker is what memo 25 specs
          but doesn't have the bench evidence behind it.

      (d) **Gadget Kconfig default — y or n.** Today's
          recommendation is `CONFIG_UM_BACKEND_KVM_V2_GADGET
          = y` (default y) because the gadget is the
          headline perf differentiator. A more conservative
          default-n would mean reviewers see the slow path
          first and the gadget as an opt-in optimisation.
          The user-space ABI test (RDX/R8/R10 across 11
          gadget syscalls) passes 33/33, so we have evidence
          for default-y; but the conservative choice is
          worth flagging.

    === Reproducing the numbers ===

    Build kvm-v2:

      make ARCH=um O=~/src/uml-builds/uml-smp \
          olddefconfig
      scripts/config --file ~/src/uml-builds/uml-smp/.config \
          --enable UM_BACKEND_KVM_V2 \
          --enable UM_BACKEND_KVM_V2_GADGET \
          --enable SMP \
          --set-val NR_CPUS 4
      make ARCH=um O=~/src/uml-builds/uml-smp \
          olddefconfig
      make ARCH=um O=~/src/uml-builds/uml-smp -j$(nproc)

    Run the gates:

      # Substrate parity
      tools/testing/selftests/um/cpython-parity/cpython-parity.sh

      # Bench-micro getpid (gadget hot path)
      tools/testing/selftests/um/bench-micro/run.sh getpid

      # Bench-py (Python startup proxy)
      tools/testing/selftests/um/bench-py/run.sh

      # mt-mini SMP soak
      tools/testing/selftests/um/mt-mini/run.sh \
          --threads 8 --ncpus 4 --iters 400

      # Phase J daemon (2 h sample)
      tools/testing/selftests/um/soak/run-soak-daemon.sh \
          --budget-sec 7200 \
          --workloads memcheck,iocheck,stress-ng,tier1-pylibs

    Per host CPU + KVM extension requirements:

      - x86-64 host with /dev/kvm accessible.
      - KVM_CAP_SYNC_REGS (required — KVM ≥ 4.16).
      - KVM_CAP_SET_GUEST_DEBUG (required).
      - User in the `kvm` group or equivalent permission.
      - AMD Zen 2 / Intel Skylake or newer for the fleet
        bench (the host fleet `s0`-`s7` documented in
        `02-workstreams/D-kvm-backend/measurements.md` spans
        Skylake / Kaby / Alder Lake P+E / Zen 4).

    === Changelog ===

    RFC v2 (this draft): full rewrite. v1 (per-task / shadow
    PT) archived at `arch/um/backend/kvm-v1-archive/` (gated
    on BROKEN). v2 substrate is per-CPU vCPU pool + TDP +
    kernel-half-only PML4[448] trampoline + revived in-guest
    LSTAR gadget. 14 SMP-T## fix cycles closed (D106..D122).
    AVX/XSAVE enabled (SMP-T57 Phase A). Phase J validation in
    progress; this series gated on Phase J DONE.

    v1: 2026-04-27 initial Stage-A draft.

    Michael Bommarito (19):
      um: kvm-v2: Kconfig + backend probe + ops registration
      um: kvm-v2: per-VM context lifecycle (KVM_CREATE_VM,
                  TSS, IDENTITY_MAP_ADDR)
      um: kvm-v2: CPUID curation + capability negotiation
      um: kvm-v2: per-CPU vCPU pool (KVM_CREATE_VCPU,
                  SYNC_REGS)
      um: kvm-v2: TDP memslot registration via
                  KVM_SET_USER_MEMORY_REGION
      um: kvm-v2: task → vCPU dispatch + migrate_disable +
                  cross-task SREGS
      um: kvm-v2: lazy CPUID first-run install + EFER.SCE
      um: kvm-v2: LSTAR trampoline + IO-port syscall trap
      um: kvm-v2: kernel-half PML4[448] install + physmem
                  identity memslot
      um: kvm-v2: IDT/GDT/IST/TSS + exception handlers via
                  IO ports
      um: kvm-v2: signal delivery + EINTR-mid-KVM_RUN +
                  per-task FPU swap-out
      um: kvm-v2: PF-stub user-RAX recovery (EINTR-mid-stub
                  carve-out)
      um: kvm-v2: SMP — per-CPU IDT/IST/TSS install
      um: kvm-v2: SMP — cross-vCPU TLB kick + KVM_REQ
                  request mechanism
      um: kvm-v2: per-vCPU FPU-dirty epoch flag (lazy FPU)
      um: kvm-v2: AVX/XSAVE enable (CR4.OSXSAVE +
                  KVM_SET_XCRS + CPUID un-mask)
      um: kvm-v2: XCR0 = FP|SSE|YMM via lazy first-dispatch
                  arming
      um: kvm-v2: systrap gadget — lstar_gadget.S, state
                  page, 11 CLASS_E handlers, ABI preserve,
                  KUnit byteshape
      Documentation/virt/uml: KVM v2 backend + systrap
                              gadget + MAINTAINERS

     arch/um/Kconfig                                   |  XX +
     arch/um/backend/kvm-v2/Kconfig                    |  XX +
     arch/um/backend/kvm-v2/Makefile                   |  XX +
     arch/um/backend/kvm-v2/init.c                     | XXX +
     arch/um/backend/kvm-v2/context.c                  | XXX +
     arch/um/backend/kvm-v2/vcpu.c                     | XXX +
     arch/um/backend/kvm-v2/memslot.c                  | XXX +
     arch/um/backend/kvm-v2/region.c                   | XXX +
     arch/um/backend/kvm-v2/syscall_trap.c             | XXX +
     arch/um/backend/kvm-v2/syscall_trap.h             |  XX +
     arch/um/backend/kvm-v2/exception.c                | XXX +
     arch/um/backend/kvm-v2/lstar_gadget.S             | XXX +
     arch/um/backend/kvm-v2/test_marshal.c             | XXX +
     arch/um/backend/kvm-v2/test_byteshape.c           | XXX +
     arch/um/backend/kvm-v2/kvm_v2_backend.h           | XXX +
     arch/um/backend/kvm-v2/ops.c                      |  XX +
     arch/um/backend/kvm-v2/README.md                  | XXX +
     Documentation/virt/uml/backends.rst               |  XX +
     MAINTAINERS                                       |   X +
     19 files changed, XXXX insertions(+)
     create mode 100644 arch/um/backend/kvm-v2/Kconfig
     create mode 100644 arch/um/backend/kvm-v2/Makefile
     create mode 100644 arch/um/backend/kvm-v2/init.c
     create mode 100644 arch/um/backend/kvm-v2/context.c
     create mode 100644 arch/um/backend/kvm-v2/vcpu.c
     create mode 100644 arch/um/backend/kvm-v2/memslot.c
     create mode 100644 arch/um/backend/kvm-v2/region.c
     create mode 100644 arch/um/backend/kvm-v2/syscall_trap.c
     create mode 100644 arch/um/backend/kvm-v2/syscall_trap.h
     create mode 100644 arch/um/backend/kvm-v2/exception.c
     create mode 100644 arch/um/backend/kvm-v2/lstar_gadget.S
     create mode 100644 arch/um/backend/kvm-v2/test_marshal.c
     create mode 100644 arch/um/backend/kvm-v2/test_byteshape.c
     create mode 100644 arch/um/backend/kvm-v2/kvm_v2_backend.h
     create mode 100644 arch/um/backend/kvm-v2/ops.c
     create mode 100644 arch/um/backend/kvm-v2/README.md

    --
    2.XX.X

---

End of draft. Post-Phase-J-DONE + post-squash pass, this
becomes the real `0000-cover-letter.patch` with the filled-in
diffstat and the re-captured fleet bench numbers.
