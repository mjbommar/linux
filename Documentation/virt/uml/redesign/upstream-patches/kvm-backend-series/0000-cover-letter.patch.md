# 0000-cover-letter.patch — draft

Draft form of the cover letter that will head the 15-patch
series emitted from `git format-patch`. Not yet a `.patch`
file because the patches themselves haven't been squashed from
the branch. Final `.patch` generation happens after Series 4
(backend-ops-abstraction-rfc) lands and the squash pass
completes.

## 2026-04-27 update — Stage A redesign landed

Post-Stage-A architectural state (commit `7f94922a356f`) — to be
folded into the cover letter when this series is squashed:

- The pre-Stage-A KVM backend ran every UML task on a singleton
  vcpu0_fd via cooperative `switch_threads`/longjmp on the same
  host thread. This violated KVM's API contract ("1 host thread
  = 1 vCPU for life", per-vCPU run mmap is single-writer,
  vcpu ioctls host-thread-bound) and produced a 5-class race
  playbook (memo 19) that ~75% per-trial gate flakiness sat on.
- Stage A allocates a `struct kvm_vcpu_handle` per UML task,
  pinned to `current->thread.arch.kvm.vcpu` for the task's
  lifetime, freed by `exit_thread()`. KVM_SET_SIGNAL_MASK
  installed at vCPU creation blocks every host signal except
  SIGALRM (timer-driven preemption) and KVM_UM_KICK_SIGNAL
  (SMP eviction primitive). Singleton vcpu0_fd deleted under
  `CONFIG_UM_BACKEND_KVM_INTEGRATED`.
- Architectural rationale: 4 independent agent reviews
  (gVisor / production-VMMs / first-principles / radical-PKU)
  all converged on "per-task vCPU + KVM_SET_SIGNAL_MASK" as
  the minimal structural fix. See
  `Documentation/virt/uml/redesign/03-architecture-review-2026-04-27/`
  for the synthesis and individual analyses.
- Stage A.4d (per-vCPU `last_flushed_tlb_gen` vs per-shadow
  `tlb_gen`): scaffolding landed; consumer-side activation
  deferred until a CLONE_VM-shared-mm stress test exposes the
  cross-task TLB staleness deterministically. cpython-parity
  gate runs each module in its own process (single-mm, single-
  task) so doesn't exercise the cross-task TLB race.
- Stage B (next): replace the multi-writer shadow PT with TDP/EPT
  via per-mm memslots (`KVM_SET_USER_MEMORY_REGION`) + a shared
  kernel-half PGD (PML4[256..511]). Design memo at
  `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/20-stage-b-design.md`.
  Closes Race classes B/C/D structurally; net delete of ~3-4k LOC.
- Stage C (after B): cleanup, KUnit suite update, 24h soak.

The Stage A→B→C plan supersedes the iterative bug-hunt approach
of memos 16-19. For the upstream submission, Stage A is shippable
on its own as a foundation; Stage B follows as a separate series
once side-by-side TDP+shadow validation completes (B.6).

---

    Subject: [PATCH RFC 00/15] um: add KVM backend with in-guest systrap gadget

    From: Michael Bommarito <michael.bommarito@gmail.com>

    This series adds a third execution backend to User Mode Linux
    alongside the existing ptrace and seccomp backends. KVM replaces
    the tracer-round-trip syscall dispatch (~52000 cyc per getpid on
    Xeon W-2123 via ptrace, ~41000 via seccomp) with hardware-enforced
    ring-3 execution inside a KVM-managed VM, bringing per-syscall
    fallback cost down to ~165000 cyc (dominated by the VMEXIT round-
    trip through UML's handle_syscall) and adding an optional in-guest
    "systrap gadget" fast path that services hot-path syscalls at
    ~100 cyc each (26-34 ns depending on silicon, measured across 8
    fleet hosts spanning Intel Skylake / Alder Lake P+E / AMD Zen 4).

    TL;DR (Xeon W-2123, post-round-6-audit-closure 2026-04-24):

      ptrace      53000 cyc / 14750 ns per getpid (baseline)
      seccomp     43000 cyc / 11970 ns per getpid (baseline)
      kvm-fallb  163000 cyc / 45250 ns per getpid (VMEXIT path)
      kvm-gadget     97 cyc /    27 ns per getpid (LSTAR-trap path)

    gadget:seccomp ratio is 0.002 (seccomp ~440× slower); gadget:
    kvm-fallback ratio is 0.001. The gadget cleared a pre-registered
    <100 ns design target on every host in the validation fleet
    (8 hosts spanning Intel Skylake / Skylake-SP / Kaby / Alder
    Lake P+E / AMD Zen 4) with 3.1×-4.3× margin.

    Organization:

      Patches 1-3:   backend shell, Kconfig, KVM_RUN loop skeleton,
                     reg-marshalling
      Patches 4-6:   shadow page table (allocator, PTE encoder,
                     eager + lazy fill, mm_map/unmap wiring)
      Patches 7-9:   bootstrap page (GDT + LSTAR + IDT + #PF + TSS
                     + IST) and kvm_enter_guest
      Patches 10-11: KVM_RUN dispatcher + syscall classifier
      Patches 12-14: systrap gadget (state channel + 11 handler
                     bodies + gadget-fault → -EFAULT)
      Patch 15:      Documentation + MAINTAINERS

    The gadget is gated on CONFIG_UM_BACKEND_KVM_GADGET (default n).
    With it disabled, the KVM backend falls back to the VMEXIT
    dispatcher on every syscall and behaves semantically identically
    to the ptrace / seccomp backends — same -EFAULT on bad user
    pointers, same handle_syscall path for every NR.

    === Why a third backend ===

    UML's existing backends each have tradeoffs the project has
    lived with for two decades:

      - ptrace is portable to any Linux host but every syscall costs
        a tracer round-trip (~14 us on modern silicon). Single-CPU
        only. No TLS fast-path.

      - seccomp (the 2022 addition) replaces the tracer with
        SECCOMP_RET_USER_NOTIF dispatch, dropping per-syscall cost
        to ~11 us. Requires the guest to cooperate with the seccomp
        filter shape and pins the dispatch path through the
        seccomp syscall-user-dispatch infrastructure.

      - KVM (this series) uses /dev/kvm to run the guest in a
        hardware-enforced ring-3 VM whose CR3 points at a shadow
        page table we walk from UML's logical pgd. SYSCALL traps
        into our LSTAR handler via hardware; most syscalls still
        VMEXIT to handle_syscall (the "fallback" path, class A in
        memo 10's taxonomy), but 11 hot-path syscalls are serviced
        entirely in-guest via the gadget (class E).

    The KVM backend's fallback cost (~46 us per syscall) is worse
    than seccomp on paper. The value proposition is the gadget:
    28 ns per getpid beats both existing backends by 400-500×, and
    glibc-linked workloads call getpid / clock_gettime / gettid
    enough that the amortized cost across a realistic workload
    dominates the microbenchmark comparison.

    === What the gadget is ===

    The LSTAR trampoline in our bootstrap page contains 327 bytes of
    hand-assembled x86_64 code that implements a ~10-entry syscall
    dispatch table plus handler bodies for:

      __NR_getpid, __NR_gettid, __NR_getppid, __NR_getuid,
      __NR_geteuid, __NR_getgid, __NR_getegid, __NR_sched_yield,
      __NR_clock_gettime(CLOCK_MONOTONIC), __NR_time, __NR_getcpu

    The seven pid-family handlers read their value from a per-vCPU
    state page mapped at %gs:0..0x20 — the page is refreshed from
    current->tgid/tid/pid/uid/etc. at every kvm_enter_guest, and
    MSR_KERNEL_GS_BASE points to it so the gadget's swapgs bracket
    activates the read.

    clock_gettime reads a seqlock-protected vvar page at
    %gs:0x1000+ populated with live ktime_get_ns(). A call-budget
    counter bounds vvar staleness to at most 10000 gadget calls
    (~300 us) before forcing a fallback that refreshes the page —
    without it, a tight gadget clock loop never VMEXITs and
    time freezes for the duration of the loop.

    sched_yield returns 0 without actually yielding (UML's
    scheduler runs on the next VMEXIT; sched_yield is advisory per
    POSIX). time reads REAL_SEC from the same vvar page. getcpu
    reads cpu_id from the state page.

    Every handler ends in `sysretq`. If the handler's fast path
    doesn't apply (bad pointer, unsupported clockid, out-of-budget,
    etc.), the gadget falls through to a common `out %al, $0xf4`
    trampoline which triggers KVM_EXIT_IO — the exact same path a
    non-gadget syscall takes.

    The LSTAR table is a static bytes array. A KUnit test
    byte-matches the live table against an expected copy so any
    reshuffle gets caught at boot, not production.

    === Security posture ===

    The gadget runs in ring-0 inside the guest VM's own CR3. It
    cannot escape to the host without an exploitable KVM CVE, same
    trust model as any KVM guest.

    Shadow page table mapping:
      - bootstrap page (GDT, IDT, LSTAR code, #PF handler, TSS, IST
        stack): P | RW, no US. Ring-0 accesses work; ring-3 faults.
      - gadget state page: P only. Ring-0 reads work; ring-3 faults.
      - gadget vvar page: P | RW, no US. Same access model as
        bootstrap; US dropped so ring-3 can't scrape the vvar for
        side-channel timing.

    The syscall classifier has a class-D "trap" list for NRs whose
    successful execution would be semantically dangerous for a
    bare-userspace guest — ptrace, reboot, init_module / finit_
    module / delete_module, kexec_load / kexec_file_load, bpf,
    plus modify_ldt + set_thread_area (which UML does not
    virtualize through KVM). class-D short-circuits to -EPERM
    before handle_syscall runs.

    User RFLAGS round-trip through SYSCALL and recoverable #PF
    preserves DF, CF, PF, AF, ZF, SF, TF, OF correctly (a prior
    revision clobbered them by hardcoding R11 = 0x3202). KUnit
    covers four input cases including the notorious `rep movs
    after std` DF-preservation scenario.

    Upper-NR guard in the dispatch prologue rejects any RAX with
    bits 8..31 set (except the specifically-handled 0x135 =
    getcpu), preventing syscall-number aliasing attacks where a
    guest could call utimensat (NR 280 = 0x118) to hit the
    sched_yield handler (NR 24 = 0x18) via the low-byte `cmp %al,
    imm8` dispatch.

    === Measurement artifacts ===

    Reproducing the numbers in this cover letter:

      # Build two UML kernels — one with gadget, one without.
      make ARCH=um O=/tmp/uml-kvmint olddefconfig
      make ARCH=um O=/tmp/uml-kvmint -j$(nproc)
      scripts/config --file /tmp/uml-kvmbench/.config \
          -e UM_BACKEND_KVM_GADGET
      make ARCH=um O=/tmp/uml-kvmbench olddefconfig
      make ARCH=um O=/tmp/uml-kvmbench -j$(nproc)

      # Dual-binary perf-getpid gate:
      UML_BINARY=/tmp/uml-kvmint/linux \
          UML_GADGET_BINARY=/tmp/uml-kvmbench/linux \
          bash tools/testing/selftests/um/perf-getpid/run-perf-getpid.sh

    On our dev host (Xeon W-2123 / Skylake-SP), post-round-6
    audit closure:

      PERF_GETPID: backend=ptrace       cyc_per_call=53128
      PERF_GETPID: backend=seccomp      cyc_per_call=43112
      PERF_GETPID: backend=kvm-fallback cyc_per_call=162901
      PERF_GETPID: backend=kvm          cyc_per_call=97
      PERF_GETPID: GADGET_SUMMARY ratio_gadget_over_fallback=0.001
      PERF_GETPID: PASS (gadget gate also green)

    Fleet bench (s0-s7, covering Intel Skylake / Kaby Lake /
    Skylake-SP / Alder Lake P+E / AMD Zen 4 × 3); numbers
    captured 2026-04-24 after audit round 6 closed:

      Host         CPU                      gadget_cyc  gadget_ns  margin
      s0  i9-12900K (Alder Lake P)           97         30         3.3×
      s1  Xeon E3-1225 v6 (Kaby Lake)       100         31         3.2×
      s2  Xeon E3-1225 v5 (Skylake)         103         32         3.1×
      s3  Xeon W-2123 (Skylake-SP)           97         27         3.7×
      s4  i5-12600K (Alder Lake E)          115         31         3.2×
      s5  Ryzen 7 7840HS (Zen 4)             87         23         4.3×
      s6  Ryzen 7 7840HS (Zen 4)             88         23         4.3×
      s7  Ryzen 7 7840HS (Zen 4)             88         23         4.3×

    margin = 100 ns target / gadget_ns. Cleared on every host
    with ≥3.1× headroom.

    clock_gettime gadget cost (post-G1 bounds check + RAX-
    preservation restructuring) measured at 122-139 cyc /
    36-40 ns on the hosts with valid runs (s1, s3, s4) —
    still ≥2.5× margin under the 100 ns target. The bounds
    check is a per-call ~25 cyc overhead that buys
    correctness on supervisor-VA + non-canonical user
    pointers.

    === Relation to other upstream series ===

    This series depends on the ops-table abstraction introduced by
    "[PATCH RFC 00/12] um: backend ops abstraction" (Series 4 per
    the local submission queue; commit ids when landed). Patches
    1-2 include the KVM ops-table entry point alongside the
    existing ptrace + seccomp entries, gated by CONFIG_UM_BACKEND_
    KVM.

    The static-key gates series ("um: static-key hot-path
    gating", Series 5) is orthogonal — this series compiles with
    the key infrastructure present or absent.

    === Known limitations ===

    The following are not fixed by this series and are acknowledged
    as follow-on work. All audit findings through round 6
    (P0 / P1 / P2 / P3 across rounds 4 + 5 + 6) are closed except
    the two P2 follow-ons listed first below.

      - **Bootstrap page is mapped RW with no code/data split.**
        The minimum F5 fix dropped the ring-3 US bit (D81); the
        page is still ring-0-writable and contains both the LSTAR
        code and the IST stack. A defense-in-depth split into
        RO code page + RW data page (D86 / D90 references) is
        tracked as redesign branch task #230 and will land in a
        focused follow-on.

      - **Eager kvm_touch_all_user_vmas read probe.** The eager
        per-page copy_from_user that pre-populates UML's pgd is
        a O(VA-space) cost on every kvm_enter_guest. A first
        attempt at dropping it triggered an infinite #PF loop
        because the host-side copy_from_user from #PF-recovery
        context doesn't trigger UML's vm_fault the same way a
        guest access would. The proper fix is to reroute the
        #PF recovery through `handle_mm_fault`; tracked as
        task #238.

      - **Shadow-PT TLB flush.** Relies on kvm_enter_guest's
        per-entry KVM_SET_SREGS(CR3) flushing the guest TLB.
        Source review of arch/x86/kvm/x86.c::kvm_set_cr3 (D89)
        confirms the same-CR3 path still calls
        kvm_invalidate_pcid; a CR3-toggle workaround is unneeded
        on current upstream KVM but staged in case a future KVM
        ever skips flushes on same-CR3.

      - **Only x86_64.** The gadget bytes are hand-assembled
        x86_64 and the shadow-PT encoder hardcodes the x86 PTE
        layout. Porting to arm64 or other arches is a future
        series — the dispatcher / shell / KVM_RUN loop are
        arch-agnostic but the LSTAR gadget table is not.

    Audit-closed findings (for reviewer credibility — list of
    issues we ALREADY fixed, with decisions-log refs in
    `Documentation/virt/uml/redesign/04-risks/decisions-log.md`):

      A1/A2/A4 — per-trap interrupt_end, CPL-derived is_user,
        loud writeback failures (D70).
      F1     — Layer-1 probe refactor scope confirmed (D76).
      F2     — user RFLAGS preserved across SYSCALL + #PF (D75).
      F3     — coverage closed via existing perf-getpid + DF
               selftest follow-on (D77, D88).
      F4     — upper-NR guard prevents low-byte alias hijack
               (D75 in G6-followon).
      F5     — bootstrap page US dropped (D81).
      F6     — kvm_shadow_invalidate_va_range + mm_unmap wiring
               (D82).
      F7/1   — gadget-mid-store fault diverts to handle_syscall
               for proper -EFAULT (D86).
      F7/2   — clock_gettime seqlock %edx (RAX preserved) (D75).
      F8     — vvar call-budget bounds staleness ≤300 µs (D83).
      F9     — kvm_touch_all_user_vmas write-back removed (D85).
      F10    — class B/C reconciled with dispatcher (modify_ldt
               + set_thread_area demoted to D) (D84).
      G1     — gadget bounds check via TASK_SIZE_CAP (D93).
      G2     — shadow PGD user-half cleared on cross-mm switch
               (D90).
      G3     — #PF error code propagated; RO-write touched gate
               fixed (D92).
      G4     — ncpus=1 enforced in kvm_init (D91).
      G5     — sched_yield demoted from class E to A (D94).
      G8     — Kconfig + DF-selftest doc drift fixed (D95
               equivalent rolled into the same commit).

    === Outstanding review questions ===

    Three items where reviewer input would particularly help:

      (a) Is a per-vCPU MSR_KERNEL_GS_BASE pointing at a
          kernel-allocated page an acceptable pattern for in-guest
          state channels? Or should we move this to a dedicated
          KVM ioctl?

      (b) The shadow-PT invalidation currently flags kvm_um.
          shadow_dirty and defers the actual flush to
          kvm_enter_guest's SREGS reload. Would maintainers
          prefer an explicit KVM_REQ_TLB_FLUSH-style mechanism
          here?

      (c) The classifier's class-D trap list ends up with 10
          entries (ptrace, reboot, init_module, finit_module,
          delete_module, kexec_load, kexec_file_load, bpf,
          modify_ldt, set_thread_area). Should any of these
          pass through to handle_syscall (which would return
          -ENOSYS on UML's logical syscall_table lookup) instead
          of -EPERM at the dispatcher layer? -EPERM is more
          precise ("refused here, not missing") but -ENOSYS
          might match what some programs expect.

    === Changelog ===

    v1: initial post.

    Michael Bommarito (15):
      um: kvm: Kconfig + shell lifecycle ops
      um: kvm: memslot registration + basic KVM_RUN loop
      um: kvm: vcpu0 creation + kvm_regs ↔ uml_pt_regs marshalling
      um: kvm: shadow PGD allocator + x86 PTE encoding
      um: kvm: eager shadow fill + lazy #PF-on-demand path
      um: kvm: mm_map / mm_unmap wire the shadow PT
      um: kvm: bootstrap page (GDT + LSTAR + SYSRET gadget)
      um: kvm: IDT + TSS + #PF handler in bootstrap
      um: kvm: kvm_enter_guest (SREGS + MSRs + SYSRETQ bootstrap)
      um: kvm: run_userspace dispatcher — per-trap interrupt_end
      um: kvm: syscall classifier + class-D trap path
      um: kvm: gadget state channel (MSR_KERNEL_GS_BASE + vvar)
      um: kvm: LSTAR gadget body — 11 CLASS_E handlers
      um: kvm: gadget-mid-fault → -EFAULT via handle_syscall
      Documentation/virt/uml: KVM backend + systrap gadget

     arch/um/Kconfig                                 |  XX +
     arch/um/backend/kvm/Makefile                    |  XX +
     arch/um/backend/kvm/kvm_backend.c               | XXX +
     arch/um/backend/kvm/kvm_backend.h               | XXX +
     arch/um/backend/kvm/lifecycle.c                 | XXX +
     arch/um/backend/kvm/mm.c                        |  XX +
     arch/um/backend/kvm/thread.c                    | XXX +
     arch/um/backend/kvm/syscall_class.c             |  XX +
     arch/um/backend/contract/test_ops.c             | XXX +
     arch/um/backend/contract/test_kvm_hooks.h       |  XX +
     Documentation/virt/uml/backends.rst             |  XX +
     MAINTAINERS                                     |   X +
     15 files changed, XXXX insertions(+)
     create mode 100644 arch/um/backend/kvm/Makefile
     create mode 100644 arch/um/backend/kvm/kvm_backend.c
     create mode 100644 arch/um/backend/kvm/kvm_backend.h
     create mode 100644 arch/um/backend/kvm/lifecycle.c
     create mode 100644 arch/um/backend/kvm/mm.c
     create mode 100644 arch/um/backend/kvm/thread.c
     create mode 100644 arch/um/backend/kvm/syscall_class.c
     create mode 100644 arch/um/backend/contract/test_kvm_hooks.h

    --
    2.XX.X

---

End of draft. Post-squash pass, this becomes the real
`0000-cover-letter.patch` with the filled-in diffstat.
