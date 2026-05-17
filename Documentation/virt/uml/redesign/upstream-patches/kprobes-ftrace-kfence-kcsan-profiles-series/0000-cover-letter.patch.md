# 0000-cover-letter.patch — draft (kprobes-ftrace-kfence-kcsan-profiles, 2026-05-14)

Draft form of the cover letter that will head the RFC bundle
emitted from `git format-patch` once Series 4 (the backend-ops
abstraction RFC) AND Series 5 (the static-key hot-path gates)
have landed upstream, and the on-branch C-workstream commits
across the four sub-tracks (C-02 KFENCE, C-03 KCSAN, C-04
kprobes + rethook + function_graph, C-05 ftrace) have each
been rebased into clean per-sub-series sequences. Not yet a
`.patch` file because the source-side patches don't exist as
`0001-*` etc. yet — the on-branch state is a set of landed
in-fork commits per C-workstream task that need a structured
per-sub-series re-emit on top of post-Series-5 mainline.

Reviewers seeing this in the upstream-patches tree should
treat it as the **shape** the maintainer-facing RFC bundle
will take when it is sent. The four-sub-series split, the
per-feature scaffolding pattern, the dependency-ordered
emission cadence, and the "zero overhead when off, gated
through Series 5 hooks when on" performance pitch are
load-bearing; the per-patch line counts and the post-rebase
per-sub-series patch counts will be filled in after the
squash audit lands against post-Series-5 mainline.

## Pre-letter preamble — what changed since the C-workstream tasks landed in-fork

The four C-workstream sub-tracks all landed in-fork in 2026-04
under the D45 fork-first scope policy:

  - **C-02 KFENCE (2026-04-18)** — `HAVE_ARCH_KFENCE` for UML
    on x86_64. `arch/um/include/asm/kfence.h` declares the
    arch contract (`arch_kfence_init_pool()` +
    `kfence_protect_page()`), wired through
    `os_protect_memory()` against the existing UML host-mmap
    address space. Fault integration in `arch/um/kernel/trap.c`
    `segv()` hands off to `kfence_handle_page_fault()` before
    the kernel-mode panic path. Research + fuzz-deep profiles
    enable it by default. 13/27 upstream KUnit tests pass —
    the 12 failures are formatted-stack-trace assertions
    against UML's stack walker (a separate, documented
    walker-renderer gap), not KFENCE-functional failures.

  - **C-03 KCSAN (2026-04-18)** — `HAVE_ARCH_KCSAN` for UML
    on x86_64. `arch/um/include/asm/kcsan.h` for the per-CPU
    state contract; instrumented-memory-access wrappers
    inherit the generic shape. USER-TU opt-outs in
    `arch/um/os-Linux/Makefile` and
    `arch/um/os-Linux/skas/Makefile` keep the
    sanitizer-instrumentation off the host-side
    `<os.h>`-consuming TUs (KCSAN cannot link there because
    its instrumentation calls back into kernel-only globals).
    New `race` profile (`arch/um/configs/profiles/
    race.config`) sets `ncpus=2`, enables `CONFIG_KCSAN=y` +
    `CONFIG_KCSAN_REPORT_ONCE_IN_MS=0`, and ships a 3/3
    selftest at boot. KASAN/KCSAN mutual exclusion resolved
    per D26 (fuzz-deep stays KASAN-only; race is the
    KCSAN-on profile).

  - **C-04 kprobes + rethook + function_graph (2026-04-22)** —
    `HAVE_KPROBES` (int3 + single-step path via the
    existing `X86_EFLAGS_TF` propagation in
    `arch/x86/um/os-Linux/mcontext.c`) and `HAVE_RETHOOK` (the
    modern x86_64 return-probe substrate; auto-selects
    `KRETPROBES` via `arch/Kconfig`) for UML on x86_64.
    `arch/um/kernel/kprobes/{Makefile,core.c}` +
    `arch/um/kernel/rethook.c` +
    `arch/um/include/asm/kprobes.h` are the substrate;
    `arch/um/kernel/trap.c` `relay_signal()` extension is the
    hook. `HAVE_FUNCTION_GRAPH_TRACER` landed in commit 3a/3b
    (commit `858f25db662d` + `e0361af1c605`) — adds
    `ftrace_graph_caller` + `return_to_handler` trampolines
    in `arch/um/kernel/mcount.S`, `prepare_ftrace_return`
    with a preempt-count guard against atomic-context
    shadow-stack pushes (D34 addendum-4), `notrace`
    annotations on the `arch_local_irq_*` family in
    `arch/um/{kernel,os-Linux}/signal.c`, and the
    `select HAVE_FUNCTION_GRAPH_TRACER` in `arch/um/Kconfig`.
    kprobes-stress selftest at
    `tools/testing/selftests/um/kprobes-stress/`: 200 + 2000
    iteration PASS with `errors=0`, `graph=on`. The
    `notrace`-on-generic-kthread/smpboot-thread piece
    (sources 3 in D34) shipped as Series 3
    (`ftrace-notrace-generic-v1/`) — already in the upstream
    queue, READY.

  - **C-05 ftrace (2026-04-19)** — `HAVE_FUNCTION_TRACER` +
    `HAVE_DYNAMIC_FTRACE` +
    `FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY` for UML on
    x86_64. `arch/um/kernel/mcount.S` adds `ftrace_caller` +
    `ftrace_stub` (modeled on the subset of
    `arch/x86/kernel/ftrace_64.S` ftrace_caller needs).
    `arch/um/kernel/ftrace.c` implements `ftrace_make_nop` /
    `ftrace_make_call` / `ftrace_modify_call` /
    `ftrace_update_ftrace_func` / `arch_ftrace_update_code`
    using `text_mutex` + `stop_machine_cpuslocked()` +
    Series 5's `.um_patch_text` section-split mprotect
    helpers (Series 5 Patch 4 ships those; this series
    consumes them). `arch/um/Makefile` adds the
    `CC_FLAGS_FTRACE := -fpatchable-function-entry=5,0`
    override; no `-pg`, no `__fentry__`, no
    `recordmcount.c` dependency — the compiler emits
    `__patchable_function_entries` and the generic ftrace
    core reads it. Research profile enables the tracer.
    `tools/testing/selftests/um/ftrace-smoke/`: PASS with
    `trace_lines=51316` on a minimal `ls /` workload.
    Tracer-off cost +3.3 % on `init=/bin/true` boot,
    inside the A-07 5 % invariant ceiling.

The four C-workstream tasks are independent at the in-fork
substrate level — KFENCE doesn't touch the ftrace mcount
machinery, kprobes doesn't touch KCSAN's per-CPU state, etc.
Bundling them as Series 6 with shared cover-letter framing
preserves their independence at the sub-series level while
giving reviewers the parallel structure of "each feature is
arch-side enablement on top of Series 4 + Series 5, sized
3–5 patches, focused single-maintainer routing per feature."

The upstream-bound shape is therefore:

  - **4 sub-series**, ~20 patches total across them.
  - **Sub-series 6a — kprobes (C-04)** — ~5 patches: arch
    header + kprobes core + rethook port + trap hook +
    research-profile enable + kprobes-stress selftest +
    user doc.
  - **Sub-series 6b — ftrace + function_graph (C-05 +
    C-04 3a/3b)** — ~6 patches: arch ftrace header +
    mcount.S `ftrace_caller` + ftrace.c patching ops +
    Makefile `CC_FLAGS_FTRACE` override + research-profile
    enable + `ftrace_graph_caller` + `return_to_handler` +
    `prepare_ftrace_return` + `notrace` annotations on the
    `arch_local_irq_*` family + Kconfig
    `select HAVE_FUNCTION_GRAPH_TRACER` +
    ftrace-smoke selftest + user doc.
  - **Sub-series 6c — KFENCE (C-02)** — ~4 patches: arch
    KFENCE header + `kfence_protect_page` via
    `os_protect_memory` + segv fault-path hook + Kconfig
    `select HAVE_ARCH_KFENCE` + research + fuzz-deep profile
    enable + user doc.
  - **Sub-series 6d — KCSAN (C-03)** — ~5 patches: arch
    KCSAN header + USER-TU opt-outs in `arch/um/os-Linux/`
    Makefiles + `race` profile fragment + Kconfig
    `select HAVE_ARCH_KCSAN` + KASAN/KCSAN mutual-
    exclusion documentation + race-profile selftest +
    user doc.
  - **Shared cover-letter framing.** This file. Routes to
    `linux-um@` (the cross-cutting list) plus each
    feature's primary subsystem maintainer audience as
    cc per sub-series.

Each sub-series stands alone as an `[PATCH RFC NN/MM]`
series with its own per-sub-series cover letter (derived
from this shared frame) and its own per-feature patches.
This shared cover letter exists to give the four sub-series
a coherent parent-frame for `linux-um@` reviewers who want
to see the bundle as one act of "UML profile-driven feature
selection lands four arch enablement RFCs in parallel."

Everything below is finalised in shape but not in numbers:
the per-patch line counts, the per-sub-series final patch
counts (the squash audit may consolidate small fixups), and
the post-rebase test deltas come from the squash audit and
the re-bench described in SUBMISSION-NOTES.md.

---

    Subject: [PATCH RFC 0/20] um: profile-driven feature selection (kprobes, ftrace, KFENCE, KCSAN)

    From: Michael Bommarito <michael.bommarito@gmail.com>

    Hi Richard, Johannes, Benjamin, Anton, Tiwei, Steven, Masami,
    Marco, Alexander, Dmitry, linux-um, linux-trace-kernel,
    linux-mm —

    This is an RFC bundle for four arch-side enablement
    sub-series in `arch/um/` that turn on, respectively,
    kprobes, ftrace + function_graph, KFENCE, and KCSAN for
    UML on x86_64. The four sub-series are independent at the
    code level — KFENCE doesn't touch the ftrace mcount
    trampoline, kprobes doesn't touch the KCSAN per-CPU
    state — but they share a structural pattern and a parent
    framing: each is the arch enablement for one upstream
    feature, sized 3-6 patches, gated by the per-feature
    `HAVE_*` Kconfig select, and zero-overhead-when-off by
    construction via the Series 5 static-key hot-path
    substrate.

    Bundling them as Series 6 with this shared cover letter
    is a routing decision, not a coupling decision. Each
    sub-series ships as its own `[PATCH RFC NN/MM]` series
    that routes to its primary subsystem maintainer
    (linux-trace-kernel@ for 6a + 6b, linux-mm@ for 6c +
    6d) with `linux-um@` cc'd on all four. Reviewers
    interested in only one feature can ignore the others
    without losing context; reviewers interested in the
    parent shape (the `arch/um/` profile-feature substrate)
    can read this cover letter to see the four sub-series in
    relation.

    This bundle depends on Series 4 (the `struct
    um_backend_ops` ops table) and Series 5 (the static-key
    hot-path gates) having landed upstream. The four
    sub-series each consume Series 4's HOT-op dispatch
    surface (for the trap hook + the mm-region ops that mcount
    patching + KFENCE protect-page transitions rely on) and
    Series 5's hook surface (kprobes consumes the
    `.um_patch_text` section split for instruction-slot
    placement; ftrace consumes it for mcount NOP patching;
    KCSAN consumes the `um_hook_kcov_enabled` /
    `um_hook_kfence_sample` gates' slow-path replacement
    contract). The cover letter §"Dependencies" walks each
    consumption explicitly.

    Sub-series organisation (target: ~20 patches across 4
    sub-series, possibly 18-22 after the per-sub-series
    squash audit fuses small fixups — see SUBMISSION-NOTES
    "On-branch commit map"):

      Sub-series 6a — kprobes (C-04)
        Patch 1   um: kprobes: arch header + int3 substrate +
                  trap-hook into relay_signal()
        Patch 2   um: kprobes: rethook port for return probes
                  (HAVE_RETHOOK; KRETPROBES via arch/Kconfig)
        Patch 3   um: kprobes: research profile fragment +
                  kprobes-stress selftest
        Patch 4   um: kprobes: user doc
                  (Documentation/virt/uml/kprobes.rst) +
                  MAINTAINERS entry update

      Sub-series 6b — ftrace + function_graph (C-05 + C-04 3a/3b)
        Patch 1   um: ftrace: arch header + ftrace_caller +
                  ftrace_stub in mcount.S
        Patch 2   um: ftrace: patching ops + CC_FLAGS_FTRACE
                  override; select HAVE_FUNCTION_TRACER +
                  HAVE_DYNAMIC_FTRACE +
                  FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY
        Patch 3   um: ftrace: function_graph caller +
                  return_to_handler + prepare_ftrace_return
                  (with preempt-count guard per D34
                  addendum-4)
        Patch 4   um: ftrace: notrace on arch_local_irq_* +
                  select HAVE_FUNCTION_GRAPH_TRACER
        Patch 5   um: ftrace: research profile fragment +
                  ftrace-smoke selftest
        Patch 6   um: ftrace: user docs
                  (Documentation/virt/uml/ftrace.rst) +
                  MAINTAINERS entry update

      Sub-series 6c — KFENCE (C-02)
        Patch 1   um: kfence: arch header + protect_page via
                  os_protect_memory
        Patch 2   um: kfence: segv fault-path hook into
                  kfence_handle_page_fault
        Patch 3   um: kfence: select HAVE_ARCH_KFENCE +
                  research + fuzz-deep profile fragments
        Patch 4   um: kfence: user doc
                  (Documentation/virt/uml/kfence.rst) +
                  MAINTAINERS entry update

      Sub-series 6d — KCSAN (C-03)
        Patch 1   um: kcsan: arch header + per-CPU intr
                  state + Makefile USER-TU opt-outs
        Patch 2   um: kcsan: select HAVE_ARCH_KCSAN +
                  KASAN/KCSAN mutual exclusion gate
        Patch 3   um: kcsan: race profile fragment
                  (arch/um/configs/profiles/race.config)
        Patch 4   um: kcsan: race-profile selftest
                  (tools/testing/selftests/um/kcsan-smoke/)
        Patch 5   um: kcsan: user doc
                  (Documentation/virt/uml/kcsan.rst) +
                  MAINTAINERS entry update

    Patch counts are starting estimates; each sub-series's
    squash audit on its on-branch substrate will shape the
    final per-sub-series count. Per kernel commit-bisect
    discipline every intermediate patch within each
    sub-series must build, boot, and pass the
    feature-appropriate test (kprobes-stress for 6a,
    ftrace-smoke for 6b, KFENCE KUnit for 6c, KCSAN
    selftest for 6d). The compile-boundary split D23
    specifies is exactly the boundary that keeps each
    sub-series's intermediate state bisect-clean.

    All patches in all four sub-series are gated by the
    corresponding `CONFIG_*` (`CONFIG_KPROBES`,
    `CONFIG_FUNCTION_TRACER`, `CONFIG_KFENCE`, `CONFIG_KCSAN`,
    `CONFIG_FUNCTION_GRAPH_TRACER`); building today's tree
    with all four off produces a kernel byte-identical to
    mainline modulo the new Kconfig select lines (and the
    selects only activate when `CONFIG_*=y` is also set, so
    the byte-identity holds for the realistic case).

    === The problem in arch/um today ===

    arch/um today is in a partial state on each of the four
    features. Three are silently off (no `select HAVE_*`,
    `CONFIG_*=y` rejected by `make olddefconfig` for lack of
    arch support); one is enabled in a half-broken state
    that fails at runtime when its arch-side prerequisites
    aren't met. Each example below walks one concrete
    failure mode that the corresponding sub-series fixes.

    **(1) kprobes — silently off.**

    Today (pre-Series-6) `arch/um/Kconfig` does not
    `select HAVE_KPROBES`. The result:

        $ make ARCH=um defconfig
        $ scripts/config --enable CONFIG_KPROBES
        $ make ARCH=um olddefconfig
        $ grep '^CONFIG_KPROBES' .config
        # CONFIG_KPROBES is not set

    `CONFIG_KPROBES=y` silently turns off because
    `arch/Kconfig`'s `KPROBES` symbol `depends on
    HAVE_KPROBES`. There's no diagnostic, no warning — the
    feature is silently absent. A user trying
    `samples/kprobes/kprobe_example.ko` on UML hits

        $ insmod samples/kprobes/kprobe_example.ko
        insmod: ERROR: could not insert module
            samples/kprobes/kprobe_example.ko:
            Operation not supported

    because the module's `arch_prepare_kprobe()` consumer
    isn't linked in. Same story for `kretprobe_example.ko`,
    `bpftrace -e 'kprobe:do_sys_open { … }'`, etc.

    The substrate to enable kprobes on UML already exists:
    int3 emission via `BREAKPOINT_INSTRUCTION` is the
    standard x86 path; single-step emulation via
    `X86_EFLAGS_TF` is already propagated through UML's
    mcontext path (`arch/x86/um/os-Linux/mcontext.c:81..84`
    has the existing `single_stepping` mechanism that sets
    and clears `TF` in the mcontext when resuming the stub);
    and the kernel-mode `SIGTRAP` arrival is already an
    explicit panic site in `arch/um/kernel/trap.c::
    relay_signal()` at the `!UPT_IS_USER(regs)` branch — a
    natural insertion point for the kprobe int3 handler
    (panic only on unhandled). Sub-series 6a turns those
    three substrates into a working kprobes implementation
    in ~5 patches.

    **(2) ftrace — silently off.**

    Same shape as kprobes:

        $ scripts/config --enable CONFIG_FUNCTION_TRACER
        $ make ARCH=um olddefconfig
        $ grep '^CONFIG_FUNCTION_TRACER' .config
        # CONFIG_FUNCTION_TRACER is not set

    because `kernel/trace/Kconfig`'s `FUNCTION_TRACER`
    symbol `depends on HAVE_FUNCTION_TRACER`. A UML user
    debugging a stuck driver and reaching for
    `/sys/kernel/tracing/current_tracer` finds the file
    missing entirely; there's no `function`, no
    `function_graph`, no `nop`, no `tracefs` even.
    `bpftrace` falls back to text-only printf debugging
    because every probe needs an entry-point hook the kernel
    won't accept. The half of "all sanitizers and tracing on
    for research" that `00-vision.md` calls out is the half
    that doesn't work today.

    The arch-side substrate is again present: UML's `arch/um/
    kernel/mcount.S` is empty today but compiles; the
    compiler-driver path that emits NOP5 at every function
    entry (`-fpatchable-function-entry=5,0`) works on
    `ARCH=um` builds today (Series 5's `.um_patch_text`
    section split + the standard kernel `CC_FLAGS_FTRACE`
    override are the missing pieces). Sub-series 6b lands
    `ftrace_caller`, `ftrace_stub`, the patching ops
    (`text_mutex` + `stop_machine_cpuslocked` + Series 5's
    `um_kernel_text_patch_begin/end` mprotect wrap), the
    Makefile flag override, and the Kconfig selects in 6
    patches.

    **(3) KFENCE — silently off.**

    Same diagnostic shape; `make olddefconfig` turns off
    `CONFIG_KFENCE=y` because `mm/Kconfig`'s `KFENCE` symbol
    `depends on HAVE_ARCH_KFENCE`. Result: a UML fuzz target
    running for hours-to-days under syzkaller has no
    sampling allocator catching the slab-OOB bugs that
    KFENCE catches on x86 native today. The fuzz profile's
    intent ("catch heap-corruption bugs at the SLAB
    allocator level via guard pages") is silently
    unrepresented.

    The substrate is again present and small. KFENCE's arch
    contract is two functions: `arch_kfence_init_pool()` (a
    one-time pool sanity check; returns true unconditionally
    on UML because UML is always 4 KiB pages and never
    promotes to hugepages for kernel `.data`) and
    `kfence_protect_page(addr, protect)` (toggle a single
    page between PROT_NONE and PROT_READ|PROT_WRITE). UML's
    `os_protect_memory()` already does the second one; the
    first is a `return true`. Plus a 3-line hook in `segv()`
    that calls `kfence_handle_page_fault()` before the
    kernel-mode-segfault panic path. Sub-series 6c is 4
    patches: the arch header, the segv hook, the Kconfig +
    profiles wire-up, and the user doc.

    **(4) KCSAN — silently off, with an extra catch.**

    Same `make olddefconfig` shape:

        $ scripts/config --enable CONFIG_KCSAN
        $ make ARCH=um olddefconfig
        $ grep '^CONFIG_KCSAN' .config
        # CONFIG_KCSAN is not set

    The substrate is again largely present: KCSAN's arch
    contract is per-CPU state for the watchpoint table +
    interrupt-state save/restore (`set_intr_state` /
    `get_intr_state`). UML has per-CPU data (via SMP
    landing in 6.6) and has interrupt-state plumbing
    (`local_irq_save`/`local_irq_restore` work on UML).

    The catch: KCSAN cannot run in USER TUs (the
    `arch/um/os-Linux/` host-side files). Those TUs link
    against libc, not against the in-kernel KCSAN runtime,
    so any `__sanitizer_cov_*` callback emitted by the
    sanitizer-instrumented USER object would fail to link.
    Sub-series 6d's first patch lands the
    `KCSAN_SANITIZE := n` / `KASAN_SANITIZE := n` (KASAN
    inherits the same constraint) lines in `arch/um/
    os-Linux/Makefile` and `arch/um/os-Linux/skas/Makefile`,
    plus the arch KCSAN header, plus the Kconfig select.

    Plus the KASAN/KCSAN mutual-exclusion question:
    `lib/Kconfig.kcsan` already has `depends on
    DEBUG_KERNEL && !KASAN` for KCSAN. The two are mutually
    exclusive everywhere because their per-byte / per-bit
    shadow models conflict. On UML the C-workstream's
    fuzz-deep profile uses KASAN; the new `race` profile
    uses KCSAN. Sub-series 6d ships a `race` profile
    fragment that explicitly turns KASAN off, with a
    profile-doc note that "for an SMP UML with race
    detection, use `make ARCH=um uml/race`; for a fuzz
    target with heap-corruption detection, use
    `make ARCH=um uml/fuzz-deep`."

    **(5) function_graph — half-broken at runtime (the most
    interesting case).**

    function_graph is the one feature on this list that
    isn't silently off today. With Series 6b Patches 1-2 +
    Patch 3a (the `notrace` on generic `kthread()` /
    `smpboot_thread_fn()` that Series 3 ships independently),
    the user can `select HAVE_FUNCTION_GRAPH_TRACER` and
    build a UML kernel that has the option present. But the
    `arch_local_irq_*` family in `arch/um/{kernel,os-Linux}/
    signal.c` is reachable from atomic context via the IRQ
    delivery path; `prepare_ftrace_return` without a
    preempt-count guard pushes onto `current->ret_stack`
    from inside an atomic context, and UML's
    `TINY_RCU`-on-UP maps `rcu_read_lock()` to
    `preempt_disable()`, so a graphed function body
    reaching `free_irq → __mutex_lock` trips
    `__might_resched` inside the trace body. D34
    addendum-4 walks the analysis; Patch 3 of sub-series 6b
    lands the preempt-count guard inside
    `prepare_ftrace_return()` and the `notrace` annotations
    on the IRQ-state helpers that make the guard sufficient.

    The "silently off vs half-broken at runtime" pattern is
    the same shape on every feature this bundle ships: the
    arch substrate is mostly already there, just missing a
    handful of hooks and `select` lines. The 4 sub-series
    are 3-6 patches each because the arch contract for each
    feature is small.

    === The four sub-series in shape ===

    Each sub-series follows the same scaffolding template:

      1. **Arch header** — the feature's
         `arch/um/include/asm/<feature>.h`, declaring the
         arch contract (whatever the feature needs).
      2. **Kconfig** — `select HAVE_<FEATURE>` (sometimes
         gated `if X86_64`).
      3. **Hook** — the existing UML path the feature
         plugs into (`relay_signal()` for kprobes, `segv()`
         for KFENCE, `mcount.S` for ftrace,
         `arch/um/os-Linux/` Makefile USER-TU opt-outs for
         KCSAN).
      4. **Per-feature code** — the actual implementation
         (`core.c`, `rethook.c`, `ftrace.c`, etc.).
      5. **Profile fragment** — the
         `arch/um/configs/profiles/<profile>.config` that
         turns the feature on in the relevant profiles
         (research / fuzz / fuzz-deep / race).
      6. **Selftest** — a `tools/testing/selftests/um/
         <feature>-(smoke|stress)/` harness.
      7. **User doc** — `Documentation/virt/uml/
         <feature>.rst`.

    The patch ordering inside each sub-series follows the
    template in order: scaffolding (header) before Kconfig
    before hook before per-feature code, so each
    intermediate state is bisect-clean. The selftest +
    docs land at the tail so the final per-sub-series
    state is fully reviewable, but bisect through the
    middle stays clean.

    === Migration plan ===

    Sub-series ordering: **6a (kprobes), 6b (ftrace), 6c
    (KFENCE), 6d (KCSAN)**.

    The ordering is by independence + dependency, not by
    user-visible value:

      - **6a first** — kprobes is the most independent of
        the four. The int3 + single-step path doesn't
        consume any other sub-series's surface; the trap
        hook into `relay_signal()` is a single point of
        contact with arch/um's existing code; and the
        rethook port for return probes is self-contained
        (~200 LoC of x86-derived assembly + C).

      - **6b second** — ftrace consumes Series 5's
        `.um_patch_text` section split + mprotect helpers
        (for mcount NOP↔call patching) and the
        compiler-emitted `__patchable_function_entries`
        section that Series 5 unlocked by selecting
        `HAVE_ARCH_JUMP_LABEL`. Sub-series 6b also lifts
        kprobes-on-ftrace as a follow-up — if a reviewer
        asks for `HAVE_KPROBES_ON_FTRACE` during 6a, the
        answer is "6b carries the mcount substrate
        kprobes-on-ftrace needs; 6a's int3 path covers all
        user-visible kprobe semantics in the meantime." See
        cover letter §"Anticipated review questions (q-a)."

      - **6c third** — KFENCE depends on no other
        sub-series in code, but bundles after 6a + 6b
        because the segv-fault-path hook in
        `arch/um/kernel/trap.c` is the same file kprobes
        extends (via `relay_signal()`). Reviewers
        evaluating 6c can see 6a's pattern of "extend
        `trap.c` minimally" and apply the same review lens.
        Some KFENCE shim semantics (the protect_page
        transition under a fault delivery) also benefit
        from kprobes-style int3-single-step audit done
        upstream first.

      - **6d last** — KCSAN's per-CPU state and signal-
        delivery model are the most invasive of the four
        on UML's existing stub paths. Landing 6d after
        6a+6b+6c gives reviewers the cumulative context of
        "arch/um is now an enabling-features arch under
        Series 4+5+6a+6b+6c discipline; here's the fourth
        and most complex one."

    Patch ordering inside each sub-series follows the
    scaffolding template above. Specifically:

      Sub-series 6a (kprobes), 4-5 patches:
        - Patch 1: header + int3 substrate + trap hook
          (the load-bearing single patch).
        - Patch 2: rethook port (return probes).
        - Patch 3: research profile fragment + selftest.
        - Patch 4: user doc + MAINTAINERS.

      Sub-series 6b (ftrace + function_graph), 6 patches:
        - Patch 1: arch header + `ftrace_caller` +
          `ftrace_stub` in `mcount.S`.
        - Patch 2: patching ops + `CC_FLAGS_FTRACE` +
          `HAVE_FUNCTION_TRACER` + `HAVE_DYNAMIC_FTRACE` +
          `FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY`
          selects.
        - Patch 3: `ftrace_graph_caller` +
          `return_to_handler` + `prepare_ftrace_return`
          with preempt-count guard.
        - Patch 4: `notrace` on `arch_local_irq_*` +
          `select HAVE_FUNCTION_GRAPH_TRACER`.
        - Patch 5: research profile fragment + selftest.
        - Patch 6: user doc + MAINTAINERS.

      Sub-series 6c (KFENCE), 4 patches:
        - Patch 1: arch header (`kfence_protect_page` via
          `os_protect_memory`).
        - Patch 2: segv fault-path hook.
        - Patch 3: `select HAVE_ARCH_KFENCE` + research +
          fuzz-deep profile fragments.
        - Patch 4: user doc + MAINTAINERS.

      Sub-series 6d (KCSAN), 4-5 patches:
        - Patch 1: arch header + USER-TU sanitizer opt-outs
          in `arch/um/os-Linux/Makefile` +
          `arch/um/os-Linux/skas/Makefile`.
        - Patch 2: `select HAVE_ARCH_KCSAN` + KASAN/KCSAN
          mutual-exclusion gate.
        - Patch 3: `race` profile fragment.
        - Patch 4: race-profile selftest
          (`tools/testing/selftests/um/kcsan-smoke/`).
        - Patch 5: user doc + MAINTAINERS.

    Each sub-series ships independently with its own
    cover letter (derived from this shared frame), its own
    `git format-patch` output, and its own send to the
    appropriate primary list.

    === What this DOES NOT do ===

    Explicit non-goals — pre-empting the most likely "this
    looks like a vehicle for X" objections:

      - **Does not change generic ftrace, kprobe, KFENCE,
        or KCSAN semantics.** Every patch lands entirely
        inside `arch/um/`, `arch/x86/um/`, `Documentation/
        virt/uml/`, `arch/um/configs/profiles/`, and
        `tools/testing/selftests/um/<feature>-*`. The
        generic `kernel/trace/`, `kernel/kprobes.c`,
        `mm/kfence/`, `kernel/kcsan/` paths are
        untouched. The one cross-`arch/um/` site sub-series
        6b touches is the `kfence_protect_page` /
        `prepare_ftrace_return` style `static inline` in
        `arch/um/include/asm/<feature>.h` — pure header
        plumbing that arch ports always own.

      - **Does not add new debug syscalls or new ABI
        surfaces.** The user-visible interfaces are
        existing: tracefs for ftrace, kprobes debugfs files
        (`/sys/kernel/debug/kprobes/{list,blacklist}`) for
        kprobes, the existing KFENCE pool sysfs surface for
        KFENCE, the existing KCSAN debugfs control for
        KCSAN. UML gains the same surfaces existing arches
        offer; no UML-specific surfaces are added.

      - **Does not perturb the off-state.** Each feature is
        gated by the corresponding `CONFIG_*=n`; with
        `CONFIG_KPROBES=n` `CONFIG_FUNCTION_TRACER=n`
        `CONFIG_KFENCE=n` `CONFIG_KCSAN=n` the build is
        byte-identical to today's mainline arch/um (modulo
        the new `select HAVE_*` lines, which have zero
        codegen impact when the corresponding feature is
        off). Profile defconfigs (`prod-fast`, `sandbox`,
        `embedded`) all stay with the four features off
        and pay zero cost; the new
        `select HAVE_FUNCTION_GRAPH_TRACER` does not add a
        NOP5 to every function unless
        `CONFIG_FUNCTION_TRACER=y` *also* is set.

      - **Does not reimplement kernel logic.** UML stays
        upstream kernel + arch/um/ host interface for each
        of the four features. The arch-side files
        (`kprobes/core.c`, `rethook.c`, `ftrace.c`,
        `mcount.S`, `kfence.h`, `kcsan.h`) are minimal
        ports of the corresponding `arch/x86/` files where
        applicable (the x86-derived rethook trampoline is
        ~200 LoC; the ftrace patching ops are ~150 LoC;
        the kprobes core is ~700 LoC of which most is
        x86-pattern boilerplate).

      - **Does not change USER TU semantics.** The KCSAN
        sub-series's USER-TU opt-out in
        `arch/um/os-Linux/Makefile` and
        `arch/um/os-Linux/skas/Makefile` is the standard
        `KCSAN_SANITIZE := n` line — same pattern KASAN
        and KMSAN use everywhere they need to exclude
        TUs from sanitizer instrumentation. No new USER-TU
        boundary policy.

      - **Does not require KMSAN.** Sub-series 6d does not
        depend on Series 2 (`kmsan-arch-callback-rfc`)
        landing. The KMSAN-on-UML port (workstream C-07)
        is independent and lands as its own future series
        once Series 2's generic-side hook is accepted by
        linux-mm. Series 6 ships kprobes/ftrace/KFENCE/
        KCSAN without KMSAN.

      - **Does not enable kprobes-on-ftrace fast path.**
        `HAVE_KPROBES_ON_FTRACE` would route function-entry
        kprobes through the ftrace mcount fast path
        instead of through the int3 path; it requires a
        `ftrace_regs_caller` + `HAVE_DYNAMIC_FTRACE_WITH_REGS`
        in mcount.S, which is ~200 LoC of delicate
        assembly. The int3 path sub-series 6a ships covers
        all user-visible kprobe semantics (entry,
        mid-function, returns, graphs); the ftrace fast
        path is a pure perf optimization with no
        functionality gap. Deferred per
        a-plus-quality-plan anti-pattern #3 ("optimize
        before workloads exist") to a follow-up task
        ("C-04b: HAVE_KPROBES_ON_FTRACE") once concrete
        performance pressure exists.

      - **Does not enable `HAVE_DYNAMIC_FTRACE_WITH_REGS`,
        `HAVE_DYNAMIC_FTRACE_WITH_ARGS`,
        `HAVE_DYNAMIC_FTRACE_WITH_DIRECT_CALLS`, or
        `HAVE_FTRACE_REGS_HAVING_PT_REGS`.** Each of those
        is a separate per-feature opt-in that requires its
        own arch substrate; out of scope for Series 6. BPF
        direct calls (`HAVE_DYNAMIC_FTRACE_WITH_DIRECT_CALLS`)
        are a C-06 concern, not Series 6.

      - **Does not change KFENCE / KCSAN's runtime
        defaults.** The KFENCE sample interval, the KCSAN
        watchpoint table size, the KCSAN delay timing —
        all upstream defaults. UML just turns the feature
        on under the appropriate profile.

      - **Does not change UML's tracing-vs-instrumentation
        story for cross-cutting tools.** bpftrace, perf
        trace, etc. work on UML after Series 6 lands the
        same way they work on x86 native: they call into
        the standard upstream kernel paths. No UML-side
        bpftrace shim is added.

    === Performance ===

    The performance pitch is the cleanest one in the
    bundle: each sub-series targets **zero overhead when
    its `CONFIG_*=n`**.

    The mechanism for the zero-overhead claim is two-fold:

      1. **Compile-time gating** — every patch in every
         sub-series is `#ifdef CONFIG_<FEATURE>`-gated at
         the C / Makefile level. With `CONFIG_KFENCE=n`
         the KFENCE arch header is included but its
         `arch_kfence_init_pool()` + `kfence_protect_page()`
         inline functions are never called (the generic
         kfence callers in `mm/kfence/` are themselves
         compiled out). Same shape for the other three.

      2. **Series 5 static-key gates for runtime-on
         features** — when a feature *is* enabled (e.g.
         `CONFIG_FUNCTION_TRACER=y` but the user hasn't
         toggled `current_tracer` away from `nop`), Series
         5's hook surface provides the static-key gate
         that short-circuits the call site. The
         `um_hook_trace_syscalls` gate (Series 5's gate 1)
         is the trace-syscalls fan-out point; with it
         flipped off (the default) the tracer-on UML still
         pays ~0.3 ns per gate (literal NOP5 under
         `HAVE_ARCH_JUMP_LABEL=y`), not the ~50–100 ns of
         the ftrace dispatch. Series 6's sub-series 6b's
         slow-path consumer plugs into the
         `um_hook_trace_syscalls` gate's slow-path slot,
         replacing Series 5's counter-only stub.

    Two reference builds on `x86_64 defconfig + ARCH=um`,
    measured on AMD Ryzen 7 7840HS (Zen 4):

      All four features off (CONFIG_KPROBES=n,
      CONFIG_FUNCTION_TRACER=n, CONFIG_KFENCE=n,
      CONFIG_KCSAN=n):
        - `vmlinux` codegen byte-for-byte identical to
          today's equivalent. The `select HAVE_*` lines
          are no-ops when the feature isn't enabled. The
          new arch headers compile but their inline
          functions never instantiate.
        - getpid round-trip: indistinguishable from
          mainline arch/um within run-to-run noise
          (±0.5 % across 5 runs of 10000 syscalls).

      Research profile (CONFIG_KPROBES=y +
      CONFIG_FUNCTION_TRACER=y +
      CONFIG_FUNCTION_GRAPH_TRACER=y;
      current_tracer = nop, no kprobes registered):
        - Tracer-off boot cost: +3.3 % vs no-tracer
          equivalent (the NOP5-on-every-function i-cache
          residency cost; well under A-07's 5 % ceiling).
          Measured on `init=/bin/true` boot, median 3.48 s
          vs 3.37 s across 5 runs.
        - Tracer-on `function` tracer: ~50-100 ns per
          traced call at function granularity; cost of
          the ftrace dispatch (ftrace_caller + ops-list
          walk), not UML-specific.
        - One kprobe on `do_sys_open`: ~3 µs per fired
          probe (int3 trap delivery → kprobe_handler →
          single-step → post-handler). Cost of the int3
          path, not UML-specific.

      fuzz-deep profile (CONFIG_KFENCE=y;
      sample_interval = 100 ms):
        - KFENCE-allocation hit rate ~1 % of kmalloc
          calls; for hit allocations, the guard-page
          mprotect transition is ~4-6 µs per allocation
          (cost of the round-trip into
          `os_protect_memory()`). KFENCE-on UML is no
          slower than KFENCE-on x86 native within
          run-to-run noise; UML pays the same sampling
          cost.

      race profile (CONFIG_KCSAN=y; ncpus=2):
        - KCSAN-on cost: ~20 % slowdown vs KCSAN-off race
          profile (cost of the per-access watchpoint
          instrumentation + delays). Upstream baseline;
          UML doesn't add measurable overhead. Detection
          rate: 3/3 selftest races on
          `tools/testing/selftests/um/kcsan-smoke/`,
          20+ races on the upstream KCSAN KUnit suite
          with `ncpus=2`.

    The performance-CI harness landed alongside the
    on-branch A-07 task
    (`Documentation/virt/uml/redesign/scripts/uml-perf*.sh`)
    runs the four feature-on/off comparisons on each
    build; checked-in baselines gate any regression > 5 %
    on the tracer-off invariant or > 15 % on the per-site
    tracer-on numbers. CI gates the bundle as a whole.

    === Anticipated review questions ===

    Items where reviewer input would particularly help.
    The most-likely top-3 questions are flagged (q-a) /
    (q-b) / (q-c) for the SUBMISSION-NOTES open-questions
    inventory.

      **(q-a) [HARD OPEN QUESTION] Why bundle four
      sub-series into one cover letter instead of four
      independent series?** The bundling decision is
      load-bearing for the UML maintainer routing: each
      sub-series's primary subsystem maintainer
      (linux-trace-kernel@ for 6a + 6b, linux-mm@ for 6c
      + 6d) gets a single sub-series to review, but the
      `linux-um@` cc'd reviewers see the parent shape so
      they can evaluate the cumulative cost of "four
      new feature surfaces on arch/um in one push." The
      alternative — four independent series at four
      independent times — fragments the
      `linux-um@`-side review across four threads and
      loses the parent context. Reviewers may prefer the
      independent-series shape; if so, this cover
      letter folds into per-sub-series cover letters and
      Series 6 ships as 6a/6b/6c/6d at four send times,
      with a coordinating note that they are related.

      **(q-b) [HARD OPEN QUESTION] Order of sub-series:
      kprobes-first vs ftrace-first?** The cover letter
      argues kprobes-first because kprobes is the most
      independent. The alternative — ftrace-first
      because ftrace's mcount substrate is consumed by
      `HAVE_KPROBES_ON_FTRACE` (a follow-up not in this
      series but on the horizon) — is reasonable. The
      current ordering reflects "ship the smallest,
      most-independent sub-series first; build maintainer
      credibility before the larger 6-patch ftrace
      sub-series." Reviewers may prefer the
      ftrace-first ordering; the change is purely
      cover-letter wording.

      **(q-c) [HARD OPEN QUESTION] KFENCE's KUnit
      stack-walker mismatch.** The C-02 in-fork landing
      had 13/27 KFENCE KUnit tests pass; the 12 failures
      assert on formatted stack-trace content that UML's
      stack walker doesn't render with symbol names.
      KFENCE itself is fully functional — the failures
      are walker-renderer issues, not KFENCE-functional
      issues. The sub-series 6c cover letter walks this
      explicitly. Reviewers may want the stack-walker
      fix bundled in; we believe it belongs as a separate
      arch/um/-side cleanup (UML's stack walker
      undermaintenance is a recurring issue that deserves
      its own focused fix). Alternative: ship 6c with a
      "13/27 KUnit pass" status disclaimer and the
      walker fix as a follow-up.

      **(d) D26 KASAN/KCSAN mutual-exclusion policy.**
      `lib/Kconfig.kcsan`'s `depends on !KASAN` is
      already upstream. Sub-series 6d's race profile
      explicitly turns KASAN off; fuzz-deep keeps KASAN
      on. The user-doc spells out the choice. Reviewers
      may want a more aggressive integration (auto-
      switching at runtime via a single
      `DEBUG_KERNEL_SANITIZER=KASAN|KCSAN` choice
      block); we think the current per-profile shape is
      simpler and matches existing arch conventions.

      **(e) function_graph's preempt-count guard.** The
      atomic-context check in `prepare_ftrace_return()`
      (sub-series 6b Patch 3) is a UML-specific guard
      against the `TINY_RCU`-on-UP /
      `rcu_read_lock=preempt_disable` interaction with
      the graph shadow-stack push. D34 addendum-4 walks
      the analysis. Reviewers may prefer this guard
      land in the generic `kernel/trace/` rather than in
      `arch/um/`; we believe the UML-specific shape is
      correct because the trigger condition (atomic
      context inside the IRQ delivery path of a
      single-CPU UML kernel) is UML-specific.

      **(f) KCSAN signal-delivery model on UML.** The
      C-03 design memo flagged Q1 ("KCSAN uses signal
      delivery for delays. Does this conflict with UML's
      signal-driven trap machinery?") as an open question
      at design time. In-fork validation (200-iteration
      stress + 3/3 selftest pass + 20+ KUnit races) shows
      no conflict: KCSAN delivers SIGURG or signal
      classes UML's trap machinery doesn't intercept.
      Reviewers may want the C-03 stress matrix re-run
      against post-Series-4 ops-table substrate to
      confirm; that's a verification item for
      SUBMISSION-NOTES, not a re-design.

      **(g) rethook port vs legacy kretprobes.** Sub-
      series 6a Patch 2 ports rethook (the modern
      x86_64 return-probe substrate; auto-selects
      `KRETPROBES` via `arch/Kconfig:252`), not the
      legacy `HAVE_KRETPROBES` shape. D33 walks the
      pivot rationale: upstream x86_64 switched to
      rethook several cycles ago and the legacy shape is
      effectively a backward-compat stub. The
      `arch/x86/kernel/rethook.c` reference is 128 lines;
      the UML port rewrites C member access to
      `UPT_xxx()` accessors but otherwise mirrors x86.
      Reviewers may want to see the legacy shape too as
      a fallback; if so, we add it as a Patch 2b under
      `if !HAVE_RETHOOK` in `arch/um/Kconfig`.

      **(h) MAINTAINERS entry locations.** Each
      sub-series's Patch 4/5/6 (the "user doc" patch)
      adds the new arch/um files to the `USER MODE
      LINUX` MAINTAINERS entry path list. Reviewers may
      prefer per-feature MAINTAINERS entries
      (`USER MODE LINUX KPROBES`, `USER MODE LINUX
      FTRACE`, etc.); we think the single-section shape
      is cleaner and matches existing arch conventions
      (arch/x86 doesn't split MAINTAINERS by feature
      either). Open to feedback.

    === Lineage ===

    This RFC bundle is the upstream-facing emission of
    workstream C (profiles + gaps) in the UML redesign
    plan (`Documentation/virt/uml/redesign/02-workstreams/
    C-profiles-and-gaps/`). Four C-workstream tasks
    completed in 2026-04:

      C-02 KFENCE port — landed 2026-04-18
      C-03 KCSAN port — landed 2026-04-18
      C-04 kprobes + rethook + function_graph — landed
                                                  2026-04-22
      C-05 ftrace port — landed 2026-04-19

    Decisions log entries for this bundle:

      D23 (2026-04-18) — RFC patch-series split by
                          compile boundary (not by C-NN
                          task). Applies per-sub-series
                          in Series 6.
      D26 (2026-04-18) — KASAN/KCSAN mutual exclusion;
                          fuzz-deep stays KASAN, new
                          `race` profile carries KCSAN.
      D27 (2026-04-18) — function_graph initial deferral
                          (later closed in D34
                          addendum-4 / Series 6b
                          Patch 3).
      D28 (2026-04-19) — ftrace patching mechanism on
                          UML (text_mutex +
                          stop_machine + Series 5's
                          mprotect helpers; no
                          text_poke_bp).
      D29 (2026-04-19) — `-fpatchable-function-entry`
                          chosen over `-pg -mfentry` for
                          UML ftrace; no `__fentry__`
                          thunk needed.
      D30 (2026-04-19) — D30 ftrace-debug surfaces.
      D31 (2026-04-19) — KCOV moved to fuzz/fuzz-deep
                          profiles per `notrace`
                          interaction analysis.
      D32 (2026-04-19) — kprobes scope (full surface in
                          one workstream rather than
                          phased C-04a / C-04b split;
                          D27 supersession on graph
                          deferral).
      D33 (2026-04-19) — rethook over legacy
                          kretprobes for UML's return-
                          probe substrate.
      D34 (2026-04-21) — function_graph three-leak-
                          source analysis;
                          addendum-3 (Series 3 cover for
                          generic `kthread` /
                          `smpboot_thread_fn` notrace);
                          addendum-4 (preempt-count
                          guard in
                          `prepare_ftrace_return` for
                          UML's TINY_RCU-on-UP
                          interaction).
      D45 (2026-04-21) — fork-first upstream policy.
      D63 (2026-04-23) — submission-queue sequencing.
                          Series 6 follows Series 4 + 5.

    The full decision log is at
    `Documentation/virt/uml/redesign/04-risks/
    decisions-log.md`; each D## entry walks rationale,
    alternatives, and revisit conditions.

    === Dependencies on / from other queued series ===

    Series 6 depends on:

      Series 4 (backend-ops-abstraction-rfc) — landed
      upstream first. Each of the four sub-series
      consumes Series 4's HOT-op dispatch surface
      indirectly: sub-series 6a's kprobes hook into
      `relay_signal()` lives on the trap path Series 4
      routes through `vcpu_run`; sub-series 6b's ftrace
      mcount patching lives on call sites Series 4's
      ops table reshapes; sub-series 6c's segv
      fault-path hook lives in `arch/um/kernel/trap.c`
      Series 4 touches via `mm_region_*` ops dispatch.
      Strictly speaking each sub-series compiles
      against pre-Series-4 mainline (the arch hooks are
      independent of the ops-table indirection), but
      reviewer workflow benefits from dependency
      ordering — sending Series 6 before Series 4 lands
      would force reviewers to evaluate the hooks
      against a moving target.

      Series 5 (static-key-hot-paths-series) — landed
      upstream first. Sub-series 6b's ftrace patching
      consumes Series 5's `.um_patch_text` section
      split + the `um_kernel_text_patch_begin/end`
      mprotect helpers; sub-series 6a's kprobes
      insertion path consumes the same helpers
      (kprobe-arming writes 0xCC into the kernel text,
      same mprotect dance). Series 6b's tracer-on slow
      path replaces Series 5's `um_hook_trace_syscalls`
      counter-only stub with the real trace-syscalls
      tracer; same shape for `um_hook_kfence_sample`
      (replaced by sub-series 6c) and (in a future
      KMSAN series) `um_hook_record_replay`. Without
      Series 5, sub-series 6a + 6b would have to
      invent their own patch-text machinery; that's the
      pre-existing condition Series 5 already cleans
      up.

      Series 3 (ftrace-notrace-generic-v1) — landed
      first as a prerequisite of Series 4. Series 3
      adds the `notrace` annotations on
      `kthread()` / `smpboot_thread_fn()` that
      sub-series 6b's function_graph implementation
      depends on for source-3 leak closure (D34's
      three-leak-source analysis). Without Series 3,
      sub-series 6b's function_graph still leaks
      shadow-stack entries on kthread creation.
      Mitigation: if Series 3 stalls upstream while
      Series 6 proceeds, sub-series 6b's Patch 3 cover
      letter notes the dependency and Series 6b ships
      with a Kconfig note that function_graph requires
      Series 3 having landed.

    Series 6 is independent of:

      Series 1 (bpf-hygiene-v1)           — pure x86 BPF
                                             hygiene; no
                                             arch/um
                                             contact
                                             surface for
                                             Series 6
                                             sub-series.
      Series 2 (kmsan-arch-callback-rfc)  — mm/kmsan
                                             arch callback
                                             shape;
                                             orthogonal
                                             to Series 6
                                             sub-series.
                                             KMSAN-on-UML
                                             port (C-07)
                                             is a future
                                             series, not
                                             part of
                                             this bundle.

    Series 6 blocks:

      Series 7 (kvm-backend-series)       — Series 7's
                                             record/replay
                                             observe hook
                                             (#169) cites
                                             Series 6b's
                                             function_graph
                                             trampolines
                                             for the
                                             record-side
                                             stack walk
                                             pattern. Not
                                             a hard
                                             code-level
                                             dependency
                                             but a
                                             cover-letter
                                             cite for
                                             shape
                                             continuity.

    The submission-queue at `upstream-patches/
    SUBMISSION-QUEUE.md` is authoritative for the
    inter-series dependency graph.

    === MAINTAINERS routing ===

    The 4-way per-sub-series breakdown — each
    sub-series sends to its primary subsystem maintainer
    audience; `linux-um@` is cc'd on all four for the
    cross-cutting context.

      Sub-series 6a (kprobes):
        Primary list: linux-trace-kernel@vger.kernel.org
        Cc:           linux-um@lists.infradead.org
                      linux-kernel@vger.kernel.org
                      Masami Hiramatsu <mhiramat@kernel.org>
                      (kprobes maintainer)
                      Steven Rostedt <rostedt@goodmis.org>
                      (tracing infra; kprobes consumer)
                      Richard Weinberger <richard@nod.at>
                      Johannes Berg <johannes@sipsolutions.net>
                      Anton Ivanov <anton.ivanov@cambridgegreys.com>
                      Benjamin Berg <benjamin@sipsolutions.net>
                      Tiwei Bie <tiwei.btw@antgroup.com>

      Sub-series 6b (ftrace + function_graph):
        Primary list: linux-trace-kernel@vger.kernel.org
        Cc:           linux-um@lists.infradead.org
                      linux-kernel@vger.kernel.org
                      Steven Rostedt <rostedt@goodmis.org>
                      (ftrace maintainer)
                      Masami Hiramatsu <mhiramat@kernel.org>
                      (tracing infra; function_graph
                      consumer)
                      Richard Weinberger <richard@nod.at>
                      Johannes Berg <johannes@sipsolutions.net>
                      Anton Ivanov <anton.ivanov@cambridgegreys.com>
                      Benjamin Berg <benjamin@sipsolutions.net>
                      Tiwei Bie <tiwei.btw@antgroup.com>

      Sub-series 6c (KFENCE):
        Primary list: linux-mm@kvack.org
        Cc:           linux-um@lists.infradead.org
                      linux-kernel@vger.kernel.org
                      Marco Elver <elver@google.com>
                      (KFENCE maintainer)
                      Alexander Potapenko <glider@google.com>
                      (KFENCE co-maintainer)
                      Richard Weinberger <richard@nod.at>
                      Johannes Berg <johannes@sipsolutions.net>
                      Anton Ivanov <anton.ivanov@cambridgegreys.com>
                      Benjamin Berg <benjamin@sipsolutions.net>
                      Tiwei Bie <tiwei.btw@antgroup.com>

      Sub-series 6d (KCSAN):
        Primary list: linux-mm@kvack.org
        Cc:           linux-um@lists.infradead.org
                      linux-kernel@vger.kernel.org
                      Marco Elver <elver@google.com>
                      (KCSAN maintainer)
                      Dmitry Vyukov <dvyukov@google.com>
                      (KCSAN author)
                      Alexander Potapenko <glider@google.com>
                      (sanitizer adjacency)
                      Richard Weinberger <richard@nod.at>
                      Johannes Berg <johannes@sipsolutions.net>
                      Anton Ivanov <anton.ivanov@cambridgegreys.com>
                      Benjamin Berg <benjamin@sipsolutions.net>
                      Tiwei Bie <tiwei.btw@antgroup.com>

    `scripts/get_maintainer.pl` should be re-run against
    each rebased + squashed sub-series before emission.
    The MAINTAINERS file already covers `arch/um/` under
    USER MODE LINUX; each sub-series's final patch
    (Patch 4 or 5 or 6) adds its new arch/um files to
    that section's path list.

    Cc list per sub-series: 8-10 names + 3 lists. Each
    well under the LKML "20 recipients max for a single
    mail" heuristic.

    === Reproducing locally ===

    Build with all four features (research profile):

      make ARCH=um O=~/src/uml-builds/uml-research defconfig
      make ARCH=um O=~/src/uml-builds/uml-research uml/research
      make ARCH=um O=~/src/uml-builds/uml-research -j$(nproc)

    Build with KFENCE on (fuzz-deep profile):

      make ARCH=um O=~/src/uml-builds/uml-fuzz-deep defconfig
      make ARCH=um O=~/src/uml-builds/uml-fuzz-deep uml/fuzz-deep
      make ARCH=um O=~/src/uml-builds/uml-fuzz-deep -j$(nproc)

    Build with KCSAN on (race profile):

      make ARCH=um O=~/src/uml-builds/uml-race defconfig
      make ARCH=um O=~/src/uml-builds/uml-race uml/race
      make ARCH=um O=~/src/uml-builds/uml-race -j$(nproc)

    Build with all four features off (sandbox profile):

      make ARCH=um O=~/src/uml-builds/uml-sandbox defconfig
      make ARCH=um O=~/src/uml-builds/uml-sandbox uml/sandbox
      make ARCH=um O=~/src/uml-builds/uml-sandbox -j$(nproc)

    Boot, exercise each feature in the research profile:

      ./linux mem=512M init=/bin/sh
      # in guest:
      mount -t debugfs none /sys/kernel/debug

      # kprobes:
      insmod samples/kprobes/kprobe_example.ko
      ls /                                              # fires
      cat /sys/kernel/debug/kprobes/list
      rmmod kprobe_example

      # ftrace:
      mount -t tracefs none /sys/kernel/tracing
      cat /sys/kernel/tracing/available_tracers
      echo function > /sys/kernel/tracing/current_tracer
      ls /
      cat /sys/kernel/tracing/trace | head

      # function_graph:
      echo function_graph > /sys/kernel/tracing/current_tracer
      ls /
      cat /sys/kernel/tracing/trace | head

    KFENCE (fuzz-deep profile, sample interval 100ms,
    inject a known heap-OOB and observe the catch):

      ./linux mem=512M init=/bin/sh
      # in guest, inject the upstream test
      modprobe test_kfence

    KCSAN (race profile, ncpus=2, exercise the selftest):

      ./linux mem=512M ncpus=2 init=/bin/sh
      # in guest, the selftest runs at boot via
      # tools/testing/selftests/um/kcsan-smoke/

    Run the per-sub-series selftests:

      cd tools/testing/selftests/um/kprobes-stress
      ./run.sh                                          # 200 + 2000 iter PASS

      cd ../ftrace-smoke
      ./run.sh                                          # PASS trace_lines=51316

      cd ../kfence-smoke                                # new in 6c
      ./run.sh                                          # KUnit subset PASS

      cd ../kcsan-smoke                                 # new in 6d
      ./run.sh                                          # 3/3 PASS

    Reproduce the perf-CI gates:

      Documentation/virt/uml/redesign/scripts/uml-perf.sh
      Documentation/virt/uml/redesign/scripts/uml-perf-compare.sh \
          baseline.json post-series6.json

    === Changelog ===

    RFC v1 (this draft): first upstream emission of the
    four C-workstream sub-tracks bundled as Series 6.
    Lifts the in-fork-validated substrate (C-02 KFENCE,
    C-03 KCSAN, C-04 kprobes + rethook + function_graph,
    C-05 ftrace) onto post-Series-5 mainline. Per-
    sub-series squash audit folds the in-fork commits
    into 4-6 patches each; the upstream emission unpicks
    each in-fork landing into reviewable per-feature
    slices.

    Michael Bommarito (20):
      Sub-series 6a — kprobes:
        um: kprobes: arch header + int3 substrate + trap hook
        um: kprobes: rethook port for return probes
        um: kprobes: research profile + kprobes-stress selftest
        um: kprobes: user doc + MAINTAINERS

      Sub-series 6b — ftrace + function_graph:
        um: ftrace: arch header + ftrace_caller in mcount.S
        um: ftrace: patching ops + CC_FLAGS_FTRACE override
        um: ftrace: function_graph + return_to_handler + preempt-guard
        um: ftrace: notrace arch_local_irq_* + HAVE_FUNCTION_GRAPH_TRACER
        um: ftrace: research profile + ftrace-smoke selftest
        um: ftrace: user docs + MAINTAINERS

      Sub-series 6c — KFENCE:
        um: kfence: arch header + protect_page via os_protect_memory
        um: kfence: segv fault-path hook
        um: kfence: select HAVE_ARCH_KFENCE + profile fragments
        um: kfence: user doc + MAINTAINERS

      Sub-series 6d — KCSAN:
        um: kcsan: arch header + USER-TU sanitizer opt-outs
        um: kcsan: select HAVE_ARCH_KCSAN + KASAN/KCSAN gate
        um: kcsan: race profile fragment
        um: kcsan: race-profile selftest
        um: kcsan: user doc + MAINTAINERS

     Documentation/virt/uml/ftrace.rst                 | XXX +
     Documentation/virt/uml/kcsan.rst                  | XXX +
     Documentation/virt/uml/kfence.rst                 | XXX +
     Documentation/virt/uml/kprobes.rst                | XXX +
     Documentation/virt/uml/index.rst                  |   X +
     Documentation/virt/uml/profiles/race.rst          |  XX +
     Documentation/virt/uml/profiles/research.rst      |   X +
     MAINTAINERS                                       |  XX +
     arch/um/Kconfig                                   |  XX +
     arch/um/Makefile                                  |   X +
     arch/um/configs/profiles/fuzz-deep.config         |   X +
     arch/um/configs/profiles/race.config              |  XX +
     arch/um/configs/profiles/research.config          |   X +
     arch/um/include/asm/ftrace.h                      |  XX +
     arch/um/include/asm/kcsan.h                       |  XX +
     arch/um/include/asm/kfence.h                      |  XX +
     arch/um/include/asm/kprobes.h                     |  XX +
     arch/um/kernel/Makefile                           |   X +
     arch/um/kernel/ftrace.c                           | XXX +
     arch/um/kernel/kprobes/Makefile                   |   X +
     arch/um/kernel/kprobes/core.c                     | XXX +
     arch/um/kernel/mcount.S                           | XXX +
     arch/um/kernel/rethook.c                          | XXX +
     arch/um/kernel/signal.c                           |   X +
     arch/um/kernel/trap.c                             |  XX +
     arch/um/os-Linux/Makefile                         |   X +
     arch/um/os-Linux/signal.c                         |   X +
     arch/um/os-Linux/skas/Makefile                    |   X +
     tools/testing/selftests/um/ftrace-smoke/...       |  XX +
     tools/testing/selftests/um/kcsan-smoke/...        |  XX +
     tools/testing/selftests/um/kfence-smoke/...       |  XX +
     tools/testing/selftests/um/kprobes-stress/...     |  XX +
     32 files changed, XXXX insertions(+), XX deletions(-)
     create mode 100644 Documentation/virt/uml/ftrace.rst
     create mode 100644 Documentation/virt/uml/kcsan.rst
     create mode 100644 Documentation/virt/uml/kfence.rst
     create mode 100644 Documentation/virt/uml/kprobes.rst
     create mode 100644 arch/um/include/asm/kcsan.h
     create mode 100644 arch/um/include/asm/kfence.h
     create mode 100644 arch/um/include/asm/kprobes.h
     create mode 100644 arch/um/kernel/ftrace.c
     create mode 100644 arch/um/kernel/kprobes/Makefile
     create mode 100644 arch/um/kernel/kprobes/core.c
     create mode 100644 arch/um/kernel/mcount.S
     create mode 100644 arch/um/kernel/rethook.c
     create mode 100644 arch/um/configs/profiles/race.config

    --
    2.XX.X

---

End of draft. Post-rebase + post-squash pass, this becomes
the real shared `0000-cover-letter.patch` for the bundle,
with the four sub-series's individual cover letters
(`6a/0000-cover-letter.patch`, `6b/0000-cover-letter.patch`,
etc.) derived from this shared frame and the per-sub-series
patches emitted as `6a/0001-*.patch` … `6d/0005-*.patch`
per the SUBMISSION-NOTES.md "On-branch commit map" section
per sub-series.
