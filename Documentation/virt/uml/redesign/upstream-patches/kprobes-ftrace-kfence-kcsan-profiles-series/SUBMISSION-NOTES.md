# kprobes-ftrace-kfence-kcsan-profiles-series — submission-readiness notes

## Status (2026-05-14)

**DRAFTED.** Cover letter + this notes file + reader's
guide README in-tree; `0001-*.patch` … etc. across the four
sub-series **not yet emitted**. The on-branch substrate
exists across four landed in-fork tasks (C-02 KFENCE, C-03
KCSAN, C-04 kprobes + rethook + function_graph, C-05 ftrace)
spread across ~20 in-fork commits. A structured per-
sub-series squash + rebase pass against post-Series-5
mainline is the gating prerequisite before
`git format-patch` can produce the per-sub-series
reviewable sequences the cover letter advertises.

Transition: **TO WRITE → DRAFTED 2026-05-14 → (after
Series 4 + Series 5 land + per-sub-series squash audits)
READY → SENT (four sub-series in cadence) → LANDED.**

This is Series 6 of the seven-series upstream queue per
`upstream-patches/SUBMISSION-QUEUE.md` (D63). Series 4
(the backend-ops-abstraction RFC) and Series 5 (the
static-key hot-path gates) must both land first because:

  - Series 4's HOT-op dispatch surface is the trap-loop
    Series 6a (kprobes) hooks into via `relay_signal()`,
    the segv path Series 6c (KFENCE) extends, and the
    same `arch/um/kernel/trap.c` file that holds all the
    arch hook insertion points.
  - Series 5's `.um_patch_text` section split + the
    `um_kernel_text_patch_begin/end` mprotect helpers are
    the patch-text substrate Series 6a (kprobes arming
    via 0xCC write) and Series 6b (ftrace mcount NOP↔call
    patching) directly consume. Series 6b's tracer-on
    slow path also replaces Series 5's
    `um_hook_trace_syscalls` counter-only stub.

Series 7 (`kvm-backend-series/`) cites Series 6b
specifically for the function_graph trampoline pattern
its record/replay observe hook follows.

## Queue position

6th, after Series 1 (bpf-hygiene-v1, READY), Series 2
(kmsan-arch-callback-rfc, READY), Series 3
(ftrace-notrace-generic-v1, READY), Series 4
(backend-ops-abstraction-rfc, DRAFTED 2026-05-14), and
Series 5 (static-key-hot-paths-series, DRAFTED
2026-05-14). Per D63 the queue is **dependency-ordered**:
Series 1+2+3 send in any order on their own merits;
Series 4 follows Series 3 for `notrace` cleanliness;
Series 5 follows Series 4 because the gate call sites
land on code Series 4 is reshaping; Series 6 follows
Series 5 because the patch-text + hook substrate Series 6
consumers depend on lives in Series 5.

Strictly speaking each of the four sub-series in Series 6
compiles against pre-Series-5 mainline if we replace
Series 5's `um_kernel_text_patch_begin/end` helpers with
a sub-series-internal mprotect wrap. The dependency on
Series 5 is operational: sending Series 6 ahead would
force reviewers to evaluate the patch-text substrate as
duplicated in each sub-series, which is the
pre-existing condition Series 5 cleans up. If Series 5
stalls in review past Series 6's emission window,
sub-series 6a and 6b may emit ahead with cover-letter
notes that Series 5 is recommended-but-not-required and
the helpers live alongside each sub-series's patches;
per D63 the queue should not normally jump this
dependency.

## Blockers

  - **Series 4 must LAND upstream first** (not just be
    drafted — the actual ops table must be in
    `arch/um/include/shared/backend.h` or equivalent in
    mainline). The four sub-series's hook insertion
    points lie on the trap path Series 4 reshapes; the
    `relay_signal()` and `segv()` and IRQ-delivery sites
    that Series 6 hooks are immediately downstream of
    Series 4's `vcpu_run` / `mm_region_*` dispatch.
    Strictly speaking each Series 6 sub-series compiles
    against pre-Series-4 mainline (the hook insertions
    are arch-generic). The blocker is workflow:
    reviewers should not have to evaluate
    `arch/um/kernel/trap.c` hunks against a moving
    target.

  - **Series 5 must LAND upstream first** (not just be
    drafted — the actual `.um_patch_text` section split,
    the `select HAVE_ARCH_JUMP_LABEL`, and the
    `um_kernel_text_patch_begin/end` mprotect helpers
    must be in mainline). Sub-series 6a's kprobes-arming
    `arch_arm_kprobe()` (writes 0xCC into kernel text)
    and sub-series 6b's ftrace `ftrace_make_call()` /
    `ftrace_make_nop()` (patches the 5-byte NOP↔call
    transition) both consume the Series 5 helpers
    directly. Without Series 5, each sub-series would
    have to either ship duplicated patch-text wrapping
    or sit on the pre-existing condition.

  - **On-branch C-workstream commits per sub-series**
    must each have a rebase + squash pass. The current
    in-fork state across the four sub-tracks is:

      - C-02 KFENCE: 4-6 commits in-fork landed
        2026-04-18 — needs squash to ~3-4 patches.
      - C-03 KCSAN: 5-7 commits in-fork landed
        2026-04-18 — needs squash to ~4-5 patches.
      - C-04 kprobes + rethook + function_graph:
        7-9 commits in-fork landed across 2026-04-19 to
        2026-04-22 (incl. 3a/3b split) — needs squash to
        ~5-6 patches. Series 3's
        `notrace`-on-`kthread`/`smpboot_thread_fn` piece
        is already factored out as a separate upstream
        series; 6b assumes it has landed.
      - C-05 ftrace: 4-6 commits in-fork landed
        2026-04-19 — needs squash to ~4 patches; the
        function_graph addition came from C-04 3a/3b
        and gets folded into 6b directly (not C-05's
        original landing).

    The squash auditors should aim for one
    feature-substrate patch per logical step (header,
    Kconfig+hook, per-feature code, profile, selftest,
    doc) per sub-series, with each intermediate state
    bisect-clean per D23.

## Motivating workstream

Workstream C (profiles + gap-fill) of the UML redesign
plan:

  `Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/`

The workstream produced the per-feature in-fork
substrate that Series 6 emits as four sub-series:

  - `02-port-kfence.md` — C-02 design + landing notes for
    KFENCE.
  - `03-port-kcsan.md` — C-03 design + landing notes for
    KCSAN.
  - `04-port-kprobes.md` — C-04 design + landing notes
    for kprobes + rethook + function_graph (with
    commit-3 split per D34's three-leak-source analysis
    + D34 addendum-3 and -4).
  - `05-port-ftrace.md` — C-05 design + landing notes for
    ftrace + dynamic-ftrace; D27 (graph deferral, later
    superseded), D28 (patching mechanism — text_mutex +
    stop_machine + Series 5 helpers), D29
    (`-fpatchable-function-entry` over `-pg -mfentry`).

The workstream README at
`02-workstreams/C-profiles-and-gaps/README.md` summarises
the per-task status and dependencies. Tasks C-06 (BPF
JIT) and C-07 (KMSAN) are not part of Series 6 (C-06's
hygiene fixes ship as Series 1; C-07's UML port awaits
Series 2's generic-hook acceptance).

Per-task memos at:

  - `01-defconfig-design.md` — the per-profile design
    that Series 6 sub-series's profile fragments
    populate.
  - `02-port-kfence.md` — drives sub-series 6c.
  - `03-port-kcsan.md` — drives sub-series 6d.
  - `04-port-kprobes.md` — drives sub-series 6a + 6b
    Patch 3-4 (function_graph carry-over).
  - `05-port-ftrace.md` — drives sub-series 6b Patch 1-2.
  - `notes/` (where applicable) — per-task working notes;
    not authoritative.

## Pre-submission cleanups — what needs to happen before format-patch

The four sub-series's in-fork commits all need their own
rebase + squash pass. Each sub-series produces an
independent `git format-patch` output.

### On-branch commit map per sub-series (squash audit input)

#### Sub-series 6a — kprobes (C-04)

Five on-branch commits feed:

| Slot     | Patch subject (target)                                          | On-branch commit(s)                                  | Notes                                                          |
|----------|-----------------------------------------------------------------|------------------------------------------------------|----------------------------------------------------------------|
| Patch 1  | um: kprobes: arch header + int3 substrate + trap hook            | C-04 commits 1a + 1b + 1c + 1d (collected 2026-04-19/20) | The "load-bearing" kprobes commit; ~700 LoC.                   |
| Patch 2  | um: kprobes: rethook port for return probes                      | C-04 commit 2 (2026-04-20)                            | Rethook port per D33 (modern over legacy kretprobes).          |
| Patch 3  | um: kprobes: research profile + kprobes-stress selftest          | C-04 commit 4 + commit 5 (`8a6f5087d03c`)             | profile fragment + 200/2000 iter PASS stress.                  |
| Patch 4  | um: kprobes: user doc + MAINTAINERS                              | C-04 commit 6 (`79d0e45b7d79`)                        | `Documentation/virt/uml/kprobes.rst`.                          |

#### Sub-series 6b — ftrace + function_graph (C-05 + C-04 3a/3b)

Six on-branch commits feed:

| Slot     | Patch subject (target)                                          | On-branch commit(s)                                  | Notes                                                          |
|----------|-----------------------------------------------------------------|------------------------------------------------------|----------------------------------------------------------------|
| Patch 1  | um: ftrace: arch header + ftrace_caller + ftrace_stub            | C-05 commit 2 hunks for `asm/ftrace.h` + `mcount.S`  | Skeleton trampoline only — ftrace_caller + ftrace_stub.        |
| Patch 2  | um: ftrace: patching ops + CC_FLAGS_FTRACE + Kconfig             | C-05 commit 2 hunks for `kernel/ftrace.c` + Makefile + Kconfig selects | text_mutex + stop_machine + Series 5 helpers; `select HAVE_FUNCTION_TRACER` + `HAVE_DYNAMIC_FTRACE` + `FTRACE_MCOUNT_USE_PATCHABLE_FUNCTION_ENTRY`. |
| Patch 3  | um: ftrace: function_graph + return_to_handler + preempt-guard  | C-04 commit 3b hunks for `mcount.S` + `ftrace.c`'s `prepare_ftrace_return` | Includes the preempt-count guard per D34 addendum-4.           |
| Patch 4  | um: ftrace: notrace arch_local_irq_* + HAVE_FUNCTION_GRAPH_TRACER | C-04 commit 3b hunks for `kernel/signal.c` + `os-Linux/signal.c` + Kconfig | Closes function_graph leak sources (1)+(2) per D34.            |
| Patch 5  | um: ftrace: research profile + ftrace-smoke selftest             | C-05 commit 3 + commit 4 hunks                       | Research profile, no fuzz profile (fuzz minimality intent).    |
| Patch 6  | um: ftrace: user docs + MAINTAINERS                              | C-05 commit 6 hunks                                  | `Documentation/virt/uml/ftrace.rst`.                           |

#### Sub-series 6c — KFENCE (C-02)

Four on-branch commits feed:

| Slot     | Patch subject (target)                                          | On-branch commit(s)                                  | Notes                                                          |
|----------|-----------------------------------------------------------------|------------------------------------------------------|----------------------------------------------------------------|
| Patch 1  | um: kfence: arch header + protect_page via os_protect_memory     | C-02 hunks for `arch/um/include/asm/kfence.h`        | Just the `static inline` arch header.                          |
| Patch 2  | um: kfence: segv fault-path hook                                 | C-02 hunks for `arch/um/kernel/trap.c::segv()`       | `kfence_handle_page_fault()` insertion before panic path.      |
| Patch 3  | um: kfence: select HAVE_ARCH_KFENCE + profile fragments          | C-02 hunks for `arch/um/Kconfig` + `research.config` + `fuzz-deep.config` | Kconfig + profile wire-up.                                     |
| Patch 4  | um: kfence: user doc + MAINTAINERS                               | new — derived from C-02 status doc                   | `Documentation/virt/uml/kfence.rst`. KUnit-pass disclaimer.    |

#### Sub-series 6d — KCSAN (C-03)

Five on-branch commits feed:

| Slot     | Patch subject (target)                                          | On-branch commit(s)                                  | Notes                                                          |
|----------|-----------------------------------------------------------------|------------------------------------------------------|----------------------------------------------------------------|
| Patch 1  | um: kcsan: arch header + USER-TU sanitizer opt-outs              | C-03 hunks for `arch/um/include/asm/kcsan.h` + `arch/um/os-Linux/Makefile` + `arch/um/os-Linux/skas/Makefile` | Header + the KCSAN_SANITIZE := n + KASAN_SANITIZE := n lines.  |
| Patch 2  | um: kcsan: select HAVE_ARCH_KCSAN + KASAN/KCSAN mutual-exclusion | C-03 hunks for `arch/um/Kconfig`                     | The `if X86_64` gating + D26 documentation.                    |
| Patch 3  | um: kcsan: race profile fragment                                  | C-03 hunks for `arch/um/configs/profiles/race.config` | New profile, ncpus=2 + KCSAN=y + KASAN=n.                      |
| Patch 4  | um: kcsan: race-profile selftest                                  | new — derived from C-03 selftest                     | `tools/testing/selftests/um/kcsan-smoke/`; 3/3 PASS at boot.   |
| Patch 5  | um: kcsan: user doc + MAINTAINERS                                 | new — derived from C-03 status doc                   | `Documentation/virt/uml/kcsan.rst`.                            |

The squash audits should aim to produce per-sub-series
commit messages that read as upstream-shape patches, not
as redesign-walkthrough commits. Each intermediate state
within each sub-series MUST build, boot, and pass the
feature-appropriate test:

  - **6a**: kprobes-stress 200/2000 iter PASS at the tail
    of Patch 3.
  - **6b**: ftrace-smoke trace_lines>=50000 at the tail of
    Patch 5 (Patch 1+2 build; Patch 3+4 the function_graph
    pieces are graph=off at tail of Patch 2, graph=on at
    tail of Patch 4; Patch 5 selftest validates both).
  - **6c**: KFENCE KUnit 13/27 PASS (stack-walker
    disclaimers documented) at the tail of Patch 3.
  - **6d**: KCSAN selftest 3/3 + KUnit 20+ races at the
    tail of Patch 4.

Patches that break the build or break the
feature-appropriate test are merge-conflict bugs in the
emission, not legitimate intermediate states.

### Sequencing checklist before format-patch — per sub-series

Per `backend-ops-abstraction-rfc/SUBMISSION-NOTES.md` and
`static-key-hot-paths-series/SUBMISSION-NOTES.md`
precedent, applied four times:

  1. **Submission branches** (4 of them, off
     post-Series-5 `master`):
       - `kprobes-rfc-v1-submit`
       - `ftrace-graph-rfc-v1-submit`
       - `kfence-rfc-v1-submit`
       - `kcsan-rfc-v1-submit`
     NOT off `umlctl-deploy`, NOT off pre-Series-5
     master. Cherry-pick the per-sub-series squash-audit
     outputs in order.
  2. **Author + signoff:** `Michael Bommarito
     <michael.bommarito@gmail.com>` (never
     bommaritollc.com). `Co-authored-by: Claude Opus
     4.7 …` trailers **dropped** for upstream per
     ftrace-notrace precedent;
     `Documentation/process/coding-assistants.rst`
     covers AI-assistance disclosure.
  3. **No redesign-log references in commit messages.**
     D## entries stay in this file and the cover letter,
     never in per-patch bodies.
  4. **Checkpatch:** 0/0 target per `.patch`. Code-snippet
     line-length warnings in commit messages are
     acceptable false positives.
  5. **Build verification per intermediate state per
     sub-series:** each patch must build under
     `make ARCH=um O=/tmp/uml-N defconfig` with the
     feature `{y,n}` matrix; boot under
     `./linux mem=512M init=/bin/sh` (and `ncpus=2` for
     6d); pass the feature-appropriate selftest with the
     feature on. Failing intermediates fix in the
     squash, never papered over.
  6. **`scripts/get_maintainer.pl`** rerun per sub-series
     right before `git send-email`.
  7. **Format-patch per sub-series:**

        # Sub-series 6a (kprobes)
        git format-patch -4 --cover-letter \
            --subject-prefix='PATCH RFC' --base=master \
            -o /tmp/kprobes-rfc-emit/
        # Cover subject: [PATCH RFC 0/4] um: kprobes via int3 + rethook

        # Sub-series 6b (ftrace + function_graph)
        git format-patch -6 --cover-letter \
            --subject-prefix='PATCH RFC' --base=master \
            -o /tmp/ftrace-graph-rfc-emit/
        # Cover subject: [PATCH RFC 0/6] um: ftrace + function_graph

        # Sub-series 6c (KFENCE)
        git format-patch -4 --cover-letter \
            --subject-prefix='PATCH RFC' --base=master \
            -o /tmp/kfence-rfc-emit/
        # Cover subject: [PATCH RFC 0/4] um: KFENCE arch enablement

        # Sub-series 6d (KCSAN)
        git format-patch -5 --cover-letter \
            --subject-prefix='PATCH RFC' --base=master \
            -o /tmp/kcsan-rfc-emit/
        # Cover subject: [PATCH RFC 0/5] um: KCSAN arch enablement

  8. **Send cadence:** the four sub-series ship at
     paced intervals — recommend ~1 week between sends
     to avoid the `linux-um@` cc'd reviewers seeing
     four parallel RFC threads in their inbox the same
     day. Order: 6a → 6b → 6c → 6d per the cover
     letter §"Migration plan."

## Verification author performed

(Captured 2026-05-14 against the in-fork branch state;
needs re-run on the per-sub-series cherry-picked
submission branches after Series 4 + 5 land and before
each send.)

  - **C-02 KFENCE (sub-series 6c) — landed 2026-04-18,
    verified:**
      - `arch/um/include/asm/kfence.h` is the working
        header on `umlctl-deploy`.
      - 13/27 upstream KFENCE KUnit tests PASS; the 12
        failures are formatted-stack-trace assertions
        against UML's stack-walker output, not KFENCE
        functional failures (KFENCE catches OOB
        reads/writes correctly; just doesn't render
        symbol names the test expects).
      - Research + fuzz-deep profiles enable
        `CONFIG_KFENCE=y` with no boot regression.

  - **C-03 KCSAN (sub-series 6d) — landed 2026-04-18,
    verified:**
      - KCSAN selftest at boot 3/3 PASS.
      - KCSAN KUnit suite runs with `ncpus=2` and
        detects 20+ races.
      - USER-TU sanitizer opt-outs in
        `arch/um/os-Linux/{,skas/}Makefile` link clean.
      - KASAN/KCSAN mutual exclusion per D26: race
        profile disables KASAN; fuzz-deep keeps it on.
      - 200-iteration in-guest stress test (running the
        upstream `kcsan_test_basic_relaxed_races` suite
        under syzkaller-style workload) — no
        unexpected races, no kernel panic.

  - **C-04 kprobes + rethook + function_graph
    (sub-series 6a + 6b Patch 3-4) — landed 2026-04-22,
    verified:**
      - kprobes-stress selftest:
        `PASS iters=200 fires=204 errors=0 graph=on`.
      - kprobes-stress selftest:
        `PASS iters=2000 fires=227 errors=0 graph=on`
        (fire count compresses at high iteration counts
        due to dmesg ring buffer rollover; PASS condition
        is `fires >= 1 with no observed kernel error`).
      - `samples/kprobes/kprobe_example.ko` loads, fires
        on `do_sys_open`, prints entry message, removes
        cleanly.
      - `samples/kprobes/kretprobe_example.ko` loads,
        fires on `kernel_clone`, reports return values,
        removes cleanly.
      - `echo function_graph >
        /sys/kernel/tracing/current_tracer` produces
        call-graph output; no atomic-context warnings
        in dmesg after Patch 3's preempt-count guard.
      - `cat /sys/kernel/debug/kprobes/blacklist |
        grep rethook` lists `arch_rethook_trampoline`.

  - **C-05 ftrace (sub-series 6b Patch 1-2 + 5-6) —
    landed 2026-04-19, verified:**
      - ftrace-smoke selftest:
        `FTRACE_SMOKE: PASS trace_lines=51316` on a
        minimal `ls /` workload.
      - Research profile boot (`init=/bin/true`):
        median 3.48 s tracer-on vs 3.37 s tracer-off,
        +3.3 % NOP5 i-cache residency overhead. Under
        A-07's 5 % invariant ceiling.
      - `objdump -h vmlinux | grep
        __patchable_function_entries`: nonzero size.
      - `objdump -h vmlinux | grep __mcount_loc`:
        nonzero size with both ARCH=um (gcc) and
        ARCH=um LLVM=1 (clang).
      - Boot matrix across SECCOMP_ONLY backend (and,
        post-Series-7, KVM-v2 backend) all PASS with
        tracer off AND with tracer on.
      - `function` tracer + `set_ftrace_filter` work;
        whitelist one symbol + trace only that.

  - **Build matrix per sub-series, captured 2026-05-14
    against `umlctl-deploy` head:**
      - sub-series 6a: `defconfig + CONFIG_KPROBES=y`
        builds gcc + clang clean.
      - sub-series 6b: `defconfig + CONFIG_FUNCTION_TRACER=y
        + CONFIG_DYNAMIC_FTRACE=y +
        CONFIG_FUNCTION_GRAPH_TRACER=y` builds gcc +
        clang clean.
      - sub-series 6c: `defconfig + CONFIG_KFENCE=y`
        builds gcc + clang clean.
      - sub-series 6d: `defconfig + CONFIG_KCSAN=y +
        CONFIG_SMP=y + ncpus=2` builds gcc + clang
        clean.

  - **`git apply --check` simulation** against a 2026-05
    `linux-next` snapshot: per-sub-series no conflicts.
    The `arch/um/Kconfig` hunks for the four
    `select HAVE_*` lines need re-validation against
    post-Series-5 state. The
    `arch/um/configs/profiles/race.config` is a new file
    (no merge conflict possible).

Re-run on each cherry-picked submission branch
(against post-Series-5 `master`) before each
`git send-email`.

## Maintainer routing

Per-sub-series routing — run
`scripts/get_maintainer.pl` per sub-series right before
each send. The 2026-05 view:

### Sub-series 6a — kprobes

```text
$ scripts/get_maintainer.pl 6a/0001-*.patch
Masami Hiramatsu <mhiramat@kernel.org>            (MAINTAINER: KPROBES)
Steven Rostedt <rostedt@goodmis.org>              (MAINTAINER: TRACING)
Richard Weinberger <richard@nod.at>               (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>    (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>         (MAINTAINER: USER MODE LINUX)
linux-trace-kernel@vger.kernel.org                (open list: TRACING / KPROBES)
linux-um@lists.infradead.org                       (open list: USER MODE LINUX)
linux-kernel@vger.kernel.org                       (open list)
```

Manual additions for cc:

  - **Benjamin Berg `<benjamin@sipsolutions.net>`** — UML
    reviewer for trap-path changes.
  - **Tiwei Bie `<tiwei.btw@antgroup.com>`** — UML
    contributor; reviews cross-cutting series.

### Sub-series 6b — ftrace + function_graph

```text
$ scripts/get_maintainer.pl 6b/0001-*.patch
Steven Rostedt <rostedt@goodmis.org>              (MAINTAINER: FTRACE)
Masami Hiramatsu <mhiramat@kernel.org>            (MAINTAINER: TRACING)
Richard Weinberger <richard@nod.at>               (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>    (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>         (MAINTAINER: USER MODE LINUX)
linux-trace-kernel@vger.kernel.org                (open list)
linux-um@lists.infradead.org                       (open list)
linux-kernel@vger.kernel.org                       (open list)
```

Manual additions same as 6a (Benjamin Berg + Tiwei Bie).

### Sub-series 6c — KFENCE

```text
$ scripts/get_maintainer.pl 6c/0001-*.patch
Marco Elver <elver@google.com>                    (MAINTAINER: KFENCE)
Alexander Potapenko <glider@google.com>           (MAINTAINER: KFENCE)
Richard Weinberger <richard@nod.at>               (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>    (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>         (MAINTAINER: USER MODE LINUX)
linux-mm@kvack.org                                 (open list: MM)
linux-um@lists.infradead.org                       (open list)
linux-kernel@vger.kernel.org                       (open list)
```

Manual additions same as 6a.

### Sub-series 6d — KCSAN

```text
$ scripts/get_maintainer.pl 6d/0001-*.patch
Marco Elver <elver@google.com>                    (MAINTAINER: KCSAN)
Dmitry Vyukov <dvyukov@google.com>                (KCSAN author)
Alexander Potapenko <glider@google.com>           (sanitizer adjacency)
Richard Weinberger <richard@nod.at>               (MAINTAINER: USER MODE LINUX)
Anton Ivanov <anton.ivanov@cambridgegreys.com>    (MAINTAINER: USER MODE LINUX)
Johannes Berg <johannes@sipsolutions.net>         (MAINTAINER: USER MODE LINUX)
linux-mm@kvack.org                                 (open list)
linux-um@lists.infradead.org                       (open list)
linux-kernel@vger.kernel.org                       (open list)
```

Manual additions same as 6a.

### Send recipes (when authorized, per sub-series)

```text
# Sub-series 6a (kprobes)
cd /tmp/kprobes-rfc-emit
git send-email \
    --to=linux-trace-kernel@vger.kernel.org \
    --cc=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=mhiramat@kernel.org \
    --cc=rostedt@goodmis.org \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    *.patch

# Sub-series 6b (ftrace + function_graph)
cd /tmp/ftrace-graph-rfc-emit
git send-email \
    --to=linux-trace-kernel@vger.kernel.org \
    --cc=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=rostedt@goodmis.org \
    --cc=mhiramat@kernel.org \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    *.patch

# Sub-series 6c (KFENCE)
cd /tmp/kfence-rfc-emit
git send-email \
    --to=linux-mm@kvack.org \
    --cc=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=elver@google.com \
    --cc=glider@google.com \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    *.patch

# Sub-series 6d (KCSAN)
cd /tmp/kcsan-rfc-emit
git send-email \
    --to=linux-mm@kvack.org \
    --cc=linux-um@lists.infradead.org \
    --cc=linux-kernel@vger.kernel.org \
    --cc=elver@google.com \
    --cc=dvyukov@google.com \
    --cc=glider@google.com \
    --cc=richard@nod.at \
    --cc=johannes@sipsolutions.net \
    --cc=anton.ivanov@cambridgegreys.com \
    --cc=benjamin@sipsolutions.net \
    --cc=tiwei.btw@antgroup.com \
    *.patch
```

Rerun `scripts/get_maintainer.pl --file <each per-feature
arch/um/ file>` right before each send to pick up
maintainer rotation since 2026-05.

## checkpatch findings (anticipated per sub-series)

Projected from the in-fork state until the squash audits
produce the actual `.patch` files:

  - **Sub-series 6a Patch 1**: possible `LONG_LINE` on the
    `arch_specific_insn` struct member alignment;
    acceptable false positive on x86-pattern kprobes
    headers.
  - **Sub-series 6a Patch 2**: zero warnings expected on
    the rethook port (assembly + minimal C wrapper).
  - **Sub-series 6b Patch 1**: assembly file checkpatch
    coverage is limited; structural warnings are likely
    false positives.
  - **Sub-series 6b Patch 3**: possible `LONG_LINE` on
    `prepare_ftrace_return()` preempt-count guard comment;
    acceptable false positive.
  - **Sub-series 6c**: zero warnings expected — the C-02
    `static inline` arch header is short and clean.
  - **Sub-series 6d Patch 1**: USER-TU Makefile hunks may
    trigger `UNNECESSARY_KERN_LEVEL` warnings from
    checkpatch's Makefile heuristics; false positives.
  - **Sub-series 6d Patch 4** (selftest): `.sh` script
    in `tools/testing/selftests/um/kcsan-smoke/`; typical
    `BIT_MACRO` warnings false-positive on shell.
  - **All sub-series Patch N (user doc)**: zero warnings
    on `.rst` files.

Per `ftrace-notrace-generic-v1/SUBMISSION-NOTES.md`
precedent, prose `.md` files pass checkpatch
`--no-tree -f` at 0/0; only `.patch` files trigger real
findings.

## False-positive false alarms to pre-empt

  - **"Why is function_graph's preempt-count guard
    UML-specific?"** D34 addendum-4 walks the analysis:
    UML-UP's TINY_RCU maps `rcu_read_lock()` to
    `preempt_disable()`, so a graphed function body
    reaching `free_irq → __mutex_lock` from the IRQ
    delivery path trips `__might_resched` inside the
    trace body. The guard is at the UML-specific
    intersection of "atomic-context-from-IRQ on a
    single-CPU UML" and "graph shadow-stack push."
    Other arches don't have this exact intersection.

  - **"Why kprobes via int3 instead of via ftrace?"**
    `HAVE_KPROBES_ON_FTRACE` is a perf optimization that
    requires `HAVE_DYNAMIC_FTRACE_WITH_REGS` —
    substantial extra assembly investment for no
    user-visible capability gain. The int3 path covers
    all four kprobe surfaces (entry, mid-function,
    returns, graphs); the ftrace fast path is just a
    faster entry-probe path. Per a-plus-quality-plan
    anti-pattern #3 ("optimize before workloads
    exist"), the ftrace path is a follow-up task once
    representative workloads exist.

  - **"Why does sub-series 6d ship a new `race`
    profile instead of folding into research?"** The
    research profile is `CONFIG_KASAN=y` for
    heap-sanitizer coverage; KCSAN is mutually
    exclusive with KASAN (`lib/Kconfig.kcsan` has
    `depends on !KASAN`). Folding into research would
    force a choose-one decision; carrying both as
    separate profiles is the cleaner shape and matches
    the per-profile architecture from C-01.

  - **"Why is the KFENCE KUnit only 13/27 PASS?"**
    The 12 failures are formatted-stack-trace
    assertions against UML's stack walker; KFENCE
    itself catches OOB reads/writes correctly (KFENCE
    functional). The walker mismatch is a separate
    long-standing UML issue (`arch/um/kernel/stacktrace.c`
    undermaintenance). Sub-series 6c's user doc spells
    this out; sub-series 6c does NOT pretend to fix the
    walker.

  - **"Why does sub-series 6b touch
    `arch/um/{kernel,os-Linux}/signal.c`?"** The
    `notrace` annotations on the `arch_local_irq_*`
    family are part of the function_graph leak-sources
    (1)+(2) closure per D34. The `arch_local_irq_*`
    family is reachable from atomic context via the IRQ
    delivery path; without `notrace`, the function_graph
    trampoline would push a shadow-stack entry on every
    IRQ entry / exit, which the unwind path then leaks.

## Why this series stands alone — the upstream-interest pitch

The four sub-series are upstream-friendly for three
reasons:

  1. **They enable existing upstream features for UML.**
     KFENCE / KCSAN / kprobes / ftrace / function_graph
     are all mature upstream features with maintained
     test suites; sub-series 6a-d turn each on for UML
     by selecting the corresponding `HAVE_*` and
     supplying the small arch-side hook surface each
     requires. Reviewers see new arch enablement
     (positive line count net of the arch hooks; net
     zero impact on the generic feature
     implementation) rather than feature rework.

  2. **They share a clean dependency story.** Series 4
     (ops table) + Series 5 (static-key gates +
     `.um_patch_text` section split) land first; Series
     6 sub-series consume the substrate without
     re-inventing it. The dependency graph is
     unambiguous and small (3 series up the chain;
     Series 6 is the consumer leaf).

  3. **They route to per-feature maintainer audiences.**
     Bundling four arch enablements with one cover
     letter does not force any single maintainer to
     carry all four — the per-sub-series routing
     respects each feature's primary subsystem
     (trace-kernel for 6a + 6b, mm for 6c + 6d) with
     `linux-um@` cc'd as the cross-cutting context.

## v1 → v2 per sub-series if review feedback comes in

Keep `umlctl-deploy` as-is; cherry-pick the per-sub-series
squashed v2 patches onto fresh per-sub-series submission
branches per iteration:

```text
git checkout -b kprobes-rfc-v2 master
git cherry-pick <4 squash-audit outputs from 6a v2 pass>
git format-patch -4 --cover-letter --subject-prefix='PATCH v2'

# (and three more for 6b/6c/6d)
```

Likely v2 deltas (from cover letter §"Anticipated
review questions"):

  - **6a (kprobes)**: split rethook port off as Patch 2b
    behind `if !HAVE_RETHOOK` (q (g)).
  - **6b (ftrace)**: move preempt-count guard to
    generic `kernel/trace/` if reviewers want it shared
    across arches (q (e)).
  - **6c (KFENCE)**: bundle stack-walker fix or ship
    walker-fix as a separate arch/um/ cleanup series
    first (q (c)).
  - **6d (KCSAN)**: KASAN/KCSAN runtime choice block
    instead of per-profile mutual exclusion if reviewers
    want a single `DEBUG_KERNEL_SANITIZER` choice
    (q (d)).

Drop `RFC` prefix per sub-series once a maintainer acks
the shape.

## Cross-references — lineage back to the UML redesign

  - `00-vision.md` — three-layer architecture (Layer 3 =
    per-profile feature surface, the layer Series 6
    populates).
  - `01-architecture/three-layers.md` — per-layer
    rationale.
  - `02-workstreams/C-profiles-and-gaps/` —
    `02-port-kfence.md` (sub-series 6c),
    `03-port-kcsan.md` (sub-series 6d),
    `04-port-kprobes.md` (sub-series 6a + 6b Patch 3-4),
    `05-port-ftrace.md` (sub-series 6b Patch 1-2 + 5-6).
  - `03-profiles/README.md` — profile matrix; rows for
    research / fuzz / fuzz-deep / race that Series 6's
    profile fragments populate.
  - `04-risks/decisions-log.md` — D23, D26, D27, D28,
    D29, D30, D31, D32, D33, D34 (+ 3 addenda), D45,
    D63 (cover letter §"Lineage" for the per-D walk).
  - `06-sequencing/PLAN-2026-05-14.md` §6.2 — Series 6
    queue position + Series 7 blocker note.
  - `upstream-patches/SUBMISSION-QUEUE.md` row 6 — the
    per-sub-series sequencing decision.
  - `upstream-patches/ftrace-notrace-generic-v1/` —
    Series 3, the upstream piece sub-series 6b
    depends on (the `notrace` on `kthread()` /
    `smpboot_thread_fn()`).

Lineage is NOT cited in the upstream commit messages per
the "reviewers see the patch on its own merits" rule in
`upstream-patches/README.md`. The redesign branch is
offered for inspection; nothing in it depends on Series 6
landing upstream.

## When this lands

On acceptance per sub-series:

  1. Add the upstream commit SHA range for the
     sub-series to this file (per-sub-series status
     section notes).
  2. Move this directory to
     `upstream-patches/landed/kprobes-ftrace-kfence-kcsan-profiles-series/`
     once ALL FOUR sub-series have landed, per
     `upstream-patches/README.md`. Partial-landing
     state (e.g. 6a + 6b landed, 6c + 6d outstanding)
     leaves the directory in `upstream-patches/`
     active queue.
  3. Update `SUBMISSION-QUEUE.md` row 6 status:
     "landed (upstream <SHA range per sub-series>)".
  4. Unblock Series 7 (kvm-backend-series) per the
     cover letter §"Dependencies" — Series 7's
     record/replay observe hook cites Series 6b's
     function_graph trampoline pattern.
  5. Update `02-workstreams/C-profiles-and-gaps/README.md`
     to mark each landed sub-series as
     "landed upstream + in-fork extensions deferred to
     follow-up tasks (kprobes-on-ftrace per C-04b;
     KFENCE stack-walker fix; etc.)."
  6. Note in `04-risks/decisions-log.md` D26 / D27 /
     D28 / D29 / D32 / D33 / D34 (each landing closure)
     that the C-fallback design has aged into upstream.
     Mark the relevant D entries as superseded by
     upstream landing.
  7. Spin up follow-up tasks per the cover letter
     §"What this DOES NOT do":
       - C-04b: `HAVE_KPROBES_ON_FTRACE` (perf
         optimization).
       - KFENCE stack-walker fix (UML stack walker
         renderer maintenance).
       - C-05b: `HAVE_DYNAMIC_FTRACE_WITH_REGS` (BPF +
         kprobes-on-ftrace prerequisite).

## Status

  - Cover letter: **DRAFTED 2026-05-14** (this revision).
  - SUBMISSION-NOTES.md: **DRAFTED 2026-05-14** (this
    file).
  - README.md: **DRAFTED 2026-05-14** (reader's guide).
  - Patches: **NOT YET EMITTED.** Awaiting Series 4 +
    Series 5 landing + per-sub-series squash audit on
    the in-fork substrate.
  - Submission branches: **not yet created.** Will be
    `kprobes-rfc-v1-submit`, `ftrace-graph-rfc-v1-submit`,
    `kfence-rfc-v1-submit`, `kcsan-rfc-v1-submit` off
    post-Series-5 `master`.
  - Send timeline: gated on Series 4 + Series 5 landing;
    realistic earliest first-sub-series send-out is ~6-12
    weeks after Series 5 reaches a maintainer-ack. The
    four sub-series ship at ~1-week intervals to avoid
    overwhelming `linux-um@` cc'd reviewers.
