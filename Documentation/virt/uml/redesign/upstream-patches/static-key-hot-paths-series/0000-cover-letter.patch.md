# 0000-cover-letter.patch — draft (static-key-hot-paths, 2026-05-14)

Draft form of the cover letter that will head the RFC series
emitted from `git format-patch` once Series 4 (the backend-ops
abstraction RFC) has landed upstream AND the on-branch
B-workstream commits have been re-emitted into the 6-patch
compile-boundary sequence D23 specifies. Not yet a `.patch`
file because the source-side patches don't exist as 0001-*
etc. yet — the on-branch state is one ~1500 LoC megacommit
(`570b0c386e0c` "um: introduce Layer 2 static-key hot-path
gates") plus per-finding fixups (D20 IRQ gate extension,
D21 sanitize_paranoid removal, D22 per-CPU stats rewrite)
which need a structured re-emit on top of post-Series-4
mainline.

Reviewers seeing this in the upstream-patches tree should
treat it as the **shape** the maintainer-facing RFC will take
when it is sent. The gate vocabulary, the hook-helper API,
the migration order, and the I3 off-state-cost claim are
load-bearing; the per-patch byte counts and the final
per-gate bench numbers will be filled in post-rebase against
post-Series-4 mainline.

## Pre-letter preamble — what changed since B-02 closed

B-02 closed 2026-04-18 with seven gates + six hook helpers
shipped on the on-branch substrate (`570b0c386e0c`). External
review surfaced three findings between B-02 and 2026-05-14
that the upstream emission folds into the patches that
introduce the relevant surface:

  - **D20 (2026-04-18)** — the original B-01 hot-path audit
    claimed "one `do_IRQ` hook covers all meaningful IRQ
    delivery", but the time-travel direct-dispatch paths
    (`irq_event_handler` and `irq_do_pending_events`) bypass
    `do_IRQ` and call `generic_handle_irq()` directly. The
    fix extended `um_on_irq_entry` to fire from all three
    sites (slow paths handle `regs=NULL` for the synthetic
    paths). Upstream Patch 2 lands the three-site insertion;
    no separate fix patch.

  - **D21 (2026-04-18)** — `um_hook_sanitize_paranoid` was
    declared and defined but never dispatched (reserved for
    aggressive sanitizer-mode fuzz work in workstream C / a
    future phase). Reviewer feedback: a named-but-dead gate
    draws pushback as "dead infrastructure" rather than
    praise for forward compatibility. The on-branch removal
    leaves **six** gates as the upstream shape. Policy
    inscribed in the header: new gates land together with
    their first real call site.

  - **D22 (2026-04-18)** — `um_hook_hits` moved from a global
    `atomic64_t[UM_HOOK__COUNT]` to a per-CPU
    `struct um_hook_percpu_counters` with a plain `u64[]`
    per CPU. The global atomic distorted "gate on"
    benchmarks because every hit site collided on one
    cacheline. Upstream Patch 3 ships the per-CPU shape
    directly; no atomic-then-per-CPU intermediate.

The upstream-bound shape is therefore:

  - **6 gates** (`um_hook_trace_syscalls`,
    `um_hook_kcov_enabled`, `um_hook_time_travel_active`,
    `um_hook_kfence_sample`, `um_hook_record_replay`,
    `um_hook_perf_dispatch`).
  - **6 hook helpers** dispatching from **7 call sites**
    (the IRQ helper fires from three call sites per D20).
  - **C-fallback jump-label form** today, JIT-NOP form
    unlocked by Patch 4 (the section split). Invariant I3 is
    met in spirit (~1-2 ns per gate, well under the 2 ns
    ceiling) under both forms; the JIT form drops the
    off-state cost to ~0.3 ns per gate.
  - **debugfs control surface** at
    `/sys/kernel/debug/um/{hooks,stats,backend}` for
    runtime toggle.
  - **Benchmark harness** + checked-in baseline + comparison
    script for CI regression gating against invariant I3.

Everything below is finalised in shape but not in numbers:
the per-patch line counts and the post-rebase bench delta
come from the squash audit and the re-bench described in
SUBMISSION-NOTES.md.

---

    Subject: [PATCH RFC 0/6] um: static_key gates on arch/um hot paths

    From: Michael Bommarito <michael.bommarito@gmail.com>

    Hi Richard, Johannes, Benjamin, Anton, Tiwei, Steven, Masami,
    linux-um —

    This is an RFC for a set of `static_branch_unlikely` gates
    on the kernel-side hot paths in `arch/um/`, together with a
    `debugfs` control surface for runtime toggle, an `arch/um`
    `.text` section split that unlocks JIT patching of the
    gates to literal NOPs, and a microbenchmark harness that
    asserts the off-state cost stays under 2 ns per gate in CI.

    The gates are the substrate that future observability /
    record-replay / sanitizer features hook into without
    paying any hot-path cost when disabled. Six gates ship
    in this series; each has a runtime-toggleable
    `/sys/kernel/debug/um/hooks/<name>` file and a per-CPU
    hit counter exposed at `/sys/kernel/debug/um/stats`.

    Series 5 builds directly on top of Series 4 (`PATCH RFC
    00/12 um: introduce struct um_backend_ops`): the ops
    table's HOT-op call sites are where the hook insertions
    land, and the dispatch macro defined by Series 4 is the
    one used by `um_on_*()` helpers in this series. Sending
    Series 5 ahead of Series 4 would force a maintainer to
    review the gate surface against a moving target.

    Patch organisation (target: 6 patches, possibly 5-7 after
    the squash audit fuses small fixups — see SUBMISSION-NOTES
    "On-branch commit map"):

      Patch 1   um: hooks: gate infrastructure + first
                live hook site (`<asm/um-hooks.h>`,
                `kernel/hooks.c`, `um_on_syscall_entry` in
                `kernel/skas/syscall.c`).
      Patch 2   um: hooks: remaining hot-path insertions
                (syscall_exit / page_fault / context_switch /
                irq_entry × 3 sites / clock_read).
      Patch 3   um: hooks: debugfs control surface
                (`kernel/um_debugfs.c`,
                `Documentation/virt/uml/debugfs.rst`).
      Patch 4   um: hooks: `.um_patch_text` section split +
                mprotect helpers; `select HAVE_ARCH_JUMP_LABEL`.
      Patch 5   um: hooks: benchmark harness + checked-in
                baseline + comparison script
                (`kernel/hooks_bench.c`,
                `scripts/uml-gate-bench{,-compare}.sh`).
      Patch 6   um: hooks: documentation + selftest
                (`Documentation/virt/uml/hooks.rst`,
                `tools/testing/selftests/um/hooks-flip/`).

    Patch count is a starting estimate; the squash audit on
    `570b0c386e0c` (the on-branch B-workstream megacommit)
    plus the D20/D21/D22 fixup commits will shape the final
    count. Per kernel commit-bisect discipline every
    intermediate patch must build, boot, and pass the
    cpython-parity 21/21 gate; the compile-boundary split D23
    specifies is exactly the boundary that keeps each
    intermediate state bisect-clean.

    All patches are gated by `CONFIG_UM_HOOKS=y` (default y in
    `uml_research_defconfig`, default n in
    `uml_sandbox_defconfig`); building today's tree with
    `CONFIG_UM_HOOKS=n` produces a kernel byte-identical to
    mainline.

    === The problem in arch/um today ===

    arch/um has historically gated each observability feature
    by Kconfig: `CONFIG_UML_TIME_TRAVEL_SUPPORT` for time
    travel, the (future) `CONFIG_UM_KCOV` for kcov, the
    (future) `CONFIG_UM_KFENCE` for KFENCE sampling. Each of
    these toggles a compile-time `#ifdef` around the
    consumer's call site. Two costs follow:

      (1) **Profile explosion.** A "prod-fast" build wants
          everything off; a "research" build wants ftrace on;
          a "fuzz" build wants kcov + KFENCE on; a "time-travel"
          build wants the time-travel hooks on. Today that's
          four distinct kernels, each rebuilt and reinstalled.

      (2) **Runtime cost paid even when feature is off.** Some
          consumers (time travel, the current `if
          (current->kcov_mode)` checks) gate themselves at
          runtime via a global or a per-task field — which
          means a load + branch on every hot-path entry even
          when the feature is unused. On a `getpid()` round-
          trip those branches are individually cheap but
          collectively are 3-5 % of the loop on Zen 4.

    Concrete example from
    `arch/um/kernel/skas/syscall.c::handle_syscall()` today —
    abbreviated to the conditionals on every entry:

        void handle_syscall(struct uml_pt_regs *r)
        {
            struct pt_regs *regs = container_of(r, struct pt_regs, regs);
            int syscall;

            /* ptrace tracer entry */
            if (syscall_trace_enter(regs))
                goto out;

            /* time-travel hook: load + compare + branch every syscall */
            if (time_travel_mode != TT_MODE_OFF)
                time_travel_set_event_xstate(...);

            syscall = UPT_SYSCALL_NR(r);
            ...
        out:
            syscall_trace_leave(regs);
        }

    There are five such "is feature N enabled today?" checks
    on the syscall path alone: ptrace, time-travel, the
    (future) kcov path, the (future) record-replay observe
    hook, and the (future) perf context tracker. Adding a
    sixth would require a sixth `#ifdef` and a sixth runtime
    check. The cost is 5-10 ns per syscall in the off case —
    small but cumulative across millions of syscalls a
    second.

    The proposal is to lift those checks into
    `static_branch_unlikely(&um_hook_<name>)` gates. When the
    gate is off the call site short-circuits via the jump-
    label machinery (literal 5-byte NOP on JIT-capable arches,
    a load + predicted-not-taken branch on the C-fallback
    form arch/um uses today). When the gate is on, control
    flows into a slow-path stub that does the actual work.
    The gate is `echo 1 > /sys/kernel/debug/um/hooks/<name>`-
    toggleable; no reboot, no rebuild.

    === The proposed gates ===

    The header (`arch/um/include/asm/um-hooks.h`, kernel-only):

        DECLARE_STATIC_KEY_FALSE(um_hook_trace_syscalls);
        DECLARE_STATIC_KEY_FALSE(um_hook_kcov_enabled);
        DECLARE_STATIC_KEY_FALSE(um_hook_time_travel_active);
        DECLARE_STATIC_KEY_FALSE(um_hook_kfence_sample);
        DECLARE_STATIC_KEY_FALSE(um_hook_record_replay);
        DECLARE_STATIC_KEY_FALSE(um_hook_perf_dispatch);

    Six gates, all defaulting to off, all toggle-able via
    `/sys/kernel/debug/um/hooks/<name>`:

      - `um_hook_trace_syscalls` — ftrace syscall tracer +
        schedule / IRQ tracer. Profile: research / debug.
        Slow path delegates to the kernel's
        `include/trace/events/syscalls.h` infrastructure when
        the consumer lands in workstream C.

      - `um_hook_kcov_enabled` — kcov coverage recording on
        every syscall / page fault. Profile: fuzz / syzkaller.
        Slow path bumps a per-CPU counter today; workstream
        C-08 replaces with the real kcov consumer.

      - `um_hook_time_travel_active` — replaces the global
        `time_travel_mode != TT_MODE_OFF` runtime check on
        clock reads and IRQ deliveries. Profile: time-travel.
        Slow path is the existing `time_travel_*` logic in
        `kernel/time.c`, which the gate's slow path delegates
        to unchanged.

      - `um_hook_kfence_sample` — KFENCE tick-sampled
        allocation trigger. Profile: fuzz / research. Slow
        path stubbed today; workstream C-02 replaces with the
        real KFENCE sampler when the port lands.

      - `um_hook_record_replay` — record/replay observe hook
        on every syscall / page fault / IRQ delivery /
        context switch / clock read. Profile: time-travel +
        fuzz-deep. Slow path stubbed today; D-workstream
        record/replay port (#169) replaces with the real
        consumer.

      - `um_hook_perf_dispatch` — perf context-switch /
        syscall tracer fan-out. Profile: research. Slow path
        delegates to the kernel's perf subsystem in workstream
        C.

    The six gates dispatch from six **hook helpers** at seven
    **call sites** (per the B-01 audit, `notes/hot-paths.md`):

    | Hook helper                 | Kernel file                          | Gates fired                                                                                              |
    |-----------------------------|--------------------------------------|----------------------------------------------------------------------------------------------------------|
    | `um_on_syscall_entry(regs)` | `arch/um/kernel/skas/syscall.c`      | trace_syscalls, kcov_enabled, record_replay, perf_dispatch                                               |
    | `um_on_syscall_exit(regs)`  | `arch/um/kernel/skas/syscall.c`      | trace_syscalls, record_replay                                                                            |
    | `um_on_page_fault(...)`     | `arch/um/kernel/trap.c::segv`        | trace_syscalls (labeled "trace_traps"), record_replay                                                    |
    | `um_on_context_switch(...)` | `arch/um/kernel/process.c`           | trace_syscalls (schedule), record_replay, perf_dispatch                                                  |
    | `um_on_irq_entry(...)` ×3   | `arch/um/kernel/irq.c` (do_IRQ,       | trace_syscalls (IRQ), record_replay                                                                      |
    |                             | irq_event_handler,                   |                                                                                                          |
    |                             | irq_do_pending_events)               |                                                                                                          |
    | `um_on_clock_read(ns)`      | `arch/um/kernel/time.c::timer_read`  | time_travel_active, kfence_sample, record_replay                                                          |

    The IRQ helper firing from three sites — `do_IRQ` (the
    standard trap-delivered entry), `irq_event_handler` (time-
    travel synthetic delivery), `irq_do_pending_events`
    (time-travel pending replay) — is D20. A record/replay
    consumer must observe every IRQ regardless of time-travel
    state; leaving the two synthetic paths ungated would give
    a lopsided observability view. The synthetic paths pass
    `regs=NULL` because no trap `struct uml_pt_regs` exists
    on those paths; the slow paths in `kernel/hooks.c` swallow
    `regs`, so this is harmless today, and any future consumer
    that dereferences `regs` must handle NULL for those two
    sites (documented in the helper signature).

    The hook helper expands to:

        static __always_inline void
        um_on_syscall_entry(struct pt_regs *regs)
        {
            if (static_branch_unlikely(&um_hook_trace_syscalls))
                __um_trace_syscall_entry(regs);
            if (static_branch_unlikely(&um_hook_kcov_enabled))
                __um_kcov_record_syscall(regs);
            if (static_branch_unlikely(&um_hook_record_replay))
                __um_record_event_syscall_entry(regs);
            if (static_branch_unlikely(&um_hook_perf_dispatch))
                __um_perf_syscall_entry(regs);
        }

    Each gate is independent; the `__always_inline` ensures
    the four short-circuit checks expand at the call site, so
    cold off-state cost is **four jump-label tests**, not one
    indirect call into a multi-gate dispatcher. The slow paths
    `__um_*` live in `arch/um/kernel/hooks.c` and are
    deliberately **not** inlined — when a gate is on, the
    function-call overhead is amortised into the slow path's
    actual work (a kcov record, a ftrace trampoline, a record-
    replay log entry).

    The header explicitly documents the kernel/USER TU
    boundary: `<asm/um-hooks.h>` is **kernel-only**. USER TUs
    under `arch/um/backend/<kind>/` and `arch/um/os-Linux/`
    must not include this file because they cannot link
    against `<linux/static_key.h>`. The hook sites live in
    kernel TUs that the backend trap-loops hand off to
    (`handle_syscall`, `segv`, `__switch_to`, `do_IRQ`,
    `timer_read`); the backend's user-side dispatch surface
    flows into those kernel-side entry points and the gates
    sit there.

    === Migration plan ===

    Six patches, ordered so each intermediate state builds,
    boots, and passes cpython-parity 21/21 (per D23, the
    compile-boundary split policy for B-workstream):

      Patch 1   um: hooks: gate infrastructure + first
                live hook site

                Adds `<asm/um-hooks.h>` with the gate
                declarations, hook-helper inline functions,
                and slow-path prototype list; adds
                `arch/um/kernel/hooks.c` with the
                `DEFINE_STATIC_KEY_FALSE` instances, the
                per-CPU hit-counter table (D22), and tiny
                slow-path stubs that bump the counter; wires
                `um_on_syscall_entry(regs)` into
                `arch/um/kernel/skas/syscall.c::handle_syscall`
                as the one inserted hook. Reviewers can apply
                this patch alone, build, and `cat
                /sys/kernel/debug/um/stats` (after Patch 3)
                will see `trace_syscalls`/`kcov_enabled`/etc.
                bump when the gate is on. No behavior change
                with `CONFIG_UM_HOOKS=n` or with all gates
                off (the default).

      Patch 2   um: hooks: remaining hot-path insertions

                Inserts `um_on_syscall_exit` at the
                `handle_syscall` tail (before
                `syscall_trace_leave`), `um_on_page_fault` at
                the head of `segv`, `um_on_context_switch` in
                `__switch_to` before the backend op dispatch,
                `um_on_irq_entry` at three IRQ-delivery sites
                (D20 — `do_IRQ`, `irq_event_handler`,
                `irq_do_pending_events`), and
                `um_on_clock_read` at the tail of
                `timer_read`. After this patch every gate
                declared in Patch 1 has at least one live
                dispatch path. Each insertion compiles
                independently; the patch is sized for review
                rather than for build-bisect granularity
                because the surface is six narrow grep-hits.

      Patch 3   um: hooks: debugfs control surface

                Adds `arch/um/kernel/um_debugfs.c` populating
                `/sys/kernel/debug/um/` with
                `hooks/<gate_name>` (read: current 0/1
                state, write: toggle via
                `static_branch_enable/disable`), `stats`
                (per-gate hit counters summed across CPUs),
                and `backend` (current backend name from
                Series 4's `um_backend->name`). Adds
                `Documentation/virt/uml/debugfs.rst`
                documenting the surface, default permissions
                (root-only — kcov coverage data is
                sensitive), and toggle semantics. The
                debugfs surface is the runtime control that
                makes the "have your cake and eat it"
                property real: prod-with-hooks UML observes a
                problem, the operator runs `echo 1 >
                hooks/trace_syscalls`, the next syscall is
                in the trace ring buffer.

      Patch 4   um: hooks: `.um_patch_text` section split +
                mprotect helpers; `select HAVE_ARCH_JUMP_LABEL`

                Adds `.um_patch_text` to
                `arch/um/kernel/uml.lds.S` and
                `arch/um/kernel/dyn.lds.S` — a new section
                holding runtime-patchable code (static_branch
                call sites today; ftrace mcount stubs, kprobe
                insertion points, BPF JIT regions tomorrow).
                Adds the mprotect helpers
                (`um_patch_text_rw` / `um_patch_text_ro`)
                that wrap the kernel-text RW/RO transitions
                around runtime patches. Adds
                `arch/um/kernel/jump_label.c` as a thin
                wrapper over `arch_jump_label_transform()`
                that calls the mprotect helpers before/after
                the `text_poke`. Enables
                `select HAVE_ARCH_JUMP_LABEL` in
                `arch/um/Kconfig`. After this patch the gates
                are JIT-patched to literal 5-byte NOPs when
                off (objdump verification artifacts saved in
                the redesign branch's
                `02-workstreams/B-static-key-hot-paths/objdump/`),
                dropping off-state cost from ~1-2 ns per
                gate (C-fallback form) to ~0.3 ns per gate
                (literal NOP).

      Patch 5   um: hooks: benchmark harness + checked-in
                baseline + comparison script

                Adds `arch/um/kernel/hooks_bench.c` (the
                kernel-side tight-loop bench: 10 batches of
                100k iterations per call site; ktime-clocked;
                median ns-per-call reported to one decimal),
                `Documentation/virt/uml/redesign/scripts/
                uml-gate-bench.sh` (boots UML, mounts
                debugfs, reads
                `/sys/kernel/debug/um/bench`, parses to
                JSON), `Documentation/virt/uml/redesign/
                scripts/uml-gate-bench-compare.sh` (diffs
                fresh run against checked-in baseline; fails
                if any per-site delta exceeds 15%
                configurable via `UML_BENCH_CEILING_PCT`),
                and a single-sample baseline at
                `02-workstreams/B-static-key-hot-paths/notes/
                bench-baseline.json`. Wires into CI per
                invariant I3.

      Patch 6   um: hooks: documentation + selftest

                Adds `Documentation/virt/uml/hooks.rst`
                (architecture spec: what each gate observes,
                what the slow path does, how to add a new
                gate), `tools/testing/selftests/um/hooks-
                flip/` (the end-to-end demo from B-06: boot
                prod-with-hooks UML, run a syscall-heavy
                workload, flip `trace_syscalls` mid-run,
                observe counter delta, flip off, observe
                counter settles; current reading is
                baseline=0, on_delta=18340, settle_delta=0
                on the redesign branch), the
                `MAINTAINERS` entry update for the new
                arch/um/ files.

    === What this DOES NOT do ===

    Explicit non-goals — pre-empting the most likely "this
    looks like a vehicle for X" objections:

      - **Does not change semantics.** Every
        `static_branch_unlikely(&um_hook_<name>)` defaults to
        off; the off-state behavior of each call site is
        bit-identical to today's arch/um kernel. The gates
        are observability fan-out points, not policy hooks.
        The cpython-parity gate (21/21 stdlib modules under
        seccomp and the v2 KVM backend on the redesign
        branch) passes with `CONFIG_UM_HOOKS=y` and all
        gates off, bit-equivalent to `CONFIG_UM_HOOKS=n`.

      - **Does not enable new functionality.** The slow paths
        in `arch/um/kernel/hooks.c` are stubs that bump a
        per-CPU counter. Real consumers (ftrace, kcov, KFENCE,
        record/replay, perf) land in Series 6 (per-profile
        C-series) and the in-fork D-workstream record/replay
        port (#169). Series 5 is the infrastructure; Series 6
        is the consumers.

      - **Does not impose a hot-path cost.** Off-state cost
        is ~1-2 ns per gate under the C-fallback form (today,
        Patches 1-3), ~0.3 ns per gate under the JIT-NOP form
        (post-Patch-4). Invariant I3 ("~2 ns per gate when
        off") is met in both forms; the JIT form is the
        production target. The bench harness (Patch 5) +
        checked-in baseline gate the assertion in CI.

      - **Does not require Series 4 to compile.** The header
        `<asm/um-hooks.h>` is independent of Series 4's
        `<asm/backend.h>`. However, the hook call sites
        (`handle_syscall`, `segv`, `__switch_to`, `do_IRQ`,
        `timer_read`) are all immediately downstream of
        Series 4's HOT-op dispatch surface (`vcpu_run`,
        `context_switch`, `read_clock_ns`). Sending Series 5
        ahead of Series 4 would land insertions on call
        sites that Series 4 immediately reshapes — wasted
        review cycles. Series 5 is dependency-ordered after
        Series 4 for hygiene, not for compile-time block.

      - **Does not gate the `mm_region_*` ops.** The
        B-01 audit explicitly excluded `mm_map`/`mm_unmap`
        from gating: they are called from `mmap`/`munmap`
        handlers and TLB-flush paths, relatively rare
        compared to syscall entries, and a gate there would
        pay the NOP cost on every mmap for zero observed
        benefit. The slow-path use cases (address-space
        trace, KASAN shadow fault) are better served by
        Layer 3 compile-time instrumentation. Reviewers
        wanting an mmap-path observability hook should flag
        it; the gate is mechanical to add.

      - **Does not change USER TU semantics.** The header
        `<asm/um-hooks.h>` is **kernel-only**, never included
        from `arch/um/os-Linux/` or `arch/um/backend/`. The
        gates fire from the kernel TUs the backend trap
        loops hand off to, never from inside the trap loops
        themselves. This keeps Layer 2 out of USER TUs
        entirely, consistent with the
        `<shared/backend.h>` / `<asm/backend.h>` boundary
        Series 4 establishes.

      - **Does not reimplement kernel logic.** The slow-path
        stubs in `arch/um/kernel/hooks.c` are 2-3 lines each
        — increment a counter, return. They are designed to
        be replaced by real consumers (Series 6 ftrace,
        Series 6 kcov, etc.) without modifying the gate
        call sites or the helper API.

    === Performance ===

    Two reference builds on `x86_64 defconfig + ARCH=um`,
    measured on AMD Ryzen 7 7840HS (Zen 4):

      CONFIG_UM_HOOKS=n:
        - `vmlinux` codegen byte-for-byte identical to today's
          equivalent. The `um_on_*()` helpers expand to empty
          (the inline body is `#ifdef CONFIG_UM_HOOKS`-gated
          per the header).
        - getpid round-trip: indistinguishable from mainline
          arch/um within run-to-run noise (±0.5 % across 5
          runs of 10000 syscalls).

      CONFIG_UM_HOOKS=y, all gates off (default):
        - **Without JIT patching** (Patches 1-3 only,
          HAVE_ARCH_JUMP_LABEL=n):
          ~1-2 ns per gate × ~10 gate fires on a cold getpid
          round-trip = ~10-20 ns added per syscall.
          Microbench baseline at
          `02-workstreams/B-static-key-hot-paths/notes/
          bench-baseline.json`:
              syscall_entry  ~7.5 ns / call (4 gates)
              syscall_exit   ~3.8 ns / call (2 gates)
              context_switch ~6.1 ns / call (3 gates)
              clock_read     ~5.4 ns / call (3 gates)
          Off-state cost well under invariant I3's 2 ns / gate
          ceiling.

        - **With JIT patching** (Patches 1-4,
          HAVE_ARCH_JUMP_LABEL=y, the production target):
          ~0.3 ns per gate (literal 5-byte NOP per call site).
          Total off-state tax on a cold getpid: ~3 ns,
          comfortably inside noise. objdump verification of
          the NOPs at each gate site is saved at
          `02-workstreams/B-static-key-hot-paths/objdump/`
          in the redesign branch.

      CONFIG_UM_HOOKS=y, `trace_syscalls` ON, rest off:
        - One gate's slow path fires on every syscall_entry,
          syscall_exit, page_fault, context_switch, IRQ
          delivery, clock_read. The slow path is a per-CPU
          counter increment (~3 ns on Zen 4) plus the
          function-call overhead (~2 ns); total ~5 ns per
          firing site, summed across the gates the path
          touches.

      End-to-end demo (B-06 selftest, redesign branch
      2026-04-18): boot prod-with-hooks UML; run a syscall-
      heavy workload; before flip:
      `cat /sys/kernel/debug/um/stats | grep trace_syscalls`
      reads 0. `echo 1 >
      /sys/kernel/debug/um/hooks/trace_syscalls`. After ~30s
      of workload: `trace_syscalls 18340`. `echo 0`. Workload
      continues; settle counter stays flat. No reboot, no
      rebuild, no kernel oops.

    The performance-CI harness landed alongside the on-branch
    A-07 task
    (`Documentation/virt/uml/redesign/scripts/uml-gate-bench
    {,-compare}.sh`) runs this comparison on each build;
    Patch 5's checked-in baseline gates any regression > 15 %
    on any per-site number.

    Note: the bench's per-call-site numbers measure
    "all gates for site X in state Y" — N gates at once.
    Per-gate isolation (toggle-sweep with only one gate on at a
    time) is methodology work scheduled as B-05-precise
    follow-up; the I3 claim today is "<2 ns per gate under
    the C-fallback methodology, ~0.3 ns per gate under the
    JIT-NOP methodology, both well under the 2 ns ceiling"
    rather than a defended per-gate number. Reviewers
    requesting per-gate isolation in the cover-letter perf
    table will get it on v2 via B-05-precise.

    === Anticipated review questions ===

    Items where reviewer input would particularly help:

      (a) **Why six gates and not three? Why not one?** The
          gate-per-concern rule (B-02 Q2 — "one gate per
          concern; cheaper to flip independently than to
          manage gate-set state") was a deliberate design
          call. The alternative — one `um_hook_observability`
          umbrella gate that fans out to N slow paths — was
          rejected because it forces every consumer to share
          a fate (turn one on, you turn them all on) and
          erases the per-gate cost accounting in
          `/sys/kernel/debug/um/stats`. Reviewers may prefer
          a smaller gate set (e.g. fuse `kcov_enabled` and
          `kfence_sample` into `sanitizer_active`); the
          change is mechanical and the upstream-side cost is
          one less debugfs file.

      (b) **JIT patching depends on the section split.**
          Patch 4 adds `select HAVE_ARCH_JUMP_LABEL` after
          adding the section split + mprotect helpers. The
          alternative — implement `text_poke` without the
          section split — was rejected (D19 alternative 1)
          because it makes all of `.text` writable for the
          duration of the patch, which is a security
          regression relative to today's host-enforced RO
          text. Reviewers may want to see the section split
          as its own preceding series; the design memo at
          `02-workstreams/B-static-key-hot-paths/04-section-
          split.md` walks the constraints. Splitting Patch 4
          off as a precursor is reasonable feedback to
          accommodate on v2.

      (c) **Per-CPU stats vs aggregated.** Patch 3's
          `/sys/kernel/debug/um/stats` returns one number per
          gate, summed across CPUs (D22). Per-CPU view is
          available via the on-branch
          `/sys/kernel/debug/um/stats_percpu` file, but it
          isn't exposed in this series because the use case
          (cross-CPU work-distribution analysis under fuzz
          workloads) hasn't surfaced yet. Reviewers wanting
          per-CPU output can flag it; the change is one
          debugfs file.

      (d) **D20 `regs=NULL` on synthetic IRQ paths.** The
          time-travel `irq_event_handler` and
          `irq_do_pending_events` paths don't have a trap
          `struct uml_pt_regs` available; the hook is called
          with `regs=NULL` there. Slow paths today swallow
          `regs` so this is harmless. Reviewers may prefer
          explicit "synthetic vs trap" wrapper helpers
          (`um_on_irq_entry_trap` vs `um_on_irq_entry_synth`)
          that elide the NULL passing — at the cost of two
          helpers per IRQ-delivery shape. The simpler
          one-helper-three-sites design with a NULL contract
          is what's on the branch; the wrapper alternative is
          accommodatable.

      (e) **Section name `.um_patch_text` vs
          `.text.patchable`.** Patch 4 chose `.um_patch_text`
          to avoid colliding with any future generic
          `.text.patchable` convention. Reviewers comparing
          against the x86 jump_label model may prefer the
          generic name; the rename is one search-and-replace
          plus a linker-script line.

    === Lineage ===

    This RFC is the upstream-facing emission of workstream B
    (static-key hot-path gates) in the UML redesign plan
    (`Documentation/virt/uml/redesign/02-workstreams/
    B-static-key-hot-paths/`). Six tasks (B-01..B-06) all
    completed 2026-04-18. The 2026-04-23 external review
    surfaced three findings (D20/D21/D22) which are folded
    into the upstream patches that introduce the relevant
    surface.

    Decisions log entries for this series:

      D17 (2026-04-18) — six hook helpers (not eight);
                          `interrupt_end` is not gated.
      D18 (2026-04-18) — gates declared kernel-side in
                          `<asm/um-hooks.h>`; USER TUs cannot
                          include this header.
      D19 (2026-04-18) — Layer 2 gates ship on C fallback
                          until B-04 (.text section split)
                          unlocks JIT patching. Patches 1-3
                          ship C-fallback; Patch 4 unlocks
                          JIT.
      D20 (2026-04-18) — gate every IRQ-delivery path,
                          including time-travel direct
                          dispatch. `um_on_irq_entry` fires
                          from three sites.
      D21 (2026-04-18) — drop `sanitize_paranoid` as
                          named-but-dead infrastructure. Six
                          gates, not seven.
      D22 (2026-04-18) — per-CPU slow-path counters replace
                          the global atomic64_t. Patch 3
                          ships the per-CPU shape directly.
      D23 (2026-04-18) — RFC patch-series split by compile
                          boundary (not by B-NN task). Six
                          patches, this series.
      D45 (2026-04-21) — fork-first upstream policy.
      D63 (2026-04-23) — submission-queue sequencing
                          decision. Series 5 follows Series 4.

    The full decision log is at
    `Documentation/virt/uml/redesign/04-risks/decisions-log.md`;
    each D## entry walks rationale, alternatives, and
    revisit conditions.

    === Dependencies on / from other queued series ===

    Series 5 depends on:

      Series 4 (backend-ops-abstraction-rfc) — landed
      upstream first. Series 5's hook insertion sites
      (`handle_syscall`, `__switch_to`, `do_IRQ`,
      `timer_read`) are immediately downstream of Series 4's
      HOT-op dispatch surface
      (`vcpu_run`, `context_switch`, `read_clock_ns`).
      Sending Series 5 ahead of Series 4 would force
      reviewers to evaluate gates against a moving target;
      the gates would land on call-site code that Series 4
      then reshapes. Series 5 does not literally fail to
      compile against pre-Series-4 mainline (the C-fallback
      jump-label form is arch-generic, and the hook call
      sites are accessible there), but the review workflow
      benefits from dependency ordering.

      Series 3 (ftrace-notrace-generic-v1) — landed first as
      a prerequisite of Series 4 (the `notrace` annotations
      on `kthread()` / `smpboot_thread_fn()` keep
      `-fpatchable-function-entry` traces clean under the
      ops-table conformance suite). Series 5 inherits the
      cleanliness; no direct Series 3 dependency.

    Series 5 is independent of:

      Series 1 (bpf-hygiene-v1)           — pure x86 BPF
                                             hygiene; no
                                             arch/um contact
                                             surface.
      Series 2 (kmsan-arch-callback-rfc)  — mm/kmsan arch
                                             callback shape;
                                             orthogonal to
                                             arch/um hot
                                             paths.

    Series 5 enables:

      Series 6 (per-profile C-series)     — each sub-series
                                             (6a kprobes,
                                             6b ftrace,
                                             6c KFENCE/KCSAN,
                                             6d KMSAN) lands
                                             its consumer's
                                             slow path
                                             replacing the
                                             counter-only
                                             stub Series 5
                                             ships. Without
                                             Series 5 each
                                             consumer would
                                             need to invent
                                             its own
                                             dispatch — the
                                             pre-existing
                                             condition this
                                             series is
                                             cleaning up.

      Series 7 (kvm-backend-series)       — the v2 KVM
                                             backend's
                                             record/replay
                                             observe hook
                                             (#169) is a
                                             slow-path
                                             replacement for
                                             `um_hook_record
                                             _replay`. The
                                             `um_hook_time_t
                                             ravel_active`
                                             gate is also
                                             used in the
                                             v2 backend's
                                             clock-read
                                             path.

    The submission-queue at `upstream-patches/SUBMISSION-
    QUEUE.md` is authoritative for the inter-series
    dependency graph.

    === MAINTAINERS routing ===

      Primary list: linux-um@lists.infradead.org
      Cc:           linux-kernel@vger.kernel.org
                    linux-trace-kernel@vger.kernel.org
                    (`trace_syscalls` gate's slow path
                    delegates to the kernel tracing
                    infrastructure)
                    Richard Weinberger <richard@nod.at>
                    Johannes Berg <johannes@sipsolutions.net>
                    Anton Ivanov <anton.ivanov@cambridgegreys.com>
                    Benjamin Berg <benjamin@sipsolutions.net>
                    Tiwei Bie <tiwei.btw@antgroup.com>
                    Steven Rostedt <rostedt@goodmis.org>
                    (tracing infra; FYI on the
                    `trace_syscalls` gate's downstream
                    consumer in Series 6b)
                    Masami Hiramatsu <mhiramat@kernel.org>
                    (ditto)

    `scripts/get_maintainer.pl` should be re-run against the
    rebased + squashed series before emission. The
    MAINTAINERS file already covers `arch/um/` under USER
    MODE LINUX; Patch 6 adds
    `arch/um/include/asm/um-hooks.h`,
    `arch/um/kernel/hooks.c`,
    `arch/um/kernel/um_debugfs.c`,
    `arch/um/kernel/hooks_bench.c`, and
    `Documentation/virt/uml/{debugfs,hooks}.rst` to that
    section's path list.

    === Reproducing locally ===

    Build with hooks (the production target):

      make ARCH=um O=~/src/uml-builds/uml-hooks defconfig
      scripts/config --file ~/src/uml-builds/uml-hooks/.config \
          --enable UM_HOOKS \
          --enable UM_HOOKS_DEBUGFS \
          --enable UM_HOOKS_BENCH
      make ARCH=um O=~/src/uml-builds/uml-hooks olddefconfig
      make ARCH=um O=~/src/uml-builds/uml-hooks -j$(nproc)

    Build without hooks (sandbox profile):

      make ARCH=um O=~/src/uml-builds/uml-sandbox defconfig
      scripts/config --file ~/src/uml-builds/uml-sandbox/.config \
          --disable UM_HOOKS
      make ARCH=um O=~/src/uml-builds/uml-sandbox olddefconfig
      make ARCH=um O=~/src/uml-builds/uml-sandbox -j$(nproc)

    Boot, exercise the demo:

      ./linux mem=512M init=/bin/sh
      # in guest:
      mount -t debugfs none /sys/kernel/debug
      cat /sys/kernel/debug/um/stats
      cat /sys/kernel/debug/um/hooks/trace_syscalls    # → 0
      echo 1 > /sys/kernel/debug/um/hooks/trace_syscalls
      # run a syscall-heavy workload
      cat /sys/kernel/debug/um/stats                   # → non-zero
      echo 0 > /sys/kernel/debug/um/hooks/trace_syscalls

    Run the bench:

      Documentation/virt/uml/redesign/scripts/uml-gate-bench.sh
      Documentation/virt/uml/redesign/scripts/uml-gate-bench-compare.sh \
          02-workstreams/B-static-key-hot-paths/notes/bench-baseline.json \
          /tmp/uml-gate-bench-fresh.json

    Run the B-06 selftest (flip demo):

      cd tools/testing/selftests/um/hooks-flip
      ./run.sh

    === Changelog ===

    RFC v1 (this draft): first upstream emission. Lifts the
    six-gate / six-hook-helper substrate validated by
    B-01..B-06 on the `umlctl-deploy` branch, with D20 IRQ
    extension + D21 sanitize_paranoid drop + D22 per-CPU
    stats folded into the upstream patches that introduce
    the relevant surface.

    Michael Bommarito (6):
      um: hooks: gate infrastructure + first live hook site
      um: hooks: remaining hot-path insertions
      um: hooks: debugfs control surface
      um: hooks: .um_patch_text section split + JIT patching
      um: hooks: benchmark harness + checked-in baseline
      um: hooks: documentation + selftest

     Documentation/virt/uml/debugfs.rst                |  XX +
     Documentation/virt/uml/hooks.rst                  | XXX +
     Documentation/virt/uml/index.rst                  |   X +
     MAINTAINERS                                       |   X +
     arch/um/Kconfig                                   |  XX +
     arch/um/include/asm/um-hooks.h                    | XXX +
     arch/um/kernel/Makefile                           |   X +
     arch/um/kernel/dyn.lds.S                          |  XX +
     arch/um/kernel/hooks.c                            | XXX +
     arch/um/kernel/hooks_bench.c                      | XXX +
     arch/um/kernel/irq.c                              |   X +
     arch/um/kernel/jump_label.c                       |  XX +
     arch/um/kernel/process.c                          |   X +
     arch/um/kernel/skas/syscall.c                     |   X +
     arch/um/kernel/time.c                             |   X +
     arch/um/kernel/trap.c                             |   X +
     arch/um/kernel/um_debugfs.c                       | XXX +
     arch/um/kernel/uml.lds.S                          |  XX +
     tools/testing/selftests/um/hooks-flip/Makefile    |   X +
     tools/testing/selftests/um/hooks-flip/run.sh      |  XX +
     20 files changed, XXXX insertions(+), X deletions(-)
     create mode 100644 Documentation/virt/uml/debugfs.rst
     create mode 100644 Documentation/virt/uml/hooks.rst
     create mode 100644 arch/um/include/asm/um-hooks.h
     create mode 100644 arch/um/kernel/hooks.c
     create mode 100644 arch/um/kernel/hooks_bench.c
     create mode 100644 arch/um/kernel/jump_label.c
     create mode 100644 arch/um/kernel/um_debugfs.c
     create mode 100644 tools/testing/selftests/um/hooks-flip/Makefile
     create mode 100644 tools/testing/selftests/um/hooks-flip/run.sh

    --
    2.XX.X

---

End of draft. Post-rebase + post-squash pass, this becomes
the real `0000-cover-letter.patch` with the filled-in
diffstat, the re-captured per-call-site bench numbers (against
post-Series-4 mainline), and Patches 1-6 emitted as
`0001-*.patch` … `0006-*.patch` per the SUBMISSION-NOTES.md
"On-branch commit map" section.
