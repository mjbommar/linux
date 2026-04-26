# UML Redesign — Status Tracker

Last updated: 2026-04-26

This document is the single source of truth for "where are we, what's
broken, what's next." Updated whenever priorities or blockers change.

If something contradicts a memo in `02-workstreams/` or
`04-risks/decisions-log.md`, this file wins until the underlying memo
catches up.

---

## TL;DR — what works, what doesn't

| Workload                                    | Backend  | Reliable? |
|---------------------------------------------|----------|-----------|
| `init=/bin/echo`                            | kvm      | yes       |
| `init=/usr/bin/python3 -V`                  | kvm      | yes       |
| `python3 -c "import hashlib; sha256(...)"`  | kvm      | 5/5       |
| `python3 -c "import _bisect / _datetime / _ssl / _hashlib / _struct"` | kvm | yes (single C-extension import, no test framework) |
| `subprocess.run(['/bin/echo', 'x'])`        | kvm      | ~8/10     |
| `python3 -c "import unittest; ..."`         | kvm      | **NO** (NULL-page deref inside python3.14, downstream of corrupted state) |
| Any module from CPython's test suite        | kvm      | **NO** (fails because the harness needs `import unittest`) |
| Same workloads under `backend=force=seccomp` | seccomp | yes       |

**The headline fact (corrected 2026-04-26 by `kvm_shadow_audit_va`
diagnostic, commit `44abfd6e6657`): under integrated KVM, a single
Python `import unittest` faults inside `python3.14`'s text or
`ld-linux-x86-64.so.2`'s text — depending on ASLR layout — NOT
inside ld-linux processing `_lzma.cpython-*.so` relocations as
previously claimed.** With ASLR off (`randomize_va_space=0`) the
crash is deterministic: `cr2=0x300`, `rip=0x4002625f` in
`ld-linux-x86-64.so.2[2625f]`, which addr2line resolves to
`dl_main` at `elf/rtld.c:1953`. The faulting instruction is
`mov 0x300(%rbx),%edi`; the immediately-preceding instruction at
`0x26236` is `mov -0x260(%rbp),%rbx`, so dl_main loads rbx from
the stack slot `[rbp-0x260]` which is supposed to hold the main
executable's `struct link_map *` (arg1 to the upcoming call to
`_dl_map_object_deps`). The slot reads as 0. Several other
callee-saved regs (r12/r13/r15) are also 0 — consistent with the
initial register state at process start, suggesting dl_main's
earlier code never wrote to them.

Same script under `backend=force=seccomp randomize_va_space=0`
succeeds (UT_PRE → UT_DONE → exit 0). So the bug is specifically
in the KVM backend's interaction with dl_main's early
initialization, not in glibc/python.

Diagnostics ruled out so far (commits `44abfd6e6657`,
`2fdfd77f7264`, `97c73735c47a`, `b458b8605c8f`):
  - Shadow PT divergence at the fault VA (audit at !touched: EQUAL
    on every run)
  - Stale-true cache on the cached-skip path (lockstep audit:
    DIV=0 even at 2253 leaves)
  - Syscall round-trip clobbering callee-saved regs
    (CALLEE-SAVE-CLOBBER never fires across the boot)

Important meta-finding from `b458b8605c8f`: the underlying race
this bug is rooted in is **timing-sensitive enough that adding
work on the hot path measurably changes its trigger rate**. The
audit_pgd lockstep walk on every cached-skip dropped hashlib
smoke from ~95% to ~30%; even just adding the dead-code regs-dump
bytes to the !touched branch dropped it to ~80%. Diagnostics that
run in the steady-state path must be gated behind the
`kvm_diag_*` kernel command-line knobs (default off) and only
enabled for specific debugging boots — otherwise they will
themselves trigger the bug they are trying to characterise. Knobs:

  kvm_diag_pf_dump_regs=1       enable GP-reg dump on !touched #PF
  kvm_diag_audit_pgd_skip=1     enable full pgd vs shadow lockstep
                                audit on every cached-skip

Remaining hypotheses to investigate:
  - Shadow PT has stale leaves the pgd doesn't (shadow→pgd
    direction not yet audited)
  - Guest TLB caches stale mappings even though shadow PT is
    correct (CR3 reload / TLB flush ordering)
  - Non-syscall paths (signal injection, page fault recovery,
    initial process setup via execve) corrupt user state
  - dl_main takes a different code path under our backend due to
    different return values from early syscalls (e.g. brk, mmap,
    arch_prctl, set_tid_address) — would need per-syscall return-
    value diff vs seccomp baseline

That blocks the entire CPython test suite, which is the merge gate
for closing #274 ("Python is table stakes; if UML can't run a normal
Python script that millions of people use, what is it good for?").

---

## The merge gate

`tools/testing/selftests/um/cpython-parity/cpython-parity.sh` runs
21 curated stdlib test modules under both backends and reports
module-by-module divergence. Statuses: `PARITY`,
`DIVERGE_KVM_REGRESSION`, `DIVERGE_KVM_BETTER`, `SKIP_NO_RESULT`,
`BOTH_FAIL`.

Today's reading:

```
TOTAL: parity=0 diverge=21 skip=0
```

Every module passes under seccomp, every module crashes under kvm.
**Until this reads `parity=21 diverge=0`, no other phase-1/2/3 work
proceeds.**

When parity reaches zero diverges, the gate widens:
1. Tier 1: pytest-driven third-party libraries (`requests`, `cryptography`, `numpy`)
2. Tier 2: `pip install <pkg> && pytest <pkg>` — full network + tarball + subprocess
3. Tier 3: small Django/FastAPI server actually serving requests over loopback (needs `CONFIG_UML_NET_VECTOR=y` rebuild + tap setup)

Each tier is an explicit kselftest. Width before depth.

---

## Open in_progress

### #274 — KVM SIGSEGVs on Python C-extension import (table stakes)

The umbrella for all Python-runs-under-kvm work. Stays open until
`cpython-parity` reads zero diverges. Currently in_progress.

**Subordinate work:**

- #275 — per-mm shadow PGD. **Completed** (commit `b9d78a7eeb51`).
  Each UML mm now owns its own `struct kvm_shadow_mm`. Eliminated
  the cross-process leaf-leak that was the dominant failure mode
  for `subprocess.run`. Reliability went from 1/5 to 8/10.

- #276 — skip `os_map_memory` in `kvm_mm_map`. **Pending (reverted).**
  Hypothesis was that under integrated KVM the host VA mapping
  wasn't load-bearing because the guest CPU goes through shadow PT
  → memslot → uml_physmem linear mapping. Empirically wrong:
  `import unittest` crashed in libc when os_map_memory was skipped.
  Restored. Real fix needs per-mm host-VA isolation (child host
  process per UML mm, matching seccomp's stub-child model).
  Substantial refactor; defer until the within-mm cumulative-imports
  staleness is fixed first because that may turn out to be the
  dominant remaining bug regardless.

- #277 — full CPython test suite as merge gate. **Pending.** The
  gate exists (cpython-parity.sh). Closing #277 requires #274 to
  close first.

### #250 — KVM-aware `um_snapshot_ready`

Snapshot/forkserver primitives shipped (commit `040bdb2f6b04`); the
hook into `um_snapshot_ready` so `snapshot-smoke` PASSES under kvm
is the remaining work. Deferred until #274 closes — there's no
point gating snapshot-replay on a backend that can't run real user
programs.

### #253 — first-class time-travel + record/replay (KVM determinism)

Per-NR record matrix covers `getrandom / read / pread64 / recvfrom
(sockaddr) / readv (iov)`. Strict-replay mode is default. Round-
trip KUnit cases pass. Open ladder rungs: `recvmsg` (msghdr +
optional msg_control), PMU-driven instruction-boundary recording,
MMIO recording. Deferred until #274 closes.

---

## The actual debugging plan for #274

Goal: `cpython-parity` flips to `parity=21 diverge=0`.

Method: every fix must show a parity delta. "Looks correct" without
a delta is rejected. Three phases:

### Phase 1 — make divergence visible

Until I can SEE pgd-vs-shadow-PT divergence in a log, every "fix" is
a guess. Add three diagnostics:

1. **Divergence audit at every guest #PF entry.** Walk UML's pgd for
   `cr2`'s VA, walk the shadow PT for the same VA, log
   `(cr2, pgd_pte, shadow_pte, equal?)`. The first faulting access
   where shadow != pgd identifies the missed sync point.

   **Status (2026-04-26):** First instance landed as
   `kvm_shadow_audit_va` wired into the !touched branch of the
   UM_KVM_PF_PORT handler (commit `44abfd6e6657`). Empirically the
   SIGSEGV-bound fault is `EQUAL` on every run — the keystone
   hypothesis ("first divergence is at cr2") is wrong for this bug
   class. Search must shift to non-faulting VAs (item #2 below) and
   to per-mutation tracing (item #3 below).

2. **Divergence audit at every kvm_enter_guest.** Sample 20 random
   user VAs from `current->mm`'s vmas, walk both trees, count
   divergences. Log a histogram each entry. If divergences accumulate
   over time, the rate identifies the leaky operation class.

3. **Tracepoint every `set_pte_at` / `kvm_shadow_map_page` /
   `kvm_shadow_invalidate_va_range`** with `(mm, va, old_pfn, new_pfn,
   flags, caller_function)`. Then I can grep: every `set_pte_at` on a
   user VA should have a paired shadow update. The unpaired one is
   the bug.

4. **Stress-test reproducer.** A C program (not Python) that does N
   rounds of `mmap(PROT_RW)` → write → `mprotect(PROT_RO)` → read →
   `munmap`. Tunable N. Find the smallest N at which divergence first
   appears. Deterministic non-Python reproducer the diagnostics can
   attach to.

Phase 1 is done when one boot of `python3 -c "import unittest"`
produces a single log line saying exactly which (VA, mm, syscall,
timestamp) the first divergence occurred.

### Phase 2 — match each divergence to a code path

Each divergence event from phase 1 has a calling stack. Group by
stack. The groups are bug families.

Likely candidates I haven't ruled out:

- **`flush_tlb_range` not draining before guest entry.** UML's
  flush_tlb_range marks ranges via `um_tlb_mark_sync`. `um_tlb_sync`
  drains. If flush is called and sync isn't drained before the next
  guest entry, divergence.

- **mprotect / madvise / munmap.** Each can change PTEs. Verify each
  path triggers our shadow invalidation.

- **handle_mm_fault from inside copy_to_user.** UML's `maybe_map`
  calls `handle_page_fault` mid-syscall. The resulting `set_pte_at`
  on the active mm doesn't trigger a fresh `current_mm_sync` until
  the next iteration of `kvm_run_userspace`. Within-syscall stale
  shadow.

- **Implicit anonymous fault-in.** Generic Linux's `do_anonymous_page`
  allocates a zero page on first access. If the next iteration's
  `current_mm_sync` doesn't drain THIS specific range, divergence.

- **Exec mm replacement timing.** When `execve`'s `exec_mmap`
  installs a new mm, the OLD `mm_id` (with its `kvm_shadow`) gets
  freed. If KVM_RUN's saved register state references the old
  shadow's CR3 GPA, and we re-enter without rebuilding sregs,
  freed memory.

- **mmap_lock vs shadow update ordering.** UML's pgd mutations
  happen under `mmap_lock`. `kvm_shadow_invalidate_va_range`
  doesn't take any lock. If the GUEST is reading the shadow tree
  while the kernel mutates pgd → marks sync → drains via
  `kvm_mm_map` → invalidates shadow, racy reads can see torn state.

The phase-1 traces tell me which apply.

### Phase 3 — fix one root cause, prove it with the parity gate

For each identified divergence:
1. Add a regression test triggering the exact pattern (the C
   reproducer + a Python repro).
2. Fix it.
3. Run `cpython-parity`. Metric: how many module rows flipped from
   DIVERGE to PARITY.
4. If zero modules flipped, the fix isn't load-bearing — revert and
   look harder.
5. Commit only fixes where parity moves.

### Phase 4 — structural endgame: collapse the sync chain

Today's chain:
```
set_pte_at → um_tlb_mark_sync → (later) um_tlb_sync →
ops.mmap → kvm_shadow_invalidate_va_range
```

Every step is a place where the chain can break. The structural
fix: under `CONFIG_UM_BACKEND_KVM_INTEGRATED`, have `set_pte_at`
directly call into the active mm's shadow update under one
critical section. No "mark + drain later." pgd write and shadow
write happen transactionally.

May not be necessary if phases 1-3 close enough divergences. But
the moment three "missed sync point" bugs show up in three
different places, that pattern becomes evidence for needing the
structural fix.

---

## Discipline rules in effect

These are the habits adopted after the 2026-04-25 retrospective
("how did you get this far without paying attention to how broken
this was?").

1. **`cpython-parity` is the only metric I report.** Parity number,
   nothing else. Latency, commit count, line count, kselftest count
   are not answers to "does this work."

2. **No new feature work until #274 closes.** Phase 5 LKML series,
   perf optimizations, record/replay extensions, snapshot-forkserver
   integration — all paused.

3. **No test downgrades.** If a kselftest harness sees a fatal
   signal, segfault, panic, or non-zero exit, it must FAIL. Skipping
   is allowed only for "the prerequisite isn't installed on this
   host" — never for "the thing crashed but we'll call it OK." All
   existing harnesses get audited for swallow-failures patterns
   before any new work.

4. **Width before depth.** Adding KUnit cases for the dispatcher is
   depth — measures the same surface more carefully. Adding
   `test_subprocess`, real third-party library tests, real package
   installs is width — measures whether the thing is useful at all.
   Width catches "your foundation is broken." Depth doesn't.

5. **Smell test before every "done" claim.** Would I bet $100 that
   a user can do <the use case> on this build right now? If no,
   it's not done.

6. **Disagreement with the user is a signal to slow down, not push
   through.** When the user says "are you sure?" — re-test. When
   they say "fix the real problem" — stop adding small fixes and
   look at the structural problem. When they say the same thing
   twice in the same tone, drop everything else immediately.

7. **Periodically widen the test surface.** At every phase boundary,
   ask: does real software run on this? Not "did the green
   checkmarks light up?"

---

## Audit todo: kselftest harness honesty

Before resuming feature work, audit every
`tools/testing/selftests/um/*/run-*.sh` for swallow-failure
patterns. Specifically look for:

- `|| true` on a UML invocation followed by parsing OUT for "PASS"
  patterns instead of failing on non-zero exit
- `echo "FOO: PASS runtime skipped (...)"` on a fatal signal
  detection — `kvm-record-smoke/run-kvm-record-smoke.sh:154-157`
  has this pattern today
- Any harness that downgrades a SIGSEGV into "best-effort"
- Any harness whose passing condition is just "boots and exits"
  rather than "exercises the thing under test"

Each downgrade gets removed, and if the underlying behavior is
broken, that's a new task in the tracker.

---

## Snapshot of the task tracker (as of 2026-04-26)

### in_progress (P0 — gating)

- **#274** — KVM SIGSEGVs on Python C-extension import (table stakes)
- **#275** — per-mm shadow PGD (completed; tagged in_progress
  pending closure of #274)
- **#250** — KVM-aware um_snapshot_ready (was in_progress; deprioritized)

### pending (P0 — gating)

- **#276** — per-mm host VA isolation
- **#277** — full CPython test suite as merge gate

### pending (Phase 1 follow-on)

- **#244** — huge-page (2 MiB) shadow PT for UML physmem (perf
  lever #5; deferred)

### pending (Phase 2)

- **#246** — per-handler size-aware G1 caps
- **#247** — A3 expansion: move using_seccomp probe

### pending (Phase 3)

- **#252** — vm/uml syzkaller backend
- **#253** — first-class time-travel + record/replay extensions

### pending (Phase 4)

- **#256** — ARM64 KVM backend port
- **#257** — RISC-V KVM backend port
- **#258** — cross-host CI matrix expansion

### pending (Phase 5 — LKML submissions, ALL PARKED until #274 closes)

- **#259** — refresh Series 7 cover letter post-Phase-1 numbers
- **#260** — submit Series 1 (bpf-hygiene-v1)
- **#261** — submit Series 2 (kmsan-arch-callback-rfc)
- **#262** — submit Series 3 (ftrace-notrace-generic-v1)
- **#263** — write + submit Series 4 (backend-ops-abstraction-rfc)
- **#264** — write + submit Series 5 (static-key-hot-paths-series)
- **#265** — write + submit Series 6 (profiles series)
- **#266** — submit Series 7 (kvm-backend-series)

### completed (selected highlights, this session and recent)

- **#275** — per-mm shadow PGD landed (`b9d78a7eeb51`)
- **#274 partial** — `current_mm_sync()` before KVM_RUN
  (`96bfe20b3616`), IRETQ-frame RSP/RIP/RFLAGS restore on ring-3 #PF
  (`401c231766ac`), CPUID XSAVE/AVX masking (`1a62c7d27e0d`)
- **#273** — KVM_SET_CPUID2 passthrough so guest libc loads
- **#272** — dyn-loader 0xc680 wild-jump fix
- **#271** — kvm_bootstrap_gva separated from kvm_bootstrap_va
- **#269** — IDT[8] #DF handler
- **#268** — perf bench statistical hygiene
- **#267** — /dev/kvm ACL self-heal
- **#251** — snapshot kselftest
- **#255** — PMU passthrough via KVM_CAP_PMU_EVENTS
- **#254** — runtime-flippable hooks via static keys

---

## How to use this document

When starting a session: read the TL;DR, check the merge gate
state, pick the highest-priority `in_progress` task. Do not pick a
non-P0 task while a P0 is open.

When ending a session: update the TL;DR, the merge gate state, and
move tasks between `in_progress` / `pending` / `completed`. Commit
this file with the change.

When in doubt about whether to work on something: if it's not P0
and the merge gate is red, the answer is no.
