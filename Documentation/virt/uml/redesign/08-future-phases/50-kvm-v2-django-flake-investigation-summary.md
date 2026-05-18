# kvm-v2 Django Flake — Investigation Summary (Rounds 1–10)

**Status:** Mechanism not fully identified. Operational workaround
verified: `CONFIG_UM_BACKEND_KVM_V2_GADGET=n`.

**Date range:** 2026-05-14 → 2026-05-18.

**Top-level disposition:** the bug is in `arch/um/backend/kvm-v2/`,
specifically gated on the LSTAR gadget being enabled. Ground truth
confirmed (CPython patched dump). 10 hypotheses tested and ruled
out. Specific corrupting code path not yet pinned to a single
file:line.

## Problem Statement

CPython 3.14 workloads running under `kvm-v2` backend (UML on x86_64
KVM) intermittently abort with:

```
Fatal Python error: _PyEval_EvalFrameDefault: Executing a cache.
```

Rate: ~1.0–2.5% over n=240 across many builds and config variants.
Workload: `django-loopback-none` (Phase J Tier 3 — Django http.server
loopback). Affects both pin and unpin, both SMP=1 and SMP=2 guest
configurations.

Seccomp backend on the SAME kernel binary running the SAME workload:
30/30 PASS. Bug is unambiguously kvm-v2-specific.

## Ground Truth

CPython 3.14.4 patched at `Python/generated_cases.c.h:1498`
(`TARGET(CACHE)`) to dump bytecode bytes + register state + memory
map at the moment of the abort. Four hits captured across multiple
soak runs.

**Finding: bytecode bytes read as ZERO where real opcodes should be.**

Direct comparison against the source .py file's compiled bytecode:

| Hit | Module                                | Offset | Real opcode (expected) | Dump (observed) | Zero region size |
| --- | ------------------------------------- | -----: | :--------------------- | :-------------- | ----------------- |
| 1   | html/entities.py `<module>`           | 8      | 0x002f (BUILD_TUPLE 0) | 0x0000          | 16 bytes (8 cu)   |
| 2   | `<frozen getpath>` `<module>`         | 12     | (real opcode)          | 0x0000          | 26 bytes (13 cu)  |
| 3   | `<frozen importlib._bootstrap>` _fix_up_module | 12 | (real opcode)         | 0x0000          | 40 bytes (20 cu) |
| 4   | enum.py convert_class                 | 13     | (real opcode)          | 0x0000          | 24 bytes (12 cu)  |

CACHE opcode value = 0 in CPython 3.14. So a `0x0000` codeunit
dispatches to the CACHE TARGET and aborts. The bytes that should be
there are missing — replaced with zero.

All hits happen in **module-init code** (top-level module
statements, frozen modules' init). All hits' bytecode is in **user
heap** (`rwxp`, anonymous): `/proc/self/maps` shows
`550000698000-5500007e7000 rwxp 00000000 00:00 0 [heap]`.

The bug is NOT:
- PEP 659 specialization race
- PC drift in the interpreter
- Cache-slot misinterpretation
- A CPython bug

It is a **real memory corruption**: the bytecode bytes get
overwritten with zeros (or read as zero) at specific addresses
during normal execution.

## Hypotheses Tested

Listed with the round they were tested in.

| # | Hypothesis | Round | Disposition |
| - | --- | --- | --- |
| H1 | TDP `prev_roots[]` staleness on cross-vCPU dispatch (SMP-T33 class) | 3, 4 | Partially supported then falsified — Round 4's lag-gate fix appeared to reduce rate but Round 7 statistical re-baseline showed effect was sample-size variance |
| H2 | CPython 3.14 PEP 659 self-modifying bytecode + KVM SMC coherence | 4, 5 | Falsified — disabling specialization (sys.settrace) eliminates "Executing a cache" but the underlying bytecode corruption mechanism is still there; Round 6 seccomp baseline proves bug isn't architectural |
| H3 | TLB-flush gap (KVM_SET_SREGS doesn't queue guest TLB flush) | 5 | Falsified by code audit — `__set_sregs` already calls `kvm_mmu_reset_context` + queues `KVM_REQ_TLB_FLUSH_GUEST` on every CR4 change (PGE toggle ensures every dispatch) |
| H4 | Per-host-CPU vCPU pool / cross-host-CPU dispatch | 6, 8 | Falsified — `taskset -c 3` pin (forces single vCPU) at n=240: 1.7%; unpin n=240: 1.25%. Wilson 95% CIs overlap. Pool-share is NOT the dominant mechanism |
| H5 | mmu_notifier wiring: UML guest PTE updates bypass notifier | 6, 8 | Wiring is correct by design (notifier registered on spawner mm at KVM_CREATE_VM). PGE toggle compensates for UML's physmem PTE writes. Sub-experiment: disabling `vm.compaction_proactiveness=0` (suppressed kcompactd → 73× reduction in `kvm_unmap_gfn_range`) made rate WORSE, not better — kcompactd is NOT the cause |
| H6 | Icache coherence at pool boundaries (cross-host-CPU L1i pollution) | 8 | Falsified by pin test (same as H4) |
| H7 | EINTR-mid-LSTAR-gadget GPR recovery (T58, analog of T41) | 7, 8, 9 | Real ABI-compliance fix shipped (`bab1d5c54056`). Recovers `SAVE_RDX/R8/R10` from per-vCPU state page when EINTR catches gadget post-entry-save. BUT does not change Django flake rate. Bug not in this path |
| H8 | Heavy-vs-cheap path asymmetry for `kvm_v2_load_user_sregs` | 7 | Falsified — Branch B per-vCPU dispatch counters showed both paths run as designed; Branch A forcing heavy every dispatch didn't fix |
| H9 | Gadget LSTAR scratch handlers (h_time/h_getcpu/h_clock_gettime) writing to wrong user pointer via bypassed TASK_SIZE_CAP | 9 | Partial — disabling all three (`jmp fallback`) at n=120 dropped from 2/120 to 1/120, within CI overlap |
| H10 | Gadget entry-save block writing `RDX/R8/R10` to wrong location via corrupted `KERNEL_GS_BASE` (post-SMP-T56 residual) | 9 (T60 audit) | Definitively falsified — `kvm_v2_handle_io_trap` audit comparing state-page SAVE slots vs `run->s.regs.regs.{rdx,r8,r10}` fired ZERO mismatches across 120 iters (vs 2 cache hits in same run). The entry-save writes always go to the correct state-page location |

## Disposition After Round 10

**What we KNOW:**

1. **Bytecode bytes ARE corrupted to zero** (ground truth from
   patched CPython dump). Not a CPython-internal bug.
2. **Disabling the LSTAR gadget eliminates the bug**:
   `CONFIG_UM_BACKEND_KVM_V2_GADGET=n` produces 0 cache hits across
   120 iters in 3 separate trials. The gadget code path is causally
   involved.
3. **Entry-save block is correct** (T60 audit, 0 mismatches in
   120 iters). The 3 movq writes to `%gs:SAVE_RDX/R8/R10` land at
   the correct host-kernel-visible state-page location.
4. **Pool-share is not the dominant cause** (pin n=240 ≈ unpin
   n=240, both ~1.5%).
5. **kcompactd-driven SPTE invalidation is not the cause**
   (disabling proactive compaction made rate worse, not better).
6. **The scratch handlers alone are not the sole source** —
   disabling all three (`jmp fallback`) still saw 1 cache hit in
   120 iters, within Wilson CI overlap of the baseline.

**What we DON'T KNOW:**

The specific code path that causes the corruption. The bug
requires:
- The gadget to be installed (confirmed by H_disable result)
- A code path that doesn't go through the entry-save (T60 audit
  shows entry-save is innocent)
- A path that runs even when scratch handlers are disabled (H9 result)

The hypothesis space is narrow but the actual mechanism remains
unidentified.

## Operational Workaround

Ship with `CONFIG_UM_BACKEND_KVM_V2_GADGET=n` for now. This
disables the in-guest fast path for `getpid` / `gettid` /
`clock_gettime` / `time` / `getcpu` etc., reverting them to the
full `KVM_EXIT_IO → handle_syscall` host-side dispatch.

Cost: the perf-getpid speedup (~1050× over seccomp on Zen 4) is
forfeited. bench-py (Python startup) likely takes a hit; mt-mini
and substrate gates may shift.

Benefit: 0 Django cache aborts across all soaks tested.

## Investigation Artifacts

* **Diagnostic CPython patch:**
  `tools/uml/diag/round9-cpython-cache-dump/cpython-cache-abort.patch`
  — applies to CPython 3.14.x's `Python/generated_cases.c.h:1498`.
  Captures bytecode bytes + 8 GPRs + /proc/self/maps line at the
  abort moment. Output: `/home/mjbommar/cache-abort-dump.log`.
* **Captured aborts:**
  `tools/uml/diag/round9-cpython-cache-dump/cache-abort-dump.log`
  — 4 hits with full context.
* **T60 kernel audit:** SAVE-slot mismatch detector at
  `arch/um/backend/kvm-v2/syscall_trap.c:2170+` (commit
  `74a005fc4a15`). Cheap on success path, freezes state-trace
  ring + ratelimited pr_emerg on mismatch.
* **T58 GPR-recovery fix:** EINTR-mid-LSTAR-gadget recovers
  user `RDX/R8/R10` from state-page slots (commit `bab1d5c54056`).
  Real ABI-compliance fix even though it doesn't close the Django
  flake.

## Round-by-Round Memo Chain

| Round | Commit | Focus | Result |
| ----- | ------ | ----- | ------ |
| 1   | (early observation) | First Django flake observation | rate ~3-6% |
| 2   | `ef817423ab85`   | Regs leak hypothesis | Ruled out |
| 3   | `ceacd0d1d890`   | Regs-leak NEGATIVE | Confirmed |
| 4   | `d6042361e13b`   | TDP `prev_roots[]` lag-gate fix | Apparent ~13× rate drop |
| 5   | `89bc9797de82`   | TLB flush audit; CPython PEP 659 confirmed as trigger | Ruled out architectural fix |
| 6   | `56b9f2f6b7ad`, `f24078123c6a` | Backend-specific (seccomp 30/30), Heisenbug trace overhead | Confirmed kvm-v2-specific |
| 7   | `84855aff33d5`, `4756d69069e5` | Heavy-vs-cheap path counters | Path asymmetry falsified |
| 8   | `1c1b9bd96bce`   | Pool-share pin + mmu_notifier audit + kcompactd | All four original hypotheses falsified |
| 9   | `bab1d5c54056`, `5b92264186c4` | T58 GPR recovery + CPython ground-truth dump | T58 doesn't close; bytecode-corruption confirmed |
| 10  | `74a005fc4a15`   | T60 gadget SAVE-slot audit | Entry-save innocent; GS_BASE corruption ruled out |

## Falsifiable Next-Round Hypotheses (Not Yet Tested)

1. **Bytecode corruption is in the TAIL block** — `movq %gs:SAVE_*,
   %rN; swapgs; sysretq`. The swapgs and sysretq are atomic, but
   if EINTR catches between the slot-restore and swapgs, KVM
   captures a state where `RDX/R8/R10` are user-correct but
   `GS_BASE = STATE_GVA`. On resume after EINTR rewind, the gadget
   re-runs from LSTAR, entry-swapgs swaps `GS_BASE` (=STATE_GVA) ↔
   `KERNEL_GS_BASE` (= STATE_GVA per SMP-T56 fix). Result:
   `GS_BASE = STATE_GVA` (correct), `KERNEL_GS_BASE = STATE_GVA`
   (same). Gadget body runs normally. At exit-swapgs, swap again:
   `GS_BASE = STATE_GVA`, `KERNEL_GS_BASE = STATE_GVA`. User
   resumes with `GS_BASE = STATE_GVA` instead of user-normal-0.
   User reads `%gs:0` → reads from STATE_GVA (kernel page, U/S=0)
   → page fault. Not zeros, so likely not this.
2. **bpftrace + ring-buffer history**: log every
   `kvm_v2_marshal_from_kvm_regs` + `kvm_v2_load_user_sregs` +
   `kvm_v2_handle_io_trap` to a per-CPU bpftrace map. When CPython
   aborts (caught via `tracepoint:signal:signal_deliver` for
   SIGABRT to comm="python3"), dump the last N entries. May reveal
   a marshal pattern that only happens before failures.
3. **CPython compiled with `--with-pydebug` + gdb scripting**:
   break at `_PyEval_EvalFrameDefault`'s
   `case TARGET(CACHE)` (post-patch, in gdb), examine the frame's
   `instr_ptr`, scan the bytecode page for the corruption
   boundary, dump the `Py_BUILDVALUE` / specialization counter
   state, walk the page table via gdb's `info proc mappings` to
   correlate physical pages.
4. **Per-byte forensic comparison**: at module load, compute
   SHA256 of every code object's `co_code`. At every function
   entry, recompute. First mismatch identifies the exact frame
   and the exact write that occurred.
5. **Audit gadget exit paths that ARE NOT fallback**: every
   gadget invocation that reaches the `tail` block goes through
   `swapgs + sysretq` — neither of which writes to memory. But
   the tail also runs the 3 `movq %gs:SAVE_*, %rN` restores. If
   GS_BASE was wrong at tail (perhaps GS_BASE swap on entry was
   different than KERNEL_GS_BASE at exit-swapgs), the restores
   read from wrong location. The `regs` would have wrong values
   but no memory write. So not this directly either.
6. **System call interception via a sleeping kprobe in
   `__do_munmap` / `__do_madvise`** scoped to UML's pid, logging
   addresses. If UML's deferred-flush queue ever drains a
   misaligned or out-of-range op against pid 32 (the python),
   captured.

## Open Question

Why is bytecode corruption gadget-dependent if the gadget's writes
(entry-save, body, restore, exit-swapgs+sysretq) are all
demonstrably correct under audit? Either:
- A non-write code path of the gadget triggers downstream
  corruption (cache-coherency, TLB, prefetch).
- The gadget's effect on the dispatch timeline (fewer dispatch
  boundaries → less frequent `um_tlb_sync` drains → accumulation
  of deferred PTE updates somewhere in UML's mm) causes mm
  staleness that manifests as zero-fill.
- A code path I haven't audited yet.

The next round needs the bpftrace + CPython-debug-with-gdb
tooling I keep promising but haven't actually wired up.
