# Three test paths — pool-architecture pivot validation (2026-05-21)

**Sprint:** post-2026-05-19
**Author:** 2026-05-21 evening session
**Status:** plan; no code in this commit
**Prereq reads:** `post-2026-05-21-pool-architecture-comparison.md`
(the comparative analysis this plan operationalises);
`post-2026-05-21-hardening-plan.md` (the rubric this work is
graded against).

---

## 0. Why three paths and not one

The comparative analysis recommended a single sequenced plan
(1.6 + 1.7 + 1.3 from the comparison memo).  But "do steps 1
through 3 in sequence" puts ~5 days at risk before any
dispositive evidence comes back.  Three independent test paths
let us spend hours to days, not days to weeks, before
re-evaluating.  Each produces a different KIND of evidence:

  * **Path A**: does the rt_sigreturn primitive even work on this
    host's libc + kernel?
  * **Path B**: is snapshot+exec a viable architecture for UML,
    latency-wise?
  * **Path C**: is the v1 ceiling still actually the blocker
    after the 2026-05-21 fixes landed?

All three are commit-by-commit reversible.  None blocks Series 7
send (#7) or the 24h soak (#8).

Run **Path A first**, then **Path C**, then **Path B as
fallback** — see §4 for the rationale.

---

## 1. Path A — Validate the rt_sigreturn primitive in isolation

### 1.1 The hypothesis

CRIU's `rt_sigreturn`-based atomic register restore
(<https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>
line 1079 `rst_sigreturn`) is the canonical mechanism for
leaving a corrupted-stack context and resuming with a fresh one.
The comparison memo (§1.7) names this as the v1-ceiling escape.

**Before** committing days to porting it into UML, prove it works
at all on this host's compiler + glibc + kernel as a vanilla
userspace test.  If the host's kernel rejects our hand-built
`rt_sigframe` (kernel header drift since CRIU was written can be
material), the entire 1.7 → 1.3 line of work in the comparison
memo collapses.

### 1.2 Concrete shape

A standalone C program (~80-120 LoC), no Linux kernel build:

```c
#define _GNU_SOURCE
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The host kernel's rt_sigframe layout for x86_64.  This is the
 * shape we need to populate.  Compare against
 * arch/x86/kernel/signal_64.c::ia32_setup_rt_frame and
 * include/uapi/asm-generic/ucontext.h.
 */
struct rt_sigframe {
    char *pretcode;
    ucontext_t uc;
    siginfo_t info;
    /* fp state pointer goes in uc.uc_mcontext.fpstate */
    /* ... fp state in here ... */
};

extern void clean_entry(void) __attribute__((noreturn));

static char alt_stack[64 * 1024] __attribute__((aligned(16)));

static void build_frame_and_jump(void)
{
    struct rt_sigframe f;
    memset(&f, 0, sizeof(f));

    /* Mirror the host kernel's expected layout.  See
     * arch/x86/kernel/signal_64.c for the exact field offsets.
     * The critical ones: gregs[REG_RIP], gregs[REG_RSP],
     * gregs[REG_EFL], the signal mask.
     */
    f.uc.uc_stack.ss_sp = alt_stack;
    f.uc.uc_stack.ss_size = sizeof(alt_stack);
    f.uc.uc_mcontext.gregs[REG_RIP] = (greg_t)clean_entry;
    f.uc.uc_mcontext.gregs[REG_RSP] =
        (greg_t)(alt_stack + sizeof(alt_stack) - 16);
    f.uc.uc_mcontext.gregs[REG_EFL] = 0x202;        /* IF=1, bit-1 */

    /* rsp must point at the frame; rt_sigreturn reads from rsp. */
    asm volatile (
        "movq %0, %%rsp\n\t"
        "movq $15, %%rax\n\t"   /* __NR_rt_sigreturn */
        "syscall\n\t"
        : : "r"(&f) : "memory"
    );
    __builtin_unreachable();
}

void clean_entry(void)
{
    write(1, "RT_SIGRETURN: arrived\n", 22);
    _exit(0);
}

int main(void)
{
    /* Run from inside a deeply-nested call chain to mimic the
     * "syscall_handler → vfs_write → ...  → fork_on_resume_loop"
     * shape UML hits.  Saved RIPs on the stack should be
     * irrelevant once rt_sigreturn fires.
     */
    write(1, "main entered\n", 13);
    build_frame_and_jump();
    write(1, "BUG: returned from rt_sigreturn\n", 32);
    return 1;
}
```

Build: `gcc -O0 -o rt_sigreturn_test rt_sigreturn_test.c`.

### 1.3 Acceptance

```
$ ./rt_sigreturn_test
main entered
RT_SIGRETURN: arrived
$ echo $?
0
```

Plus: `strace -e raw=rt_sigreturn ./rt_sigreturn_test` shows one
`rt_sigreturn` syscall and exits 0.

### 1.4 Failure modes + what they tell us

  * **`rt_sigreturn` returns `-EFAULT`**: frame layout is wrong.
    Inspect the actual host kernel's `struct rt_sigframe`
    expectation via `bpftrace` on `arch_restore_signal_frame` or
    by reading `arch/x86/kernel/signal_64.c::do_rt_sigreturn`.
    Workable; just needs more careful shape.
  * **Process SEGVs at the destination**: stack alignment or
    EFLAGS issue.  Probably the 16-byte SysV ABI alignment on
    `RSP` at function entry.  Workable.
  * **`rt_sigreturn` returns normally (NOT a jump)**: the kernel
    rejected the frame entirely.  Read dmesg for "bad sigreturn"
    messages.  Major signal — the host kernel won't accept our
    frame, this approach is dead on this host.
  * **`__builtin_unreachable` is reached**: same as above.

### 1.5 Cost + time

~2 hours total: write, compile, debug to PASS.  No kernel
rebuild.  No UML.

### 1.6 Where to land the code

If it works: at `tools/testing/selftests/um/rt-sigreturn-isolation/`
as a tools/ selftest that runs at host level (not in a UML
guest).  Selftests/lib.mk supports host-only tests.

If it doesn't work: capture the failure mode in a state-audit
memo under `02-workstreams/D-kvm-backend/state-audit/` and
**pivot to Path B**.

---

## 2. Path B — Snapshot+exec via the existing #181 ELF64 writer

### 2.1 The hypothesis

We landed `arch/um/backend/kvm-v2/snapshot_elf.c` at
`ee244842a5da` — the ELF64 writer that serialises vCPU + memslots
to a memfd in `ET_CORE` shape.  Firecracker SnapStart (the
industry baseline) achieves p50 3.2 ms / p99 8.7 ms restore via
`MAP_PRIVATE` snapshot files.  We have HALF of the same
architecture; what's missing is the READER side.

If we add the reader and measure restore latency in the ~30 ms
range, snapshot+exec is a viable architecture that **sidesteps
the v1 ceiling entirely** — no fork-from-running-master, no
jmp_buf hazards, no shared physmem.

### 2.2 Concrete shape

#### 2.2.1 Kernel-side reader

New file `arch/um/backend/kvm-v2/snapshot_elf_load.c`:

```c
/*
 * SMP-Tnext-1: ELF64-core snapshot reader.  Mirrors the writer's
 * shape (snapshot_elf.c) but consumes instead of produces.
 *
 * Activation: kvm_v2_snapshot_load=/path/to/core.elf on the
 * kernel cmdline.  Handler runs at late_initcall_sync (after
 * kvm-v2's pool is up but before any guest userspace dispatches).
 *
 *  1. Open the file via filp_open(path, O_RDONLY).
 *  2. Read + validate Elf64_Ehdr (e_ident magic, ELFCLASS64,
 *     ELFDATA2LSB, e_type == ET_CORE).
 *  3. Iterate Elf64_Phdrs.  For each PT_LOAD: copy bytes into the
 *     corresponding guest physical region via the memslot ABI.
 *     For PT_NOTE: parse NT_PRSTATUS / NT_FPREGSET / NT_X86_XSTATE
 *     and the UML-private (n_type=0x554d4c01) note containing
 *     sregs / events / xcrs / msrs.
 *  4. Call kvm_v2_snapshot_restore_full_vcpu(snap, vcpus[0]).
 *  5. Patch vcpus[0]->kvm_run->s.regs.regs.rip to the snapshot's
 *     post-init RIP.  Mark snapshot_loaded=true.
 *
 * Errors at any step pr_warn + boot continues from cold-start
 * (no abort — the snapshot is best-effort optimisation).
 */
```

~250 LoC.

#### 2.2.2 Cmdline + Kconfig

```
config UM_BACKEND_KVM_V2_SNAPSHOT_LOAD
    bool "Enable boot-time snapshot-load from ELF64 core file"
    depends on UM_BACKEND_KVM_V2
    default y
    help
      Accept kvm_v2_snapshot_load=PATH at boot.  When present,
      the late_initcall reader populates vCPU + memslots from
      the ELF64 file produced by kvm_v2_snapshot_elf_export.
      Used by `umlctl pool serve --snapshot-restore=PATH` to
      spawn pool members without paying cold-boot latency.

      Say N to drop the reader from the kernel; the writer
      remains available regardless of this option.
```

#### 2.2.3 Daemon-side flow

`tools/uml/uml-launcher/src/bin/umlctl/pool_serve.rs`:

  1. On daemon start: boot ONE master, let it run to `READY`,
     trigger `umlctl snapshot export` to a memfd.
  2. On each `pool take`: fork+exec a NEW UML process with
     `kvm_v2_snapshot_load=/proc/<daemon>/fd/<memfd>` on the
     cmdline + per-take identity blob env vars.
  3. The fresh process boots cold to `late_initcall_sync`, then
     the snapshot-load handler populates state and jumps to the
     master's saved RIP.

### 2.3 Acceptance

A new selftest `tools/testing/selftests/um/snapshot-load-bench/`:

  * Boot master, take snapshot, save to file.
  * Boot a fresh UML with `kvm_v2_snapshot_load=<file>`.
  * Measure boot-to-first-syscall latency (host-side
    `clock_gettime` deltas).
  * **Gate**: p50 ≤ 50 ms, p99 ≤ 200 ms.  (More generous than
    Firecracker's 3.2 ms because UML pays Linux's `start_kernel`
    cold-init cost we can't easily skip.)

If the gate passes: snapshot+exec is viable; Path A's rt_sigreturn
work is not strictly needed.  Hospital-ready pool latency is
within reach via this path.

If the gate fails (e.g. snapshot doesn't capture enough state,
or `start_kernel` cold-init dominates): document why and pivot
back to Path A → 1.3 from the comparison memo.

### 2.4 Failure modes + what they tell us

  * **Snapshot doesn't capture timer / device state**: the
    restored kernel has a stale clocksource, drifts, or
    crashes on first timer tick.  Fixable by extending the
    UML-private PT_NOTE schema; bounded engineering.
  * **memslot copy is too slow** (PT_LOAD writes take > 200 ms
    for 256 MiB): need MAP_PRIVATE on a real file like Firecracker
    rather than copy bytes.  Substantial pivot.
  * **`start_kernel` cold-init dominates** (>500 ms before
    late_initcall_sync runs): we can't beat cold-boot this way
    without skipping more init.  Substantial pivot.

### 2.5 Cost + time

~2-3 days.  ~250 LoC reader, ~50 LoC Kconfig+cmdline, ~50 LoC
restore-on-late-initcall, ~100 LoC selftest, ~100 LoC daemon
plumbing.

### 2.6 Where to land the code

  * `arch/um/backend/kvm-v2/snapshot_elf_load.c` (new TU)
  * `arch/um/backend/kvm-v2/Kconfig` (new option)
  * `arch/um/backend/kvm-v2/Makefile` (add to obj-y under the
    new config)
  * `tools/testing/selftests/um/snapshot-load-bench/` (new
    selftest)
  * `tools/uml/uml-launcher/src/bin/umlctl/pool_serve.rs`
    (extend daemon)

---

## 3. Path C — Pre-fork pool with no rt_sigreturn — observe what
   actually breaks today

### 3.1 The hypothesis

The v1 ceiling was last characterised in
`09-fork-server-EXTERNAL-RESEARCH.md` (2026-05-20) — BEFORE the
2026-05-21 fixes landed (SMP-T78 migrate_disable release around
handle_syscall, SMP-T79 strict pick_vcpu, SMP-T80 is_user flag
on KVM_RUN-interrupted ticks).  Specifically: the 05-21 fixes
changed signal-handling semantics in ways the prior research
didn't analyse.

**Maybe** the original Memo 09 §2 design (pre-fork pool with
SIGSTOPped children, no rt_sigreturn) just works on the current
HEAD because something we changed since 2026-05-19 already
addressed the v1 ceiling.  Maybe not.

This is a 1-day experiment to find out.

### 3.2 Concrete shape

  1. Revert `d057cf28f482` (the private-stack clone fix that
     SIGKILLed children) — keep everything else from 2026-05-21.
  2. Rewrite `fork_on_resume_loop` to match Memo 09 §2:
     ```c
     /* Pre-fork N children at startup, each lands in SIGSTOPped state. */
     for (i = 0; i < N; i++) {
         pid = syscall(__NR_fork);
         if (pid == 0) {
             /* child: sit forever in SIGSTOP-wait loop */
             um_skas_respawn_all_stubs();
             um_template_identity_apply(&blob);
             /* Now run the guest. */
             return 0;  /* return up through um_template_pause_enter */
         }
         /* parent: register child pid + put it in SIGSTOP */
         kill(pid, SIGSTOP);
     }
     /* parent: park forever in a blocking host read on a
      * supervisor-fd, as AFL's snapshot.c does. */
     for (;;) os_blocking_read(...);
     ```
  3. Boot under `template-pause-fork-smoke` and ONE manual SIGCONT
     of a child.  Observe.

### 3.3 Acceptance

Three possible outcomes:

  **Outcome 1 — child boots cleanly** (best case): the v1 ceiling
  was actually fixed by one of the 05-21 commits (probably the
  SMP-T80 is_user flag work).  We can ship 1.3 from the
  comparison memo WITHOUT rt_sigreturn.  Path A becomes
  optional; Path B becomes a backup plan.

  **Outcome 2 — child panics in the SAME way Phase 2a panicked**
  (panic at `um_template_pause_enter+0xf0`): confirms the v1
  ceiling is alive; rt_sigreturn (Path A) is the right next
  step.  Pivot to A.

  **Outcome 3 — child fails in a NEW way**: extract the state-
  trace ring dump + dmesg + IP at crash, write a state-audit
  memo characterising the new failure.  Then decide A vs B.

### 3.4 Failure modes + observability

The state-trace ring is exactly the tool for this kind of
investigation (see
`02-workstreams/D-kvm-backend/state-audit/05-toolkit.md`).
Enable it via `echo 1 > /sys/kernel/debug/um_kvm_v2_trace/enabled`
before the SIGCONT; dump on first panic.

### 3.5 Cost + time

~1 day total: revert `d057cf28f482`, write the modified
`fork_on_resume_loop`, build, run, characterise.

### 3.6 Where to land the code

Branch off `umlctl-deploy` (not pushed) for the experiment.
**Do not merge** — this is purely diagnostic.  Capture the
findings in a state-audit memo regardless of outcome.

If Outcome 1: the experiment branch becomes the basis for the
real implementation; rebase + merge.
If Outcome 2 or 3: experiment branch is the diagnostic record;
keep for archaeology, merge the audit memo only.

---

## 4. Recommended sequencing

### 4.1 Run Path A first

**~2 hours**, cheapest, most dispositive.  Outputs:

  * **A passes**: we know rt_sigreturn is viable.  Run Path C as
    a sanity check; if C's Outcome 1 also passes, we don't even
    need A integrated.  If C's Outcome 2 or 3, A's primitive is
    ready to integrate.  Total budget through this gate: < 1
    day, full clarity.

  * **A fails**: pivot directly to Path B.  Save the days that
    Path C would have spent setting up a fork-pool path we'd be
    abandoning anyway.

### 4.2 Run Path C second (if A passes)

**~1 day**, dispositive on whether the 05-21 work already
addressed the v1 ceiling.

### 4.3 Run Path B as fallback (if A fails)

**~2-3 days**, fall-back architecture if rt_sigreturn isn't
viable.  Snapshot+exec gives us a known-good escape path
regardless of fork hazards.

### 4.4 Decision tree

```
Path A (rt_sigreturn isolation test)
    ├── PASS → Path C (pre-fork pool, no rt_sigreturn)
    │           ├── Outcome 1 (child boots) → ship 1.3 directly, skip rt_sigreturn integration
    │           ├── Outcome 2 (Phase 2a panic) → integrate A into 1.3 build-out (4-7 days)
    │           └── Outcome 3 (new failure) → audit memo, then A integration OR Path B
    │
    └── FAIL → Path B (snapshot+exec via #181 reader)
                ├── PASS gate → ship snapshot-load model (~2-3 days)
                └── FAIL gate → escalate; the latency story for
                                hospital-ready pools is harder
                                than any current approach
                                accommodates.  Decide between
                                accepting Phase 1a/1b cold-boot
                                latency (207 ms) or doing the
                                AFaaS-style multi-week tree work.
```

### 4.5 What this gates

  * #4 (syzkaller shim) — currently blocked on #19 (pre-fork
    pool).  Whichever of A → 1.3 or B lands first unblocks it.
  * #7 (Series 7 send) — orthogonal.  The pool-architecture work
    is a *follow-on* to Series 7, not a prerequisite.  Series 7
    ships with the kvm-v2 backend; long-lived pool members are
    a syzkaller-integration story.
  * The hospital-scenario question — closed only after one of A
    or B paths produces a working long-lived pool member with
    end-to-end TAP + mconsole verification.

---

## 5. Mandatory invariants for all three paths

Regardless of which path runs:

  * **No regressions to existing gates**: cpython-parity 21/21,
    mt-mmap-stress 10/10 at ncpus=4, kvm-record-smoke 8/8,
    kvm_v2_snapshot KUnit 4/4, kvm_v2_record KUnit 8/8, bench-py
    ≤ 0.30 ratio (kvm-v2 ≥ 3.3× faster than seccomp).
  * **No new BUG: UNVERIFIED commits** (the failure pattern from
    earlier in this session).  Each commit lands with measured
    evidence in the commit message OR is explicitly marked as
    a diagnostic-only experiment branch (Path C).
  * **No "deferred to next session" language** for anything
    flagged in the hardening plan.  If we can't fix it tonight,
    say so in a state-audit memo with a falsifiable test that
    proves the gap; don't paper over.

---

## 6. Cross-references

  * `post-2026-05-21-pool-architecture-comparison.md` — the
    comparison memo this plan operationalises.
  * `post-2026-05-21-hardening-plan.md` — the grading rubric.
  * `09-fork-server-EXTERNAL-RESEARCH.md` — 746-line prior
    survey including the CRIU rt_sigreturn pattern at §1.1.
  * `09-fork-server-PRIOR-ART.md` — in-tree AFL precondition
    table at §1.1.
  * `arch/um/backend/kvm-v2/snapshot_elf.c` — the writer
    Path B builds on.
  * `arch/um/kernel/template_pause.c` — current Phase 2a
    fork_on_resume_loop, the code Path C modifies.
  * `arch/um/kernel/snapshot.c` lines 366-396 — the original
    documentation of the v1 ceiling that Phase 2a hit.

---

## 7. What "done" looks like

The closure criterion for the pool-architecture-pivot work:

  1. Path A or Path B (or both) has landed in tree with a
     selftest that PASSes.
  2. End-to-end pool-exec-smoke produces **Case A**
     (`ok=true` with real output from the guest) rather than
     Case B (the current fallback envelope).
  3. End-to-end pool-bench memory-amplification gate produces a
     real measurement: N live pool members with measurable RSS,
     gate verdict reflects actual N-fork amplification not
     "0 live, gate cannot measure."
  4. State-audit memo documenting which path won, the latency
     numbers, the failure modes encountered, and the remaining
     follow-ups.

When 1-4 hold, #4 (syzkaller shim) unblocks; STATUS.md gets a
"pool members are long-lived" line; Series 7 cover letter
mentions UML-as-syzkaller-target with concrete latency numbers.
