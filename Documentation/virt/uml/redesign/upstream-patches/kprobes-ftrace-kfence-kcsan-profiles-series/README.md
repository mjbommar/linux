# kprobes-ftrace-kfence-kcsan-profiles-series — reader's guide

Series 6 of the upstream-bound queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63). A bundle of
four arch-side enablement sub-series for UML on x86_64.

## What's in this directory

  - `0000-cover-letter.patch.md` — draft shared cover
    letter that heads the four-sub-series bundle. Walks
    the per-feature pre-Series-6 broken state, the
    4-sub-series shape, the per-sub-series migration plan,
    the dependency story (Series 4 + 5 first), the
    performance pitch (zero overhead when off; Series 5
    static-key gates carry the on-state), and the 4-way
    maintainer routing. `.md` because the source-side
    patches don't yet exist — the in-fork state is the
    landed C-02, C-03, C-04, C-05 commits across the
    `umlctl-deploy` branch.
  - `SUBMISSION-NOTES.md` — operational notes per the
    `upstream-patches/README.md` directory convention.
    Per-sub-series on-branch commit maps, per-sub-series
    squash audit checklists, per-sub-series pre-
    submission cleanups (drop `Co-authored-by:`,
    michael.bommarito@gmail.com identity, no UML-specific
    log references in commit messages), 4-way maintainer
    routing recipes, checkpatch expectations, and
    post-landing cleanup checklist (move to `landed/`
    once ALL FOUR sub-series have landed; unblock
    Series 7's record/replay function_graph cite).
  - `README.md` (this file).

`0001-*.patch` etc. appear per sub-series after the
per-sub-series squash audit passes (4 separate audits,
one per sub-series).

## What Series 6 is

Upstream emission of workstream C (profiles + gap-fill)
across four sub-tracks:

  - **Sub-series 6a — kprobes (C-04).** Selects
    `HAVE_KPROBES` + `HAVE_RETHOOK` for UML on x86_64.
    `arch/um/include/asm/kprobes.h` +
    `arch/um/kernel/kprobes/{Makefile,core.c}` +
    `arch/um/kernel/rethook.c` are the substrate;
    `arch/um/kernel/trap.c::relay_signal()` is the
    int3 + single-step hook. ~5 patches.

  - **Sub-series 6b — ftrace + function_graph (C-05 +
    C-04 3a/3b).** Selects `HAVE_FUNCTION_TRACER` +
    `HAVE_DYNAMIC_FTRACE` +
    `FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY` +
    `HAVE_FUNCTION_GRAPH_TRACER`. Adds `ftrace_caller` +
    `ftrace_stub` + `ftrace_graph_caller` +
    `return_to_handler` to `arch/um/kernel/mcount.S`;
    adds `prepare_ftrace_return` with preempt-count
    guard (D34 addendum-4) and the patching ops to
    `arch/um/kernel/ftrace.c`. Adds the
    `-fpatchable-function-entry=5,0` flag override in
    `arch/um/Makefile` and `notrace` annotations on the
    `arch_local_irq_*` family in `arch/um/{kernel,
    os-Linux}/signal.c`. ~6 patches.

  - **Sub-series 6c — KFENCE (C-02).** Selects
    `HAVE_ARCH_KFENCE`. `arch/um/include/asm/kfence.h`
    declares the two-function arch contract
    (`arch_kfence_init_pool()` returns true on
    4 KiB-only UML; `kfence_protect_page()` wraps
    `os_protect_memory()`); `arch/um/kernel/trap.c::
    segv()` hands off to `kfence_handle_page_fault()`
    before the kernel-mode panic path. ~4 patches.

  - **Sub-series 6d — KCSAN (C-03).** Selects
    `HAVE_ARCH_KCSAN`. `arch/um/include/asm/kcsan.h`
    for the per-CPU state contract; USER-TU opt-outs in
    `arch/um/os-Linux/Makefile` and
    `arch/um/os-Linux/skas/Makefile`; new `race`
    profile (`arch/um/configs/profiles/race.config`)
    explicitly disables KASAN per D26's mutual-
    exclusion policy. ~5 patches.

The four sub-series are independent at the code level
but share a parent framing in the cover letter: "UML
profile-driven feature selection." Each sub-series
routes to its primary subsystem maintainer
(linux-trace-kernel@ for 6a + 6b, linux-mm@ for 6c +
6d) with `linux-um@` cc'd on all four.

## Relationship to C-workstream memos

The C-workstream substrate lives at
`Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/`.
Per-task memo → sub-series drive:

  - `02-port-kfence.md` → sub-series 6c.
  - `03-port-kcsan.md` → sub-series 6d.
  - `04-port-kprobes.md` → sub-series 6a + 6b Patch 3-4
    (function_graph carry-over from C-04's 3a/3b
    split per D34).
  - `05-port-ftrace.md` → sub-series 6b Patch 1-2 + 5-6.

Decisions D23, D26, D27, D28, D29, D30, D31, D32, D33,
D34 (+ 3 addenda) in `04-risks/decisions-log.md` record
the per-decision rationale (see cover letter
§"Lineage").

## Relationship to other upstream-bound series

  - **Depends on Series 4 (`backend-ops-abstraction-rfc/`)
    landed first.** Each sub-series hooks the trap path
    Series 4 reshapes.
  - **Depends on Series 5 (`static-key-hot-paths-series/`)
    landed first.** Sub-series 6a + 6b consume Series 5's
    `.um_patch_text` section split + the
    `um_kernel_text_patch_begin/end` mprotect helpers
    directly. Sub-series 6b's tracer-on slow path
    replaces Series 5's `um_hook_trace_syscalls`
    counter-only stub.
  - **Depends on Series 3 (`ftrace-notrace-generic-v1/`)
    landed first.** Sub-series 6b's function_graph closes
    source (3) of D34's three-leak-source analysis via
    Series 3's `notrace`-on-`kthread()`/`smpboot_thread_fn()`.
  - **Blocks Series 7 (`kvm-backend-series/`).** Series
    7's record/replay observe hook (#169) cites
    sub-series 6b's function_graph trampoline pattern.

## When this directory changes

  - Per-sub-series squash audit produces the
    per-sub-series patch sequences: add
    `6a/0001-*.patch` … `6d/000N-*.patch`; convert each
    sub-series's cover letter to `.patch` with
    filled-in diffstat. The shared
    `0000-cover-letter.patch.md` stays as the parent
    framing doc.
  - On send per sub-series: SUBMISSION-NOTES flips that
    sub-series's status from DRAFTED → SENT (with
    `Message-Id`).
  - On land per sub-series: SUBMISSION-NOTES flips that
    sub-series's status from SENT → LANDED.
  - On ALL FOUR landed: move directory to
    `upstream-patches/landed/kprobes-ftrace-kfence-kcsan-profiles-series/`.
