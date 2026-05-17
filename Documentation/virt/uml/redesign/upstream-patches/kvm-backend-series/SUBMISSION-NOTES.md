# Series 7 (KVM backend, kvm-v2 shape) — submission-readiness notes

**Status (2026-05-14):** SCOPED, but **GATED on Phase J DONE.**
Cover-letter draft refreshed for the v2 redesign in
`0000-cover-letter.patch.md` (this directory). Patches not yet
rebased. Dependent on Series 4 (backend-ops-abstraction-rfc)
landing upstream first AND on the Phase J 24 h continuous soak
+ Tier 1/2/3 + LTP wired-with-passing-cycle definition closing
(PLAN-2026-05-14 §3).

This file replaces the 2026-04-27 Stage-A notes in place. The
2026-04-27 notes described a 15-patch series organised around a
per-task vCPU + shadow-PT design. That shape was archived at
tag `kvm-v1-archive-20260428` (`arch/um/backend/kvm-v1-archive/`,
gated on `BROKEN`). The v2 redesign — per-CPU vCPU pool, TDP,
kernel-half-only PML4[448] trampoline, revived in-guest LSTAR
gadget — is what this series will ship.

## Status at a glance

  - **Cover letter:** v2-shape refresh landed in this commit
    (~900 lines), replacing the 414-line Stage-A draft.
  - **Submission branch:** not yet created. Authoring branch
    is `umlctl-deploy` (`origin/umlctl-deploy`). 105+ commits
    on `arch/um/backend/kvm-v2/` since `master`; total
    ~846 commits on the branch since `master` across all
    workstreams.
  - **Patches:** not yet rebased / squashed. Final shape will
    be 17-19 upstream-shaped patches per the planned ordering
    in §"Planned patch ordering" below. The squash audit is
    the moment the patch numbering becomes load-bearing.
  - **Hard prerequisites:** (1) Series 4 landed upstream,
    (2) Phase J DONE certificate, (3) squash audit converged,
    (4) checkpatch clean per patch, (5) fleet bench re-captured
    on a 24-h-validated host.

## Authoring branch + commit shape

Authoring branch: **`umlctl-deploy`** (pushed to
`origin/umlctl-deploy`). HEAD is `d2d30f5f68ac` as of
2026-05-16, advancing daily during the Phase J track.

Commit count since `master`:

  ~846 total commits.
  ~105 commits touching `arch/um/backend/kvm-v2/` directly.
  ~70 additional commits touching adjacent paths
  (`arch/um/{include,kernel,os-Linux,x86}`,
  `tools/testing/selftests/um/`, `Documentation/virt/uml/`,
  `MAINTAINERS`).

Commit granularity on the branch is optimised for redesign
review — per-audit-finding fixes, per-G-ladder step,
per-memo-decision, per-SMP-T## state-audit closure. The
upstream squash is a separate engineering exercise that
produces 17-19 coherent patches from the ~175 substantive
fork commits.

Squash plan (refreshed from the 2026-04-27 Stage-A list to
the v2 list — see §"Planned patch ordering"). Each upstream
patch is a semantic unit that survives `git bisect` cleanly
(boot, substrate gate, cpython-parity gate); the redesign
walkthrough (T##/G-ladder breadcrumbs) is documented in
`02-workstreams/D-kvm-backend/` for archaeology purposes but
does NOT show up in the patch series. Upstream reviewers see
the coherent final shape.

## Companion docs (refreshed from Stage-A list to v2 memos)

  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-v2-implementation-plan.md`
    — the v2 architecture's authoritative spec. Memo 26 is
    the 10-phase (A-J) plan that the v2 substrate executed.
    All architectural choices (per-CPU vCPU pool, TDP /
    per-region memslots, kernel-half PML4[448] trampoline,
    IO-port syscall trap, gadget revival shape) are
    rationalised there.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/25-v2-restart-guide.md`
    — the pre-memo-26 "we are doing v2 because v1 is not
    salvageable" rationale. Memo 25 is the meta-context
    upstream reviewers will want when they ask "why not just
    fix v1 instead of starting over."
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md`
    — the ELI5 of v1's failure modes that motivated the v2
    redesign. Useful as a reviewer onboarding doc.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/23-fix-plan.md`
    — v1's last fix plan, retained as a record of what was
    tried before the clean-slate decision.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/22-dlopen-repro.md`
    — the canonical v1 bug-class reproducer that v2's
    architecture makes structurally impossible (Bug B in
    memo 22's taxonomy is the user-half / kernel-half PML4
    alias on the bootstrap page).
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/`
    — the 8-layer state-audit framework + per-bug closure
    memos (08-smp-t13-FIXED.md through 25-smp-t57-xsave-
    enable-plan.md). Operational tooling for future SMP /
    concurrency / state-ownership investigations in v2.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/measurements.md`
    — bench-py + bench-micro + perf-py-startup numbers across
    the 8-host fleet (Intel Skylake / Kaby / Skylake-SP /
    Alder Lake P+E / AMD Zen 4 ×3). Pre-Phase-A and
    post-Phase-A captures both retained.
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md`
    — Phase J pilot soak diary (240 / 240 = 100 % across 3
    short workloads).
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-soak-2h-2026-05-14.md`
    — first 2 h post-Phase-A daemon soak diary
    (kvm-v2 200 / 200 = 100 %).
  - `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md`
    — Phase J full validation design (24 h continuous,
    Tier 1/2/3, LTP curation).
  - `Documentation/virt/uml/redesign/04-risks/decisions-log.md`
    — D106..D122 (May 2026 catalogue of the SMP-T## fix
    cycles, gadget revival, Phase J pilot + 2 h soak).
    Authoritative chronology.

## Planned patch ordering (~17-19 patches)

Refreshed from the Stage-A 15-patch list to the v2 list.
Each numbered bullet becomes one upstream-shape patch.
Listed in send-order. Estimated count is 17-19; the squash
audit will shape the final number (fixups may fuse or split).

### Foundation (patches 1-3) — Kconfig + lifecycle + per-VM context

1. **`um: kvm-v2: Kconfig + backend probe + ops registration`** —
   Phase A.1. `CONFIG_UM_BACKEND_KVM_V2` +
   `CONFIG_UM_BACKEND_KVM_V2_GADGET` (gadget default y).
   `init.c` probes `/dev/kvm`, negotiates KVM_CAP_SYNC_REGS
   + KVM_CAP_SET_GUEST_DEBUG, registers v2 ops table via
   `um_register_backend("kvm-v2", &kvm_v2_ops)`. `ops.c`
   typed `struct um_backend_ops` instance (Series 4 dep).
   `kvm_v2_backend.h` internal header. Origin:
   `1a83522e3ea4` + descendants.

2. **`um: kvm-v2: per-VM context lifecycle`** — Phase A.2.
   `context.c` — `struct kvm_v2_vm` (vm_fd, caps, memslot
   list, spinlock). `kvm_v2_vm_create`: KVM_CREATE_VM +
   KVM_SET_TSS_ADDR (gpa 0xfffd0000) +
   KVM_SET_IDENTITY_MAP_ADDR. Origin: `427f1d88cc42`.

3. **`um: kvm-v2: CPUID curation seed`** — Initial seed for
   `kvm_v2_curate_cpuid`: host passthrough with curated mask
   (RDRAND / RDSEED / parts of XSAVE / AVX family). Patches
   7 and 16 expand on this (lazy first-run install and AVX
   un-mask). Splitting CPUID seed from lazy-install lets the
   patches bisect cleanly. Origin: A.2 era commits +
   memo 26 §A.

### Substrate (patches 4-6) — per-CPU vCPU pool + TDP + dispatch loop

4. **`um: kvm-v2: per-CPU vCPU pool`** — Phase C.1 + C.3.
   `struct kvm_v2_vcpu` (vcpu_fd, kvm_run mmap, cpu,
   sentinel). At init create `min(nr_cpu_ids, NR_CPUS)`
   vCPUs; mmap each kvm_run. SYNC_REGS valid-mask set at
   create. Net 4 ioctls/dispatch → 1 KVM_RUN. Origin:
   `0df41d13febf`, `124db82a0ccb`.

5. **`um: kvm-v2: TDP memslot registration`** — Phase B.1 +
   B.2 + B.5. `kvm_v2_memslot` per-VM list + bitmap slot
   allocator. Backend ops `mm_region_added/removed/
   protected`. `kvm_v2_load_cr3` writes `__pa(pgd)` to
   `s.regs.sregs.cr3` + ORs `KVM_SYNC_X86_SREGS` into
   `kvm_dirty_regs`. Origin: `a80a03c02743`,
   `fd9df1834e8a`, `1a879e8cd9fe`.

6. **`um: kvm-v2: task dispatch + migrate_disable +
   cross-task SREGS`** — Phase C.2 + SMP-T13/T33/T47 folded.
   `kvm_v2_vcpu_run` pins via `migrate_disable()` (SMP-T13:
   UML's `CONFIG_PREEMPT_VOLUNTARY` no-PREEMPT_COUNT makes
   `preempt_disable()` a NO-OP). `kvm_v2_load_user_sregs`
   cross-task path issues full `KVM_SET_SREGS` (drops KVM
   `prev_roots[]`); same-task takes SYNC_REGS fast path.
   `WARN_ON_ONCE` on cross-task SET failure. Origin:
   `7b29a64af5f3` + descendants.

### Phase D activation (patches 7-9) — IO-port syscall trap

7. **`um: kvm-v2: lazy CPUID first-run install + EFER.SCE`** —
   Phase D.0a + D.4a. `cpuid_primed` flag on
   `struct kvm_v2_vcpu`; install at top of vcpu_run sticky
   per pool entry. Must happen before any guest instruction.
   Add `KVM_EFER_SCE` to load_user_sregs mask — without it,
   CPU raises #UD on SYSCALL. Origin: `812e6725d48d`.

8. **`um: kvm-v2: LSTAR trampoline + IO-port syscall trap`** —
   Phase D.1 + D.2 + D.4-MSR. 5-byte trampoline
   (`out %al, $0xf4; sysretq`) at VA `0xffffe00000000040`.
   `kvm_v2_handle_io_trap` decodes KVM_EXIT_IO port 0xf4:
   `PT_SYSCALL_NR = HOST_AX`, fix HOST_IP ← HOST_CX +
   HOST_EFLAGS ← HOST_R11, call `handle_syscall`, marshal
   return. MSR programming (LSTAR, STAR, FMASK=0x47700)
   once at vcpu_create.

9. **`um: kvm-v2: kernel-half PML4[448] + physmem identity
   memslot`** — Phase D.4b-pre + D.4b.
   `BUG_ON(pgd_index(KVM_V2_TRAMPOLINE_GVA) != 448)` runtime
   assertion (2026-04-29 codex audit caught a "508" typo in
   earlier drafts). Physmem identity-offset memslot at
   vm_create (gpa=0, hva=uml_physmem, size=physmem_size).
   PT chain pages with `_KERNPG_TABLE` non-leaf flags
   (`_PAGE_TABLE` has US=1, wrong for kernel-half). Explicit
   patch of `swapper_pg_dir[448]` + `init_mm.pgd[448]`
   (mm_list not exported). PT chain VM-lifetime.

### Exception + signal handling (patches 10-12)

10. **`um: kvm-v2: IDT/GDT/IST/TSS + exception handlers via
    IO ports`** — Phase E.1+E.2+E.3. IDT in dedicated guest
    page (gpa 0xfffe0000). Per-vCPU IST stacks; TSS IST1.
    KVM_SET_TSS_ADDR set in Phase A.2. In-guest handlers
    (#PF/#GP/#UD/#DE/#OF) emit IO ports (0xf6/0xf9/0xfa/
    0xfb/0xfc) paralleling v1's `UM_KVM_*_PORT`. #BP uses
    `KVM_GUESTDBG_USE_SW_BP` → `KVM_EXIT_DEBUG`. Origin:
    tasks #81-#96.

11. **`um: kvm-v2: signal delivery + EINTR-mid-KVM_RUN +
    per-task FPU swap-out`** — Phase E.3 + F.1 + F.2 + D.3.
    `kvm_v2_install_signal_mask` at vcpu_create installs
    KVM_SET_SIGNAL_MASK (sigfillset minus SIGALRM). EINTR
    on KVM_RUN: `if (rc < 0 && rc != -EINTR) panic;` fall
    through to `migrate_enable; return;`.
    `kvm_v2_fpu_capture_for_switch_out` KVM_GET_FPU into
    `current->thread.arch.kvm_v2.fpu` hooked from
    `__switch_to`. Origin: `5297fe2bd165`, `f0e4d1d95c2e`.

12. **`um: kvm-v2: PF-stub user-RAX recovery (EINTR-mid-stub
    carve-out)`** — SMP-T41 / D111. PF stub opens with
    `push %rax`, then `mov %cr2, %rax` / `pop %rax`. EINTR
    in `[stub+0x0d, stub+0x18)` leaves
    `eintr_regs.rax = CR2`, not user RAX. Recovery: capture
    `stub_rip_at_eintr` BEFORE overwriting `gp[HOST_IP]`;
    if RIP > stub_start, recover user RAX from
    `*(u64 *)(top-56)` (untouched by post-push writes).
    Canonical closure of the mt-mini STRICT_MEMSET_FAIL
    byte[0]=0 residual that survived T31..T40. Origin:
    `af659ad4297d`.

### SMP (patches 13-14)

13. **`um: kvm-v2: SMP — per-CPU IDT/IST/TSS install`** —
    Phase G.1. Install IDT/IST/TSS for ALL vCPUs, not just
    cpu=0 (the 2026-04-30 CPU=1+ zeroed-IST/TSS panic
    cleared after the FPU snapshot + mmu_gather + sregs
    round-trip work). 4-CPU build + cpython-parity 21/21
    + 8-parallel-worker init clean. Origin: task #140.

14. **`um: kvm-v2: SMP — cross-vCPU TLB kick`** — Phase G.2,
    activated after T13 migrate_disable removed the IPI-storm
    scenario. KVM_REQ_TLB_FLUSH +
    `kvm_make_all_cpus_request`. SMP-T36 (mmu_gather batch
    walk every batch under CONFIG_UML) + SMP-T37 (per-CPU
    UML host pthread CPU pinning) fold here. Origin:
    `7e1c255a09ad`, `9f0ff6257e8b`, `ad18db7c3768`,
    `bedd73af5033`, `1b1febc1ffba`.

### Performance + FPU (patch 15)

15. **`um: kvm-v2: per-vCPU FPU-dirty epoch flag (lazy FPU)`** —
    SMP-T55 / D119. `fpu_dirty` (bool) + `fpu_owner_task`
    (task_struct *) on `struct kvm_v2_vcpu`. Skip post-vmexit
    `KVM_GET_FPU` iff `fpu_dirty=false` AND
    `fpu_owner_task == current`. Mark dirty on:
    vcpu_create_one; cross-task arrival in load_user_sregs;
    post-vmexit `cr0 & X86_CR0_TS == 0`; `handle_io_nm`
    clearing TS; `fpu_install_on_first_run` after fresh
    KVM_SET_FPU. Mark clean on: successful pre-run
    KVM_SET_FPU; successful post-vmexit KVM_GET_FPU. Restores
    Phase H.2 lazy-FPU while preserving T26/T27 cross-task
    XMM guarantee (24 000 forks ⇒ 0 CHILD_FAIL).
    perf-py-startup ratio 1.250 → 1.10-1.17 on Zen 4 (gate
    ceiling 1.20). Origin: `fd2f9639b0ce`.

### AVX/XSAVE (patches 16-17)

16. **`um: kvm-v2: AVX/XSAVE enable (CPUID un-mask +
    KVM_SET_XCRS)`** — SMP-T57 Phase A / D121.
    `kvm_v2_install_xcrs()` helper: KVM_SET_XCRS xcrs[0]=0x7
    (FP|SSE|YMM). `curate_cpuid` un-masks Leaf 1 ECX bits
    12/26/27/28/29 (FMA/XSAVE/OSXSAVE/AVX/F16C) and Leaf 7.0
    EBX bit 5 (AVX2). Drops Leaf 0xD zero-pass (KVM needs it
    for `guest_supported_xcr0`). Origin: `ab68bf077de3`.

17. **`um: kvm-v2: lazy first-dispatch CR4.OSXSAVE arming`** —
    In vcpu_run's existing `cpuid_primed=false` block:
    install CPUID → synchronous GET+SET_SREGS adding
    `X86_CR4_OSXSAVE` → `install_xcrs`. Two chicken-and-egg
    KVM validations resolved by ordering (KVM rejects
    CR4.OSXSAVE before CPUID's bit lands; rejects SET_XCRS
    before CR4.OSXSAVE is synchronous in `vcpu->arch.cr4`).
    `install_production_sregs` seed kept WITHOUT OSXSAVE.
    Squash audit may fuse with patch 16.

### Systrap gadget (patch 18)

18. **`um: kvm-v2: systrap gadget — lstar_gadget.S + 11
    CLASS_E handlers`** — D113 (gadget revival) + D114
    (T56 carve-out). `arch/um/backend/kvm-v2/lstar_gadget.S`
    (pattern: `arch/x86/kernel/ftrace_64.S` + `ftrace.c`),
    externs `kvm_v2_lstar_{fallback,gadget}_{start,end}`.
    Two-phase install: 5-byte fallback LSTAR at vm_create,
    gadget upgrade after `exception_install` confirms state
    pages mapped. 11 CLASS_E handlers (getpid family +
    sched_yield + clock_gettime MONO + time + getcpu) inline.
    Per-vCPU state page (P only, no US/RW) at
    `bootstrap_va+PAGE_SIZE`; vvar (P|RW, no US) at
    `bootstrap_va+2*PAGE_SIZE`. Per-task field refresh in
    load_user_sregs (TGID/TID/PPID/UID/EUID/GID/EGID +
    CPU_ID); writer-side seqlock for MONO_*. Full x86_64 ABI:
    gadget entry saves user RDX/R8/R10 into SAVE_* slots;
    exits restore before SYSRETQ. Upper-NR guard prevents
    low-byte alias hijack. clock_gettime seqlock retry uses
    %edx so NR survives fallback; call budget bounds
    staleness to ≤ 10 000 calls. T56 LSTAR-EINTR carve-out
    extended to gadget body: on RIP ≥ LSTAR+3, KVM_SET_MSRS
    restores `MSR_KERNEL_GS_BASE = STATE_GVA(cpu)`; rewind
    HOST_IP to user SYSCALL retry. KUnit `kvm_v2_byteshape`
    locks entry preamble (swapgs + 3× movq %reg,
    %gs:SAVE_*) and page-fit. `CONFIG_UM_BACKEND_KVM_V2_
    GADGET` Kconfig (default y); `=n` falls back to slow
    KVM_EXIT_IO path. Origin: `7ebcd8aac347` + `db9170b5a7b3`.

### Docs + MAINTAINERS (patch 19)

19. **`Documentation/virt/uml: KVM v2 backend + systrap
    gadget + MAINTAINERS`** — Phase I.3.
    `Documentation/virt/uml/backends.rst` kvm-v2 §§ (backend
    lifecycle, TDP, per-CPU pool, kernel-half PML4[448]
    trampoline, IO-port trap, gadget, Kconfig).
    `arch/um/backend/kvm-v2/README.md` operator quickstart.
    MAINTAINERS entry under "USER MODE LINUX" subsection for
    `arch/um/backend/kvm-v2/`.

## Routing

Per `SUBMISSION-QUEUE.md` §"Series 7":

  - **Primary list:** `linux-um@lists.infradead.org`,
    `kvm@vger.kernel.org`.
  - **Cc:**
      Paolo Bonzini <pbonzini@redhat.com>
      Sean Christopherson <seanjc@google.com>
      Richard Weinberger <richard@nod.at>
      Johannes Berg <johannes@sipsolutions.net>
      Anton Ivanov <anton.ivanov@cambridgegreys.com>
      Benjamin Berg <benjamin@sipsolutions.net>
      Tiwei Bie <tiwei.btw@antgroup.com>
      linux-arch@vger.kernel.org

  - Run `scripts/get_maintainer.pl` against the rebased
    series before emission; the list above is from the
    2026-04 tree and may have drifted.

## Hard prerequisites (refreshed)

Before this series can go out, all of the following must be
true:

1. **Series 4 (backend-ops-abstraction-rfc) landed upstream.**
   Series 7 patches 1-3 wire kvm-v2 into the typed `struct
   um_backend_ops` Series 4 introduces. Sending 7 before 4
   forces reviewers to reconstruct the dispatch shape.

2. **Phase J DONE certificate.** PLAN-2026-05-14 §3 defines
   Phase J DONE: 24 h continuous soak ≥ 99.5 % per workload
   (Wilson 95 % lower bound ≥ 99.0 %); Tier 1/2/3 wired with
   ≥ 1 passing cycle each; LTP runner integrated; headline
   ratio recorded. As of 2026-05-14 the daemon driver is
   live, Tier 1 done, Tier 2 designed, Tier 3 + LTP wired
   but operator pre-flight pending. **The 24 h run has not
   yet been kicked off.**

3. **Squash audit against the umlctl-deploy branch.** The
   current 105+ kvm-v2-touching commits (~175 substantive
   commits across all paths) are optimised for redesign
   review, not upstream. Squashing produces 17-19
   semantically-coherent patches. Needs:
   - A clean topic branch off `master`.
   - `git rebase -i` with per-patch semantic grouping (the
     §"Planned patch ordering" above is the seed).
   - `checkpatch.pl --strict --no-signoff` clean on every
     squashed patch.
   - Build + KUnit + cpython-parity verification on every
     step of the bisect.

4. **Fleet bench re-captured on a Phase-J-validated host.**
   The 2026-05-07 numbers in this draft are pre-24h-soak.
   The cover-letter perf table will be updated once a host
   has completed a 24 h soak run AND the bench numbers
   captured against the same kernel binary.

5. **Upstream audit round on a fresh branch.** After
   squashing, rerun checkpatch + the external audit prompts
   against the final patch-emission order. Any findings
   become a v2 of the upstream series.

## Pre-submission cleanups (same shape as Series 1)

Borrowed from Series 1's SUBMISSION-NOTES, with adjustments
for the kvm-v2 surface:

### Trailer hygiene

  - Strip `Co-authored-by:` trailers from squashed commits
    (LKML convention: drop or pair with `Co-developed-by:`+
    `Signed-off-by:`. Plain `Co-authored-by:` is flagged by
    checkpatch as non-standard).
  - Author + `Signed-off-by:` both read `Michael Bommarito
    <michael.bommarito@gmail.com>` (the user's git config).

### Format-patch output (target shape)

Will be generated at `/tmp/kvm-v2-series-emit/` once the
squash audit converges:

  - `0000-cover-letter.patch` — subject + body from this
    directory's `0000-cover-letter.patch.md` with the
    final diffstat filled in.
  - `0001-um-kvm-v2-Kconfig-backend-probe-ops-registration.patch`
  - `0002-um-kvm-v2-per-VM-context-lifecycle.patch`
  - ... through `0019-Documentation-virt-uml-KVM-v2-...`

### Build verification recipe

```
# Clean topic branch off master
git checkout master
git checkout -b kvm-v2-series-submit
# Apply the 17-19 squashed patches
for p in /tmp/kvm-v2-series-emit/00*.patch; do
    git am "$p"
done

# Verify each commit builds + passes substrate + cpython-parity
for c in $(git log master..HEAD --reverse --format=%H); do
    git checkout "$c"
    make ARCH=um O=~/src/uml-builds/uml-smp olddefconfig
    make ARCH=um O=~/src/uml-builds/uml-smp -j$(nproc) || exit 1
    tools/testing/selftests/um/cpython-parity/cpython-parity.sh \
        --quick || exit 1
done

# Final SMP soak gate on the tip commit
git checkout kvm-v2-series-submit
make ARCH=um O=~/src/uml-builds/uml-smp -j$(nproc)
tools/testing/selftests/um/mt-mini/run.sh \
    --threads 8 --ncpus 4 --iters 400
# Expect: 400 / 400 = 100 %
```

### checkpatch discipline

Every squashed patch must pass `./scripts/checkpatch.pl
--strict --no-signoff` clean. The authoring branch already
keeps checkpatch clean per commit (kernel discipline rule in
CLAUDE.md / `feedback_commit_signoff` memory), so the squash
pass inherits that baseline; the squash itself doesn't
introduce new warnings.

Known items to spot-check:

  - `lstar_gadget.S`'s ASM comments referencing disp32 values
    (`0xffffff7b` etc.) — code-snippet lines >75 cols;
    checkpatch's LONG_LINES heuristic doesn't distinguish.
    Not actionable; reviewers won't object.
  - Multi-line macro definitions in `kvm_v2_backend.h`
    (`KVM_V2_*` constants) — already hand-checked for
    parenthesisation.
  - `BUG_ON(pgd_index(...) != 448)` — runtime assertion;
    checkpatch sometimes flags `BUG_ON` use. Justified at
    the call site (PML4 index is a structural invariant,
    not a soft check).

## Anticipated review questions + decisions-log pointers

Top-of-mind reviewer questions the cover letter does not
fully answer, with pointers into `04-risks/decisions-log.md`:

### Q1. Why per-CPU vCPU pool instead of per-task?

The cover letter's "the v2 architecture §(1)" section
explains. Supporting memos:

  - `02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md`
    — ELI5 of v1's failure modes (the contract violation that
    motivated the redesign).
  - `02-workstreams/D-kvm-backend/25-v2-restart-guide.md`
    — pre-memo-26 restart rationale.
  - `02-workstreams/D-kvm-backend/26-v2-implementation-plan.md`
    §C — Phase C design with the per-CPU rationale.
  - D106..D112 — May 2026 SMP fix cycle catalogue (the
    cross-task transitions that surface under the per-CPU
    model and how each one was closed).

### Q2. -EPERM vs -ENOSYS for class-D syscalls

The classifier's class-D trap list (ptrace, reboot,
init_module, finit_module, delete_module, kexec_load,
kexec_file_load, bpf, modify_ldt, set_thread_area) currently
short-circuits to -EPERM. Reviewer may prefer falling through
to UML's logical syscall_table lookup → -ENOSYS for some of
these.

  - Memo 10 (`10-syscall-classification.md`) is the
    authoritative class A/B/C/D/E inventory with the
    -EPERM/-ENOSYS reasoning per NR.
  - D84 (audit F10 — class B/C reconciliation with
    dispatcher).

### Q3. Per-mm host worker model — why not done?

Memo 25's R4 refactor (each guest mm becomes its own host
process with its own VM context) was deferred during the
Phase A→I sprint. With Phase J validating the substrate, the
question reopens.

  - Memo 25 R4 spec.
  - PLAN-2026-05-14 §6 (per-mm worker as a v3 follow-up
    candidate).
  - Decision deferred until Phase J DONE.

### Q4. CPUID curation rationale

Why mask RDRAND / RDSEED / specific XSAVE / AVX bits?

  - v1 archive `vcpu.c:118-174` for the curated mask history.
  - D121 — the un-mask of AVX/AVX2/FMA/F16C/XSAVE/OSXSAVE
    explanation.
  - The dance: install CPUID → add CR4.OSXSAVE → install
    XCRS. KVM rejects each before the prior step is
    synchronous in `vcpu->arch.cr4`.

### Q5. Why kernel-half-only trampoline (PML4[448])?

  - Memo 22 (`22-dlopen-repro.md`) — Bug B taxonomy
    (user-half / kernel-half PML4 alias on bootstrap page).
  - Memo 26 Phase D rewrite at `d186d870e8eb` — the
    vmcall → IO-port mechanism change (vmcall is
    structurally impossible on stock KVM; arch/x86/kvm/
    x86.c:10456,10520-10523 returns `-KVM_ENOSYS` for
    unknown nrs).
  - 2026-04-29 codex audit correction of the PML4 index
    arithmetic (508 → 448; v1 was also at 448).

### Q6. SMP-T55 stretch — why not chase the +50% UP-hop?

  - D118 (T55 surfaced — policy: do NOT loosen the gate
    ceiling).
  - D119 (T55 closed via per-vCPU FPU-dirty epoch).
  - State-audit memo 23 §6.2 (the +50% UP-hop is a clutch of
    SMP-correctness commits; bisecting is deferred until a
    gate failure justifies the focused investigation budget).

### Q7. AVX-512 deferral (SMP-T57 Phase B)

  - D121 (Phase A landed, Phase B catalogued).
  - State-audit memo 25 §3.2 + §5.1 (the marshal struct
    question — `kvm_fpu` is 512-byte FXSAVE-shape;
    switching to `struct kvm_xsave` is the Phase B structural
    change).

### Q8. Why a gadget at all (vs. just the fallback path)?

  - The "Why a third backend" section in the cover letter
    answers the strategic question. Supporting evidence:
  - D113 (gadget revival decision — the perf budget from
    SMP-correctness work made gadget viable).
  - Memo 11 (`11-systrap-gadget.md`) — gadget design spec
    (G1-G8 ladder, safety discipline, upstream known
    limitations).
  - `02-workstreams/D-kvm-backend/measurements.md` —
    bench-micro getpid table (gadget ~95 cyc vs fallback
    ~9 000-15 000 cyc vs seccomp ~109 000 cyc).

### Decisions-log queue-ordering rationale (D63)

The pre-2026-05 decision to put kvm-backend last in the
upstream queue (Series 7 after 1-6) is D63. Useful when the
reviewer asks "why not send the KVM patches first if they're
the headline?"

### Decisions-log gadget go/no-go (D70)

D70 is the 2026-04-24 GO decision for shipping the gadget at
all, capturing the fleet bench evidence + risk register
state at the time of the GO call. Useful when reviewers
question whether the gadget is mature enough to land.

## Risk register entry

### Risk: review iteration timeline

This is a **large series** (17-19 patches; ~1500 LoC of v2
substrate plus ~500 LoC of selftests / KUnit / docs). Past
LKML practice for series of this size:

  - 2-4 review rounds typical.
  - 4-8 weeks per round for maintainer + community review.
  - Total: 2-6 months from RFC v2 to merge.

Mitigations:
  - Series is gated on Phase J DONE so we don't ship
    something that hasn't earned its acceptance criteria.
  - Companion docs (memos 24/25/26 + decisions-log
    D106..D122) give reviewers the "why" context without
    forcing it into the patch series itself.
  - Cover-letter §"Outstanding review questions" pre-empts
    the items where we know reviewers will push back.
  - Cover-letter §"Reproducing the numbers" gives a
    concrete bench recipe so reviewers can replicate on
    their own host fleet.

### Risk: linux-um maintainers may push back on per-CPU vCPU pool

linux-um maintainers (Weinberger, Berg, Ivanov) have lived
with UML's per-task switch_threads model for years. They
may default to "make kvm-v2 fit the per-task model" rather
than accept that per-CPU is what KVM expects.

  - Counter-argument is in the cover letter's "the v2
    architecture §(1)" section + memo 26 §C.
  - Empirical counter-argument: v1 (per-task) lived through
    14 SMP-T## fix cycles to reach 100 % on mt-mini SMP T=8
    N=400, and even then 75 % per-trial gate flakiness was
    sitting on the contract violation. v2 (per-CPU) reaches
    100 % structurally.
  - **Fallback plan if pushback is hard:** offer a
    `CONFIG_UM_BACKEND_KVM_V2_VCPU_MODEL = pool|per_task`
    Kconfig switch with `per_task` gated on `BROKEN`.
    Documents the option without committing to support it.
    Probably not necessary; the per-CPU evidence is strong.

### Risk: KVM maintainers may flag CPUID curation as overly
restrictive

The CPUID mask (mask RDRAND, RDSEED, parts of XSAVE / AVX
family on Leaf 1 / Leaf 7) is a conservative starting
point. Reviewers from kvm@vger.kernel.org may want either
host-passthrough or a finer-grained curation argument.

  - D121 catalogues the AVX/XSAVE un-mask reasoning (Phase
    A).
  - Phase B (AVX-512) will land as a follow-up that further
    relaxes the mask.
  - Engaging the discussion is fine; the curation is not
    load-bearing for the v2 substrate — it's a defense-in-
    depth choice that can be relaxed without architectural
    impact.

### Risk: per-vCPU FPU-dirty epoch flag pattern is unfamiliar

The lazy-FPU optimisation (Patch 15 / D119) is a
v2-substrate-specific pattern. Reviewers may not have
context for the per-CPU vCPU pool's FPU lifecycle.

  - The "Security posture" section of the cover letter
    explains the cross-task FPU isolation argument.
  - State-audit memo 23 has the per-state-machine-delta
    analysis.
  - Risk: reviewers ask for an FPU regression test that
    runs cross-task transitions. Already have it
    (threaded-fork-malloc 24 000 forks / 6 boots ⇒ 0
    CHILD_FAIL). Cited in cover letter; ready to ship
    pointer to the selftest source.

### Risk: in-guest gadget reviewed as a security surface

The gadget runs in ring-0 inside the guest's CR3 and reads
host-populated state via `%gs:disp32`. Even though the trust
model is "same as any KVM guest", reviewers may flag the
state-page pattern as load-bearing for review.

  - Cover letter §"Security posture" covers the page-table
    mapping (P only on state page; P|RW on vvar; no US on
    either).
  - D113 covers the gadget revival audit (ABI preservation,
    KUnit byteshape, Kconfig opt-out).
  - The gadget is gated default-y but reviewers can request
    default-n; this is question (d) in the cover-letter's
    outstanding review questions.

## Post-send resolution path

Each review round = new `v<N>/` directory sibling in
`upstream-patches/kvm-backend-series/`. On land, move to
`upstream-patches/landed/kvm-backend-series-vN/` and append
the landing commit IDs to this file.

If the squash audit produces an emission shape that diverges
from the §"Planned patch ordering" above, update both this
file's plan and the cover letter's "Patch organization"
table before sending.

## Session log pointer

Authoring activity 2026-03 → 2026-05-14:

  - 2026-03 → 2026-04-27: v1 sprint (Stage A landed at
    `7f94922a356f`; archived at tag `kvm-v1-archive-20260428`).
  - 2026-04-28 → 2026-05-01: memo 26 v2 implementation
    Phases A → I.
  - 2026-05-02 → 2026-05-04: SMP-T13/T16/T17/T19/T20/T22/T23/
    T25/T26/T27/T29 fix cycle (decision-log D106..D108) —
    state-audit framework operational.
  - 2026-05-03 → 2026-05-04: SMP-T33/T36/T37/T41/T47 fix
    cycle (D108..D112) — true mt-mini residual closure
    (T41 user RAX recovery from IST top-56).
  - 2026-05-04: gadget revival (D113), SMP-T54 init.sh-hang
    closure (D115), SMP-T56 carve-out extension (D114).
  - 2026-05-05: Phase J pilot soak rig + 240/240 first run
    (D117).
  - 2026-05-07: SMP-T55 surfaced + closed via per-vCPU
    FPU-dirty epoch (D118 + D119). Phase J daemon driver
    landed (D120). SMP-T57 Phase A AVX/XSAVE enable (D121).
  - 2026-05-14: first 2 h post-Phase-A daemon soak,
    200/200 = 100 % on kvm-v2 (D122). PLAN-2026-05-14
    finalises forward roadmap.
  - 2026-05-16: cover-letter + SUBMISSION-NOTES refresh
    (this commit). Snapshot port Phase 1 landed (D123 — not
    part of this series).

Decisions-log is authoritative for chronology.
