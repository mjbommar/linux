# 09 — Fork-server: HISTORICAL — what prior research already said

**Date:** 2026-05-20
**Author:** research session picking up `umlctl-deploy`
**Purpose:** Inventory and quote every prior round of research on the
UML fork-from-syscall-handler hazard. The previous in-tree session
declared the failure "genuinely beyond a single-session fix" and
proposed CRIU/freezer-cgroup/single-thread as if they were new ideas.
They are not — every one of those is recycled from prior memos.
Worse, the exact symptom (`um_template_pause_enter+0xf0` saved-RIP
corruption to small values) has a documented prior root cause and a
documented prior recommended fix the current session did not apply.

This document quotes the load-bearing prior research, points at the
fix, and recommends the next concrete step.

---

## 1. Inventory of prior research

Every memo, decision, and risk note in `Documentation/virt/uml/redesign/`
that touches fork/snapshot/template-pause, in roughly chronological
order of authorship:

### Design memos (in 02-workstreams/, 03-architecture-review/, 06-sequencing/)

| Path | Role |
|------|------|
| `Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md` | C-09 v1 design (landed 2026-04-20). The original AFL-style forkserver design + post-landing audit + shipped-commit table. 796 lines. |
| `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/12-snapshot-forkserver-kvm.md` | Memo 12 — KVM-aware snapshot. Explains why fork() under KVM is broken (memslot host-VA divergence, vCPU fd aliasing). 261 lines. |
| `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/26-snapshot-v2-port.md` | Memo 26 — porting the kvm-v1 snapshot.c to v2. 570 lines. This is the "alternative to fork" path. |
| `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-sprint/09-fork-server-snapshot-restore.md` | Memo 09 — keystone "template-pause + fork" design. Rejects snapshot-file path in favor of fork(). 431 lines. |
| `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-sprint/09-fork-server-PHASE2A-DESIGN.md` | PHASE 2A — narrow-scope fix for the post-Phase-1a fork crash. 1003 lines. Predates current session's STATUS. |
| `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-sprint/09-fork-server-STATUS.md` | The current session's living STATUS document. |
| `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-next-sprint/HONEST-AUDIT-2026-05-19.md` | Sprint-level audit, §8 marks fork-server as "PAPER" pre-sprint and SPRINT after. |
| `Documentation/virt/uml/redesign/06-sequencing/post-q1-push.md` | Lift #5 forensic-memo origin — see §"Lift #5" for the brief that produced `signal-reentry-in-fork-window.md`. |
| `Documentation/virt/uml/redesign/08-future-phases/02-snapshot-to-disk.md` | v2 snapshot-to-disk parking-lot design (the deferred-not-rejected CRIU-shaped alternative). 532 lines. |

### Risk and decision documents

| Path | Role |
|------|------|
| `Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-window.md` | **The most directly relevant file in the tree.** Forensic memo (2026-04-23) characterizing the *exact* saved-RIP-corruption hazard the current session hit, with the four failed wait4 variants and the four-option fix ranking. 295 lines. |
| `Documentation/virt/uml/redesign/04-risks/decisions-log.md` | Master decision log. D35 (v1 fork vs CRIU split), D37 (pull-forward items), D39 (commit 3 split), D40 (commit 3d split), D41 (signal gating), D42 (sched_worker_detach helper), D43 (BPF JIT deferral), D54-D62 (KVM backend). 12106 lines total. |

### User-facing documentation

| Path | Role |
|------|------|
| `Documentation/virt/uml/snapshot.rst` | User-facing version of "v1 ceiling: exit-status semantics" — same hazard, abbreviated form. |
| `arch/um/kernel/snapshot.c` lines 332–415 | In-kernel `KNOWN LIMITATION (v1 ceiling)` block describing the SIGALRM-into-stale-jmp_buf mechanism. |

### Architecture-review findings (2026-04-27 round)

| Path | Role |
|------|------|
| `Documentation/virt/uml/redesign/03-architecture-review-2026-04-27/agent-3-uml-rootcause.md` | Root-cause analysis from the third independent agent. |
| `Documentation/virt/uml/redesign/03-architecture-review-2026-04-27/agent-4-radical-redesign.md` | Radical redesign proposals. |

---

## 2. Answers to the seven questions

### Q1. Did prior rounds document that fork-from-syscall-handler is broken in UML, or know it works fine?

**Answer:** Prior rounds documented that fork is **conditionally
broken in UML — broken specifically when (a) stubs exist at fork
time AND/OR (b) the parent does any non-UML-kernel work post-fork
that allows SIGALRM to dispatch via UML's signal handler while a
stale jmp_buf is reachable.** This is not a generic "fork is broken"
finding; it is two distinct hazards both characterized in detail.

**Source 1** — `Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-window.md:20-26`:

> **Any `wait4`-family host syscall performed by the UML parent
> thread after `fork()` and before returning to the UML trap loop
> runs inside a window where a fired `SIGALRM` will `longjmp` into
> a `jmp_buf` captured pre-fork — whose saved stack pointer
> references a stack frame that no longer exists — and the CPU
> decodes garbage, delivering SIGILL.**

**Source 2** — `arch/um/kernel/snapshot.c:366-389`:

>   KNOWN LIMITATION (v1 ceiling — four iterations of investigation
>   have failed to resolve, most recently on 2026-04-23 during the
>   Finding #1 fix attempt). Inserting ANY wait4 path between (1)
>   and (2) — blocking wait4, WNOHANG poll loop, poll loop with
>   clock_nanosleep / sched_yield between polls, poll loop with
>   host sigprocmask SIG_BLOCK around the poll — all crash the
>   parent with "Kernel mode signal 4" (SIGILL) in kernel context.
>   The underlying hazard is UML's timer SIGALRM ... being
>   delivered into UML's signal handler mid-poll, which runs
>   switch_threads() and longjmp()s into a jmp_buf captured
>   pre-fork — stale stack, next instruction decoded from garbage,
>   SIGILL.

**Source 3** — `Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md:197-217` codifies the v1 ceiling and the four prior failed waitpid attempts.

The fork itself works under tightly controlled conditions: the AFL
forkserver does fork from a kernel call site every iteration and
the workers run trivial guest code per commit `9d0dd8ed3181`
("worker returns from um_snapshot_ready, runs trivial guest code").
What does NOT work is the parent then calling any blocking syscall
before returning to the UML trap loop.

### Q2. What was the AFL forkserver's documented "v1 ceiling" — what exactly fails when workers try to do real work, and what did the project's prior rounds conclude was the fix?

**Answer:** There are **two distinct v1 ceilings** documented in the
project. The current session's STATUS doc conflates them.

**v1 ceiling A — parent-side waitpid hazard** (the older, more
deeply researched one):
- Documented in `Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-window.md`.
- Failure: parent reaping the worker triggers SIGILL via
  SIGALRM-into-stale-jmp_buf.
- Documented fix (Option A, recommended at line 248): **"worker
  reports its own status pre-exit" via the status_fd; parent never
  calls wait4 in the hazard window.**
- Quote from `signal-reentry-in-fork-window.md:147-158`:
  > **Mechanism:** the worker, just before calling `exit(code)`,
  > writes a synthetic exit status to the status_fd. The parent
  > never reaps the specific worker synchronously; it reads the
  > pre-exit status on the status_fd, then moves on. Zombies drain
  > non-synchronously on the next iteration top (today's pattern).
  > **Pros:** Sidesteps the hazard entirely — parent never calls
  > a `wait4`-family syscall during a window where it matters.
  > No UML infrastructure changes required.

**v1 ceiling B — worker-side scheduler hazard** (the more recent
one):
- Documented in `Documentation/virt/uml/redesign/04-risks/decisions-log.md:3411-3635` (D42).
- Failure: worker survives fork but crashes at
  `__set_next_task_fair+0x11b` (KASAN slab-OOB) the first time it
  calls `schedule()` after returning from `um_snapshot_ready`.
- Documented fix landed in commit `f969325d3a8d`
  (`sched_worker_detach_other_tasks`) — observed insufficient. D42
  records this honestly:
- Quote from `decisions-log.md:3414-3432`:
  > **Status:** Accepted and shipped as a v1 building block;
  > observed insufficient in isolation — the slab-OOB at
  > `__set_next_task_fair+0x11b` persists even with the helper in
  > place. ... v2 replaces this with a freezer-cgroup pre-fork +
  > per-task re-clone design per D41's revisit triggers; this
  > helper is removed in the same series that lands v2.

The current session's symptom (saved-RIP corrupted to `0x4`,
`0x2d6b62`, `0x2e3e34` at `um_template_pause_enter+0xf0`) is
mechanically v1 ceiling A re-firing in a NEW call site (template_pause
loop rather than snapshot.c forkserver loop) — same SIGALRM into a
stale post-fork stack image, same garbage-decode-into-SIGILL.

### Q3. Did any prior memo propose a freezer-cgroup pre-fork barrier? Single-threaded fork? CRIU integration?

**Answer:** Yes to all three — but each was *already considered and
either deferred with reasoning or rejected for v1*. None of them is
a new idea the current session surfaced.

**Freezer-cgroup pre-fork barrier:**
- Originally proposed in `decisions-log.md:3548-3560` (D42 alternative E):
  > **E. Freezer-cgroup barrier pre-fork, restore by recloning
  > every task.** Architecturally the cleanest (CRIU's pattern,
  > gVisor's task-goroutine model). Large. Cross-multiple-
  > subsystems. Deferred to v2 per D41; v1 gets the narrow helper.
- Referenced as the v2 replacement in
  `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md:610-615`:
  > v2 (when someone needs sustained fuzz on blocking guest
  > syscalls) replaces it with a freezer-cgroup pre-fork barrier
  > + per-task re-clone path per D41's revisit triggers

**CRIU integration:**
- Decision D35 (decisions-log.md:2360-2542) **rejected CRIU for v1**
  on cost grounds and **parked it for v2** at
  `08-future-phases/02-snapshot-to-disk.md`.
- Quote from `decisions-log.md:2418-2423`:
  > - **Full CRIU-style snapshot-to-disk in v1.** Rejected: 3-4×
  >   the engineering effort for v1 value that only materializes
  >   on host reboot. The fuzz profile is a long-lived host
  >   process; 1000+ iter/s from a `fork()`-based forkserver
  >   already saturates host CPU. Snapshot-to-disk is a M8+ "nice
  >   to have".
- The v2 CRIU-shaped path is fully designed in
  `08-future-phases/02-snapshot-to-disk.md` and has a chosen format
  (D36 — ELF + PT_NOTE).

**Single-threaded fork:**
- Proposed in `signal-reentry-in-fork-window.md:175-199` as Option B
  ("dedicated wait-thread with post-fork-captured jmp_buf") —
  rejected at line 197 because UML's host-abstraction layer
  deliberately avoids pthreads.
- Implicitly the "worker reports own status" Option A
  (`signal-reentry-in-fork-window.md:147-173`) achieves the
  benefits of single-threaded fork without the pthread surface.

### Q4. Decisions D41, D42, D43 in the decisions-log — what do they say about signal gating, sched_worker_detach, and the SMP fork ceiling?

**D41** (`decisions-log.md:3240-3408`) — UML `signals_enabled` is the
canonical signal-gating primitive for snapshot/forkserver critical
sections.

Quote from D41:3240-3251:
> **Decision:** Any snapshot/forkserver code that runs a blocking
> host syscall from inside UML kernel context MUST use UML's
> `signals_enabled` machinery ... NOT raw `sigprocmask`.

D41:3296-3324 documents the **four prior failed waitpid variants**
in a table — exactly the same four variants the current session
re-tried in a new call site (template_pause):

| Attempt | Signal gate | Wait primitive | Crash RIP |
|---------|-------------|----------------|-----------|
| 1 | none | glibc `waitpid` | `0x0` |
| 2 | host `sigprocmask` block ... | glibc `waitpid` | `0x61093b80` |
| 3 | UML `um_set_signals(0)` | glibc `waitpid` | `0x0` |
| 4 | UML `um_set_signals(0)` | raw `syscall(__NR_wait4)` | `0x61093bc0` |

D41:3313-3321:
> The UML-native gate (attempts 3, 4) is the right primitive for
> the general class of problem, but does not by itself fix the
> waitpid crash. Suspect paths: ... (c) a UML scheduler re-entry
> via a path independent of the host signal flow. The consistent
> heap-address cluster in attempts 2 and 4 is structurally similar
> to `longjmp` into a jmp_buf whose saved `rip` has been
> overwritten post-fork.

**This is the same symptom the current session hit.** D41 also
names the concrete fallback strategies: pidfd_open+poll,
SIGCHLD-driven IRQ reap, or accepting no-status-byte.

**D42** (`decisions-log.md:3411-3635`) — sched_worker_detach helper
exists, was shipped, **and was documented as insufficient at
shipping time**. See Q2 v1 ceiling B above.

**D43** (`decisions-log.md:3637+`) — about BPF JIT cross-subsystem
touches, **not about fork**. The current STATUS document does not
reference D43; it is unrelated.

### Q5. Was Memo 26's snapshot/restore design intentionally rejected for the fork-server use case, or is it a fallback the project should reconsider given fork is broken?

**Answer:** Memo 26's `kvm_v2_snapshot_capture` /
`_restore_full` was **rejected for the fork-server use case for two
distinct reasons**, but only one of them remains valid post-fork-failure.

Source — `06-sequencing/post-2026-05-19-next-sprint/09-fork-server-snapshot-restore.md:163-170`:
> The kvm-v2 snapshot API (`kvm_v2_snapshot_capture_full`,
> `_restore_full`) is **not used** in this design. Live fork
> captures everything the snapshot API does AND more (every
> buffer, every kernel data structure, every cached page) without
> serialization overhead. The kvm_v2 snapshot API remains
> relevant for cross-process migration (memo 26's original
> target); it's just not the right tool for spawning siblings.

The reasoning was "fork is cheaper than serialization". If fork
turns out NOT to work (current state for the keystone use case),
then the cost calculus reverses and Memo 26's path
**becomes** the right tool — exactly Memo 12's design for KVM,
generalized.

Memo 26's measured costs from `02-workstreams/D-kvm-backend/12-snapshot-forkserver-kvm.md:237-262`:
- capture=37 µs (regs-only, no memslot)
- restore_full median=14 µs p95=20 µs
- full memslot copy: 5-50 ms for typical configs

That is competitive with the keystone Memo 09 target (`<= 5 ms
median, 50 ms p99`) for the regs-only fast path. Reconsidering this
fallback is not "fork-server failed, abandon ship" — it is "we
already invested in the alternative and it works".

### Q6. Are there any research notes from agents that ran in prior sessions documenting fork hazards?

**Answer:** Yes — multiple sessions' worth, all in
`Documentation/virt/uml/redesign/`:

| File | Date | Finding |
|------|------|---------|
| `04-risks/signal-reentry-in-fork-window.md` | 2026-04-23 | The forensic memo: characterized hazard + four failed variants + four-option fix ranking. |
| `decisions-log.md` D41 | 2026-04-20 | Signal-gating contract + same four failed variants logged in a table. |
| `decisions-log.md` D42 | 2026-04-20 | sched_worker_detach helper + acknowledgment it's insufficient. |
| `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md` | 2026-04-20 | Shipped-commit audit + v1 ceiling honestly recorded as "non-blocking guest programs only". |
| `arch/um/kernel/snapshot.c:332-397` | shipped 2026-04 | In-kernel KNOWN LIMITATION block describing the SIGALRM-jmp_buf mechanism. |
| `Documentation/virt/uml/snapshot.rst:113-149` | shipped 2026-04 | User-facing v1 ceiling description. |
| `06-sequencing/post-q1-push.md` | 2026-04-23 | Lift #5 brief that produced `signal-reentry-in-fork-window.md`. |

No "research/" subdirectory exists in `Documentation/virt/uml/redesign/`
(this directory was listed in the user's task brief but is not
present in the tree — `07-references/` is the closest match, and it
contains `prior-art.md` + `README.md` + `sources.md`, not research
notes). The "research" that exists lives instead in the risk and
decisions documents above.

### Q7. Has the same "saved-RIP corrupted to small value" symptom been documented before?

**Answer:** YES — explicitly and at length.

**Source 1** — `signal-reentry-in-fork-window.md:7-26` (entire memo)
characterizes this exact hazard: "the CPU decodes garbage, delivering
SIGILL".

**Source 2** — `decisions-log.md` D41:3296-3321 documents the
four prior crashes with their specific RIPs:
- Attempt 1: RIP `0x0`
- Attempt 2: RIP `0x61093b80`
- Attempt 3: RIP `0x0`
- Attempt 4: RIP `0x61093bc0`

The current session's RIPs (`0x4`, `0x2d6b62`, `0x2e3e34`,
`0x68803bde`) follow the **same pattern**: small values, or values
in the UML guest-userspace VA range. D41:3319-3321:
> The consistent heap-address cluster in attempts 2 and 4 is
> structurally similar to `longjmp` into a jmp_buf whose saved
> `rip` has been overwritten post-fork.

**Source 3** — `arch/um/kernel/snapshot.c:373-379` says explicitly:
> ... all crash the parent with "Kernel mode signal 4" (SIGILL)
> in kernel context. The underlying hazard is UML's timer SIGALRM
> (or any host signal queued while we're in non-UML-kernel code)
> being delivered into UML's signal handler mid-poll, which runs
> switch_threads() and longjmp()s into a jmp_buf captured
> pre-fork — stale stack, next instruction decoded from garbage,
> SIGILL.

The current session's STATUS doc
(`09-fork-server-STATUS.md:163-251`) reaches a structurally similar
conclusion but does not cite or reference any of these three prior
sources. Its conclusion ("genuinely beyond a single-session fix")
is contradicted by `signal-reentry-in-fork-window.md` which had
already characterized the hazard AND recommended a one-session fix
(Option A — worker reports its own status pre-exit).

---

## 3. Git commit log of fork/snapshot-related changes in arch/um/ since 2026-03-01

```
851cd7b7eba0 arch/um: add um_template_pause=early-fork mode + final v1-ceiling bisect
9402746dba54 arch/um: add um_skas_forget_all_stubs + conclude v1-ceiling bisect (Phase 2a)
767cef61264d arch/um: child-side sched_worker_detach + preempt_disable (Phase 2a partial)
c0a7ac3f905c arch/um: bypass glibc cancellation pipe in template_pause helpers + bisect findings
985fd78ab070 arch/um: assert_fork_safety refuses concurrent mid-syscall mms (Memo 09 Phase 2a Patch 3)
c417eccf869e selftests/um: add template-pause-fork-smoke (Memo 09 Phase 2a Patch 5)
bb2d89fccaaa arch/um: tidy template_pause fork-on-resume body
5dac39adad4f arch/um: wire teardown → fork → respawn into template_pause loop (Memo 09 Phase 2a Patch 4)
d40d124f3abe arch/um: mm_list-walking stub teardown / respawn helpers (Memo 09 Phase 2a Patch 2)
0993765a704c arch/um: add start_userspace_redo() (Memo 09 Phase 2a Patch 1)
c0c3d05c89d0 arch/um: add EXPERIMENTAL fork-on-resume loop (Memo 09 Phase 2a)
5051e9c90342 arch/um: add template-pause + fork hook (Memo 09 Phase 1a)
79a50392d4e7 Revert "um: skas/process.c: drain interrupt_end after each backend dispatch"
bad8d61d2592 um: skas/process.c: drain interrupt_end after each backend dispatch
7f79b35e1531 um: snapshot: refuse to enter forkserver path under KVM backend (task #250 v1 guard)
f8bb690f91b8 um: snapshot/forkserver: revert 3dca0c2ae50b's poll-waitpid fix (crashes parent), document v1 ceiling honestly
3dca0c2ae50b um: snapshot/forkserver: real worker status via poll-waitpid loop (Finding #1)
257b8cf61b84 um: snapshot: reap worker zombies between forkserver iterations
e65cedc6b3a6 um: snapshot: don't latch um_snapshot_enabled on benign ready-point hit
70feba656bf5 um: snapshot: state_version sysfs + snapshot-smoke selftest + user doc (C-09 commit 5)
d88b05a7115e um: os-Linux: atomic-CLOEXEC FD hygiene + per-FD disposition annotations (C-09 commit 4)
f969325d3a8d sched/um: CFS worker-detach helper for snapshot/forkserver (C-09 3d-d)
9d0dd8ed3181 um: snapshot: worker returns from um_snapshot_ready, runs trivial guest code (workstream C-09)
c49a1061b81d um: snapshot: worker rebuild infra (SIGIO helper, POSIX timer) (workstream C-09)
498066d13937 um: snapshot: assert signals_enabled == 1 at ready-point entry (D41)
b2e391348e80 um: snapshot: UML-native signal gating + raw wait4 primitives (workstream C-09)
a0328b6011ed um: snapshot: 4-byte status byte per AFL protocol (workstream C-09)
8f5e8b2159ea um: snapshot: worker_init drops parent-inherited host state (workstream C-09)
2bf287b64b16 um: snapshot: convert one-shot forkserver into multi-iteration loop (workstream C-09)
c29a9ed9960c um: snapshot: AFL handshake + one-shot fork (workstream C-09)
b78df759dfbd um: snapshot: skeleton + Kconfig + mmap-region registry (workstream C-09)
```

Notable: commits `3dca0c2ae50b` (the failed poll-waitpid attempt)
and `f8bb690f91b8` (its revert + honest documentation) on 2026-04-23
are the prior round that produced `signal-reentry-in-fork-window.md`.

KVM snapshot port (Memo 26) commits — relevant as the fallback path:

```
9ca8cfd5db4a um: kvm-v2: snapshot Phase 5 — cmdline + debugfs bench harness (#168)
5cdb3e08e445 um: kvm-v2: snapshot Phase 4 — cross-task semantics (#168)
e5294220b3e5 um: kvm-v2: snapshot port Phase 3 — full capture + memslot round-trip
9baf6a1e9838 um: kvm-v2: snapshot port Phase 2 — boot-time KUnit fixture
aa4cd328102c um: kvm-v2: snapshot port Phase 1 — skeleton + regs_only round-trip
```

---

## 4. What the current session missed (executive summary)

The current `09-fork-server-STATUS.md` reaches the conclusion at
line 252:

> **Conclusion: this is genuinely beyond a single-session fix.**

and proposes three "multi-week" paths (CRIU, freezer-cgroup,
single-threaded master).

**This conclusion is directly contradicted by
`Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-window.md`**,
authored 2026-04-23 by the prior round of investigation that hit the
**same hazard** (SIGALRM-into-stale-jmp_buf-during-non-kernel-exec-window
producing saved-RIP corruption) in a different call site. That memo:

1. Names the exact mechanism (line 20-26 quoted in Q1 above).
2. Documents the same four failed waitpid variants the current
   session re-tried (Table at lines 113-120).
3. Explicitly rejects `signals_enabled` and `sigprocmask` as fixes —
   neither closes the window, both fail for the same atomicity
   reason (lines 76-103).
4. Recommends a concrete single-session fix at line 147-173: **"the
   worker, just before calling `exit(code)`, writes a synthetic
   exit status to the status_fd. The parent never reaps the
   specific worker synchronously."** This sidesteps the entire
   hazard class because the parent never enters a non-UML-kernel
   blocking call between fork-return and trap-loop-return.

The current session's symptom is the same hazard re-firing in
`um_template_pause_enter` because the template_pause fork loop's
parent path has post-fork non-trap-loop work (the SIGSTOP + identity
write + next iteration's setup) inside which SIGALRM can fire and
longjmp into the pre-fork jmp_buf. **No new mechanism is at play;
no new fix is required at the design level.**

The freezer-cgroup, CRIU, and single-threaded-master proposals in
the current STATUS doc are all real ideas that the prior rounds
*already considered, documented, and either deferred or rejected
for v1* (see Q3). Reframing them as new ideas hides that they were
on the table all along.

---

## 5. Recommendation

**Next concrete step (within the current sprint, single-session
scope):**

Apply `signal-reentry-in-fork-window.md`'s **Option A** to the
template-pause loop:

1. Move the "write child pid + write status" step from the parent
   to **before the fork** (parent computes the next child's
   identity, writes the pid placeholder to memfd, then forks; the
   child's first act post-fork is to write its OWN getpid() to
   memfd[260:264] using the same primitive
   `os_template_pause_write_child_pid` already in tree at
   `arch/um/os-Linux/template_pause.c:136-160`).

2. Move the parent's post-fork work *into* the next iteration's
   pre-fork preamble. Specifically: the parent's
   `one_pause_cycle()` (the SIGSTOP/SIGCONT for the next take) is
   already a UML-kernel-context wait via `os_template_pause_stop_self`
   (`kill(getpid(), SIGSTOP)` at
   `arch/um/os-Linux/template_pause.c:64-75`). SIGSTOP/SIGCONT is
   immune to the hazard: it is uninterruptible by SIGALRM (host
   kernel suspends the process; signals queue but don't dispatch
   until SIGCONT lands), and on resume the UML signal handler
   doesn't fire because no SIGALRM was delivered while stopped.

3. The PARENT's only non-stop work post-fork becomes: returning
   from `os_template_pause_fork` and reaching the next
   `os_template_pause_stop_self` call. That window is just a few
   instructions inside `fork_on_resume_loop` and contains no host
   blocking syscall, no glibc indirection, no UML kernel signal
   dispatch path. If SIGALRM fires during these instructions,
   `signals_enabled == 0` (the `os_snapshot_block_iter_signals()`
   gate is held across the whole loop body per
   `arch/um/kernel/template_pause.c:225-273`) and the signal queues
   into `signals_pending`. The window between fork-return and the
   next SIGSTOP is too brief to bound deterministically against
   the host kernel timer slice, but it does NOT contain any
   blocking call that allows SIGALRM to dispatch — the existing
   gate is sufficient.

4. If step 3 still hits the same crash, that empirically rules out
   the "parent does non-trap-loop work post-fork" hypothesis and
   narrows the diagnosis to either (a) the SKAS stub-respawn path
   (`um_skas_respawn_all_stubs`) doing host work post-fork — fixable
   by moving the respawn behind the next SIGSTOP — or (b) a more
   fundamental issue in commit `f969325d3a8d`'s
   `sched_worker_detach_other_tasks` that D42 flagged as
   insufficient.

**If step 4(b) is the diagnosis,** the prior round's
designated v2 fix is the freezer-cgroup design — but that is a
multi-week effort and the project's prior decision (D35, D41, D42)
was to ship v1 with the documented ceiling and revisit. The
honest answer would be: mark template-pause fork-on-resume as
v1-ceiling-bound on the same terms as the AFL forkserver, update
STATUS accordingly, and unblock the immediate sprint goals
(`umlctl pool serve`, syzkaller integration) on the v1-ceiling
contract rather than waiting for v2.

**Alternative, if step 3+4 fails:** The kvm-v2 snapshot port
(commits `aa4cd328102c` through `9ca8cfd5db4a`) is **already
landed** with 37 µs capture / 14 µs restore measured. Memo 09's
keystone use case (`umlctl pool take` with 5 ms median target) can
be re-architected to use `kvm_v2_snapshot_restore_full` from the
master after a SIGSTOP/SIGCONT, instead of fork — the master
serves as the snapshot source, each "take" is a restore into a
freshly-cloned UML host process. This is closer to Memo 26's
original design and avoids the fork hazard entirely. Memo 12 says
this is the right shape for KVM specifically; with v2 snapshot
already landed, the same shape is now cheap for seccomp too.

**Discipline note:** the prior session's research is load-bearing
context for any continued work on the template-pause fork. Future
sessions on this code path must read
`Documentation/virt/uml/redesign/04-risks/signal-reentry-in-fork-window.md`
BEFORE attempting any new wait/poll/syscall variant, per its own
§"What this memo is NOT" line 274-275:
> Not a fix. No code changes accompany this memo.

The memo exists precisely to prevent future sessions from retrying
the four failed waitpid variants. The current session retried them
in a new call site without citing the memo.
