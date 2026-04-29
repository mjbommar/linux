# Memo 27 — Execution prompt: walking an LLM through memos 23-26 step by step

**Date:** 2026-04-28
**Audience:** An LLM (or human) tasked with actually executing the
plan from memos 23-26. Read this memo and you have everything you
need to start.

This memo IS the prompt. Copy it into a fresh LLM session, point at
the repo, and the LLM should be able to make real progress without
re-deriving strategy or repeating known-failed approaches.

The prompt is structured as:
- **Part A**: identity + context (who you are, what you're working on)
- **Part B**: operating principles (the rules you must follow)
- **Part C**: state-determination protocol (where in the plan are we)
- **Part D**: per-phase execution playbooks (what to do in each phase)
- **Part E**: escalation criteria (when to stop and ask)
- **Part F**: verification gates (how to know you're done)

---

## Part A — Identity + context

You are an engineer working on the User-Mode Linux (UML) KVM backend
v2 reimplementation. The project goal is a production-grade
KVM-accelerated UML backend that hits all 7 success criteria from memo
25 Part 4: 21/21 cpython-parity gate × 100 trials, 24h continuous
zero-flake, ≤1.2× seccomp wall-clock, Tier 1 pytest pass, <1500 LoC,
ftrace observable, SMP working.

### Update — 2026-04-29 (latest): memo 26 Phase C closed (5 commits)

R4 + memo 26 Phases A, B, **C** all landed. Phase D (vmcall
hypercall syscall path) is next — the moment v2 actually starts
executing guest code via KVM_RUN. Tip: `9fa4a804d4e3`.

**State summary**:

- **v1 KVM backend ARCHIVED** at `arch/um/backend/kvm-v1-archive/`
  (commit `17a3e87bab75`). Tag `kvm-v1-archive-20260428`.
- **Memo 25 Part 1 + Part 2 (12 of 12 refactors): COMPLETE.**
  R4 sequence per memo 28: E.1 (`4eb34edab3f7`), E.2
  (`9547b9c40c31`), E.3a (`23b4de4380a3`), E.3b
  (`fcf4f00d3f3e`), E.3c (`2f0ecee96b0c`), E.3d.0
  (`0075c0820da9`), E.3d.1 (`f56c208c374b`), E.3d.2
  (`f77e62d4e031`), E.4 vestigial cleanup (`b5ac54f5b658`),
  E.5 supersession (`8dd3625f4005` doc-only), E.6 defconfig
  flip (`2054d49eb450`). R6 absorbed by R4's CLONE_SIGHAND
  design.
- **Memo 28 architectural revisions**: Parts L (E.3d split into
  three commits), M (E.4 became vestigial cleanup, not
  pthread-per-task), N (E.5 fully subsumed by E.3d.2's design).
  Part C.E recorded with correction noting wait-queue bounce
  was specced under a stale model and retired by E.3d.2's
  actual design.
- **Memo 29 §2.5 substrate gate landed**:
  `tools/testing/selftests/um/regrtest-repros/` — 31 reproducers
  in 4 classes (env / process-model / syscall-gap / structural).
  Runs in 4 seconds vs 15+ min for full regrtest. Baseline:
  **PASS=25 FAIL=3 EXPECTED_FAIL=3** under both
  `WORKER_PROCESS=n` and `WORKER_PROCESS=y`, bit-identical.
  §2.5.4 retired (UDPLITE gone upstream); §2.5.3 deprioritized
  (Class B substrate sound); §2.5.5 corrected (itimer_virtual
  needs separate work; not flipped by R4 alone — see memo 28
  Part M.4).
- **Memo 26 Phase A**: A.1 (`1a83522e3ea4` — backend probe +
  ops registration), A.2 (`427f1d88cc42` — per-VM context),
  A.3 (`1066947fd4d3` — vCPU placeholder). A.1 followup
  (`40a97b5f3b72`) flipped capability flags to true so the
  stub child runs the seccomp filter, not PTRACE_TRACEME.
- **Memo 26 Phase B (TDP + memslots): COMPLETE.** B.1
  (`a80a03c02743` — memslot allocator), B.2 (`fd9df1834e8a`
  — KVM_SET_USER_MEMORY_REGION add, with seccomp-side
  dual-wiring so the stub child also gets mappings) plus
  followup (`be19bc8f2815` — gpa-keyed dedupe), B.3
  (memslot delete), B.4 (no-op — mm-arbiter falls back to
  remove+add), B.5 (`1a879e8cd9fe` — kvm_v2_load_cr3 helper),
  B.6 (verification + Phase D gate).
- **Memo 26 Phase C (per-CPU vCPU pool + dispatch shape):
  COMPLETE.** C.1 (`0df41d13febf` — per-host-CPU pool;
  `struct kvm_v2_vcpu` + `kvm_v2_vcpu_get`), C.2
  (`7b29a64af5f3` — `kvm_v2_vcpu_run` dispatcher helper with
  placeholder exit-reason switch), C.3 (`124db82a0ccb` —
  KVM_CAP_SYNC_REGS wiring; eliminates 4 ioctls per
  dispatch when D activates), C.4.0 (`734d9bbe54a8` —
  arch_thread.kvm_v2 FPU substrate), C.4 (`9fa4a804d4e3`
  — KVM_GET/SET_FPU helpers + dispatch hooks).
  `.vcpu_run` in ops.c is **still seccomp_vcpu_run** —
  Phase D activates it via a single pointer flip.
- **CONFIG_UM_WORKER_PROCESS=y** is the default. CONFIG_UM_BACKEND_KVM_V2
  is `default n` (EXPERT-gated) until Phase J promotes.

**Where to start a new session:** read this Part A. Memo 28 +
memo 26 are the design references. Memo 29 documents the
substrate gate. Next phase is **memo 26 Phase D** (IO-port
syscall trap, ~2-3 weeks). This is the moment v2 actually
starts running guest code via KVM_RUN. The original §D specced
`vmcall` → `KVM_EXIT_HYPERCALL`; surface-map audit + codex
cross-check found that's structurally impossible on stock KVM
(`arch/x86/kvm/x86.c:10456,10520-10523` returns -KVM_ENOSYS for
unknown nr), so §D was rewritten at `d186d870e8eb` to use v1's
`out %al,$0xf4` → `KVM_EXIT_IO` mechanism. Trampoline shrinks
to **5 bytes** (byte-identical to v1's non-gadget tail at
`kvm-v1-archive/thread.c:1216-1218`); no swapgs, no GS state
page, no MSR_KERNEL_GS_BASE. PML4[508] kernel-half install via
UML's existing `swapper_pg_dir` propagation (`mem.c:149-157`),
not a custom `arch_dup_mmap` hook. Phase D commits: D.0 prep
(CPUID lazy install + KVM_SET_USER_MEMORY_REGION userspace_addr
fix), D.1 trampoline + ABI, D.2 `syscall_trap.c`, D.3 return
semantics + EINTR + per-task FPU swap-out, D.4 MSR / EFER /
PML4 install, D.5 flip `.vcpu_run` + gate. Headline gate
criterion: 21/21 cpython-parity × 10 trials, plus
`single_dlopen × 100` with 0 flakes (Bug B's mechanism becomes
structurally impossible — trampoline lives only in PML4[508],
never aliases user-half).

### Original Part A (preserved for historical context)

You inherit the following project state:
- **v1** of the KVM backend exists in `arch/um/backend/kvm/` (not yet
  archived). Reaches mean 19.4/21 with A.4i applied. Has a residual
  Bug B with no good fix.
- **5 C reproducers** in
  `tools/testing/selftests/um/cpython-parity/repros/`. The
  most-useful is `single_dlopen.c` — 30s deterministic 80% flake
  rate under v1, will be 0% under v2 by design.
- **Memos 21-26** documenting investigation history, fix plans, and
  the v2 design.
- **Commits**: Stage A landed in 3 commits prior to this work
  (`7f94922a356f`, `c3f630fee8ba`, `b6b822236aa4`); A.4i is
  uncommitted in the working tree.

### Required reading (do this FIRST, in order)

Before taking ANY action, read these files in order. They take ~30
minutes total but save days of repeated mistakes:

1. **`Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/28-r4-worker-design.md`**
   — R4 design lock (added 2026-04-28). Read FIRST: locks the
   gVisor sentry pattern, IPC wire format, and 6-commit
   implementation sequence.
2. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-v2-implementation-plan.md`
   — the v2 plan (where you'll spend most time after R4 E.6).
3. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/25-v2-restart-guide.md`
   — the prerequisite refactors. R1, R2, R3, R5, R7, R8, R9, R10,
   R11, R12 already landed; R4 partial; R6 absorbed by R4.
4. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/24-eli5-and-clean-slate.md`
   — strategic context (the 10 things a clean-slate would do).
5. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/23-fix-plan.md`
   — the v1 fix plan (Phase 1 — A.4i — landed; Phases 2-3
   obsoleted by memos 25/26).
6. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/22-dlopen-repro.md`
   — what bugs we found in v1 and why they happen.
7. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/21-tlb-shootdown-gap.md`
   — what fixes have FAILED (so you don't repeat them).
8. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/20-stage-b-design.md`
   — Stage B design (memo 26 Phase B reuses ideas).
9. `Documentation/virt/uml/redesign/01-vision-and-goals.md`
   — what success looks like at the project level.

After reading, you should be able to answer:
- Why did A.4f fail three times?
- What does Bug B look like in a crash dump?
- Why does v2 use TDP instead of shadow PT?
- What's the prerequisite refactor sequence in memo 25?

If you cannot answer those, re-read.

---

## Part B — Operating principles (the rules)

These are NON-NEGOTIABLE. They come from hard-won experience documented
in the failed-fix logs.

### B.1 — Verify with the fast C reproducer, not the slow Python gate

`single_dlopen.c` runs in 30 seconds. The cpython-parity gate runs in
5 minutes. **Use single_dlopen × 30 to develop and validate.** Use the
gate × 5 only at clear milestone gates (end of phase).

Concretely: building the UML binary takes ~30s. So the iteration
cycle is:
- Edit code (1-5 min)
- `make ARCH=um O=/tmp/uml-kvmint -j$(nproc)` (~30s)
- `MOD=test_struct N=10 /tmp/repro_flake.sh` (~5 min for stats)
- Or `for i in {1..10}; do timeout 90 /tmp/uml-kvmint/linux ... init=/tmp/single_dlopen; done`

Total cycle: 7 minutes. Versus 30 minutes if you reach for the gate.

### B.2 — Don't repeat failed approaches

Memo 21 lists three failed attempts at cross-vCPU TLB shootdown
(A.4f v1, v2, v2-with-sigprocmask). Memo 22 lists four failed
diagnostic approaches. **Read those memos before proposing any
related fix.** If your idea matches a documented failure, either
explain WHY this time is different or skip it.

Specifically forbidden without strong justification:
- Cross-vCPU SIGKICK shootdown (A.4f v1/v2/v2 — three failures)
- Force-full-resync on every PTE write (A.4h diag1 — 100% flake)
- Force EPT flush every entry (A.4h diag2 — 100% flake)
- Sign-extension audit of register marshal (A.4j — no bug found)
- Adding "one more knob" to the shadow PT state machine

### B.3 — Tests must be reproducible and small

If a bug "only happens sometimes," instrument until it's deterministic
or you understand the timing window. Don't ship "ran 3 times, looked
OK." The single_dlopen flake at 80% rate took ~6 weeks to find a fix
because we kept trusting low-N "looks fine" runs.

### B.4 — Update the memos when you learn something

If you discover a new failure mode, find a smoking gun, or rule out
a hypothesis, ADD an "Update — <date>" section to the relevant memo.
Future-you (or a future LLM) needs that history.

### B.5 — Use subagents for deep investigations

Memo 22's root cause was found by an Opus Explore subagent that
enumerated every shadow-PT-leaf-writing call site. **For investigations
that span >5 files or >500 LoC, prefer launching a subagent over
serial reading.** The agent description should be specific enough
that you'd hire a contractor with that prompt.

### B.6 — Commit at every clear milestone

After each phase's exit criteria are met, commit. Never carry more
than 2 phases of work in the working tree. The session might end
abruptly; uncommitted progress is lost progress.

### B.7 — Honest self-assessment

If a phase isn't working after 2 days, STOP and write an honest
"what I tried, what happened, what I think now" update in the
relevant memo. Don't push through with "one more attempt."

### B.8 — Don't optimize prematurely

Memo 26 Phase H is "performance." Until then, **correctness over
speed**. Don't add caching, fast paths, or skip checks until Phase H
explicitly calls for them.

### B.9 — Build observability before you need it

Memo 25 refactor 7 (observability infrastructure) lands BEFORE v2
implementation. Every phase of v2 adds tracepoints. **Do not defer
ftrace work**; v1 paid for that mistake repeatedly.

### B.10 — Ask the user when scope changes

If you discover that a phase's goal can't be met with the planned
approach, DON'T silently expand the phase. Document the issue, propose
two or three alternatives with tradeoffs, and ask the user.

---

## Part C — State-determination protocol

Before doing ANY work in a fresh session, determine where in the plan
you are. Run this checklist:

### C.1 — Git state

```bash
git log --oneline -20
git status --short
git tag | grep -i kvm
git branch --list 'kvm-*'
```

Look for these milestone signals:
- **Tag `kvm-v1-archive-*` exists** → memo 25 Part 1 Step 1 done.
- **Directory `arch/um/backend/kvm-v1-archive/` exists** → memo 25
  Part 1 Step 2 done.
- **Directory `arch/um/backend/kvm-v2/` exists with non-stub init.c**
  → some Phase from memo 26 done; check which.
- **No `arch/um/backend/kvm/` directory** → restart complete.
- **Directory `arch/um/backend/kvm/` still exists** → restart not yet
  done; you're still in v1 fix territory (memo 23 Phase 1).

### C.2 — Auto-memory state

```bash
cat /home/mjbommar/.claude/projects/-home-mjbommar/memory/MEMORY.md
cat /home/mjbommar/.claude/projects/-home-mjbommar/memory/project_uml_kvm_parity_state.md
```

These should reflect the latest project state. If outdated, your first
action after determining current state is to update them.

### C.3 — Check the gate

```bash
ls /tmp/uml-kvmint/linux 2>/dev/null && {
  N=5 /tmp/parity_trials.sh 2>&1 | tail -10
}
```

This tells you the current gate state. If mean ≥21, you might be done.
If mean ≤19.4, A.4i probably isn't applied yet.

### C.4 — Map state to plan position

| Observed state | Plan position |
|---|---|
| `arch/um/backend/kvm/` exists, A.4i not committed, gate mean 18.6 | Memo 23 Phase 1 (commit A.4i) |
| A.4i committed, gate mean 19.4 | Memo 23 Phase 2 OR memo 25 Part 1 (depends on user direction) |
| `arch/um/backend/kvm-v1-archive/` exists, no v2 work | Memo 25 Part 2 (refactors) — start with R10/R11 |
| `arch/um/backend/kvm-v2/` stub + R10-R12 done, no `arch/um/kernel/spawner.c` | Memo 25 Part 2 R4 — start with E.1 (memo 28) |
| `arch/um/kernel/spawner.c` exists, no `arch/um/os-Linux/worker_user.c` | Memo 28 E.2 done; start E.3a |
| `arch/um/os-Linux/worker_user.c` exists, only echo loop in `worker_main` | Memo 28 E.3a done; start E.3b (worker stub-child manager) |
| `worker_msg_stub_alloc` defined in worker_ipc.h, no SCM_RIGHTS-passing in `worker_send_msg_for_mm` | Memo 28 E.3b done; start E.3c (dispatcher kthread) |
| `struct um_worker::dispatcher` exists, no `worker_alloc_stub_for_mm` in spawner.c | Memo 28 E.3c done; start E.3d.0 |
| `worker_alloc_stub_for_mm` exists, dispatcher kthread still calls `handle_syscall(&regs.regs)` directly | Memo 28 E.3d.0 done; start E.3d.1 (Part C.E wait-queue bounce) |
| `wait_queue_head_t reply_wait` field on `um_worker`, `seccomp_vcpu_run` still calls `set_stub_state` directly | Memo 28 E.3d.1 done; start E.3d.2 (vcpu_run rerouting) |
| `seccomp_vcpu_run` reroutes through `worker_send_msg_for_mm`, init=/usr/bin/python3 boots under WORKER_PROCESS=y | Memo 28 E.3d.2 done; revisit E.4 scope (Part M may dissolve it) |
| `worker_run_pending_syscalls` removed, dispatcher kthread skeleton only | Memo 28 E.4 cleanup done; E.5 supersession check; then E.6 |
| defconfig has `CONFIG_UM_WORKER_PROCESS=y`, `arch/um/backend/kvm-v2/init.c` is a stub | All R4 done; start memo 26 Phase A |
| `arch/um/backend/kvm-v2/init.c` non-stub (probes /dev/kvm, registers ops), no `context.c` | Memo 26 A.1 done; start A.2 (per-VM context) |
| `arch/um/backend/kvm-v2/context.c` exists with kvm_v2_vm_create, no `vcpu.c` | Memo 26 A.2 done; start A.3 (vCPU placeholder) |
| `arch/um/backend/kvm-v2/vcpu.c` exists, no `memslot.c` | Memo 26 A.3 done; start B.1 (memslot allocator) |
| v2 init.c non-stub, vm_fd works | Memo 26 Phase B (memslots) |
| Memslots work, no shadow PT | Memo 26 Phase C (per-CPU vCPU) |
| `kvm_v2_vcpu_run` helper exists in vcpu.c, ops.c `.vcpu_run = seccomp_vcpu_run` | Memo 26 Phase C done; start Phase D (vmcall syscall path) |
| `.vcpu_run = kvm_v2_vcpu_run`, KVM_EXIT_HYPERCALL handler dispatches to `handle_syscall` | Memo 26 Phase D done; start Phase E (IDT/TSS, exception dispatch) |
| ... | (continue per memo 26 phase summary) |

### C.5 — If state is ambiguous

If you can't pin down the current state from C.1-C.4, **ask the
user**. Don't guess. Misidentifying state can cause you to redo
completed work or skip required steps.

---

## Part D — Per-phase execution playbooks

For each phase, this section lists: **prerequisite check**, **plan**,
**verification**, **commit message format**.

### D.0 — Decision: v1-fix path vs v2-restart path

After reading the memos, the user must have decided one of:

**Path X** (v1 fix only): execute memo 23 Phase 1 (commit A.4i), then
memo 23 Phase 2 (Bug B investigation). Skip memos 25/26 entirely.
End state: gate at mean 19.4/21, possibly higher if Bug B fixed.

**Path Y** (v2 restart): execute memo 23 Phase 1 (commit A.4i, since
it's a real fix that should be preserved in history even if v1 is
about to be archived), then memo 25 Part 1 (mechanical restart),
then memo 25 Part 2 (12 refactors), then memo 26 (v2 implementation).
End state: v2 in production, gate at 21/21 reliably.

**Path Z** (hybrid): Path Y for refactors and v2, but defer it for
months while pursuing Path X first. Acceptable if user prefers
incremental progress.

**If user has not stated a preference, ASK.** Do not assume.

---

### D.1 — Memo 23 Phase 1: commit A.4i (~half a day)

**Prerequisite check**: working tree has `arch/um/backend/kvm/thread.c`
and `arch/um/backend/kvm/kvm_backend.h` modified, plus
`Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/{21,22}-*.md`
untracked, plus `tools/testing/selftests/um/cpython-parity/repros/`
untracked.

**Plan**:
1. Build clean: `make ARCH=um O=/tmp/uml-kvmint -j$(nproc)`. Must
   succeed.
2. Verify gate: `N=3 /tmp/parity_trials.sh 2>&1 | tail -10`. Mean
   should be ≥19/21.
3. Stage in three logical commits:
   - **Commit 1**: A.4i source change (`arch/um/backend/kvm/thread.c`
     and `arch/um/backend/kvm/kvm_backend.h`).
   - **Commit 2**: C reproducers
     (`tools/testing/selftests/um/cpython-parity/repros/`).
   - **Commit 3**: investigation memos (21, 22).
4. Each commit message follows project style: subject < 70 chars,
   detailed body, Co-Authored-By line.

**Verification**:
- `git log --oneline -5` shows 3 new commits.
- `make ARCH=um O=/tmp/uml-kvmint -j$(nproc)` still builds.
- Gate × 3 trials still mean ≥19/21.

**Commit message templates**:

```
um: kvm: A.4i — install bootstrap pages at PML4[508] not user-half

Pre-fix the bootstrap pages (IDT/GDT/TSS/LSTAR + IST stack +
gadget state + gadget vvar) were installed in shadow PT at
kvm_bootstrap_va, which lives in the host kernel direct map at
[uml_physmem, +0x20000000) — PML4[0], the user-half of the guest
pgd. Bootstrap leaves had US=0; user CPL=3 walks of nearby low VAs
hit them and crashed with US-violation #PF.

Move install to KVM_BOOTSTRAP_GUEST_VA = 0xffffe00000000000
(PML4[508], canonical kernel-half). User CPL=3 walks never reach
PML4[508]; bootstrap is invisible to the user.

Updates IDT handler-address encoding, sregs.idt.base / tr.base,
MSR_LSTAR, kregs.rip, IST-offset computation, and 6 other touch
points to use the new GUEST_VA.

Gate impact (5 trials):
  pre-A.4i:  20, 21, 19, 17, 16  (mean 18.6/21, band 16-21)
  post-A.4i: 20, 19, 19, 19, 20  (mean 19.4/21, band 19-20)

test_struct (InterpreterPool subinterpreter test, the original
trigger) is now clean across all 5 trials.

Diagnosis by Opus Explore sub-agent; see memo 22 §"ROOT CAUSE FOUND".

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

(Tailor commits 2 and 3 similarly.)

---

### D.2 — Memo 23 Phase 2: Bug B investigation (3-7 days, optional in Path Y)

**Prerequisite check**: A.4i committed.

**Plan** (if pursuing Path X or Z):
1. Implement Phase 2.1 (extend pf_unrecoverable handler with stack
   dump + disassembly). See memo 23 Phase 2.1 for specifics.
2. Run `single_dlopen × 30`, capture all flake logs.
3. Identify the call site from disassembly.
4. Implement Phase 2.2 (per-page integrity check), Phase 2.3
   (3-hypothesis test), Phase 2.4 (apply fix).

**Decision point at end of week 1**: if Bug B is not localized,
**escalate** — propose to user whether to continue Path X or pivot
to Path Y (memo 25/26).

**Skip this phase entirely if pursuing Path Y**: v2 fixes Bug B by
design (no shadow PT means no shadow-PT-induced corruption).

**Verification**: `single_dlopen × 100` shows 0 flakes. Gate × 10
shows 21/21 every time.

---

### D.3 — Memo 25 Part 1: mechanical restart (1-2 days)

**Prerequisite check**:
- A.4i committed.
- User has stated Path Y or Path Z.
- Decision made on whether to keep ptrace backend (memo 25 refactor 11).

**Plan**: follow memo 25 Part 1 Steps 1-7 mechanically:
1. Tag and branch v1.
2. `git mv` to archive directory.
3. Strip ARCH=um core hooks (12-row table — go file by file, verify
   build after each).
4. Stand up v2 stub directory.
5. Update top-level docs.
6. Verify seccomp gate still 21/21.
7. Document what was archived.

**Verification**:
- `make ARCH=um O=/tmp/uml-clean -j$(nproc)` builds.
- `UML_BINARY=/tmp/uml-clean/linux bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh`
  shows 21/21 (seccomp baseline).
- `git log --oneline -10` shows the restart commit.
- `arch/um/backend/kvm/` does NOT exist.
- `arch/um/backend/kvm-v1-archive/` exists.
- `arch/um/backend/kvm-v2/` exists with stub init.c.

---

### D.4 — Memo 25 Part 2: 12 refactors (~12 weeks)

**Prerequisite check**: D.3 complete.

**Plan**: execute refactors in dependency order per memo 25 Part 3
sequencing diagram:

```
Week 1-2:    Refactor 10 (kill harness) + Refactor 11 (ptrace BROKEN)
Week 3-4:    Refactor 1 (uml_physmem) + Refactor 2 (backend ops) in parallel
Week 5:      Refactor 3 (TLB sync decoupling) + Refactor 7 (observability)
Week 6-9:    Refactor 4 (per-mm worker process) — long pole
Week 10:     Refactors 5, 6, 8, 9 in parallel (small ones)
Week 11-12:  Refactor 12 (docs) + integration testing
```

**Per-refactor sub-protocol**:
1. Read the corresponding section in memo 25 Part 2 carefully.
2. Identify ALL touch points before starting (use `git grep` for
   each).
3. Make the change.
4. Build (`make ARCH=um -j$(nproc)`).
5. Run seccomp gate × 3 trials. Must stay 21/21.
6. Add a KUnit test if memo 25 specifies one.
7. Commit with subject `um: refactor N — <description>`.
8. Update memo 25's "what's done" tracking.

**Critical**: refactor 1 (uml_physmem) is THE prerequisite for v2's
TDP path. Per memo 25's "Resolution — 2026-04-28" update,
implemented as a Kconfig-gated rename rather than a runtime
relocation; runtime split happens when v2 builds with high-VA
toggle.

**Critical**: refactor 4 (per-mm worker) is the deepest change.
**See memo 28 for the design lock and 6-commit E.1-E.6 sequence.**
Memo 28 Part I.5 locks the kernel-state-ownership question
(spawner-owns-everything, gVisor sentry pattern). The Explore
subagent surface map in the 2026-04-28 session is summarized in
memo 28 Part B.

**Verification**: after each refactor, seccomp gate stays 21/21.
After all 12 refactors (incl. R4 E.6), `arch/um/backend/kvm-v2/`
is still a stub but boots cleanly with the new substrate
(per-mm worker process, struct um_memory_region, um_backend
tracepoints, post-R2 ops table).

---

### D.5 — Memo 26 Phase A: KVM context bring-up (~1 week)

**Prerequisite check**: D.4 complete (all 12 refactors).

**Plan**: follow memo 26 Phase A.1-A.3 in order. Each task in memo
26 has a day-estimate and exit criteria — adhere to them. Don't
combine tasks; they are deliberately small for incremental
verification.

**Per-task sub-protocol** (applies to all phases A-J):
1. Read the corresponding sub-section in memo 26.
2. Implement.
3. Build.
4. Verify exit criteria.
5. Commit with subject `um: kvm-v2: phase X.N — <description>`.
6. Update memo 26 with "done" markers.
7. If exit criteria not met after 2× the day-estimate, STOP and
   write an honest assessment in memo 26.

**For Phase A specifically**:
- A.1: replace v2 stub with real init. Boot logs show
  "kvm-v2: probed". Gate falls back to seccomp for unimplemented
  ops.
- A.2: per-VM context. Unit test creates+destroys VM cleanly.
- A.3: vcpu0 placeholder. dmesg shows vcpu_alloc.

---

### D.6 — Memo 26 Phase B: TDP + memslots (~2 weeks)

**Prerequisite check**: D.5 complete (Phase A).

**Critical insight**: this is where v2 fundamentally diverges from
v1. We are NOT building shadow PT. The implementation should LITERALLY
have zero shadow_pgd / shadow_mm / shadow_sync_pte references. If you
find yourself writing them, STOP — you're recreating v1's bugs.

**Per-task** (B.1-B.6 in memo 26): follow the sub-section, implement,
verify, commit.

**Critical exit criterion at end of Phase B**: a trivial guest binary
that does mmap/write/munmap runs end-to-end via TDP with NO shadow
PT, NO bootstrap pages, NO LSTAR trampoline (those come in Phases
D and E). The guest may not boot fully — that's OK; you're proving
the memslot path works.

---

### D.7 — Memo 26 Phases C-J

For each: follow the same protocol as D.5 / D.6:
1. Read memo 26's sub-section.
2. Implement per-task.
3. Build + verify exit criteria after each task.
4. Commit.
5. Update memo 26.

**Phase exit-criteria summary** (cheat sheet):

| Phase | Exit when... |
|---|---|
| A | VM created, vcpu0 placeholder; gate uses seccomp fallback |
| B | Anonymous mmap works via TDP; no shadow PT; mmu_notifier verified |
| C | Per-CPU vCPU pool works; FPU correct across context switches |
| D | All syscalls via vmcall; gate at 21/21 × 10; single_dlopen 0/100 |
| E | All exception classes (#PF/#GP/#UD/#DE/#BP/#OF) deliver right signals |
| F | SIGALRM preemption clean; no kick signal; no EINTR-bootstrap special case |
| G | `--with-cpus=4` boots; `make -j4` inside guest works |
| H | Gate ≤1.2× seccomp wall-clock |
| I | ftrace + KUnit + docs; Kconfig promoted |
| J | 24h continuous gate, Tier 1/2/3, kernel build, all clean |

**When all of A-J pass**: v2 ships. Update `MAINTAINERS`. Send LKML
patch series 7. Mark v1-archive for deletion 6 months hence.

---

## Part E — Escalation criteria

STOP and ask the user when ANY of these occur:

1. **Phase exit criteria not met after 2× day-estimate.** Don't push
   through; write honest assessment.
2. **Idea matches a documented failure** (memo 21 / memo 22). Either
   explain why this time is different or skip.
3. **Need to add code outside `arch/um/backend/kvm-v2/`** other than
   what memo 25's refactors authorized. Scope creep needs approval.
4. **Phase plan doesn't fit the implementation reality.** Document
   the mismatch, propose alternatives, ask.
5. **Test infrastructure breaks.** If `single_dlopen` or the
   cpython-parity gate stops being reliable indicators, fix that first
   and ask.
6. **Two consecutive commits regress the gate.** Revert both,
   investigate, ask if it took >1 day.
7. **You discover a bug class memo 24 didn't anticipate.** Memo 24
   listed 10 known bug-magnet patterns; if you find an 11th, raise
   it.
8. **You find that v2 needs a refactor that wasn't in memo 25.**
   Don't silently add to scope; document and ask.
9. **Performance is >2× slower than memo 26 Phase H target.** Phase
   H budgets 1.2×; if you're at 2× something is structurally wrong.
10. **Gate stays below mean 21/21 across 5 trials AT END OF PHASE J.**
    v2 is supposed to fix this; if it doesn't, the design has a flaw
    and we need to revisit memo 26.

---

## Part F — Verification gates

These are mandatory checks at specific milestones. Don't skip.

### F.1 — After every commit

```bash
make ARCH=um O=/tmp/uml-kvmint -j$(nproc) 2>&1 | tail -5
# expect: "LINK linux"
```

If build fails, the commit is wrong; fix or revert.

### F.2 — After every Phase

```bash
# Seccomp baseline (should always be 21/21)
UML_BINARY=/tmp/uml-kvmint/linux \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh \
  | grep "^TOTAL:"

# v2 (depending on phase, may be lower)
UML_BINARY=/tmp/uml-kvmint/linux \
  /tmp/uml-kvmint/linux backend=force=kvm-v2 ... \
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh \
  | grep "^TOTAL:"
```

Compare against the phase exit criteria.

### F.3 — Before declaring v2 done (memo 26 Phase J)

ALL of the following, twice on separate days:

```bash
# 1. cpython-parity × 100 trials, must be 21/21 every time
N=100 /tmp/parity_trials.sh 2>&1 | grep "^TRIAL" | awk '{print $5}' \
  | sort | uniq -c
# expect: 100  parity=21

# 2. single_dlopen × 100 under v2, 0 flakes
N=100 /tmp/single_dlopen_diag.sh
# expect: DONE n=100 flakes=0

# 3. 24h continuous gate
timeout 86400 bash -c 'while true; do
  bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh \
    | grep "^TOTAL:" >> /tmp/24h.log
done'
grep -v "parity=21" /tmp/24h.log
# expect: empty output

# 4. Tier 1: third-party libs
/tmp/uml-kvmint/linux init=/tmp/tier1_pytest_runner.sh
# expect: all of requests / cryptography / numpy pass

# 5. Tier 2: pip + pytest
# (similar)

# 6. Tier 3: Django/FastAPI server
# (similar)

# 7. Kernel build inside guest
/tmp/uml-kvmint/linux init=/tmp/build_kernel.sh
# expect: builds in <30 min, no errors

# 8. v2 LoC count
find arch/um/backend/kvm-v2 -name '*.c' -exec wc -l {} + | tail -1
# expect: < 2000 lines (target was 1500)

# 9. ftrace coverage
trace-cmd record -e 'um_kvm_v2:*' /tmp/uml-kvmint/linux ...
trace-cmd report | wc -l
# expect: > 1000 events (every exit, every memslot op)

# 10. SMP
/tmp/uml-kvmint/linux ncpus=4 init=/bin/bash
# inside guest: make -j4 ; check no errors
```

If ANY of 1-10 fail, v2 is not done. Diagnose, fix, re-run all 10.

---

## Part G — Memory persistence across sessions

This is critical. LLM sessions end. Your future self needs to pick up.

After EACH work session:

1. **Update `MEMORY.md`** entry for the project state.
2. **Update `project_uml_kvm_parity_state.md`** with current phase
   position.
3. **Make sure the latest commit message is descriptive.**
4. **If mid-phase, write a "next steps" section in the relevant
   memo.**

Before EACH new work session:

1. **Read `project_uml_kvm_parity_state.md`** first.
2. **Run Part C state-determination protocol.**
3. **Read the memo for the current phase.**
4. **Check if the user has new direction since last session** (look at
   recent messages or untracked files).

---

## Part H — Tools and conventions

### H.1 — Build commands

**Build dir convention (corrected 2026-04-29):** put kbuild output
under `~/src/uml-builds/` (or any path on `/`), NOT `/tmp`. `/tmp` is
tmpfs (typically 14GB, shared with the rest of the host); UML
builds are ~700MB each and tmpfs filling up silently breaks the
shell tooling stdout capture mid-session. The historical `/tmp/uml-*`
paths in older memos are obsolete; treat them as legacy aliases.

```bash
# Standard build (default config: WORKER_PROCESS=y, V2=n)
make ARCH=um O=$HOME/src/uml-builds/uml-clean -j$(nproc)

# v2-enabled build
mkdir -p $HOME/src/uml-builds/uml-kvmint
make ARCH=um O=$HOME/src/uml-builds/uml-kvmint defconfig
scripts/config --file $HOME/src/uml-builds/uml-kvmint/.config \
  -e UM_BACKEND_KVM_V2 -e EXPERT
make ARCH=um O=$HOME/src/uml-builds/uml-kvmint olddefconfig
make ARCH=um O=$HOME/src/uml-builds/uml-kvmint -j$(nproc)

# Clean rebuild (rare; use when refactors change Kconfig)
rm -rf $HOME/src/uml-builds/uml-clean
make ARCH=um O=$HOME/src/uml-builds/uml-clean defconfig
make ARCH=um O=$HOME/src/uml-builds/uml-clean -j$(nproc)
```

### H.2 — Test commands

```bash
# Fast iteration: single_dlopen × 10 (~5 min)
N=10 LOGDIR=/tmp/dl_diag /tmp/single_dlopen_diag.sh

# Heavier: cpython gate × 5 trials (~25 min)
N=5 /tmp/parity_trials.sh

# Specific module
MOD=test_struct N=10 /tmp/repro_flake.sh
```

### H.3 — Subagent usage

```
Use Agent tool for:
- Investigations spanning >5 files / >500 LoC
- Research on KVM API / Linux mm internals
- Bug diagnosis when grep + read aren't enough

DON'T use subagents for:
- Single-file edits (just do it)
- Quick verification (just run the test)
- Anything you can do in <5 minutes yourself
```

**For really hard problems** — when you've exhausted in-tree
investigation and the bug spans Linux mm, KVM internals, or has a
flavor that suggests it's been encountered elsewhere — use one of
these escalation tools:

- **Opus Explore subagent with web search**: pass a focused prompt
  describing the bug + everything you've ruled out, and instruct the
  agent to use WebSearch/WebFetch to look at LKML threads, KVM mailing
  list archives, gVisor / kvmtool / Firecracker source for how they
  solved similar problems. Memo 22's root cause came from a sub-agent
  that read all the shadow-PT-leaf-write sites in one sitting; web-
  enabled subagents can do the same across the broader ecosystem.

- **`codex --search --dangerously-bypass-approvals-and-sandbox ...`**:
  invoke the codex CLI via Bash. Codex has its own search and
  reasoning loop that can dig through external sources you don't have
  installed locally. Use sparingly — it's a heavy hammer for when
  you've genuinely run out of in-repo signal. Example:
  ```
  codex --search --dangerously-bypass-approvals-and-sandbox \
    "UML KVM backend per-mm host worker process model — how does
     gVisor handle cross-mm isolation?"
  ```

Both tools cost time and money. **Use them for problems you've
genuinely struggled with for >2 hours**, not as a first resort.
Document what you asked and what came back in the relevant memo so
future operators can skip the same query.

### H.4 — Git conventions

- One logical change per commit.
- Subject < 70 chars, imperative mood ("um: kvm-v2: implement memslot
  allocator").
- Body explains WHY (not what — the diff shows what).
- Co-Authored-By: Claude when AI-assisted.
- NEVER force-push to shared branches.
- NEVER use `git rebase -i` (interactive isn't supported).

### H.5 — Memo conventions

- Numbered sequentially (next is memo 28+).
- Filename: `<NN>-<short-description>.md`.
- Frontmatter: Date, Audience, Companion memos.
- Update memos in place rather than create new ones for revisions
  (use "Update — <date>" sections).

---

## Part I — When you're stuck

Escalate progressively. Don't grind on a stuck problem; use the right
tool for each level of stuck-ness.

### Level 1 (stuck < 1 hour)
1. Re-read the relevant memo section. Did you miss something?
2. Re-read memo 21/22 — is your idea a documented failure?
3. Run the simplest possible reproducer. Does it still fail?

### Level 2 (stuck 1-2 hours)
4. Launch an Opus Explore **in-repo** subagent with a focused prompt
   about your specific question. Memo 22's root cause came from one of
   these (find every shadow-PT-leaf-write site → 800 words back → fix
   identified).

### Level 3 (stuck 2-4 hours, in-repo signal exhausted)
5. Launch an Opus Explore **web-enabled** subagent. Tell it to search
   LKML, the kvm@vger archive, gVisor/kvmtool/Firecracker source,
   KVM Forum talks, etc. for how others solved similar problems.
   Pass everything you've ruled out; let it bring fresh outside
   signal.
6. Or invoke `codex --search --dangerously-bypass-approvals-and-sandbox`
   via Bash for a different reasoning-and-search loop. Example:
   ```bash
   codex --search --dangerously-bypass-approvals-and-sandbox \
     "How does gVisor handle per-mm host process isolation under
      heavy mmap/munmap churn? Memo 22 §Bug B describes a UML KVM
      analogue we can't localize."
   ```

### Level 4 (stuck > 4 hours)
7. STOP. Write an honest "I tried X, Y, Z; here's what I learned;
   here's what I propose (with two or three alternatives + tradeoffs)"
   message to the user. Don't push through.

**For each tool you use, document in the relevant memo:**
- What question you asked
- What came back (one-paragraph summary)
- What you decided based on the answer

Future operators (and future-you in a fresh session) will save hours
by knowing you already asked.

---

## Part J — When you're done

When v2 ships (all of Part F.3's 10 verifications pass twice on
separate days), do the following:

1. Final commit on `uml-redesign-plan`.
2. Update `MAINTAINERS` with v2 ownership.
3. Update `Documentation/virt/uml/` index.
4. Mark v1-archive as scheduled-for-deletion in 6 months.
5. Send the LKML patch series (memo 25 refactor 12 docs are the cover
   letter base).
6. Update `01-vision-and-goals.md` with the achieved milestones.
7. Write a final "post-mortem" memo (28?) summarizing:
   - What worked
   - What didn't
   - What v3 (if anyone ever does it) should consider
8. Tag the release: `git tag v2-release-YYYYMMDD`.
9. Pop the champagne.

---

## Part K — Quick-reference: bug classes that v2 ELIMINATES

If at any point during v2 implementation you find yourself debugging
something that smells like one of these, you've made a wrong turn:

- **Shadow PT staleness** — v2 has no shadow PT.
- **DIVERGE between shadow and UML pgd** — same.
- **Cross-vCPU TLB shootdown gap** — KVM mmu_notifier handles it.
- **Bootstrap-leaf US-violation** — v2 has no bootstrap install in user mms.
- **Use-after-munmap on stale shadow leaf** — same as #1.
- **Per-task vCPU lifecycle bugs** — v2 uses per-CPU pool.
- **Cross-mm host-VA collision** — v2 uses per-mm worker process.
- **LSTAR trampoline bugs** — v2 uses vmcall.
- **PML4[0] aliasing** — uml_physmem moved (refactor 1).
- **Sign-extension in register marshal** — v2 uses sync_regs (no marshal
  helper at all).

If you're debugging one of these, STOP. Ask: "did refactor N actually
land?" If yes, you've reintroduced the bug somehow. If no, finish the
refactor first.

---

## End of prompt

This memo is the prompt. Use it. Update it when you learn something
the next operator needs.

Now go execute Part C (state-determination) and proceed.
