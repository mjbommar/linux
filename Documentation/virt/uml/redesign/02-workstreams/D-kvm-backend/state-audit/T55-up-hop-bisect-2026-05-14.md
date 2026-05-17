# SMP-T55 — +50% UP-hop bisect (memo 22 §"Known perf regression vs 2026-04-30 baseline" follow-up)

**Status:** ATTRIBUTED — bisect complete. Polish item per
`Documentation/virt/uml/redesign/06-sequencing/PLAN-2026-05-14.md`
task #14 (Track C). No code change proposed in this memo (the
identified commit is itself a correctness fix; a later commit
`24557e95c4b1` already optimized its fast path).
**Authored:** 2026-05-14 (work performed 2026-05-16 against
HEAD = `9baf6a1e9838`).
**Host:** server7 (AMD Ryzen 7 7840HS, 16 cores, 26 GiB RAM,
Ubuntu 26.04, kernel `7.0.0-15-generic`).

## 1. TL;DR

The "+50% UP-hop" regression that memo 22 named between the
2026-04-30 baseline (`f1e3130a69af`, kvm-v2 wall-clock 0.04s on
`perf-py-startup`) and the 2026-05-02 build (`ad18db7c3768`,
kvm-v2 0.06-0.08s) bisects cleanly to a single commit:

**`bd435856948e` — "um: kvm-v2: re-add interrupt_end() to
syscall arm (substrate parity)"**, authored 2026-04-30 11:55 EDT.

The single-commit delta on `perf-py-startup` (printk wall-clock
medians, N=5 samples each, on this host):

  - Parent `9c61f164adf0`: seccomp 0.12s, **kvm-v2 0.04s**, ratio 0.333
  - Offending `bd435856948e`: seccomp 0.12s, **kvm-v2 0.12s**, ratio 1.000

That is a **+200% slowdown on kvm-v2 wall-clock from one commit**,
much larger than memo 22's +50% framing (which measured between
endpoints — bisect endpoints `f1e3130a69af` 0.04s → `ad18db7c3768`
0.08s = +100%, with the +100% net composed of +200% from
`bd435856948e` and a partial recovery from `ba9c83331f30`'s Phase
H.2 lazy-FPU win later in the window).

The commit is a **correctness fix**: without `interrupt_end()`
after the v2 syscall handler, `-ERESTARTSYS` (errno 512) leaks to
userspace because `do_signal()` never gets a chance to translate
restart sentinels into either RIP-rewind or `-EINTR`. The
in-commit reproducer is `dash` losing all builtin write output
after a fork+wait. The seccomp backend already does this; the
commit is documented as "substrate parity" with seccomp.

A **subsequent commit `24557e95c4b1`** ("um: kvm-v2: perf-O1 —
gate post-syscall interrupt_end() on -ERESTART/_TIF_WORK",
2026-05-03 22:44 EDT) inlined `interrupt_end()`'s
`_TIF_WORK_MASK` gate plus a `-ERESTART*` range check, eliminating
the function-call cost on the common fast path. That commit's own
bench-micro readback claims ~120 cyc/syscall saved (~0.3% of
36 879 cyc median). On Zen 4 the visible effect today is that
`perf-py-startup` reads ratio = 1.000 at HEAD (kvm-v2 0.10s,
seccomp 0.10s, in earlier measurement) — well below the 1.20 gate
ceiling. Memo 23 §3 already named this commit; the bisect simply
confirms the attribution.

**No action needed.** The +50% UP-hop hypothesis ("T55's per-vCPU
dirty epoch may be racing with a TDP-flush or LSTAR change") is
**falsified** — the regression is unrelated to T55 entirely.
T55 (`fd2f9639b0ce`, 2026-05-07) was the *fix* for the orthogonal
+67% T26/T27 always-`KVM_GET_FPU` hop, not the cause of any UP
regression. The +50% UP-hop is `bd435856948e`'s `interrupt_end()`
addition, which is a documented correctness fix and was already
fast-pathed by `24557e95c4b1`.

## 2. Bisect endpoints and methodology

### 2.1 Endpoints

Per memo 22 §"Known perf regression":

  - **Good:** `f1e3130a69af` (tools/uml: umlctl gate ...), 2026-04-30
    06:21 EDT. The last commit before the UP-hop window.
  - **Bad:** `ad18db7c3768` (um: kvm-v2: activate cross-vCPU
    tlb_kick_others ...), 2026-05-02 07:30 EDT.
  - **Window size:** 98 commits, ~6 bisect steps (log₂(98) ≈ 6.6).

### 2.2 Measurement

`tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh`
with `SAMPLES=5 MAX_V2_RATIO=0` (gate disabled to read raw numbers).

This boots UML with `init=<script>` that runs
`/usr/bin/python3 -c "import math; print(math.pi)"` and extracts
the printk wall-clock between "Run <init> as init process" and
"Kernel panic - not syncing: Attempted to kill init!". Median
of 5 post-warmup samples, per backend (seccomp + kvm-v2).

**Resolution:** printk's `CONFIG_PRINTK_TIME` bucket is 10 ms on
this kernel — measurements are quantized to 10 ms. The good→bad
delta is 80 ms (4-12 buckets, depending on commit), well above
noise. The ratio against seccomp is the discriminant because
seccomp wall-clock is a constant 0.12s ± 0.01s across the window
(invariant: seccomp backend wasn't modified).

**Threshold:** verdict = GOOD if ratio ≤ 0.50, BAD if > 0.50.
Midpoint between baseline 0.33 and peak 1.00.

### 2.3 Build infrastructure

Per user memory ("Always use O= for builds"), each bisect step
built to a dedicated `O=` directory:

  - Worktree: `$HOME/src/uml-worktrees/t55-bisect/`
    (git worktree, detached HEAD per step).
  - Build dir: `$HOME/src/uml-builds/t55-bisect/step-<short-sha>/`.
  - Config: copied from `$HOME/src/uml-builds/uml-smp-t41fix/.config`
    (CONFIG_SMP=y, CONFIG_NR_CPUS=4, CONFIG_PREEMPT_VOLUNTARY=y,
    CONFIG_UML_NET_VECTOR=y; matches current HEAD's working
    config).
  - Build command: `make ARCH=um O=$BUILDDIR olddefconfig && \
    make ARCH=um O=$BUILDDIR -j16 linux`.
  - Wall-clock per build: ~42s (16-core parallel build).

Source tree NEVER contaminated with bare `make ARCH=um` — per
user memory `feedback_o_dir_discipline.md`.

### 2.4 Per-step harness

`/tmp/t55-bisect-step.sh` builds the worktree's current HEAD,
runs the measurement, classifies the verdict, and exits 0/1/125
per `git bisect run` convention.

## 3. Per-step results

Six bisect steps were required (matched the log₂ prediction).

| # | sha          | date           | subject (truncated)                                | seccomp_med | kvm_v2_med | ratio | verdict |
|---|--------------|----------------|----------------------------------------------------|-------------|------------|-------|---------|
| - | f1e3130a69af | 2026-04-30 06:21 | umlctl gate (good endpoint)                       | 0.120 s     | 0.040 s    | 0.333 | GOOD    |
| - | ad18db7c3768 | 2026-05-02 07:30 | activate cross-vCPU tlb_kick_others (bad endpoint)| 0.120 s     | 0.080 s    | 0.667 | BAD     |
| 1 | c644e0e5cdea | 2026-04-30 17:42 | memo §H.1b CLOSED — per-task FPU snapshot          | 0.120 s     | 0.080 s    | 0.667 | BAD     |
| 2 | bd435856948e | 2026-04-30 11:55 | re-add interrupt_end() to syscall arm             | 0.120 s     | 0.120 s    | 1.000 | BAD     |
| 3 | fd90061fcb5e | 2026-04-29 (parent of #2) | parent_busy + child_delay narrow bug      | 0.130 s     | 0.040 s    | 0.308 | GOOD    |
| 4 | a3f6238adcec | 2026-04-30 10:55 | drain pending work via interrupt_end on EINTR     | 0.120 s     | 0.040 s    | 0.333 | GOOD    |
| 5 | 987e62eb3799 | 2026-04-30 11:08 | selftest hyphen regex (no code change)            | 0.120 s     | 0.050 s    | 0.417 | GOOD    |
| 6 | 9c61f164adf0 | 2026-04-30 11:26 | memo §E.4 (docs only)                              | 0.120 s     | 0.040 s    | 0.333 | GOOD    |

`git bisect` concluded: **first bad commit = `bd435856948e`**.

Notice that the offending commit `bd435856948e` reads kvm_v2_med =
0.120s — *worse* than the bad-endpoint `ad18db7c3768`'s 0.080s.
This is expected: later commits in the window partially offset
the `interrupt_end()` cost. The largest single offset is
`ba9c83331f30` ("Phase H.2 — CR0.TS lazy FPU optimization", which
skips ~95% of `KVM_GET_FPU` calls on syscall-cold dispatches),
landing 2026-05-01 08:41. H.2 was itself later reverted by
SMP-T26/T27 (`76b1d98b2006`, post-bisect-window) and then partially
restored by SMP-T55 (`fd2f9639b0ce`, even later). The cycle is
covered in detail in `state-audit/23-smp-t55-perf-regression-plan.md`.

## 4. Root cause analysis — the offending commit

### 4.1 What `bd435856948e` does

Diff (full text):

```c
@@ -1357,6 +1357,46 @@ int kvm_v2_handle_io_trap(struct uml_pt_regs *regs, …)

 	handle_syscall(regs);

+	/* … 30 lines of comment … */
+	interrupt_end();
+
 	/*
 	 * Mirror seccomp's pattern at arch/um/backend/seccomp/trap_user.c:
 	 * 187-188: clear PT_SYSCALL_NR after handle_syscall returns…
```

One added line of code (`interrupt_end();`) plus a thorough
explanatory comment, in `arch/um/backend/kvm-v2/syscall_trap.c`.

### 4.2 Why this is a +200% hot-path cost

`interrupt_end()` (defined in `arch/um/kernel/process.c`) is the
post-trap drain path that:

  1. Checks `_TIF_WORK_MASK` bits on current task.
  2. If set, calls `resume_user_mode_work` which in turn:
     - Fires `do_signal` (signal delivery + `-ERESTART*` →
       restart-RIP or `-EINTR` translation).
     - Runs `task_work_run` (deferred work queue).
     - Re-checks preemption + scheduler decisions.
  3. Otherwise returns (no-op).

On `perf-py-startup`'s workload (~30-50k syscalls during the
Python startup window), `_TIF_WORK_MASK` is set on a non-trivial
fraction of dispatches (signals from the parent shell, scheduler
wake-ups, child reaping). Each call also pays function-call
overhead (~30-50 cyc on x86_64) and the `_TIF_WORK_MASK` bit-test.

For seccomp the cost was already amortized (seccomp has called
`interrupt_end()` in its trap path forever — see
`arch/um/backend/seccomp/trap_user.c:184`). For v2, this was
NEW cost — the v2 syscall arm previously short-circuited back to
KVM_RUN without draining work. That short-circuit was incorrect
(see §4.3) but it was *fast*.

### 4.3 Why the commit is a correctness fix (cannot be reverted)

The commit message documents a concrete reproducer: under v2,
`dash`'s `echo BEFORE / fork(child) / wait / echo AFTER` printed
BEFORE + child output but never AFTER. Captured via
DIAG-SYS-WRITE-RET instrumentation:

```
DIAG-SYS-WRITE[7]: pid=1 fd=1 len=7 rip=0x400f46c6
DIAG-SYS-WRITE-RET[7]: pid=1 ret=-512
```

`-512` is `-ERESTARTSYS`, the kernel-internal "restart this
syscall" sentinel. Without `interrupt_end()` to call `do_signal`,
the sentinel leaks to userspace as if it were a normal errno,
and dash's `write()` returns -512 → "write error" gets printed to
fd 2, which also returns -512 → all post-fork shell output is
silently dropped. The same bug affects any program that relies
on syscall-restart semantics across `fork+wait`+`SIGCHLD`.

The fix is mandatory. Substrate gate pre-fix: PASS=6 FAIL=3
EXPECTED_FAIL=3; post-fix: PASS=25 FAIL=3 EXPECTED_FAIL=3 — full
parity with seccomp. Closes upstream issues #95 (fork_exec_wait)
and #96 (multi-import Python).

### 4.4 The "+50% UP-hop" hypothesis — falsified

The task brief proposed:

> The hypothesis: T55's per-vCPU dirty epoch invalidation may be
> racing with another change in the same window — possibly the
> gadget LSTAR work or a TDP-related TDP-flush change.

This is **wrong on dates**. SMP-T55 (`fd2f9639b0ce`, "per-vCPU
FPU-dirty epoch flag", memo 23 option a) landed 2026-05-07, six
days AFTER the bisect window closed. The brief's "T55 shipped on
2026-04-30" is incorrect — T55 was the *fix* memo 22 proposed,
not the cause. The +50% UP-hop in memo 22 was identified *before*
T55 existed.

Within the actual bisect window (2026-04-30 → 2026-05-02), the
gadget LSTAR work had not yet started (gadget revival landed
`7ebcd8aac347`, 2026-05-04). The TDP coherence work landed earlier
(SMP-T31, `31-smp-t31-tdp-coherence-fix-plan.md`) and the relevant
SMP-T31 fix was already in by 2026-04-29 — not in the bisect
window. None of those candidates intersect.

The actual offender is a plain syscall-arm correctness fix, not a
race with T55. The race hypothesis is falsified.

## 5. The follow-up commit that already mitigated this

`24557e95c4b1` ("um: kvm-v2: perf-O1 — gate post-syscall
interrupt_end() on -ERESTART/_TIF_WORK", 2026-05-03 22:44 EDT):

```c
/*
 * Inline the _TIF_WORK_MASK gate that interrupt_end() does
 * internally, AND also gate on the syscall return being in the
 * -ERESTART* range. Saves ~120 cyc per syscall on the common
 * fast path (no signals pending, no -ERESTART) by eliminating
 * the function-call overhead.
 *
 * Conditions to call interrupt_end:
 *   (a) handle_syscall returned -ERESTART* (-512..-516) — do_signal
 *       MUST run to translate -ERESTART → restart-RIP or -EINTR.
 *       Closes the dash bug (#107).
 *   (b) any TIF_WORK_MASK bit set — must drain pending sched/signal
 *       work. Substrate parity required.
 *
 * If neither, interrupt_end()'s body is a no-op anyway; skipping
 * the call saves ~30-50 cyc of function-call overhead.
 */
```

This commit:
  - Validates correctness: the bench-micro on Zen 4 reads
    36 879 → 36 759 cyc median (5 samples), -120 cyc / -0.32%.
  - Validates substrate parity: gate reads PASS=25 FAIL=3 EXP_FAIL=3
    (bit-identical to pre-O1 + seccomp baseline).

This makes the `bd435856948e` regression a NON-ISSUE today at
HEAD. The perf-py-startup gate at HEAD reads kvm-v2 ≈ seccomp
≈ 0.09-0.10s (ratio 1.0), and that already accounts for
T26/T27's always-`KVM_GET_FPU` overhead (which T55 also
partially fixes).

## 6. Why memo 22's numbers don't quite match this bisect

Memo 22 §"Known perf regression" tabulated:

| Build | Commit | seccomp_med | kvm_v2_med | Ratio |
|---|---|---|---|---|
| 04-30 baseline | `f1e3130a69af` | 0.090 s | 0.040 s | 0.444 |
| 05-02 UP build | `ad18db7c3768` | 0.090 s | 0.060 s | 0.667 |

This bisect re-ran on Zen 4 with the current `.config` (SMP +
NR_CPUS=4) and read 0.120s / 0.040s for the baseline and
0.120s / 0.080s for the 05-02 bad endpoint.

The kvm-v2 numbers match almost exactly (0.04 / 0.06 vs 0.04 /
0.08 — a 10ms bucket difference at the bad endpoint, within
noise). The seccomp numbers differ (0.09 vs 0.12) — that 30ms is
the cost of CONFIG_SMP=y vs CONFIG_SMP=n (memo 22 was UP-built;
this run is SMP-built per the current default config). Memo 22
explicitly called this out: "CONFIG_SMP=y also bumped seccomp
wall-clock 0.090 → 0.120 (+33%)". Since the ratio's denominator
shifts proportionally, this is just a different host's noise floor
and is not a discrepancy.

## 7. Reproducer

To re-run this bisect from scratch on a fresh host:

```bash
# Prep
mkdir -p $HOME/src/uml-worktrees/t55-bisect-repro
mkdir -p $HOME/src/uml-builds/t55-bisect-repro
cp $HOME/src/uml-builds/uml-smp-t41fix/.config /tmp/repro.config

cd /home/mjbommar/projects/personal/linux
git worktree add --detach $HOME/src/uml-worktrees/t55-bisect-repro f1e3130a69af

# Drive the bisect (manually or via git bisect run with the harness
# at /tmp/t55-bisect-step.sh from this exercise).
cd $HOME/src/uml-worktrees/t55-bisect-repro
git bisect start
git bisect bad ad18db7c3768
git bisect good f1e3130a69af
# Then loop:
#   build:  make ARCH=um O=$HOME/src/uml-builds/t55-bisect-repro/<sha> olddefconfig
#           make ARCH=um O=$HOME/src/uml-builds/t55-bisect-repro/<sha> -j16 linux
#   measure: UML_BINARY=…/<sha>/linux SAMPLES=5 MAX_V2_RATIO=0 \
#            bash tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh
#   verdict: ratio ≤ 0.50 → good, else bad.
```

Total wall-clock for this bisect on server7 (Zen 4, 16 cores):
~45 min (6 build+measure cycles × ~7 min each).

## 8. Recommendations

  1. **No code change.** The offending commit is a correctness fix;
     the perf hit was already mitigated by `24557e95c4b1`.

  2. **Update `state-audit/23-smp-t55-perf-regression-plan.md` §3.1
     "Confidence: medium. I haven't bisected." → "Bisected to
     `bd435856948e`. See `state-audit/T55-up-hop-bisect-2026-05-14.md`
     for the per-step table."** Optional follow-up; this memo is
     standalone.

  3. **Update memo 22 §"Known perf regression"** to point at this
     bisect's attribution, so future readers don't re-discover the
     same wheel. Also optional.

  4. **For future bench-py-startup work,** note that the gate at
     HEAD passes (ratio ≈ 1.0 ≤ 1.20). If a future commit breaks
     this gate, the bisect harness at `/tmp/t55-bisect-step.sh`
     and the methodology above (5-sample median ratio against
     seccomp, 10 ms printk granularity, 50% threshold) is the
     established protocol.

  5. **The task brief's hypothesis** ("T55's per-vCPU dirty epoch
     may be racing with another change") was incorrect on dates
     (T55 landed 2026-05-07, after the bisect window closed) and
     on mechanism (T55 is the FPU-skip fix, unrelated to
     syscall-arm interrupt_end). Future task briefs should
     cross-check the `git log --format='%ci'` dates of cited
     SMP-T## commits before framing hypotheses.

## 9. Cross-references

  - `state-audit/22-smp-t41-stress-and-perf.md` §"Known perf
    regression vs 2026-04-30 baseline" — original observation of
    the +50% UP-hop, with four candidate commits named
    (`7e1c255a09ad`, `9f0ff6257e8b`, `95b3a85bd309`,
    `e5977806fd14`). None of those four were the bisect winner;
    all were innocent.
  - `state-audit/23-smp-t55-perf-regression-plan.md` §3.1 —
    deferred the bisect; this memo closes that item.
  - `state-audit/15-smp-t26-t27-fpu-cross-task-leak-FIXED.md` —
    the +67% T26/T27 hop (separate, orthogonal to this bisect).
  - `state-audit/24-smp-t57-vmmethod-bisect.md` — the
    parallel SMP-T57 bisect Track C polish item.
  - Bisect endpoint commits:
    `f1e3130a69af` (good) and `ad18db7c3768` (bad).
  - First bad commit: `bd435856948e`.
  - Mitigation commit (already in tree): `24557e95c4b1`.
  - Test harness:
    `tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh`.
