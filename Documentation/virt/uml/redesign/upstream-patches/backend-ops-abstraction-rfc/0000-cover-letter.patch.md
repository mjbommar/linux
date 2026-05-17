# 0000-cover-letter.patch — draft (backend-ops-abstraction RFC, 2026-05-14)

Draft form of the cover letter that will head the RFC series
emitted from `git format-patch` once the on-branch A-workstream
commits have been rebased into a clean ~12-patch sequence (see
SUBMISSION-NOTES.md "squash audit" section). Not yet a `.patch`
file because the source-side patches don't exist as 0001-* etc.
yet — the on-branch state is one ~6.6 kLoC megacommit plus
follow-on refactor commits, which need a structured re-emit.

Reviewers seeing this in the upstream-patches tree should treat
it as the **shape** the maintainer-facing RFC will take when it
is sent. The pitch, the op surface, and the migration order are
load-bearing; the per-patch byte counts and the exact diffstat
will be filled in post-rebase.

## Pre-letter preamble — what changes since the original A-01 LKML memo

The 2026-04-17 LKML memo at
`02-workstreams/A-backend-abstraction/notes/10-lkml-memo.md`
described an 18-op contract circa contract version 1, with
`run_userspace` / `mm_map` / `mm_unmap` / `context_switch` /
`read_clock_ns` as the HOT set, and ptrace + seccomp as the two
landing implementers.

Since then the on-branch substrate has moved:

  - Refactor 2 (memo 25 R2, commit `764eac6d79de`): per-op `mm_id *`
    parameters → `struct mm_struct *`, so backends manage their own
    per-mm storage layout (seccomp keys on `mm->context.id`; v2 KVM
    will key on a per-mm worker hash).
  - Refactor 5 (memo 25 R5, commit `d3b2035f4079`): introduces
    `struct um_memory_region` and replaces `mm_map` / `mm_unmap`
    with `mm_region_added` / `mm_region_removed` /
    `mm_region_protected` — a region-shaped op surface that
    matches both seccomp's stub `mmap`/`munmap` pair and v2's
    `KVM_SET_USER_MEMORY_REGION` slot table.
  - Refactor 11 (memo 25 R11 Option B, commit `06c88545ae2c`):
    removes the ptrace backend. seccomp is the sole stub-child
    backend in tree; ptrace lives in the `kvm-v1-archive-20260428`
    tag for anyone who wants to revive it. This collapses the
    "abstract over ptrace vs seccomp" framing into "abstract over
    seccomp vs the v2 KVM backend that is on the way."
  - Contract version bumped to 2 (commit `77cc1821c595`): added a
    nullable `tlb_kick_others(mm)` op for cross-vCPU TLB-flush
    delivery on backends with per-vCPU guest TLBs.
  - D59 / Phase II Lift #4d+ (commit `24699ccc795e`): removed the
    public `using_seccomp` extern; capability flags
    (`uses_stub_reaper`, `has_syscall_stub_fd_map`,
    `stub_syscall_uses_futex`, `stub_child_runs_seccomp`) on the
    ops table replace the legacy global probe.

The upstream-bound shape is therefore:

  - 17 ops + 4 capability flags in `struct um_backend_ops`.
  - Contract version 2 (the nullable `tlb_kick_others` is the only
    addition since v1).
  - 5 HOT ops: `vcpu_run`, `mm_region_added`, `mm_region_removed`,
    `context_switch`, `read_clock_ns`.
  - Two implementers visible to the upstream reader: seccomp
    (existing 6.16 substrate, now driven through the table) and a
    KVM-v2 stub registered under EXPERT (Phase A.1; the real
    KVM-v2 backend lands in Series 7).
  - Two dispatch modes: `CONFIG_UM_BACKEND_SECCOMP_ONLY` (single
    backend, inlined dispatch, zero indirect-call cost) and
    `CONFIG_UM_BACKEND_DYNAMIC` (both compiled in, one indirect
    branch on the dispatch macro).
  - One arbiter, `arch/um/kernel/backend.c::init_backend()`,
    driven by the `backend=auto|seccomp|kvm|force=*` boot
    parameter.

Everything below is finalised in shape but not in numbers: the
per-patch line counts and the final cross-host bench delta come
from the squash audit and the re-bench described in
SUBMISSION-NOTES.md.

---

    Subject: [PATCH RFC 00/12] um: introduce struct um_backend_ops

    From: Michael Bommarito <michael.bommarito@gmail.com>

    Hi Richard, Johannes, Benjamin, Anton, Tiwei, linux-um —

    This is an RFC for a typed function-pointer contract in
    arch/um/ that lifts the existing informal "backend" branches
    in arch/um/{kernel,os-Linux}/skas/ into struct um_backend_ops.

    Two trap mechanisms coexist in arch/um/ today: the legacy
    ptrace skas mode (removed on the redesign branch but still in
    today's mainline) and the seccomp mode merged in 6.16. They
    are selected at runtime via a single global `int using_seccomp`
    plus ~17 grep hits across 7 files. The mechanism works. Adding
    a third backend (KVM, gVisor-style — workstream D / Series 7)
    means adding a third runtime branch at every site and
    broadening the flag to an enum, which doesn't scale.

    The proposal is to lift the existing branches into a typed
    function-pointer struct (17 ops + 4 capability flags in
    5 categories), with the existing seccomp code as the first
    landing implementer and the dispatch surface designed so a KVM
    backend can plug in without further refactoring. The contract
    is versioned (`UM_BACKEND_CONTRACT_VERSION = 2`) so new ops
    can be added at the tail without breaking existing backends.

    Patch organisation (target: 12 patches, possibly 11-13 after
    the squash audit fuses small fixups):

      Patches 1-3:  ops-table contract — struct definition,
                    dispatch macro, header split between
                    `<asm/backend.h>` (kernel) and
                    `<shared/backend.h>` (USER-side TUs in
                    arch/um/os-Linux/skas/).
      Patches 4-6:  seccomp backend migration — peel the
                    using_seccomp branches in skas/process.c +
                    skas/mem.c + start_up.c off into the ops
                    table implementation under
                    `arch/um/backend/seccomp/`.
      Patches 7-8:  arbiter + boot-param parser — single source
                    of truth for backend selection
                    (`arch/um/kernel/backend.c::init_backend()`)
                    and the `backend=` / `backend=force=*` CLI
                    surface.
      Patches 9-10: Kconfig — choice block + matrix coverage
                    (`*_ONLY` inline mode vs `DYNAMIC` indirect
                    mode); the contract KUnit suite gated on
                    `CONFIG_UM_BACKEND_CONTRACT_TEST=y`.
      Patches 11-12: Documentation — backend-contract.rst (the
                    semantic spec every backend must obey),
                    backends.rst (the user-facing how-to), and
                    the MAINTAINERS entry update.

    Patch count is a starting estimate; the squash audit (see
    SUBMISSION-NOTES.md "On-branch commit map") will shape the
    final count. The on-branch sequence is one ~6.6 kLoC megacommit
    (`3c2d95683123`) plus four follow-on cleanups (R2, R5, R11,
    D59) and one contract bump (R5b / `tlb_kick_others`). The
    upstream emission unpicks the megacommit into reviewable slices
    per D10 ("A-02 ships in slices, not as one 6-week megaseries").

    All patches are gated by `CONFIG_UM_BACKEND_SECCOMP_ONLY` or
    `CONFIG_UM_BACKEND_DYNAMIC`; building today's tree without
    either produces a kernel byte-identical to mainline.

    === The problem in arch/um today ===

    Concrete example from `arch/um/os-Linux/start_up.c` (the seccomp
    probe in `os_early_checks`, abbreviated and dated to the v1
    state before the refactor):

        void __init os_early_checks(void)
        {
            check_coredump_limit();
            check_tmpexec();

            /* ... */

            if (init_seccomp()) {
                using_seccomp = 1;
                return;
            }

            if (seccomp_config == 2)
                fatal("SECCOMP userspace requested but "
                      "not functional!\n");

            /* fall through to ptrace */
        }

    And the consumer site in `arch/um/os-Linux/skas/process.c`
    (abbreviated from the v1 state):

        void userspace(struct uml_pt_regs *regs)
        {
            ...
            if (using_seccomp) {
                /* seccomp dispatch path:
                 *   set_stub_state, futex wake the child,
                 *   wait_stub_done_seccomp, get_stub_state,
                 *   GET_FAULTINFO_FROM_MC, ...
                 */
            } else {
                /* ptrace dispatch path:
                 *   PTRACE_SETREGS, PTRACE_CONT,
                 *   wait_stub_done, PTRACE_GETREGS,
                 *   get_skas_faultinfo, ...
                 */
            }
            ...
        }

    There are 17 such branches today (grep `using_seccomp`
    arch/um/) spread across the trap loop, the mm-attach path, the
    syscall-stub flush, the signal-handler installer, and the
    debug-dump path. Adding KVM as a third backend means converting
    every `if (using_seccomp)` into a 3-way switch and broadening
    the global from an `int` to an `enum`. The third backend's
    dispatch shape is also fundamentally different — it is a
    `KVM_RUN` ioctl, not a SIGSYS handler — so each branch site
    ends up carrying KVM-specific state-fetch logic
    (`kvm_run->s.regs.regs.rax`, the mmap'd vCPU shared page) right
    next to the seccomp message-passing logic.

    Twelve months of carry-on without abstraction is fine; carrying
    a 3-way switch at every dispatch site forever is not.

    === The proposed abstraction ===

    The header (excerpted from `arch/um/include/shared/backend.h`
    on the redesign branch, contract version 2):

        struct um_backend_ops {
            const char			*name;
            enum um_backend_kind		kind;
            u32				contract_version;

            /* observable backend metadata (replaces using_seccomp
             * et al as host-side capability flags). Each flag is
             * consulted by a specific os-Linux/ call site whose
             * dispatch shape differs across backends. */
            bool				uses_stub_reaper;
            bool				has_syscall_stub_fd_map;
            bool				stub_syscall_uses_futex;
            bool				stub_child_runs_seccomp;

            /* Lifecycle and trap (4) */
            int  (*probe)(void);
            int  (*init)(const struct um_backend_args *args);
            void (*shutdown)(void);
            void (*vcpu_run)(struct uml_pt_regs *regs);	/* HOT */

            /* Memory (5) */
            int  (*mm_create)(struct mm_struct *mm);
            void (*mm_destroy)(struct mm_struct *mm);
            int  (*mm_region_added)(struct mm_struct *mm,	/* HOT */
                                    const struct um_memory_region *region);
            int  (*mm_region_removed)(struct mm_struct *mm,	/* HOT */
                                      const struct um_memory_region *region);
            int  (*mm_region_protected)(struct mm_struct *mm,
                                        const struct um_memory_region *region);

            /* Scheduling (4) */
            int  (*thread_create)(struct task_struct *p,
                                  void *stack, void (*handler)(void));
            int  (*thread_start_idle)(void *stack,
                                      struct thread_struct *t);
            void (*context_switch)(struct task_struct *prev,	/* HOT */
                                   struct task_struct *next);
            int  (*ipi_send)(int cpu, int vector);

            /* Optional cross-vCPU TLB-flush kick. May be NULL. */
            void (*tlb_kick_others)(struct mm_struct *mm);

            /* Time (3) */
            u64  (*read_clock_ns)(void);				/* HOT */
            int  (*set_timer)(int cpu, u64 deadline_ns,
                              enum um_timer_mode mode);
            u64  (*read_persistent_clock_ns)(void);

            /* Debug / introspection (3) */
            void (*init_thread_regs)(unsigned long *gp, unsigned long *fp);
            int  (*read_guest_regs)(struct task_struct *t,
                                    struct pt_regs *regs);
            int  (*write_guest_regs)(struct task_struct *t,
                                     const struct pt_regs *regs);
        };

    Five ops are marked HOT: vcpu_run (per trap),
    mm_region_added / mm_region_removed (per mm-region change,
    drained by um_tlb_sync), context_switch (per task switch),
    read_clock_ns (per timer/clock query). The other 12 are cold
    (per fork, per init, per debug-stop, per timer-rearm).

    Two dispatch modes share the same call-site syntax via a
    one-line macro:

        #if defined(CONFIG_UM_BACKEND_SECCOMP_ONLY)
        # define um_backend_dispatch(op, ...)  seccomp_##op(__VA_ARGS__)
        #else
        # define um_backend_dispatch(op, ...)  (um_backend->op(__VA_ARGS__))
        #endif

    In `_ONLY` builds the macro is a direct call to the named
    backend symbol — zero indirect-dispatch cost, identical
    codegen to today's static `using_seccomp == 1` shape under PGO.
    In `DYNAMIC` builds it is a function-pointer call through the
    global `um_backend` pointer set once at boot by
    `init_backend()` and never updated. Branch-prediction targets
    a stable callee; the indirect-call cost is on the order of
    5 cycles per HOT call site, well-predicted.

    The boot-param surface:

        backend=auto       — Kconfig default; arbiter probes
        backend=seccomp    — prefer seccomp; fall through to
                             alternative if probe fails
        backend=kvm        — prefer KVM (Series 7); fall through
                             on /dev/kvm absent
        backend=force=...  — panic if probe fails; no fallback

    `seccomp=on/auto/off` is preserved one release as an alias to
    `backend=` for transition.

    Per-backend op symbols follow the `<kind>_<op>` naming
    convention (e.g. `seccomp_vcpu_run`, `seccomp_mm_region_added`)
    and are declared in `<shared/backend.h>` under
    `#ifdef CONFIG_UM_BACKEND_<KIND>` blocks, so the dispatch
    macro's token-paste resolves at every call site without
    forcing call-site code to know which backend is active. The
    full per-backend ops struct lives under
    `arch/um/backend/<kind>/<kind>_backend.c` as a `const`
    initialiser, linked in only when the corresponding Kconfig is
    set.

    === Migration plan ===

    Twelve patches, ordered so each intermediate state builds and
    boots:

      Patch 1   um: backend: contract header + dispatch macro

                Lands `<shared/backend.h>` + `<asm/backend.h>` with
                the struct definition, the dispatch macro, the
                `enum um_backend_kind`, the `struct um_backend_args`,
                and the `UM_BACKEND_CONTRACT_VERSION` macro. No
                consumers yet — the global `um_backend` is declared
                but not defined. Header compiles standalone in both
                kernel and USER TU contexts. No behavior change.

      Patch 2   um: backend: arbiter scaffold (init_backend stub)

                Adds `arch/um/kernel/backend.c` with a stub
                `init_backend()` that panics ("no backend
                registered"). Wires it into `linux_main()` after
                `os_early_checks()`. Defines the global
                `um_backend` symbol. No effect on running kernel
                because no backend registers yet. Sets up the
                hook for patches 3-6 to populate.

      Patch 3   um: backend: arch/um/backend/seccomp/ directory
                + ops struct populated with existing entry points

                Moves the existing seccomp entry points (in
                arch/um/os-Linux/skas/*.c that are gated on
                `using_seccomp`) into
                `arch/um/backend/seccomp/{trap_user,mm,lifecycle,
                thread,time,debug_user}.c`. Each file holds the
                seccomp-side of one ops category. The
                `arch/um/backend/seccomp/seccomp_backend.c` file
                holds the `const struct um_backend_ops
                um_backend_seccomp_ops` initialiser and links it
                via the arbiter. No behavior change — the moved
                code still runs under `using_seccomp` for now.

      Patch 4   um: backend: route the trap loop through
                vcpu_run

                Replaces the `if (using_seccomp) { ... }`
                branch in `arch/um/os-Linux/skas/process.c::
                userspace` with
                `um_backend_dispatch(vcpu_run, regs);`. The
                seccomp ops table's `vcpu_run` field carries
                the existing trap loop body. Conformance suite
                test 4 (round-trip parity) verifies the dispatch
                shape is identical pre/post.

      Patch 5   um: backend: route mm lifecycle + region changes
                through the ops table

                Migrates `init_new_context` / `destroy_context`
                in `arch/um/kernel/skas/mmu.c` to call through
                `um_backend_dispatch(mm_create, mm)` /
                `mm_destroy`. Migrates `um_tlb_sync` in
                `arch/um/kernel/tlb.c` to drive the per-region
                ops (`mm_region_added` / `mm_region_removed` /
                `mm_region_protected`) instead of the legacy
                `map` / `unmap` pair. The legacy `os_map_memory` /
                `os_unmap_memory` host helpers stay; the per-mm
                stub-side dispatch is what moves into ops.

      Patch 6   um: backend: route scheduling, time, debug
                through the ops table

                Migrates `new_thread`, `start_idle_thread`,
                `switch_threads`, `os_send_ipi`, `os_nsecs`,
                `os_timer_*`, `os_persistent_clock_emulation`,
                `get_safe_registers`, and KGDB get/set-regs
                paths to call through the dispatch macro. After
                this patch every backend-relevant site in
                arch/um/ flows through `um_backend_dispatch(...)`.
                Conformance suite test 11 (cross-call hygiene)
                gates the migration.

      Patch 7   um: backend: capability flags replace
                using_seccomp side channel

                Replaces the public `using_seccomp` extern with
                four capability flags on the ops table
                (`uses_stub_reaper`, `has_syscall_stub_fd_map`,
                `stub_syscall_uses_futex`,
                `stub_child_runs_seccomp`) consulted by the
                host-side call sites that previously branched on
                `using_seccomp`. The internal symbol stays for
                one release as a transition aid; the public
                extern is removed.

      Patch 8   um: backend: boot-param parser
                (backend=auto|seccomp|kvm|force=*)

                Adds `__uml_setup("backend=", ...)` in
                `arch/um/os-Linux/start_up.c` and the legacy
                `seccomp=` alias. Wires
                `backend_arg_requested` / `backend_arg_force`
                into `pick_dynamic_backend()`. Both globals are
                consumed by `init_backend()` (the arbiter from
                patch 2 is now functional).

      Patch 9   um: backend: Kconfig — choice block + matrix
                coverage

                Adds the `CONFIG_UM_BACKEND_SECCOMP`,
                `CONFIG_UM_BACKEND_SECCOMP_ONLY`, and
                `CONFIG_UM_BACKEND_DYNAMIC` Kconfig symbols
                under a `choice` block in `arch/um/Kconfig`.
                Default = `SECCOMP_ONLY` (matches today's
                single-backend behavior). The `DYNAMIC` choice
                is the slot a future
                `CONFIG_UM_BACKEND_KVM_V2` will plug into.
                Defconfig matrix coverage:
                `uml_seccomp_only_defconfig` builds; legacy
                `defconfig` resolves to the new
                `SECCOMP_ONLY` default.

      Patch 10  um: backend: contract KUnit suite

                Adds `arch/um/backend/contract/test_ops.c` with
                20 KUnit tests under
                `CONFIG_UM_BACKEND_CONTRACT_TEST=y`. The suite
                checks: every HOT op is non-NULL,
                `contract_version` matches the header macro,
                lifecycle / mm / scheduling / time / debug
                round-trip exercises pass on every compiled
                backend. Runs as part of
                `tools/testing/kunit/kunit.py run --arch=um`.

      Patch 11  Documentation: backend-contract.rst +
                backends.rst

                Adds `Documentation/virt/uml/backend-contract.rst`
                (the per-op semantic spec; what each op must do,
                what it may assume, what is guaranteed by the
                arbiter) and `Documentation/virt/uml/backends.rst`
                (user-facing — how to pick a backend, what the
                `backend=` boot param does, what the
                Kconfig choices imply). Updates
                `Documentation/virt/uml/index.rst` table of
                contents.

      Patch 12  MAINTAINERS: USER MODE LINUX entry update

                Adds `arch/um/backend/` and
                `arch/um/include/shared/backend.h` to the
                "USER MODE LINUX" maintainer section. No new
                maintainer; the existing UML group covers the
                files.

    === What this DOES NOT do ===

    Explicit non-goals — pre-empting the most likely "you're
    rewriting UML" objections:

      - **Does not change seccomp dispatch semantics.** The
        seccomp `vcpu_run`'s body is the existing trap loop
        bit-for-bit. Conformance test 4 (round-trip parity)
        verifies; the cpython substrate gate on the redesign
        branch shows 21/21 stdlib modules pass under both the
        pre-refactor and post-refactor builds.

      - **Does not enable the KVM-v2 backend in-tree.** Patch 9's
        Kconfig leaves
        `CONFIG_UM_BACKEND_KVM_V2` defined-but-not-selectable
        without `EXPERT` set. The real KVM-v2 backend lands as
        Series 7 (`upstream-patches/kvm-backend-series/`); this
        series only sets up the shape it plugs into. Reviewers
        do not have to evaluate the KVM-v2 design in this
        series; the abstraction stands on its own merit as a
        cleanup of the existing seccomp dispatch path.

      - **Does not perturb hot paths.** The HOT ops are
        inlined to direct calls in single-backend builds via the
        dispatch macro; codegen under
        `CONFIG_UM_BACKEND_SECCOMP_ONLY` is byte-equivalent to
        today's mainline (size delta is the addition of
        `<asm/backend.h>`, which is empty if `_ONLY` is set
        because the macro inlines to a direct call). In
        `DYNAMIC` builds the HOT ops pay one indirect call per
        site; the dispatch target is stable after boot, so
        branch-prediction is reliable.

      - **Does not reimplement any kernel logic.** UML stays
        upstream kernel + arch/um/ host interface. Only the host
        interface (`arch/um/`) changes; `kernel/`, `mm/`,
        `fs/`, `net/` are untouched.

      - **Does not introduce out-of-tree backend support.** The
        contract is internal to the kernel build; per-backend
        op prototypes live in `<shared/backend.h>` under
        `#ifdef CONFIG_UM_BACKEND_<KIND>` blocks. Out-of-tree
        backends would require a different abstraction (a real
        loadable-module interface with `EXPORT_SYMBOL` markers
        and a version-mismatch policy); that is not what this
        series is.

      - **Does not change observable host-syscall behavior.**
        The host-side syscalls UML issues (clone, mmap, ptrace,
        seccomp filter install, etc.) are preserved bit-for-bit
        because the implementations of each op are the existing
        functions, lifted into ops-table entries unchanged.

    === Performance ===

    Two reference builds on `x86_64 defconfig + ARCH=um`:

      CONFIG_UM_BACKEND_SECCOMP_ONLY:
        - `vmlinux` codegen byte-for-byte identical to today's
          equivalent under PGO. The dispatch macro expands to a
          direct call; the optimiser folds away the
          `um_backend` global access.
        - Microbenchmark (`tools/testing/selftests/um/bench-micro
          run.sh getpid`): no measurable delta within run-to-run
          noise (±0.5 % across 5 runs of 10000 syscalls).

      CONFIG_UM_BACKEND_DYNAMIC (seccomp resident, no KVM):
        - Five HOT ops pay one indirect call each per site, on a
          stable callee. On Zen 4 7840HS:
            getpid round-trip:     ~9 100 cyc → ~9 105 cyc (Δ ≤ 6 cyc).
            tight `getpid` loop:   ~109 000 cyc/syscall on seccomp
                                   baseline; the dispatch overhead
                                   is amortised into the syscall trap.
        - The cold ops pay one indirect call per call (rare); the
          per-op cost is noise vs the operation itself
          (mm-context creation, timer-rearm).

    The performance-CI harness landed alongside the on-branch
    A-07 task (`Documentation/virt/uml/redesign/scripts/uml-perf*.sh`)
    runs this comparison automatically on each build; the
    checked-in baseline JSON files (Ryzen 7 7840HS and Xeon
    E3-1225 v6) gate any regression > 5 %. Maintainers are
    welcome to drop the harness into their CI; it does not require
    LKML-mailing-list infrastructure.

    === Anticipated review questions ===

    Items where reviewer input would particularly help:

      (a) **Op granularity — 17 ops too few or too many?** The
          per-backend sketches (`02-workstreams/A-backend-abstraction/
          notes/04-ptrace-sketch.md`, `/05-seccomp-sketch.md`,
          `/06-kvm-sketch.md`) walk every site against the
          table and show every site maps cleanly. The architecture
          doc's target was "~15 ops"; the table came out to 17
          (+2 over the target, justified by the per-backend
          sketches showing cleaner mappings). Reviewers may want
          to collapse `mm_create`/`mm_destroy` into `init`/
          `shutdown` (rejected per D8 alternative 2 — mm
          lifecycle is per-mm and outlives the once-per-boot
          backend lifecycle), or split `set_timer`'s mode tag
          into separate `enable`/`disable` ops (rejected per
          02-categories.md — cold-op cost is noise either way,
          mode tag is cheaper to maintain).

      (b) **Capability flags vs ops for "what does the host-side
          differ on."** Patch 7's four flags
          (`uses_stub_reaper`, `has_syscall_stub_fd_map`,
          `stub_syscall_uses_futex`, `stub_child_runs_seccomp`)
          replace `using_seccomp` checks at specific os-Linux/
          call sites that need to know the backend's dispatch
          shape. An alternative is to add an op per call site
          and let the backend supply a no-op implementation for
          flags it doesn't care about. We picked flags because
          the consuming sites are static (SIGCHLD-handler
          install, stub-fd map dump, futex-vs-PTRACE_CONT
          dispatch select, clone-tramp init-data builder) and
          the cost of a function-pointer call there would be
          noise vs the flag check. Reviewers may prefer the
          ops-only shape; the change is mechanical.

      (c) **Kconfig defaults — `SECCOMP_ONLY` vs `DYNAMIC` as
          default.** Today the redesign branch defaults to
          `SECCOMP_ONLY` because it's a closer match to mainline
          today (one resident backend, zero indirect-call cost).
          A `DYNAMIC` default would let users override at the
          boot prompt with `backend=...`, at the cost of one
          indirect call per HOT op. The current default minimises
          surprise; the `DYNAMIC` build is opt-in via Kconfig.
          Open to reversal if maintainers prefer the runtime-
          flexible default.

      (d) **Header split between `<asm/backend.h>` and
          `<shared/backend.h>`.** Per D11, USER TUs in arch/um/
          can't include `<asm/...>` (kernel-only path) but can
          include shared headers. The dispatch macro lives in
          the shared header so the trap loop in
          `arch/um/os-Linux/skas/process.c` (a USER TU) can use
          it. Kernel-only bits (the `init_backend()` declaration,
          helpers that take `struct task_struct *` in their
          signatures) stay in `<asm/backend.h>`. An alternative
          is to keep everything in `<asm/backend.h>` and have
          USER TUs call per-backend symbols directly, bypassing
          the macro — rejected because it leaks build-mode
          (`*_ONLY` vs `DYNAMIC`) into every call site.

    === Lineage ===

    This RFC is the upstream-facing emission of workstream A
    (backend abstraction) in the UML redesign plan
    (`Documentation/virt/uml/redesign/02-workstreams/
    A-backend-abstraction/`). The redesign plan runs in a
    personal branch (`umlctl-deploy`) that has carried the work
    forward of upstream since 2026-03; per the in-fork scope
    policy (D45, 2026-04-21), the redesign branch progresses
    independently and offers each workstream upstream when its
    queue position is reached (`upstream-patches/
    SUBMISSION-QUEUE.md`, D63).

    Decisions log entries for this series:

      D2  (2026-04-17) — abstraction shape is a function-pointer
                          struct with ~15 ops + single-backend
                          inline builds for sandbox profiles.
      D8  (2026-04-17) — final contract — 18 ops, 5 hot, sync,
                          contract-versioned. (Today 17 + 4 flags
                          per memo 25 R5.)
      D9  (2026-04-17) — per-backend op prototypes in
                          `<asm/backend.h>`.
      D10 (2026-04-17) — A-02 ships in slices, not as one
                          megaseries (this RFC structures the
                          slices).
      D11 (2026-04-17) — dispatch surface in
                          `<shared/backend.h>`; struct in
                          `<asm/backend.h>`.
      D12 (2026-04-17) — `init_backend()` is the single source
                          of truth for backend selection.
      D13 (2026-04-18) — Kconfig invariant: `_ONLY` =
                          exactly one backend; `DYNAMIC` = both.
      D14 (2026-04-18) — all ops must be non-NULL; no NULL /
                          -ENOSYS fallback.
      D15 (2026-04-18) — `backend=seccomp` triggers the
                          seccomp probe;
                          `backend=auto` stays legacy-compatible.
      D59 (2026-04-23) — capability flags vs using_seccomp side
                          channel.
      D63 (2026-04-23) — submission-queue sequencing decision.

    The full decision log is at
    `Documentation/virt/uml/redesign/04-risks/decisions-log.md`;
    each D## entry walks rationale, alternatives, and revisit
    conditions.

    === Dependencies on / from other queued series ===

    Series 4 depends on:

      Series 3 (ftrace-notrace-generic-v1) — landed upstream
      first. Series 3 lands the `notrace` annotations on
      `kthread()` and `smpboot_thread_fn()` that the redesign
      branch's `-fpatchable-function-entry` instrumentation
      relies on for clean unwind. Series 4 does not literally
      depend on Series 3 to compile, but maintainers will see
      cleaner traces under Series 4's conformance suite if
      Series 3 has landed. Sending Series 4 before Series 3 is
      a recoverable mistake; sending them out of order to
      different lists in the wrong week is the only failure
      mode and the queue avoids that.

    Series 4 is independent of:

      Series 1 (bpf-hygiene-v1)           — pure x86 BPF
                                             hygiene; no UML
                                             contact surface.
      Series 2 (kmsan-arch-callback-rfc)  — mm/kmsan arch
                                             callback shape;
                                             orthogonal to
                                             arch/um/ dispatch.

    Series 4 blocks:

      Series 5 (static-key-hot-paths)     — B-workstream
                                             static-key gates
                                             cite Series 4's
                                             ops table for
                                             the gate macros'
                                             type signatures.
      Series 6 (per-profile C-series)     — sub-series 6c/6d
                                             (KFENCE / KCSAN /
                                             KMSAN) want the
                                             ops table landed
                                             so per-profile
                                             dispatch can name
                                             a backend.
      Series 7 (kvm-backend-series)       — the entire reason
                                             this abstraction
                                             exists. Series 7
                                             patches 1-3 wire
                                             KVM-v2 into the
                                             ops table this
                                             series introduces.

    The submission-queue at `upstream-patches/SUBMISSION-QUEUE.md`
    is authoritative for the inter-series dependency graph.

    === MAINTAINERS routing ===

      Primary list: linux-um@lists.infradead.org
      Cc:           linux-kernel@vger.kernel.org
                    linux-arch@vger.kernel.org   (ops-table
                                                  pattern;
                                                  other arches
                                                  may adopt
                                                  similar
                                                  shapes)
                    Richard Weinberger <richard@nod.at>
                    Johannes Berg <johannes@sipsolutions.net>
                    Anton Ivanov <anton.ivanov@cambridgegreys.com>
                    Benjamin Berg <benjamin@sipsolutions.net>
                    Tiwei Bie <tiwei.btw@antgroup.com>

    `scripts/get_maintainer.pl` should be re-run against the
    rebased + squashed series before emission. The MAINTAINERS
    file already covers `arch/um/` under USER MODE LINUX; patch
    12 adds `arch/um/backend/` and `arch/um/include/shared/
    backend.h` to that section's path list.

    === Reproducing locally ===

    Build single-backend (mainline-compatible default):

      make ARCH=um O=~/src/uml-builds/uml-seccomp-only defconfig
      scripts/config --file ~/src/uml-builds/uml-seccomp-only/.config \
          --enable UM_BACKEND_SECCOMP_ONLY
      make ARCH=um O=~/src/uml-builds/uml-seccomp-only olddefconfig
      make ARCH=um O=~/src/uml-builds/uml-seccomp-only -j$(nproc)

    Build dynamic (the multi-backend mode that Series 7 will
    extend):

      make ARCH=um O=~/src/uml-builds/uml-dynamic defconfig
      scripts/config --file ~/src/uml-builds/uml-dynamic/.config \
          --enable UM_BACKEND_DYNAMIC \
          --enable UM_BACKEND_SECCOMP \
          --enable UM_BACKEND_CONTRACT_TEST
      make ARCH=um O=~/src/uml-builds/uml-dynamic olddefconfig
      make ARCH=um O=~/src/uml-builds/uml-dynamic -j$(nproc)

    Run the contract suite:

      tools/testing/kunit/kunit.py run --arch=um \
          --kconfig_add CONFIG_UM_BACKEND_CONTRACT_TEST=y \
          backend_contract

    Run the perf-CI baseline (the on-branch baseline JSON files
    gate the comparison):

      Documentation/virt/uml/redesign/scripts/uml-perf.sh
      Documentation/virt/uml/redesign/scripts/uml-perf-capture.sh
      Documentation/virt/uml/redesign/scripts/uml-perf-compare.sh \
          baseline.json post-refactor.json

    === Changelog ===

    RFC v1 (this draft): first upstream emission. Lifts the
    18-op contract validated by A-02..A-07 on the
    `umlctl-deploy` branch, with refactors 2/5/11 + the
    `tlb_kick_others` v2 bump folded into the upstream shape.
    seccomp is the sole stub-child implementer; KVM-v2 stub
    registers under EXPERT (real KVM-v2 in Series 7).

    Michael Bommarito (12):
      um: backend: contract header + dispatch macro
      um: backend: arbiter scaffold (init_backend stub)
      um: backend: arch/um/backend/seccomp/ + ops struct populated
      um: backend: route the trap loop through vcpu_run
      um: backend: route mm lifecycle + region changes through ops
      um: backend: route scheduling, time, debug through ops
      um: backend: capability flags replace using_seccomp side channel
      um: backend: boot-param parser (backend=auto|seccomp|kvm|force)
      um: backend: Kconfig — choice block + matrix coverage
      um: backend: contract KUnit suite (20 tests under CONTRACT_TEST=y)
      Documentation/virt/uml: backend-contract.rst + backends.rst
      MAINTAINERS: USER MODE LINUX — arch/um/backend/ entry

     Documentation/virt/uml/backend-contract.rst       | XXX +
     Documentation/virt/uml/backends.rst               | XXX +
     Documentation/virt/uml/index.rst                  |   X +
     MAINTAINERS                                       |   X +
     arch/um/Kbuild                                    |   X +
     arch/um/Kconfig                                   |  XX +
     arch/um/backend/Makefile                          |  XX +
     arch/um/backend/contract/Makefile                 |   X +
     arch/um/backend/contract/test_ops.c               | XXX +
     arch/um/backend/seccomp/Makefile                  |  XX +
     arch/um/backend/seccomp/debug_user.c              |  XX +
     arch/um/backend/seccomp/lifecycle.c               |  XX +
     arch/um/backend/seccomp/mm.c                      |  XX +
     arch/um/backend/seccomp/seccomp_backend.c         |  XX +
     arch/um/backend/seccomp/seccomp_backend.h         |  XX +
     arch/um/backend/seccomp/thread.c                  |  XX +
     arch/um/backend/seccomp/time.c                    |  XX +
     arch/um/backend/seccomp/trap_user.c               | XXX +
     arch/um/include/asm/backend.h                     |  XX +
     arch/um/include/shared/backend.h                  | XXX +
     arch/um/kernel/Makefile                           |   X +
     arch/um/kernel/backend.c                          | XXX +
     arch/um/kernel/exec.c                             |   X +-
     arch/um/kernel/process.c                          |   X +-
     arch/um/kernel/skas/mmu.c                         |  XX +-
     arch/um/kernel/skas/process.c                     |   X +-
     arch/um/kernel/smp.c                              |   X +-
     arch/um/kernel/time.c                             |  XX +-
     arch/um/kernel/tlb.c                              |  XX +-
     arch/um/kernel/um_arch.c                          |  XX +
     arch/um/os-Linux/main.c                           |   X +-
     arch/um/os-Linux/skas/mem.c                       |   X +-
     arch/um/os-Linux/skas/process.c                   | XXX +---
     arch/um/os-Linux/start_up.c                       |  XX +-
     34 files changed, XXXX insertions(+), XXX deletions(-)
     create mode 100644 Documentation/virt/uml/backend-contract.rst
     create mode 100644 Documentation/virt/uml/backends.rst
     create mode 100644 arch/um/backend/Makefile
     create mode 100644 arch/um/backend/contract/Makefile
     create mode 100644 arch/um/backend/contract/test_ops.c
     create mode 100644 arch/um/backend/seccomp/Makefile
     create mode 100644 arch/um/backend/seccomp/debug_user.c
     create mode 100644 arch/um/backend/seccomp/lifecycle.c
     create mode 100644 arch/um/backend/seccomp/mm.c
     create mode 100644 arch/um/backend/seccomp/seccomp_backend.c
     create mode 100644 arch/um/backend/seccomp/seccomp_backend.h
     create mode 100644 arch/um/backend/seccomp/thread.c
     create mode 100644 arch/um/backend/seccomp/time.c
     create mode 100644 arch/um/backend/seccomp/trap_user.c
     create mode 100644 arch/um/include/asm/backend.h
     create mode 100644 arch/um/include/shared/backend.h
     create mode 100644 arch/um/kernel/backend.c

    --
    2.XX.X

---

End of draft. Post-rebase + post-squash pass, this becomes the
real `0000-cover-letter.patch` with the filled-in diffstat, the
re-captured perf-CI numbers, and Patches 1-12 emitted as
`0001-*.patch` … `0012-*.patch` per the SUBMISSION-NOTES.md
"On-branch commit map" section.
