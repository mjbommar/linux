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
| `python3 -c "import re"`                    | kvm      | **YES** (post-2026-04-26 keystone fix `901213a8d2d1`) |
| `python3 -c "import unittest"`              | kvm      | 15-20/20 (variance; SECCOMP baseline 20/20) — see "Residual flake" below |
| `read_test5` byte-integrity (8-file mmap harness) | kvm | 8/8 matches seccomp baseline (was 0/8) |
| CPython parity gate (21 stdlib modules)     | kvm      | **16-18/21 PARITY** (run-to-run variance; was 0/21 pre-keystone) |
| Same workloads under `backend=force=seccomp` | seccomp | yes (20/20 deterministic) |

## Residual flake state (post memo 17 Phase H — 2026-04-26)

Six commits landed this session moved the cpython parity gate from
**0/21** to **16-18/21** with run-to-run variance. The remaining
3-5 DIVERGE entries each run are NOT deterministic test failures —
they are SIGSEGV (`exitcode=0x0000000b`) flakes that appear at
varying RIPs across runs. SECCOMP runs the IDENTICAL workload at
20/20 deterministic, so the flakes are KVM-specific kernel bugs,
NOT Python/glibc nondeterminism.

The residual ~10-20% SIGSEGV-flake on Python startup (single
process) is the dominant blocker for full 21/21 parity. Empirical
signature pattern (memo 17 Agent 3 report):
  - Bug A: heap-data 0xAA UAF (`rdi=0xaaaaaaaaaaaaaaab`)
  - Bug B: TLS slot reads 0xff..ff (FS_BASE / TLS state leak)
  - Bug C: NULL ob_type during PyObject deref
  - Bug D/E: high-bit-corrupted pointer (`cr2=0x80000d18`,
    `cr2=0xc000c680` — bit 31 set on what should be a 0x40000xxx
    user VA)

What's been ruled out:
  - vCPU memory ordering on shadow->dirty (memo 17 Phase A)
  - TLB-flush mechanism (memo 17 Phase B / keystone)
  - FPU hash UAF (memo 17 Phase D — embedded in arch_thread)
  - VCPU_EVENTS save/restore (memo 17 Phase E + H)
  - Cross-task event leak on fresh switch-in (memo 17 Phase H)
  - Cross-task FPU leak on fresh switch-in (memo 17 Phase H)
  - dirty-flag race on consume (memo 17 Phase H finding 1)

Suspected remaining bug classes (to investigate):
  - Finding 5 (memo 17 Phase I): kvm_shadow_fill_from_uml_pgd walks
    the source pgd without holding mmap_read_lock — concurrent same-
    mm unmap could free PT pages mid-walk. Tracked separately.
  - Parent-VA contamination (memo 17 finding 4 / Phase 4 Option B):
    `kvm_mm_map` uses os_map_memory into the SINGLE UML host process
    address space; cross-mm switches leave stale host-VA mappings
    that copy_to_user/copy_from_user can hit. Architectural — needs
    per-mm host worker process.
  - Singleton IRETQ-frame buffer at `kvm_bootstrap_page_stack`: all
    tasks share the same staging frame for the bootstrap IRETQ.
    Signal-driven preemption between frame-write and KVM_RUN can
    let task B clobber task A's frame. Tested signal-blocking fix
    (Phase F) — no measurable improvement; bug must be elsewhere
    OR the signal-blocking didn't actually block at this layer
    (UML's signal infrastructure is complex). Per-task IRETQ-frame
    storage would be the surgical fix.

The session-end recommendation: Phase A-H is correct, lands solid
keystone fixes, and pushes parity from 0 to ~17/21. Pursuing 21/21
requires either the per-mm host worker (Phase 4 Option B,
multi-week) or a deep dive into the IRETQ-frame / parent-VA
race classes — both significant restructuring beyond this session.

## P0 keystone fix landed 2026-04-26 (`901213a8d2d1`)

**Root cause**: `KVM_SET_SREGS` with the same CR3 value as the vCPU's
current CR3 does NOT flush the guest TLB even when shadow PT contents
changed (e.g. munmap cleared a leaf, then user re-mmap a different
file at the same VA). KVM's `kvm_set_cr3` calls `invalidate_pcid` only
conditionally, and the VMCS-level CR3 reload is also gated on a value
change. Result: stale guest TLB → user reads OLD PFN's bytes after a
clear+remap cycle, never faulting because the cached translation is
"valid".

**Empirical proof** (`read_test5` C harness, 8 file mmaps at the same
VA): without the fix, only 1/8 user accesses to a re-mmap'd VA generate
a host #PF and only 3/8 set_ptes installs fire — the other 5 cycles
silently read stale TLB entries, returning the prior file's content
(the page at PFN 0x689 contained "glibc-ld..." from the dynamic
linker's earlier ld.so.cache mapping; functools.pyc's actual page at
PFN 0x10ac was correctly populated but never reached the user because
the shadow leaf write didn't invalidate the cached TLB).

**Fix**: in `kvm_enter_guest`, when CR3 is unchanged but shadow is
dirty, write a sentinel CR3 (XOR bit 12 — guaranteed-different,
guaranteed-valid GPA in our 512MiB physmem) before the real CR3 so
KVM observes a real change → forces VMCS reload + full TLB flush.
Cost: one extra `KVM_SET_SREGS` ioctl on dirty-shadow same-CR3
entries; the cached-skip predicate above still elides BOTH ioctls
when nothing changed.

**Impact**: cpython parity gate `parity=0 → parity=17` (of 21 modules).

**Open follow-on (task #77)**: 5 modules still diverge under KVM, all
hanging at multi-task / threading / subinterpreter / deep-recursion
paths. Distinct bug class — multi-mm shadow PT issue, not the same
TLB keystone. Per-mm CR3 changes already flush TLB via the value
change; the residual issue is likely in how clone(CLONE_VM) tasks
share but each need their own TLS / per-task vCPU state.

---

## Pre-keystone notes (kept for historical context)

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

Real bugs fixed this session (each verified against hashlib smoke
+ merge gate; none individually moved parity from 0/21, but
together they make hashlib reliably pass and the dl_main reproducer
moved FAIL→PASS):

  - `1e49886e` T1: `set_ptes()` PFN advancement bug. The mainline
    UML implementation advances PTE PFN by `nr_remaining` pages
    instead of one — under integrated KVM the wrong PFNs propagate
    into the shadow PT. This is the bug that was causing the
    deterministic dl_main rtld.c:1953 fault.
  - `55345b11` T7: always mark `shadow->dirty=true` in
    `kvm_shadow_invalidate_va_range`, not only when cleared > 0.
    Stale-TLB risk when invalidate runs on a range that hadn't
    been lazy-filled yet.
  - `664df08f` T15: `update_pte_range` (and pmd/pud/p4d variants)
    in `arch/um/kernel/tlb.c` marked PTEs uptodate even when the
    backend `ops->mmap`/`ops->unmap` returned an error — leaving
    the host VA / shadow permanently divergent from the pgd. Fixed
    to gate `*_mkuptodate` on rc==0.
  - `171330af` T13: `init_new_context`'s
    `mm_unmap(new_id, 0, STUB_START)` call was passing
    `len=0x7fffffffc000` (~128 TB) to `kvm_mm_unmap`, which
    invalidated the parent's shadow over a huge range and
    potentially destructively munmap'd host VA. Short-circuited
    under KVM_INTEGRATED.
  - `1ba17edf` T17: `kvm_ensure_cpuid_done` failure was silently
    dropped via `(void)` cast.
  - `aa3fedaa` T16: `kvm_shadow_invalidate_va_range` errors from
    `kvm_mm_map`/`unmap` were silently dropped.
  - `062c3f98` T8: freeing a `kvm_shadow_mm` whose `pgd_gpa`
    matched `cached_cr3_gpa` left the SREGS-skip cache pointing
    at freed memory.
  - `05d17152` T20: FSGSBASE exposed in CPUID without CR4.FSGSBASE
    set — guaranteed #UD on first `WRFSBASE`.
  - `e326c75a` T5: `kvm_context_switch` synced `prev->mm` not
    `prev->active_mm`, dropping kernel-thread-borrowed-mm pgd
    mutations on the floor.

What still remains (the real bug that keeps moving downstream
and now manifests as `PyList_Append` or `_PyObject_MakeTpCall`
faults with `self->ob_type == NULL` — i.e. ob_type field of a
live PyObject zeroed at offset 0x8): some path is corrupting
heap memory in early Python startup. Bisect by import:

  - sys, os, collections, pickle, threading: SUCCEED
  - subprocess, json, importlib, unittest: FAIL (different IPs
    and crash sites each time, but always the same pattern of
    NULL pointer dereference at small offset)

Failure correlates with workload size — more memory ops, more
chance to hit the corruption. Suggests a per-N-operations bug
that the audits-on-cached-skip path can't catch. Remaining
tasks worth investigating: full-pgd mm_id passthrough (T2);
FPU/XSTATE save/restore around context switches (T3);
bidirectional shadow audit (T6); fill_lock missing (T11);
turnstile missing (T12).

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
