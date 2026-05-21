# Series 7 (kvm-v2 backend) — squash audit plan

**Status (2026-05-21):** PLAN ONLY. No squash executed. Companion
to `SUBMISSION-NOTES.md` and `0000-cover-letter.patch.md` in this
directory. This memo re-derives the 17-19 upstream-shape patch
list from the actual `umlctl-deploy` commit log, attaches LoC
budgets + source commits + bisect risk + pre-flight gates per
patch, and lays out the squash execution order.

This file is the operational artifact the squash session will
work from. If a patch boundary shifts during the squash, the
diff lands here AND in `SUBMISSION-NOTES.md` AND in the cover
letter's "Patch organization" table before any `git send-email`.

Author of this plan: 2026-05-21 squash-audit session. Authoring
branch: `umlctl-deploy` (HEAD `73aed55225c4` — the `os_template_
pause_fork_clone` magic-number rename).

## Section 0 — Scope, in-series vs out-of-series, count target

### Scope of Series 7 (in-series)

Everything under `arch/um/backend/kvm-v2/` that is reachable from
the kvm-v2 boot path with `CONFIG_UM_BACKEND_KVM_V2=y` AND that
existed in some shape by the post-Phase-A `ab68bf077de3` commit.

Source-tree footprint at the squash target (rough):

  arch/um/backend/kvm-v2/Kconfig
  arch/um/backend/kvm-v2/Makefile
  arch/um/backend/kvm-v2/init.c
  arch/um/backend/kvm-v2/context.c
  arch/um/backend/kvm-v2/vcpu.c
  arch/um/backend/kvm-v2/memslot.c
  arch/um/backend/kvm-v2/region.c
  arch/um/backend/kvm-v2/syscall_trap.c
  arch/um/backend/kvm-v2/syscall_trap.h
  arch/um/backend/kvm-v2/exception.c
  arch/um/backend/kvm-v2/lstar_gadget.S
  arch/um/backend/kvm-v2/test_marshal.c
  arch/um/backend/kvm-v2/test_byteshape.c
  arch/um/backend/kvm-v2/kvm_v2_backend.h
  arch/um/backend/kvm-v2/ops.c
  arch/um/backend/kvm-v2/README.md
  arch/um/Kconfig                                 (1 hunk: select)
  arch/um/configs/{i386,x86_64}_defconfig         (optional)
  Documentation/virt/uml/backends.rst             (kvm-v2 §§)
  MAINTAINERS                                     (USER MODE LINUX subsection)
  tools/testing/selftests/um/cpython-parity/      (already landed for seccomp; gate)
  tools/testing/selftests/um/bench-{micro,py}/    (already landed; gate)
  tools/testing/selftests/um/mt-mini/             (already landed; gate)
  tools/testing/selftests/um/abi-rdx-r8-r10/      (gadget ABI test)

### Out of scope (do NOT include in this series)

These appear on `umlctl-deploy` and touch kvm-v2 but are
explicitly NOT part of Series 7:

  - **state_trace.[ch] / KVMV2_TRACE() hookpoints.** Debug
    machinery built to fix the SMP-T## fix cycle. Folded into
    the patch that introduces the surface it diagnoses IFF the
    hook is load-bearing for the structural fix; otherwise
    dropped at squash time (the redesign branch keeps it, the
    upstream patch doesn't).
  - **snapshot.c (#168 Phases 1-6).** Time-machine track. Will
    ship as a follow-up series after Phase J DONE + Series 7
    landed.
  - **record.c + replay (#169 Phases 1-7).** Same as snapshot;
    follow-up series.
  - **SMP-T58/T60/T68 gadget polish.** Post-Phase-A defensive
    fixes on `lstar_gadget.S` (EINTR-mid-gadget RDX/R8/R10
    recovery, fallback IO trap SAVE-slot audit, inverted-bounds
    check). Fold into the gadget patch (Patch 18) — they are
    the same surface, not a new one.
  - **SMP-T71 (MAP_POPULATE physmem).** Performance / NUMA
    polish. Independent of substrate correctness; can land as a
    follow-up.
  - **SMP-T73/T74/T75/T76 (Round 14 closure — KVM_GET_FPU →
    KVM_GET_XSAVE + DEBUGREGS + VCPU_EVENTS + CPUID leaf 0xD).**
    Closed 2026-05-19. These ARE in-series for Series 7 because
    they are correctness-load-bearing (the Round 14 fix is the
    one that closed the Django cache-flake bug that gated Tier
    3). They fold into patch 15 (FPU dirty epoch) and patch 16
    (AVX/XSAVE enable). See §Risk table.
  - **SMP-T78..T84 (umlctl host resource controls — cgroup v2,
    [host_resources] TOML).** Operator tooling, not in arch/um/.
  - **template_pause + fork-on-resume (Memo 09 Phases 1-2 +
    G7/G8 gates).** Separate fork-server workstream, ships as
    its own series.
  - **Refactor 4 (per-mm worker process).** Memo 25 R4 — listed
    in cover letter §"Known limitations" as a deferred v3
    follow-up. Out of this series.
  - **Refactor 11 (remove ptrace backend).** Independent change
    of policy; not bundled with kvm-v2.

### Patch count target

The cover letter says **19 patches** (with possible fusion to
17-18). This audit confirms **19 as the upper bound** and
identifies **two possible fusions** in §Patch 16/17 (AVX/XSAVE
enable + lazy first-dispatch CR4.OSXSAVE arming may collapse) and
§Patch 7 (lazy CPUID + EFER.SCE may absorb into patch 8 if
reviewers prefer fewer, fatter patches). Final shape will be
**17, 18, or 19** patches depending on those two judgement calls.

Default plan: ship **19** so the bisect granularity is maximal;
maintainers can ask for fusion in v2 if they prefer fewer
patches.

## Section 1 — The 19 patches

Each entry below is a semantic unit. Within an entry: title,
files touched, LoC budget (added / removed against master),
source commits (a 2-4-commit representative slice from the
authoring branch; not exhaustive), the bisect-risk profile, and
the pre-flight gate that the patch alone must clear.

The LoC budgets are derived from the present-tree size of each
file divided by the number of patches that build it up. They are
approximate; the squash run will pin them.

### Patch 1 — `um: kvm-v2: Kconfig + backend probe + ops registration`

Phase A.1 of memo 26. Wires kvm-v2 into the typed `struct
um_backend_ops` (Series 4 dep). Probes `/dev/kvm` at boot;
negotiates `KVM_CAP_SYNC_REGS` + `KVM_CAP_SET_GUEST_DEBUG`;
calls `um_register_backend("kvm-v2", &kvm_v2_ops)`. Defines
`CONFIG_UM_BACKEND_KVM_V2` (default n, depends on `X86_64`).
Defines `CONFIG_UM_BACKEND_KVM_V2_GADGET` (default y, depends on
`UM_BACKEND_KVM_V2`).

  - **Files touched:** `arch/um/backend/kvm-v2/{Kconfig,Makefile,
    init.c,ops.c,kvm_v2_backend.h}`, `arch/um/Kconfig` (one
    `select`-style hunk).
  - **LoC budget:** ~250 added / 0 removed. (init.c ~120,
    ops.c ~30, Kconfig ~40, Makefile ~10, header ~50.)
  - **Source commits:** `046375fd50ed` (stub directory), `1a83522
    e3ea4` (Phase A.1 backend probe), `40a97b5f3b72` (Phase A.1
    followup — ops capability flags).
  - **Bisect risk:** Low — at this patch the backend probes but
    cannot create a VM; ops registration returns "available"
    only if `/dev/kvm` opens. If broken: boot falls back to
    seccomp; `dmesg` shows "kvm-v2: probe failed". Substrate gate
    runs under seccomp baseline, not kvm-v2.
  - **Pre-flight gate (this patch alone):** `make ARCH=um O=$B
    -j$(nproc)` succeeds. Boot with `init=/bin/echo hi` succeeds
    under seccomp default (no behavioural change yet).

### Patch 2 — `um: kvm-v2: per-VM context lifecycle (KVM_CREATE_VM, TSS, IDENTITY_MAP_ADDR)`

Phase A.2. `struct kvm_v2_vm` (vm_fd, caps, memslot list,
spinlock). `kvm_v2_vm_create`: `KVM_CREATE_VM` +
`KVM_SET_TSS_ADDR(gpa=0xfffd0000)` + `KVM_SET_IDENTITY_MAP_ADDR`.
`kvm_v2_vm_destroy` symmetric.

  - **Files touched:** `arch/um/backend/kvm-v2/context.c` (new),
    `kvm_v2_backend.h` (extend).
  - **LoC budget:** ~180 added / 0 removed.
  - **Source commits:** `427f1d88cc42` (Phase A.2), `1066947fd4d3`
    (Phase A.3 — vCPU placeholder + initial CPUID install seed).
  - **Bisect risk:** Medium — vm_create reachable but no
    vCPU/memslot yet. If broken: kvm-v2 probe returns "available"
    but selection panics at first dispatch. Mitigation: gate
    activation behind Patch 4 (vCPU pool) by leaving
    `ops.vcpu_run` NULL — see Patch 5 for the activation flip.
  - **Pre-flight gate:** Build clean. Boot under seccomp default
    (kvm-v2 not yet selected). KUnit: none yet.

### Patch 3 — `um: kvm-v2: CPUID curation + capability negotiation`

Initial CPUID curation seed: host passthrough with curated mask
(RDRAND / RDSEED / parts of XSAVE / AVX family). This is the
**seed**; Patch 7 adds the lazy first-run install hook and Patch
16 expands the mask. Splitting them lets the bisect isolate
CPUID-shape regressions from XSAVE-enable regressions.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c` (CPUID
    helper), `kvm_v2_backend.h`.
  - **LoC budget:** ~150 added / 0 removed.
  - **Source commits:** `1066947fd4d3` (Phase A.3 — initial
    install seed) — note this commit also touches Patch 2's
    surface; the squash splits the vCPU placeholder from the
    CPUID curation deliberately.
  - **Bisect risk:** Medium — at this patch the CPUID install
    helper exists but is not called by the dispatch loop yet
    (Patch 7 wires it). If broken: nothing runs the helper, no
    regression visible.
  - **Pre-flight gate:** Build clean. No runtime behaviour change.

### Patch 4 — `um: kvm-v2: per-CPU vCPU pool (KVM_CREATE_VCPU, SYNC_REGS)`

Phase C.1 + C.3 + C.4. `struct kvm_v2_vcpu` (vcpu_fd, kvm_run
mmap, cpu, sentinel, last_task). At init create
`min(nr_cpu_ids, NR_CPUS)` vCPUs; mmap each kvm_run. SYNC_REGS
valid-mask set at create. The "1 vCPU per host CPU" lifetime
contract is established here. Net 4 ioctls/dispatch → 1
KVM_RUN.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c` (extend),
    `kvm_v2_backend.h` (extend).
  - **LoC budget:** ~300 added / 0 removed.
  - **Source commits:** `0df41d13febf` (C.1 per-host-CPU pool),
    `124db82a0ccb` (C.3 SYNC_REGS), `734d9bbe54a8` (C.4.0 FPU
    substrate), `9fa4a804d4e3` (C.4 GET/SET_FPU dispatch hooks).
  - **Bisect risk:** Medium — vCPUs created but `ops.vcpu_run`
    still NULL. If broken: boot fails at vcpu_create; backend
    probe degrades to "unavailable" and seccomp takes over.
  - **Pre-flight gate:** Build clean. KUnit `kvm_v2_marshal` 8/8
    (the C.4 FPU substrate is the marshal-side surface). Boot
    under seccomp default; verify `dmesg` reports
    `kvm-v2: vcpu pool initialized (N vCPUs)`.

### Patch 5 — `um: kvm-v2: TDP memslot registration via KVM_SET_USER_MEMORY_REGION`

Phase B.1 + B.2 + B.3 + B.5. `kvm_v2_memslot` per-VM list +
bitmap slot allocator. Backend ops `mm_region_added/removed/
protected`. `kvm_v2_load_cr3` writes `__pa(pgd)` to
`s.regs.sregs.cr3` + ORs `KVM_SYNC_X86_SREGS` into
`kvm_dirty_regs`.

  - **Files touched:** `arch/um/backend/kvm-v2/memslot.c` (new),
    `region.c` (new — wires mm_region_added/removed/protected
    ops), `vcpu.c` (load_cr3 helper).
  - **LoC budget:** ~400 added / 0 removed.
  - **Source commits:** `a80a03c02743` (B.1 allocator),
    `fd9df1834e8a` (B.2 memslot wiring), `d88d1084c31f` (B.3
    region_removed), `be19bc8f2815` (B.3 dedup), `88705566028a`
    (B.4 region_protected no-op), `1a879e8cd9fe` (B.5 load_cr3),
    `f77a31d1fbe4` (drop redundant os_map_memory),
    `c77a585330f3` (drop seccomp delegation under v2),
    `b4bad916ca93` (per-mm turnstile).
  - **Bisect risk:** **High** — this patch flips the substrate.
    After it, kvm-v2's backend ops are wired into the UML
    mm/region path. If broken: every guest mm operation may
    return -ENOMEM or panic in `seccomp_mm_region_added` (under
    v2 the seccomp delegation is supposed to be dropped — that's
    a load-bearing one-liner in this patch). Mitigation: the
    boot-time gate at this patch is "kvm-v2 selected if probed".
    Substrate-gate the patch with a runtime
    `backend=force=seccomp` cmdline arg path that explicitly
    skips the backend's mm hooks.
  - **Pre-flight gate:** Build clean. Boot at
    `init=/bin/echo hi` succeeds under kvm-v2. Backend selection
    log line: `kvm-v2: selected (CONFIG_UM_BACKEND_KVM_V2=y, /dev/kvm available)`.
    Substrate gate: skip (the dispatch loop is still a stub —
    activated in Patch 8).

### Patch 6 — `um: kvm-v2: task → vCPU dispatch + migrate_disable + cross-task SREGS`

Phase C.2 + SMP-T13/T29/T33/T47 folded. `kvm_v2_vcpu_run` pins
via `migrate_disable()`. `kvm_v2_load_user_sregs` cross-task
path issues full `KVM_SET_SREGS` (drops KVM `prev_roots[]`);
same-task takes SYNC_REGS fast path. `WARN_ON_ONCE` on
cross-task SET failure. T29 last_task gate on
`capture_for_switch_out`. **migrate_disable() (not preempt_
disable())** — UML's `CONFIG_PREEMPT_VOLUNTARY` no-PREEMPT_COUNT
makes the preempt variant a NO-OP, which was T13's headline
finding (state-audit Layer 8).

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c`,
    `syscall_trap.c` (skeleton — full activation in Patch 8).
  - **LoC budget:** ~250 added / ~30 removed (the
    `preempt_disable()` site rewrite to `migrate_disable()`).
  - **Source commits:** `7b29a64af5f3` (C.2 dispatch helper),
    `95b3a85bd309` (T13 migrate_disable swap), `337db869d078`
    (T13 extend to FPU capture sites), `9ccdc4300713` (T33
    prev_roots drop), `44d21b5a14ab` (T29 last_task gate),
    `602e9a27625c` (T47 WARN_ON_ONCE).
  - **Bisect risk:** **High** — the dispatch loop is now live
    for cross-task transitions but exception handling
    (Patches 10-12) and signal delivery (Patch 11) are not yet
    wired. Bisect-isolate failure modes: SIGSEGV at first
    cross-task switch, or stale FPU/XMM data visible across
    tasks. Mitigation: the patch includes T26/T27 FPU-always-GET
    (folded; SMP-T26/T27 surface is C.4 — gets folded into
    Patch 4 — and the cross-task add-on lives here).
  - **Pre-flight gate:** Build clean. Boot at `init=/bin/echo hi`
    succeeds. Single-task `python3 -V` succeeds (no cross-task
    transitions exercised yet). cpython-parity: skip
    (multi-module tests need the exception layer).

### Patch 7 — `um: kvm-v2: lazy CPUID first-run install + EFER.SCE`

Phase D.0a + D.4a. `cpuid_primed` flag on `struct kvm_v2_vcpu`;
install at top of `vcpu_run` sticky per pool entry. Must happen
before any guest instruction. Add `KVM_EFER_SCE` to
load_user_sregs mask — without it, CPU raises #UD on SYSCALL.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c`,
    `kvm_v2_backend.h`.
  - **LoC budget:** ~80 added / ~10 removed.
  - **Source commits:** `812e6725d48d` (D.4a MSR programming +
    EFER.SCE), `fef7d7db80c6` (D.0 CPUID lazy install seed).
  - **Bisect risk:** Medium — at this patch the guest can boot
    and execute non-SYSCALL instructions. Without Patch 8's
    LSTAR trampoline, any SYSCALL raises #UD which the guest
    has no handler for → vmexit on triple fault → backend
    panic. Mitigation: defer enabling the syscall path until
    Patch 8 (the dispatch loop continues to NOP out the
    syscall trap until LSTAR is installed).
  - **Pre-flight gate:** Build clean. Boot at `init=/bin/true`
    (which exits before any syscall fires) succeeds. Boot at
    `init=/bin/echo` — should NOT succeed yet; echo issues a
    write() that hits SYSCALL → #UD. Verify by looking for the
    panic message; bisect knows to expect this signature.

### Patch 8 — `um: kvm-v2: LSTAR trampoline + IO-port syscall trap`

Phase D.1 + D.2 + D.4-MSR (LSTAR/STAR/FMASK programming). 5-byte
trampoline (`out %al, $0xf4; sysretq`) at GVA
`0xffffe00000000040`. `kvm_v2_handle_io_trap` decodes
`KVM_EXIT_IO` port 0xf4: `PT_SYSCALL_NR = HOST_AX`, fix
`HOST_IP ← HOST_CX + HOST_EFLAGS ← HOST_R11`, call
`handle_syscall`, marshal return. MSR programming (LSTAR, STAR,
`FMASK=0x47700`) once at vcpu_create. Includes D.3 return
semantics + EINTR + per-task FPU swap-out.

  - **Files touched:** `arch/um/backend/kvm-v2/syscall_trap.c`
    (full), `syscall_trap.h`, `vcpu.c` (extend).
  - **LoC budget:** ~500 added / ~20 removed.
  - **Source commits:** `d77a233a42f9` (D.1 trampoline + ABI),
    `d6facb05c33f` (D.2 syscall trap dispatcher),
    `419c4dd107e8` (D.3 return semantics + EINTR + FPU
    swap-out), `124cfefa342f` (D.5-fix-1 production SREGS),
    `5297fe2bd165` (D.5-fix-2 SIGNAL_MASK + unblock_signals),
    `f0e4d1d95c2e` (D.5 vcpu_run flip).
  - **Bisect risk:** **High** — first patch where syscalls flow
    end-to-end. `python3 -V` should now work. Any miscompiled
    trampoline byte = silent #UD storm. Failure mode: boot stuck
    at first userspace syscall, or panic on `unhandled vmexit
    reason`. Mitigation: KUnit `kvm_v2_byteshape` (introduced
    here) locks the trampoline byte layout to a known-good
    `01 00 00 00 e7 f4 48 cf` shape (the disp32 values + IO
    instr + sysretq).
  - **Pre-flight gate:** Build clean. Boot at `init=/bin/echo hi`
    succeeds. `init=/bin/sh -c "echo hi"` succeeds. KUnit
    `kvm_v2_byteshape` 4/4 (trampoline cases only; gadget cases
    activate in Patch 18). Substrate gate: skip (PML4[448] not
    yet installed in Patch 9).

### Patch 9 — `um: kvm-v2: kernel-half PML4[448] + physmem identity memslot`

Phase D.4b-pre + D.4b. The runtime invariant
`BUG_ON(pgd_index(KVM_V2_TRAMPOLINE_GVA) != 448)` lives here
(the 2026-04-29 codex audit caught a "508" typo in earlier
drafts; the assert is the structural-invariant guard).
Physmem identity-offset memslot at vm_create (gpa=0,
hva=uml_physmem, size=physmem_size). PT chain pages with
`_KERNPG_TABLE` non-leaf flags (`_PAGE_TABLE` has US=1, wrong
for kernel-half). Explicit patch of `swapper_pg_dir[448]` +
`init_mm.pgd[448]` (mm_list not exported). PT chain
VM-lifetime.

  - **Files touched:** `arch/um/backend/kvm-v2/context.c`
    (extend), `vcpu.c` (extend — physmem memslot), new helper
    in `kvm_v2_backend.h`.
  - **LoC budget:** ~250 added / 0 removed.
  - **Source commits:** `8fff1742bfb4` (D.4b-pre physmem
    identity memslot), `de57a84b9288` (D.4b PML4[448]
    kernel-half install).
  - **Bisect risk:** **Highest in the patch series.** This is
    where the trampoline becomes addressable from the guest's
    kernel-half. If the PML4 index arithmetic is off (508 vs
    448), the trampoline page is mapped at the wrong GVA and
    every syscall raises #PF. Failure mode: silent — boot
    panics at first syscall. Mitigation: the runtime
    `BUG_ON(pgd_index(KVM_V2_TRAMPOLINE_GVA) != 448)` triggers
    immediately if the install path is wrong; checkpatch
    sometimes flags `BUG_ON` use but this one is justified at
    the call site.
  - **Pre-flight gate:** Build clean. **First full substrate
    boot.** `init=/bin/echo hi` succeeds. `init=/bin/sh -c "echo
    hi"` succeeds. `python3 -V` succeeds. **First cpython-parity
    short run: 21/21 PARITY (single-iter, no SMP).** Substrate
    gate: PASS=25/FAIL=3/XFAIL=3 (matches seccomp).

### Patch 10 — `um: kvm-v2: IDT/GDT/IST/TSS + exception handlers via IO ports`

Phase E.1 + E.2 + E.3 + E.4 + E.5. IDT in dedicated guest page
(gpa 0xfffe0000). Per-vCPU IST stacks; TSS IST1.
`KVM_SET_TSS_ADDR` set in Phase A.2 (Patch 2). In-guest handlers
(#PF/#GP/#UD/#DE/#OF) emit IO ports (0xf6/0xf9/0xfa/0xfb/0xfc)
paralleling v1's `UM_KVM_*_PORT`. #BP uses
`KVM_GUESTDBG_USE_SW_BP` → `KVM_EXIT_DEBUG`. SMP-T16 (cr2
preserve), SMP-T17 (inline #NM handler), SMP-T19/T20 (IST
sanity + RCU-deferred page free), SMP-T22 (vmexit-on-#NM),
SMP-T23 (cross-mm cr2-zero gate), SMP-T25 (LSTAR-EINTR HOST_IP
rewind), SMP-T41 (recover user RAX in EINTR-mid-PF-stub — the
true mt-mini residual closure) fold here.

  - **Files touched:** `arch/um/backend/kvm-v2/exception.c`
    (new — IDT/GDT/IST/TSS install + handlers + IO-port
    decoders), `syscall_trap.c` (extend — EINTR-mid-PF
    handling), `vcpu.c` (extend).
  - **LoC budget:** ~700 added / ~50 removed.
  - **Source commits:** `6233bbd8cd88` (E.1 IDT+stubs+GDT),
    `13d7bb7c2c4e` (E.2 IST + TSS), `ddb5ab5fda85` (E.3 dispatch),
    `32a7603236b0` (E.4 CPL=3), `aba0b460a034` (E.5 first KVM_RUN
    /bin/true), `2f0c87a47dcb` (T16 cr2 preserve), `85244f068ad3`
    (T17 #NM inline), `d9ed9e14c7b6` (T19/T20 IST + RCU defer),
    `ddf3cfe5cf31` (T22 vmexit-on-#NM), `a0be14e66013` (T23
    cross-mm cr2-zero), `b1421d7583e9` (T25 LSTAR-EINTR HOST_IP
    rewind), `af659ad4297d` (**T41 root cause — recover user RAX
    in EINTR-mid-PF-stub**), `ba7aa0aa0240` (#121 cr2+IDT EINTR
    preserve), `e5977806fd14` (inline PF handler), `49b3e40a968c`
    (per-task FPU snapshot).
  - **Bisect risk:** **High.** This is the second-biggest LoC
    patch and the densest fold of fix-cycle work. If broken in
    any non-trivial way: cpython-parity regresses sub-21/21,
    mt-mini falls below 100 %, or threaded-fork-malloc surfaces
    CHILD_FAIL. Mitigation: the patch is itself a fold of fix
    cycles that closed exactly these regressions; the squash
    must preserve the **closure order** so that T41 (the true
    closure) is the dominant logic and T33 is left as defensive
    hygiene per the STATUS.md "T33 superseded by T41" note.
  - **Pre-flight gate:** Build clean. cpython-parity 21/21
    (UP + SMP both). Substrate gate PASS=25/FAIL=3/XFAIL=3.
    mt-mini SMP T=8 ncpus=4 N=30 30/30.

### Patch 11 — `um: kvm-v2: signal delivery + EINTR-mid-KVM_RUN + per-task FPU swap-out`

Phase E.3 + F.1 + F.2 + D.3 signal half. `kvm_v2_install_signal_
mask` at vcpu_create installs `KVM_SET_SIGNAL_MASK` (sigfillset
minus SIGALRM). EINTR on KVM_RUN: `if (rc < 0 && rc != -EINTR)
panic;` fall through to `migrate_enable; return;`.
`kvm_v2_fpu_capture_for_switch_out` `KVM_GET_FPU` into
`current->thread.arch.kvm_v2.fpu` hooked from `__switch_to`.
SMP-T26/T27 (always-`KVM_GET_FPU` after KVM_RUN), and the
preceding lazy-FPU H.2 NO-OP attempt (`ba9c83331f30`) is dropped
at squash time as redundant — Patch 15 reintroduces lazy-FPU
properly via the dirty-epoch flag.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c` (extend),
    `syscall_trap.c` (extend — signal mask installation).
  - **LoC budget:** ~200 added / ~50 removed.
  - **Source commits:** `5297fe2bd165` (D.5-fix-2 signal mask),
    `419c4dd107e8` (D.3 return semantics + EINTR),
    `76b1d98b2006` (T26/T27 always KVM_GET_FPU — folded into
    Patch 6 if the cross-task SREGS reasoning fits there, else
    here; default plan: keep in Patch 11 because the FPU swap
    surface is here, not the dispatch loop).
  - **Bisect risk:** Medium — signal handling is now live. EINTR
    paths can be exercised by `python3 -c "import signal"`.
    Failure mode: missed signal causes hang, or signal-during-
    KVM_RUN causes the dispatch loop to wedge. Mitigation:
    `KVM_SET_SIGNAL_MASK` masks SIGALRM out so EINTR is the only
    signal that can hit KVM_RUN; all others queue.
  - **Pre-flight gate:** cpython-parity 21/21 (signal-handling
    modules included). threaded-subprocess-wait 10/10.

### Patch 12 — `um: kvm-v2: PF-stub user-RAX recovery (EINTR-mid-stub carve-out)`

SMP-T41 / D111 — promoted to a standalone patch for review
clarity. The PF stub opens with `push %rax`, then `mov %cr2,
%rax` / `pop %rax`. EINTR in `[stub+0x0d, stub+0x18)` leaves
`eintr_regs.rax = CR2`, not user RAX. Recovery: capture
`stub_rip_at_eintr` BEFORE overwriting `gp[HOST_IP]`; if RIP >
stub_start, recover user RAX from `*(u64 *)(top-56)`. Canonical
closure of the mt-mini STRICT_MEMSET_FAIL `byte[0]=0` residual
that survived T31..T40.

**Squash judgement:** Patch 12 could be folded into Patch 10
(it's textually part of the exception handler surface) — but
the user-RAX recovery is non-obvious enough that reviewers
benefit from seeing it isolated. Default plan: ship as its own
patch.

  - **Files touched:** `arch/um/backend/kvm-v2/syscall_trap.c`
    (the carve-out), `exception.c` (the recovery helper).
  - **LoC budget:** ~100 added / ~10 removed.
  - **Source commits:** `af659ad4297d` (T41 root cause fix),
    `6fb90a1ba275` (T41 N=400 + substrate gate validation).
  - **Bisect risk:** Medium — without this patch the mt-mini
    `byte[0]=0` flake re-emerges at multi-percent rate.
    Substrate gate still passes (substrate gate is single-iter,
    not flake-sensitive). cpython-parity still passes.
    Mitigation: the patch is small and surgical.
  - **Pre-flight gate:** mt-mini SMP T=8 ncpus=4 N=400 100 %.
    threaded-fork-malloc 8w × 500i × 1 boot — 0 CHILD_FAIL on
    4000 forks.

### Patch 13 — `um: kvm-v2: SMP — per-CPU IDT/IST/TSS install`

Phase G.1. Install IDT/IST/TSS for ALL vCPUs, not just cpu=0
(the 2026-04-30 CPU=1+ zeroed-IST/TSS panic that cleared after
the FPU snapshot + mmu_gather + sregs round-trip work). 4-CPU
build + cpython-parity 21/21 + 8-parallel-worker init clean.

  - **Files touched:** `arch/um/backend/kvm-v2/exception.c`
    (extend — per-CPU loop around IDT/IST/TSS install), `vcpu.c`.
  - **LoC budget:** ~120 added / ~30 removed (the cpu=0
    specialisation goes away).
  - **Source commits:** Task #140 era — `13d7bb7c2c4e` (E.2
    per-vCPU IST stacks; the IDT/TSS per-CPU loop comes in
    later commits; squash will identify the precise hashes).
    Representative slice: `13d7bb7c2c4e` + the SMP-T## state-
    audit memos that closed G.1.
  - **Bisect risk:** Medium — at this patch, SMP boot is live.
    Failure mode: CPU=1+ panic on first IDT lookup. Mitigation:
    boot at `nr_cpus=1` works trivially; the patch only matters
    for `--with-cpus=N` builds.
  - **Pre-flight gate:** SMP build (`--with-cpus=4`) boots.
    cpython-parity 21/21 under SMP. mt-mini SMP T=8 ncpus=4
    N=30 30/30.

### Patch 14 — `um: kvm-v2: SMP — cross-vCPU TLB kick + KVM_REQ request mechanism`

Phase G.2. Activated after T13 `migrate_disable` removed the
IPI-storm scenario. `KVM_REQ_TLB_FLUSH` + `kvm_make_all_cpus_
request`. SMP-T36 (mmu_gather batch walk every batch under
CONFIG_UML) + SMP-T37 (per-CPU UML host pthread CPU pinning)
fold here.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c`,
    `region.c` (extend — tlb_kick_others op), `arch/um/include/
    shared/backend.h` (extend backend contract — but this
    contract change should belong to Series 4 if it lands first;
    the squash audit's open judgement call).
  - **LoC budget:** ~150 added / ~20 removed.
  - **Source commits:** `7e1c255a09ad` (tlb_kick_others impl),
    `9f0ff6257e8b` (cmpxchg-dedup), `77cc1821c595` (backend
    contract v2 — nullable tlb_kick_others op),
    `ad18db7c3768`/`bedd73af5033` (T36 mmu_gather + T37 host
    pthread pinning).
  - **Bisect risk:** Medium — TLB kicks become live. Failure
    mode under load: cross-CPU stale TLB visible as memory
    corruption. Mitigation: cpython-parity has wide coverage;
    threaded-fork-malloc is the focused stress.
  - **Pre-flight gate:** threaded-fork-malloc 8w × 500i × 6
    boots = 24 000 forks, 0 CHILD_FAIL. cpython-parity 21/21
    SMP. perf-py-startup ratio ≤ 1.20 (would be 1.250 here
    because Patch 15's FPU-dirty epoch is not yet applied —
    confirm the bisect expects 1.250 at this point, not 1.10).

### Patch 15 — `um: kvm-v2: per-vCPU FPU-dirty epoch flag (lazy FPU) + SMP-T73/T74/T75/T76 close-out`

SMP-T55 / D119 + Round 14 closure (SMP-T73/T74/T75/T76 /
2026-05-19). `fpu_dirty` (bool) + `fpu_owner_task` (task_struct
*) on `struct kvm_v2_vcpu`. Skip post-vmexit `KVM_GET_FPU` iff
`fpu_dirty=false` AND `fpu_owner_task == current`. Mark dirty
on: vcpu_create_one; cross-task arrival in load_user_sregs;
post-vmexit `cr0 & X86_CR0_TS == 0`; `handle_io_nm` clearing
TS; `fpu_install_on_first_run` after fresh KVM_SET_FPU. Mark
clean on: successful pre-run KVM_SET_FPU; successful
post-vmexit KVM_GET_FPU. Restores Phase H.2 lazy-FPU while
preserving T26/T27 cross-task XMM guarantee.

**Round 14 fold:** SMP-T73 (`KVM_GET_FPU` → `KVM_GET_XSAVE` in 5
per-task save/restore sites) lives here because it is the
**correct shape** of the FPU save/restore path; the dirty-epoch
flag operates on the XSAVE-shape save area, not the FXSAVE-shape
save area. SMP-T74 (DEBUGREGS pinned), SMP-T75 (VCPU_EVENTS
per-task save/restore), SMP-T76 (CPUID leaf 0xD sub-leaves
masked consistently) fold here because they are
correctness-load-bearing on the same epoch transition.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c`,
    `syscall_trap.c`, `kvm_v2_backend.h`, `arch/um/include/asm/
    processor.h` (the `struct arch_thread.kvm_v2` shape — was
    `kvm_fpu`, now `kvm_xsave`; this is the T73 marshal-struct
    change).
  - **LoC budget:** ~250 added / ~80 removed (the FXSAVE→XSAVE
    rename produces a clean delta).
  - **Source commits:** `fd2f9639b0ce` (T55 dirty-epoch),
    `1f3dd82d8d4b` (T73 FPU → XSAVE), `d0045817840a` (T74/T75/T76
    state-leak follow-ups).
  - **Bisect risk:** **High.** perf-py-startup must land within
    1.20 ceiling here. If broken: Django cache-flake re-emerges
    (the Round 14 bug); cross-task YMM-upper leaks resurface.
    Mitigation: the patch is the Round 14 closure, validated
    against 120/120 PASS on the post-R14 kernel. cpython-parity
    + Tier 1/2/3 + 24h soak are the dispositive gates.
  - **Pre-flight gate:** cpython-parity 21/21. mt-mini 400/400.
    threaded-fork-malloc 0/24000 fails. **perf-py-startup ratio
    ≤ 1.20** (was 1.250 at Patch 14; must drop to 1.10-1.17
    here). Tier 3 django-loopback-none 30/30.

### Patch 16 — `um: kvm-v2: AVX/XSAVE enable (CR4.OSXSAVE + KVM_SET_XCRS + CPUID un-mask)`

SMP-T57 Phase A / D121. `kvm_v2_install_xcrs()` helper:
KVM_SET_XCRS xcrs[0]=0x7 (FP|SSE|YMM). `curate_cpuid` un-masks
Leaf 1 ECX bits 12/26/27/28/29 (FMA/XSAVE/OSXSAVE/AVX/F16C) and
Leaf 7.0 EBX bit 5 (AVX2). Drops Leaf 0xD zero-pass (KVM needs
it for `guest_supported_xcr0`). 12-of-14 stress-ng vm-methods
pass post-Phase-A (was 0-of-14).

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c` (extend
    curate_cpuid + install_xcrs helper).
  - **LoC budget:** ~150 added / ~20 removed.
  - **Source commits:** `ab68bf077de3` (T57 Phase A — AVX/XSAVE
    enable).
  - **Bisect risk:** Medium — AVX-using userspace becomes
    functional. Failure mode: SIGILL on AVX instruction (CPUID
    reports AVX, hardware vetoes via #UD). Cover: stress-ng
    `--vm --verify` matrix.
  - **Pre-flight gate:** stress-ng `--vm --verify` sampled
    vm-methods: 12-of-14 PASS (mscan + prime-incdec expected to
    fail — AVX-512 territory, Phase B follow-up). cpython-parity
    21/21 (no regression on the non-AVX path).

### Patch 17 — `um: kvm-v2: XCR0 = FP|SSE|YMM via lazy first-dispatch arming`

In vcpu_run's existing `cpuid_primed=false` block: install
CPUID → synchronous GET+SET_SREGS adding `X86_CR4_OSXSAVE` →
`install_xcrs`. Two chicken-and-egg KVM validations resolved by
ordering (KVM rejects CR4.OSXSAVE before CPUID's bit lands;
rejects SET_XCRS before CR4.OSXSAVE is synchronous in
`vcpu->arch.cr4`). `install_production_sregs` seed kept
WITHOUT OSXSAVE.

**Squash judgement:** patches 16 + 17 may fuse if reviewers
prefer fewer patches. Default plan: ship separately because the
ordering constraint is non-obvious and isolating it into its
own patch lets the bisect tell a CPUID-shape regression from an
XCR0-shape regression.

  - **Files touched:** `arch/um/backend/kvm-v2/vcpu.c`.
  - **LoC budget:** ~80 added / ~30 removed.
  - **Source commits:** Captured inside `ab68bf077de3` (the
    Phase A landing) — the ordering logic is the same commit's
    lazy first-run path. The squash splits the install_xcrs
    helper (Patch 16) from the lazy-arming first-dispatch flow
    (Patch 17).
  - **Bisect risk:** Low — narrow, ordering-only patch.
  - **Pre-flight gate:** Same as Patch 16. Full AVX/XSAVE
    matrix passes the 12-of-14 bar.

### Patch 18 — `um: kvm-v2: systrap gadget — lstar_gadget.S, state page, 11 CLASS_E handlers, ABI preserve, KUnit byteshape`

D113 (gadget revival) + D114 (T56 carve-out) + SMP-T58/T60/T68
defensive folds. `arch/um/backend/kvm-v2/lstar_gadget.S`
(pattern: `arch/x86/kernel/ftrace_64.S` + `ftrace.c`), externs
`kvm_v2_lstar_{fallback,gadget}_{start,end}`. Two-phase install:
5-byte fallback LSTAR at vm_create, gadget upgrade after
`exception_install` confirms state pages mapped. 11 CLASS_E
handlers (getpid family + sched_yield + clock_gettime MONO +
time + getcpu) inline.

T58 (recover user RDX/R8/R10 on EINTR-mid-LSTAR-gadget), T60
(audit gadget SAVE-slot writes at fallback IO trap), T68 (fix
inverted bounds check) all fold here because they touch the
same `lstar_gadget.S` + entry preamble surface.

  - **Files touched:** `arch/um/backend/kvm-v2/lstar_gadget.S`
    (new — ~450 LoC), `syscall_trap.{c,h}` (extend — 11
    handlers + state page management), `vcpu.c` (extend — vvar
    seqlock writer), `test_byteshape.c` (extend — gadget cases),
    `Kconfig` (extend — `CONFIG_UM_BACKEND_KVM_V2_GADGET`
    default y; the Kconfig flag itself was introduced in
    Patch 1 as a stub, this patch wires it).
  - **LoC budget:** ~900 added / ~30 removed.
  - **Source commits:** `7ebcd8aac347` (gadget revival Phases
    1-7 + items #1-5), `db9170b5a7b3` (T56 carve-out extend),
    `bab1d5c54056` (T58 RDX/R8/R10 recovery), `74a005fc4a15`
    (T60 SAVE-slot audit), `2cd3e7b39d8c` (T68 inverted bounds).
  - **Bisect risk:** **High.** Largest LoC patch. Gadget byte
    layout sensitive: any mis-assembled disp32 makes the gadget
    silently corrupt state. Failure mode: cpython-parity passes
    (gadget'd syscalls succeed) but bench-micro getpid shows
    fallback timing (~9000-15000 cyc instead of ~95 cyc) — the
    bisect must include a perf check, not just correctness.
    Mitigation: KUnit `kvm_v2_byteshape` 9/9 (2 gadget cases
    added in this patch) locks the entry preamble (swapgs + 3×
    movq %reg, %gs:SAVE_*) and a page-fit upper bound.
  - **Pre-flight gate:** Build clean. KUnit `kvm_v2_byteshape`
    9/9 (the 2 new gadget cases). userspace ABI test
    (RDX/R8/R10 across 11 gadget syscalls): 33/33 PASS.
    bench-micro getpid: 89-105 cyc/call (gadget hot path).
    cpython-parity 21/21 (no regression).

### Patch 19 — `Documentation/virt/uml: KVM v2 backend + systrap gadget + MAINTAINERS`

Phase I.3. `Documentation/virt/uml/backends.rst` kvm-v2 §§
(backend lifecycle, TDP, per-CPU pool, kernel-half PML4[448]
trampoline, IO-port trap, gadget, Kconfig). MAINTAINERS entry
under "USER MODE LINUX" subsection for `arch/um/backend/kvm-v2/`.
`arch/um/backend/kvm-v2/README.md` operator quickstart (an
in-tree quickstart README so the directory has a top-level
landing doc — checkpatch is OK with `.md` in arch/um/backend/
because the file is documentation, not source).

  - **Files touched:** `Documentation/virt/uml/backends.rst`,
    `MAINTAINERS`, `arch/um/backend/kvm-v2/README.md`.
  - **LoC budget:** ~400 added / 0 removed.
  - **Source commits:** `89e424c64a91` (Phase I.3 backend
    README + backends.rst v2 promotion), `3c7372ad60b1` (perf
    claims refresh post-gadget). The MAINTAINERS hunk will be
    new-write in the squash — no existing commit hosts it
    cleanly.
  - **Bisect risk:** None (docs only). Bisect skips this commit
    by convention when investigating runtime regressions.
  - **Pre-flight gate:** `make htmldocs` succeeds. checkpatch
    clean. `scripts/get_maintainer.pl` against the squashed
    series resolves the expected list.

## Section 2 — Companion artifacts

Items that travel with the series but live outside arch/um/
backend/kvm-v2/.

### Documentation/virt/uml/ updates (Patch 19)

  - `Documentation/virt/uml/backends.rst` — new sections
    "KVM v2 backend (kvm-v2)", "The per-CPU vCPU pool", "TDP
    memslot registration", "Kernel-half PML4[448] trampoline",
    "Syscall trap via IO port 0xf4", "The systrap gadget",
    "Kconfig" (CONFIG_UM_BACKEND_KVM_V2, CONFIG_UM_BACKEND_
    KVM_V2_GADGET).
  - The redesign memos under `Documentation/virt/uml/redesign/`
    do NOT travel with the series — they are project archaeology
    for upstream reviewers who want to know "why", but the patch
    itself ships the user-facing docs only.

### MAINTAINERS (Patch 19)

Add under "USER MODE LINUX" section:

```
USER MODE LINUX (UML) — KVM v2 BACKEND
M:      Richard Weinberger <richard@nod.at>
M:      Johannes Berg <johannes@sipsolutions.net>
M:      Anton Ivanov <anton.ivanov@cambridgegreys.com>
M:      Michael Bommarito <michael.bommarito@gmail.com>
R:      Paolo Bonzini <pbonzini@redhat.com>
R:      Sean Christopherson <seanjc@google.com>
L:      linux-um@lists.infradead.org
L:      kvm@vger.kernel.org
S:      Maintained
F:      arch/um/backend/kvm-v2/
F:      Documentation/virt/uml/backends.rst
F:      tools/testing/selftests/um/abi-rdx-r8-r10/
```

### Kconfig + defconfig changes

  - **In-tree Kconfig (Patch 1):**
    - `CONFIG_UM_BACKEND_KVM_V2` — default n, `depends on X86_64`.
    - `CONFIG_UM_BACKEND_KVM_V2_GADGET` — default y, `depends on
      UM_BACKEND_KVM_V2`.
  - **arch/um/Kconfig (Patch 1):** one hunk wiring the kvm-v2
    Kconfig into the backend menu.
  - **arch/um/configs/x86_64_defconfig:** **DO NOT enable
    kvm-v2 by default in the defconfig in this series.** UML's
    convention is `defconfig` = the universally-buildable
    baseline; kvm-v2 is `EXPERT`-equivalent in spirit (even
    though Patch 1 lifts the literal `EXPERT` gate per Phase
    I.4 / commit `07f680c6d1a5`). Users opt in via
    `scripts/config --enable UM_BACKEND_KVM_V2`. Documented in
    backends.rst.

### Selftests under `tools/testing/selftests/um/`

The cpython-parity, mt-mini, threaded-fork-malloc, bench-micro,
bench-py harnesses **already exist on master** (they were
landed for seccomp before kvm-v2). The kvm-v2 series adds:

  - `tools/testing/selftests/um/abi-rdx-r8-r10/` — gadget ABI
    test (RDX/R8/R10 preservation across 11 gadget syscalls ×
    3 saved registers, 33 cases). Folds into Patch 18 (gadget).
  - One-line `TEST_PROGS` add for `abi-rdx-r8-r10` in the
    selftests Makefile. Folds into Patch 18.

KUnit suites under `arch/um/backend/kvm-v2/`:

  - `test_marshal.c` — folded into Patch 4 (per-CPU pool +
    FPU substrate).
  - `test_byteshape.c` — folded into Patch 8 (trampoline 4
    cases) + Patch 18 (gadget 2 more cases, total 6 at series
    end — note: the cover letter says 9/9 PASS; the audit
    surfaces a count mismatch with `test_byteshape.c` source
    that the squash must reconcile before send).

## Section 3 — Risk table

### 3.1 Squash conflicts (hot files)

The 2026-04-27 → 2026-05-19 commit log shows where the most
rewrites happened:

| File                                          | Touches | Rewrite risk |
|-----------------------------------------------|---------|--------------|
| `arch/um/backend/kvm-v2/vcpu.c`               | 69      | **Highest** — dispatch loop, FPU, CPUID, XCRS, dirty-epoch, T13/T26/T27/T29/T33/T41/T47/T55/T73/T74/T75/T76 all touch here. |
| `arch/um/backend/kvm-v2/kvm_v2_backend.h`     | 44      | High — every struct extension goes here. |
| `arch/um/backend/kvm-v2/syscall_trap.c`       | 39      | High — IO trap dispatcher + gadget glue + EINTR carve-outs. |
| `arch/um/backend/kvm-v2/Makefile`             | 15      | Low — incremental file adds; no rewrites. |
| `arch/um/backend/kvm-v2/ops.c`                | 11      | Low — the ops table is mostly stable post-Patch 1. |
| `arch/um/backend/kvm-v2/state_trace.c`        | 9       | **Out of series** — gets dropped at squash time. |
| `arch/um/backend/kvm-v2/record.c`             | 9       | **Out of series.** |
| `arch/um/backend/kvm-v2/exception.c`          | 9       | Medium — bunched into Patch 10. |

Squash strategy for hot files:

  - **vcpu.c:** the file ends up touched by Patches 4, 6, 7,
    9, 10, 11, 13, 14, 15, 16, 17. The squash will rewrite this
    file from scratch as a tree of conflict resolutions per
    patch. Plan: cherry-pick the introducing commit first
    (`0df41d13febf`), then per-patch `git checkout HEAD~N --
    arch/um/backend/kvm-v2/vcpu.c` + manual splice from the
    fold candidates. The squash will likely take 2-3 hours
    just for vcpu.c.
  - **kvm_v2_backend.h:** simpler — most extensions are
    purely-additive. Per-patch, do `git diff master..HEAD --
    arch/um/backend/kvm-v2/kvm_v2_backend.h` and lift the
    relevant struct/macro extensions.
  - **syscall_trap.c:** Patch 8 + 12 + 18 are the major
    contributors. The gadget glue in Patch 18 should be
    additive over the IO-trap dispatcher in Patch 8.

### 3.2 Bisect breakers (introduce + revert windows)

The authoring branch has 6 explicit reverts in the kvm-v2
window. Each is a bisect breaker if folded naively — the
"intermediate state" between the introduce and the revert
is broken at bisect time. Squash must DROP both ends.

| Reverted thing                              | Introduce → revert |
|---------------------------------------------|--------------------|
| `reset CS/SS to USER selectors on every dispatch (CPL=0 leak)` | `b30e39381cc7` → `8528b8956250`. Survived as `f780bd5d54fa` (conditional reset). Squash: keep only the conditional form, drop the unconditional pair. |
| `call interrupt_end() in syscall dispatch path too` | `ad06c7f5164c` → `8955ce7f878d`. Squash: drop both; the corrected interrupt_end gating lives in `24557e95c4b1` (perf-O1, Patch 8). |
| `SMP-T28 lazy-FPU regression` | landed and reverted in same hash window (`70191a5e98c6`). Squash: drop entirely; T26/T27 alone is sufficient per the revert message. |
| `SMP-T31a — madvise per-PFN in tlb_sync` | implemented, then reverted (TDP coherence RULED OUT). Squash: drop; the negative result lives in `02-workstreams/state-audit/18` for archaeology. |
| `SMP-T34 host KVM patch infrastructure` | landed and reverted (`66cf5fc634ad`, `f63cd381343d`). Squash: drop both; mechanism was wrong. |
| `H.2 CR0.TS lazy FPU initial attempt` | landed at `ba9c83331f30`, superseded by `fd2f9639b0ce` (T55 dirty-epoch). Squash: drop the initial attempt; the dirty-epoch is the keeper. |

Squash audit's job: when reading the authoring branch top-down,
every revert is the **signal that the prior patch should be
dropped from the squash**. The squash is NOT a `git rebase -i`
that preserves the redesign walkthrough — it is a re-derivation
of the **final shape** from the structural decisions.

### 3.3 Multi-commit iteration windows

These are commits where the same problem got iterated over
multiple times. The squash must fold to the **final state**, not
to the head of the iteration window.

| Iteration window                            | Folds to |
|---------------------------------------------|----------|
| Phase E.3 / E.3.5 / E.4 / E.5 (commits `ddb5ab5fda85`, `4d4dfe1bb8ba`, `762730125c15`, `32a7603236b0`, `aba0b460a034`) | Patch 10 (final state — exception handlers via IO ports). Drop the intermediate "second activation attempt also fails" commit's diff entirely. |
| SMP-T33 → T34 → T41 (`9ccdc4300713`, `66cf5fc634ad`, `f63cd381343d`, `af659ad4297d`) | Patches 6 (T33 cross-task SREGS as defensive hygiene) + 10 (T41 user-RAX recovery as the true mt-mini closure). T34 dropped entirely. |
| SMP-T55 four-state iteration (memo state-audit/23) | Patch 15 (final FPU-dirty epoch). The intermediate diagnostics drop. |
| Round 14 T71 → T72 → T73 → T74 → T75 → T76 → T77 (4-week investigation) | T71 dropped (negative result), T72 dropped (negative result), T73-T76 fold into Patch 15, T77 is the feature-enablement checklist memo — out of patch series, stays in `02-workstreams/D-kvm-backend/state-audit/`. |
| State-trace infra (`6f65c310750c`, `e9a54318e301`, `608d072c9381`, `d43bb3423aa0`, etc.) | **Out of series.** The debug infrastructure used to fix the bugs is not load-bearing for the upstream patch — drop entirely at squash. The user-facing artifacts (KUnit + selftests) cover what reviewers need. |

### 3.4 Anticipated review questions (pre-staged)

From the cover letter §"Outstanding review questions" plus
additions surfaced by this audit's commit-shape review:

  (a) **Per-CPU vCPU pool vs. per-task vCPU.** Already in cover
      letter. Backed by D106..D112 + memo 26 §C.
  (b) **Class-D classifier — -EPERM vs -ENOSYS.** Already in
      cover letter. Memo 10 (syscall classification).
  (c) **Per-mm host worker model deferral.** Already in cover
      letter. Memo 25 R4.
  (d) **Gadget Kconfig default — y or n.** Already in cover
      letter. D70 (gadget go decision).
  **Additions surfaced by this audit:**
  (e) **Why Patch 12 (PF-stub user-RAX recovery) is a separate
      patch from Patch 10.** Reviewer may ask "this looks like
      it belongs with the IDT handlers." Answer: the EINTR
      carve-out for the PF stub is a subtle invariant
      (user-RAX recovered from `*(u64 *)(top-56)`) that
      reviewers benefit from seeing isolated.
  (f) **Why Patch 16 and Patch 17 are split.** Same shape as
      (e); reviewers may ask "fold them." Answer: the lazy
      first-dispatch arming has a non-obvious ordering
      requirement (CR4.OSXSAVE must be synchronous in
      `vcpu->arch.cr4` before SET_XCRS); isolating it helps
      bisect.
  (g) **Why state_trace.[ch] is not in the series.** Reviewer
      may notice the kvm-v2 directory on the upstream branch
      doesn't have `state_trace.c` but the redesign memos
      heavily reference `KVMV2_TRACE()`. Answer: state-trace is
      debug infrastructure used to fix the SMP-T## fix cycle.
      It is structurally optional — the fixed substrate
      operates correctly without it. Future SMP/concurrency
      investigations may bring it back as a separate series.
  (h) **The "9/9 KUnit byteshape" discrepancy.** Cover letter
      says 9/9; current source may say 6/9. Reconcile before
      send: the gadget revival added 2 cases (so it's 6 base
      trampoline cases + 2 gadget = 8?), and one more case may
      be missing. Audit must reconcile or update cover letter.
  (i) **Round 14 closure included or not.** This audit folds
      T73/T74/T75/T76 into Patch 15. Reviewer may push back
      ("the FPU patch is too big" or "DEBUGREGS belongs in
      its own patch"). Be ready to split Patch 15 into
      Patch 15a (FPU dirty epoch) + Patch 15b (DEBUGREGS +
      VCPU_EVENTS + CPUID 0xD). Default plan: ship as one
      patch (Patch 15) because the four T## fixes are the
      same epoch transition's correctness closure.

## Section 4 — Squash execution plan

**This plan is for the squash session, not for this audit.**
It documents the steps the squash session will run, ordered
sequentially. The audit memo lives in tree; the squash session
follows the plan.

### Step 0 — Pre-conditions

Before the squash session starts:

  1. Phase J DONE certificate signed (PLAN-2026-05-14 §3): 24h
     continuous soak ≥ 99.5 % per workload, Tier 1/2/3 wired
     with ≥ 1 passing cycle each, LTP runner integrated,
     headline ratio recorded.
  2. Series 4 (backend-ops-abstraction-rfc) landed upstream
     OR a clear plan to base Series 7 on Series 4's
     not-yet-merged branch (the squash branch starts from
     Series 4's tip, not `master`, if Series 4 hasn't landed).
  3. Fleet bench re-captured on a Phase-J-validated host. The
     cover letter's perf table updated.
  4. checkpatch baseline run on `umlctl-deploy` HEAD — confirm
     0 ERRORS, document any non-actionable WARNINGS (the
     `lstar_gadget.S` long-line warnings are expected).

### Step 1 — Set up the squash branch

```
git -C ~/projects/personal/linux fetch origin master
git -C ~/projects/personal/linux checkout master
git -C ~/projects/personal/linux pull --ff-only origin master
git -C ~/projects/personal/linux checkout -b kvm-v2-series-submit
# OR: based on Series 4
# git -C ~/projects/personal/linux checkout -b kvm-v2-series-submit origin/backend-ops-abstraction-rfc-v1
```

### Step 2 — Per-patch squash loop (×19)

For K in 1..19:

  1. Cherry-pick the **introducing commit** for Patch K (the
     "Source commits" §1 entry, first hash).
  2. For each additional source commit listed for Patch K:
     - `git cherry-pick -n <hash>` (no-commit)
     - Resolve conflicts manually if any
     - `git reset` partial — keep only the hunks that belong
       to Patch K's surface (the audit's "Files touched" list).
       Stage the rest into the next patch.
  3. `git commit -s --amend` to merge the cherry-pick into
     Patch K's commit. Trailer: `Signed-off-by: Michael
     Bommarito <michael.bommarito@gmail.com>`. Strip any
     `Co-authored-by:` trailers per LKML convention.
  4. **Run the pre-flight gate** for Patch K (§1, "Pre-flight
     gate" line). If it fails: STOP. Diagnose. Either fix the
     squash (the source commits weren't the right slice) or
     fix the patch (the squash surfaced a latent merge
     conflict). Do NOT proceed to Patch K+1 without a passing
     gate.
  5. `./scripts/checkpatch.pl --strict --no-signoff
     <patch-file>` clean. Document any non-actionable
     WARNINGS in the commit body if checkpatch flags
     `lstar_gadget.S` long lines or BUG_ON usage.

### Step 3 — Bisect verification on the full series

After all 19 patches squashed:

```
for c in $(git -C ~/projects/personal/linux log master..HEAD --reverse --format=%H); do
    git -C ~/projects/personal/linux checkout "$c"
    make ARCH=um O=~/src/uml-builds/kvm-v2-squash olddefconfig
    scripts/config --file ~/src/uml-builds/kvm-v2-squash/.config \
        --enable UM_BACKEND_KVM_V2 \
        --enable UM_BACKEND_KVM_V2_GADGET
    make ARCH=um O=~/src/uml-builds/kvm-v2-squash olddefconfig
    make ARCH=um O=~/src/uml-builds/kvm-v2-squash -j$(nproc) || exit 1
    # Run the patch-specific gate from §1; skip if "skip" listed
    ./run-substrate-gate.sh --build ~/src/uml-builds/kvm-v2-squash \
        --backend kvm-v2 || exit 1
    tools/testing/selftests/um/cpython-parity/cpython-parity.sh \
        --quick --build ~/src/uml-builds/kvm-v2-squash || exit 1
done
```

Expected: every commit builds. Every commit's pre-flight gate
passes. cpython-parity is allowed to "skip" only on commits
where §1 explicitly says "skip" (Patches 5, 6, 7 pre-trampoline;
Patches 1-4 pre-physmem-memslot).

### Step 4 — Final-tip soak gate

On the tip commit (Patch 19):

```
git -C ~/projects/personal/linux checkout kvm-v2-series-submit
make ARCH=um O=~/src/uml-builds/kvm-v2-squash -j$(nproc)
tools/testing/selftests/um/mt-mini/run.sh \
    --threads 8 --ncpus 4 --iters 400
# Expect: 400/400 = 100%
tools/testing/selftests/um/cpython-parity/cpython-parity.sh
# Expect: parity=21 diverge=0 skip=0
tools/testing/selftests/um/threaded-fork-malloc/run.sh \
    --workers 8 --iters 500 --boots 6
# Expect: 0 / 24000 forks CHILD_FAIL
tools/testing/selftests/um/soak/run-soak-daemon.sh \
    --budget-sec 86400 --workloads memcheck,iocheck,stress-ng,tier1-pylibs
# Expect: ≥ 99.5 % per workload (Wilson 95 % lower bound ≥ 99.0 %)
```

### Step 5 — Format-patch + cover letter fill-in

```
git -C ~/projects/personal/linux format-patch -19 \
    --cover-letter \
    --subject-prefix='PATCH RFC' \
    --output-directory /tmp/kvm-v2-series-emit/
# Edit /tmp/kvm-v2-series-emit/0000-cover-letter.patch by hand:
# - paste body from Documentation/.../0000-cover-letter.patch.md
# - update diffstat from format-patch's auto-generated stub
# - update fleet bench numbers from Step 0 re-capture
```

### Step 6 — Audit round

```
scripts/get_maintainer.pl /tmp/kvm-v2-series-emit/*.patch > /tmp/kvm-v2-cc.txt
diff /tmp/kvm-v2-cc.txt expected-cc.txt
# Then re-run an external upstream audit prompt against the
# squashed series. Any findings become a v2 of the series.
```

### Step 7 — send-email

After audit converges:

```
git -C ~/projects/personal/linux send-email \
    --to linux-um@lists.infradead.org \
    --cc kvm@vger.kernel.org \
    --cc pbonzini@redhat.com \
    --cc seanjc@google.com \
    --cc richard@nod.at \
    --cc johannes@sipsolutions.net \
    --cc anton.ivanov@cambridgegreys.com \
    --cc benjamin@sipsolutions.net \
    --cc tiwei.btw@antgroup.com \
    --cc linux-arch@vger.kernel.org \
    /tmp/kvm-v2-series-emit/
```

## Section 5 — Gates that must pass before send

Re-stated, in order of precedence. **All MUST pass** on the tip
commit (Patch 19) on a Phase-J-validated host.

  1. **Boot.** `init=/bin/echo hi` succeeds under
     `CONFIG_UM_BACKEND_KVM_V2=y`, `CONFIG_UM_BACKEND_KVM_V2_
     GADGET=y`, `--with-cpus=4`.
  2. **Substrate gate.** `PASS=25 FAIL=3 XFAIL=3` (bit-identical
     to seccomp baseline). The 3 FAILs are upstream-regrtest
     known-fails; the 3 XFAILs are UML-specific paths gated
     XFAIL upstream.
  3. **cpython-parity.** 21/21 PARITY under both UP and SMP.
     `parity=21 diverge=0 skip=0`. The wide-parity sweep (134
     curated modules) confirms 134 PARITY / 0 REGRESSION / 1
     KVM_BETTER / 5 BOTH_FAIL (the 5 BOTH_FAIL are present on
     seccomp too — not v2 regressions).
  4. **mt-mini SMP T=8 ncpus=4 N=400.** 400/400 = 100 %.
  5. **threaded-fork-malloc 8w × 500i × 6 boots.** 0 / 24 000
     forks CHILD_FAIL.
  6. **24h frozen-binary soak.** ≥ 99.5 % per workload (Wilson
     95 % lower bound ≥ 99.0 %); Tier 1/2/3 + LTP wired with at
     least one passing cycle each; headline ratio (kvm-v2 /
     seccomp on perf-py-startup) ≤ 1.20. This is the **Phase J
     DONE certificate** — the wall-clock-tracked gate that
     unblocks send.
  7. **AVX/XSAVE matrix.** stress-ng `--vm --verify` sampled
     vm-methods: 12/14 PASS post-Phase-A (mscan + prime-incdec
     known-fail = AVX-512 territory; expected fail).
  8. **userspace ABI test.** RDX/R8/R10 across 11 gadget
     syscalls × 3 saved registers: 33/33 PASS.
  9. **KUnit suites.** `kvm_v2_marshal` 8/8 +
     `kvm_v2_byteshape` 9/9 (reconcile the 9/9 vs source-tree
     count before send — see §3.4 (h)).
  10. **bench-py + bench-micro getpid.** bench-py kvm-v2 ≥ 3×
      faster than seccomp; bench-micro getpid kvm-v2 ≤ 200
      cyc/call (gadget hot path).
  11. **perf-py-startup ratio.** ≤ 1.20 (gate ceiling). Post-T55
      reading is 1.10-1.17.
  12. **checkpatch.** `./scripts/checkpatch.pl --strict
      --no-signoff` clean on every squashed patch. Non-
      actionable WARNINGS documented in commit body.
  13. **`scripts/get_maintainer.pl`** against the squashed
      series produces the expected To/Cc list (§6).

Any gate failure = STOP. Diagnose. Either re-spin the squash or
re-spin the substrate.

## Section 6 — Mailing list distribution

### Primary (To)

  - `linux-um@lists.infradead.org`
  - `kvm@vger.kernel.org`

### Cc

  - Paolo Bonzini <pbonzini@redhat.com>
  - Sean Christopherson <seanjc@google.com>
  - Richard Weinberger <richard@nod.at>
  - Johannes Berg <johannes@sipsolutions.net>
  - Anton Ivanov <anton.ivanov@cambridgegreys.com>
  - Benjamin Berg <benjamin@sipsolutions.net>
  - Tiwei Bie <tiwei.btw@antgroup.com>
  - `linux-arch@vger.kernel.org`
  - `linux-kernel@vger.kernel.org` (de rigueur; many maintainers
    auto-archive from here)

Run `scripts/get_maintainer.pl` against the rebased + squashed
series before emission; the list above is from the 2026-04
tree and may have drifted. Diff against `/tmp/kvm-v2-cc.txt` as
described in Step 6.

### Patchset subject prefix

`[PATCH RFC v2 00/19]` for the first emission. Reasoning:

  - **RFC** because Series 4 (backend-ops-abstraction-rfc) is
    itself RFC; Series 7 inherits the RFC status until Series 4
    lands.
  - **v2** because this is the second revision of the kvm-v2
    series — the 2026-04-27 Stage-A 15-patch draft is "v1" of
    this series in spirit, even though it was never sent.
    Reviewers who saw cover-letter chatter on the linux-um list
    will recognise the version bump.

If reviewers indicate Series 4 is on the merge path, change to
`[PATCH v3 00/19]` for the third emission (post-review-round-1)
and so on.

### In-reply-to of any prior RFC

**No prior RFC has been sent.** The 2026-04-27 Stage-A draft
never reached `git send-email`. There is no `Message-Id:` to
quote. The RFC v2 emission is the first time these patches hit
LKML.

If asking for `--in-reply-to` against the linux-um mailing list's
broader v2 discussion thread (any thread from
2026-04 onwards where reviewers asked "what about per-CPU?"),
that's a judgement call. Default plan: do NOT reply-in-thread —
ship as a clean top-level thread so archive search finds it.

## Section 7 — Post-send resolution path

Each review round = new `v<N>/` directory sibling in
`upstream-patches/kvm-backend-series/`. On land, move to
`upstream-patches/landed/kvm-backend-series-vN/` and append the
landing commit IDs to this file and to `SUBMISSION-NOTES.md`.

If the squash audit produces an emission shape that diverges
from §1 above, **update this file FIRST, then SUBMISSION-NOTES,
then the cover letter** before sending. The order matters
because the squash audit is the authoritative source of truth
for what got folded into what; SUBMISSION-NOTES is the planning
document; the cover letter is the reviewer-facing artifact.

## Section 8 — Caveats + open questions for the squash session

  1. **Series 4 dependency.** Series 7 patches 1-3 use the
     `struct um_backend_ops` shape Series 4 introduces. If
     Series 4 hasn't landed when the squash runs, the squash
     branch must base on Series 4's tip (not master), and the
     cover letter must say so. This audit assumes Series 4 has
     landed.
  2. **Patch 15 size.** Folding T73/T74/T75/T76 into Patch 15
     makes it the second-densest patch (LoC-wise) after Patch 8.
     Reviewers may push back asking for a split into Patch 15a
     + 15b. Be ready (§3.4 (i)). Default plan: ship as one.
  3. **State-trace exclusion.** This audit drops `state_trace.
     [ch]` and all `KVMV2_TRACE()` hookpoints from the upstream
     series. The redesign memos heavily reference them, which
     may confuse reviewers cross-referencing memo 26 to the
     emitted patches. Mitigation: cover letter §"The v2
     architecture" already glosses over the debug infrastructure;
     the redesign memos remain in
     `Documentation/virt/uml/redesign/02-workstreams/D-kvm-
     backend/state-audit/` for the curious.
  4. **PML4[448] vs Series 4's backend ops contract.** If
     Series 4 doesn't expose an "install kernel-half PML4 slot"
     hook, Patch 9 has to drive it from inside kvm-v2's backend
     init. The squash audit assumes this is the case (the
     authoring branch's `swapper_pg_dir[448]` explicit patch is
     in `arch/um/backend/kvm-v2/vcpu.c`, not in a backend-
     contract hook).
  5. **Per-mm host worker (memo 25 R4).** Out of scope per §0.
     Cover letter §"Known limitations" already documents the
     deferral. Reviewer question (c) is pre-staged.
  6. **AVX-512 (SMP-T57 Phase B).** Out of scope per §0. Cover
     letter §"Known limitations" already documents the
     deferral. Follow-up series.
  7. **The wall-clock-tracked 24h soak.** This audit does NOT
     start the soak — it documents the gate. The soak is being
     tracked in PLAN-2026-05-14 §3 (Phase J DONE definition).
     When the soak completes, this audit becomes actionable.

## Section 9 — Estimated effort

The squash session itself, end-to-end:

  - **Step 0 (pre-conditions):** ~1 day, mostly waiting for
    Phase J DONE.
  - **Step 1 (set up branch):** ~10 minutes.
  - **Step 2 (per-patch squash, 19 patches):** ~2-3 hours per
    patch × 19 = 38-57 hours. The hot-file patches (4, 6, 8,
    10, 15, 18) take longer; the docs patch (19) takes ~30
    minutes.
  - **Step 3 (bisect verification):** ~3 hours (19 patches × 10
    min/patch for olddefconfig + build + quick gate).
  - **Step 4 (final-tip soak gate):** ~26 hours wall-clock
    (24h soak is the long pole).
  - **Step 5 (format-patch + cover letter):** ~2 hours.
  - **Step 6 (audit round):** ~4 hours.
  - **Step 7 (send-email):** ~30 minutes (after the audit
    converges).

Total: ~5-7 days of focused squash work, **plus** the 24h soak,
**plus** any review-round iterations after the initial RFC v2.

## Section 10 — Sign-off

This memo is the operational plan, not the squash itself. The
squash is GATED on:

  1. Phase J DONE (PLAN-2026-05-14 §3).
  2. Series 4 landed (or a defensible plan to base on its tip).
  3. Fleet bench re-captured (cover letter perf table updated).

When all three are true, the squash session opens with §"Step
0 — Pre-conditions" and proceeds to §"Step 7 — send-email".

Until then, this plan lives in tree as the authoritative
description of how the squash will run.
