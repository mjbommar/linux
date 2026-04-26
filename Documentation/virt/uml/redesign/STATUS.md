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
| `python3 -c "import unittest; ..."`         | kvm      | **NO** (cumulative-imports crash in ld-linux relocation) |
| Any module from CPython's test suite        | kvm      | **NO** (fails because the harness needs `import unittest`) |
| Same workloads under `backend=force=seccomp` | seccomp | yes       |

**The headline fact: under integrated KVM, a single Python
`import unittest` segfaults inside ld-linux processing the
relocations of `_lzma.cpython-*.so`.** Same script under seccomp
works.

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
