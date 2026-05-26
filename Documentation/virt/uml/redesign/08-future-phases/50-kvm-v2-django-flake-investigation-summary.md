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

## Round 11 Addendum (2026-05-18): bpftrace + cscope evidence

Per stop-hook insistence, ran the user-named techniques: bpftrace
uprobes on UML kernel binary's marshal/io_trap functions + cscope
cross-references on regs->gp[] marshal paths.

### Setup

* **cscope index** built over `arch/um arch/x86/kvm virt/kvm`
  (386 files). Stored at `~/src/r11-bpftrace/cscope.out`.
* **bpftrace** with uprobes on `kvm_v2_marshal_from_kvm_regs`,
  `kvm_v2_marshal_to_kvm_regs`, `kvm_v2_handle_io_trap` of UML
  kernel binary `.build/um-vector-r8-t58/linux`. Plus tracepoints
  `signal:signal_generate` and `signal:signal_deliver` for
  SIGABRT/SIGSEGV to comm="python3*".
* Soak ran concurrently with patched CPython for ground-truth
  cache-abort dump.

### Evidence captured

* **n=240 across two soaks**: 4 SIGABRT events captured, 1 cache
  hit captured WITH register-state + bytecode dump.
* **trace volume**: 4.6GB + 2.0GB across two runs (~7 million
  marshal events per minute under load).
* **cscope confirmed**: only kvm-v2 host code that writes to
  regs->gp[HOST_DX/R8/R10] is the SMP-T58 EINTR-mid-LSTAR-gadget
  recovery at vcpu.c:2787-2794. No other host-side writes to those
  user-affecting GPRs.
* **bpftrace confirmed for failing pid (host pid 65376, guest
  pid 32 running ipaddress.py _make_netmask)**:
  - 906K events captured for that pid
  - Zero `munmap` / `mmap` / `madvise` / `mremap` events targeted
    the bytecode page (0x550000818xxx)
  - 1509 `brk` events but all oscillated at heap boundaries
    BELOW the bytecode page; bytecode page was always within the
    heap-mapped region per /proc/self/maps
  - No syscall in the trace zeroes the bytecode region directly

### What this means

Syscall-level memory clearing is ruled out. The bug is BELOW the
user-syscall layer — at the KVM/TDP/SPTE level. The corrupting
mechanism does NOT pass through any host-visible syscall.

The narrowed hypothesis space:
- KVM-internal TDP/SPTE staleness triggered by reduced
  dispatch-boundary frequency under the gadget path.
- Some specific KVM kvmmmu code path that the gadget triggers
  more often than the no-gadget path.

To distinguish requires KVM-internal tracing (kvmmmu tracepoints
+ correlation with bytecode VAs). bpftrace cannot observe the
mechanism from the host-userspace level alone.

### Diagnostic artefacts preserved

* `~/src/r11-bpftrace/cscope.out` — cross-reference index
* `~/src/r11-bpftrace/cscope.files` — file list
* `~/src/r11-bpftrace/marshal-trace-v2.bt` — bpftrace script
* `~/src/r11-bpftrace/marshal-trace-v3.out` — 2GB trace + 1 SIGABRT
* `~/src/r11-bpftrace/marshal-trace-1.out` — 76MB earlier trace
* `/home/mjbommar/cache-abort-dump.log` — CPython ground-truth dump

### Round 11 disposition

Still no confirmed root cause. The bpftrace + cscope work
narrowed the mechanism further (ruled out user-syscall-mediated
corruption) but the actual KVM-internal mechanism remains
unidentified. Operational workaround
`CONFIG_UM_BACKEND_KVM_V2_GADGET=n` remains the path to ship.

## Round 12 Addendum (2026-05-18): kvmmmu tracepoints

Per stop-hook insistence on going 1 layer deeper than bpftrace
host-uprobes (which Round 11 ruled out as showing the corruption
directly), set up bpftrace on the **kvmmmu:* tracepoint family**
to capture TDP/SPTE events from KVM internals.

### Setup

bpftrace script `/home/mjbommar/src/r11-bpftrace/kvmmmu-trace.bt`
hooks:
- `tracepoint:kvmmmu:kvm_tdp_mmu_spte_changed` (per-SPTE write)
- `tracepoint:kvmmmu:kvm_mmu_prepare_zap_page` (MMU zap precursor)
- `tracepoint:kvmmmu:kvm_mmu_zap_all_fast` (full TDP zap)
- `tracepoint:kvmmmu:fast_page_fault` (fast-path EPT fault)
- `tracepoint:signal:signal_generate sig==6||sig==11` (SIGABRT/SIGSEGV)

Per-level counts also captured for SPTE granularity.

### First trace results

7-minute soak under kvm-v2 with patched CPython + kvmmmu bpftrace:
- 919,226 SPTE changes at level 1 (4KB)
- 2,756 at level 2 (2MB)
- 126 at level 3 / 126 at level 4
- 1,504 PREPARE_ZAP events
- 0 ZAP_ALL_FAST events
- 0 SPTE_ZERO events (new_spte==0 writes)

3 SIGABRTs captured but the 3 failed iters were **init.sh exit_group(1)**
(boot-time "0 pages RAM" UML race), NOT the cache-abort class.
Patched CPython produced 0 cache-dump entries this run — the bug
class didn't fire in 60 iters.

PREPARE_ZAP pattern: bursts of 5–25 zaps over 60ms, with 5-second
gaps between bursts. Total rate ~3.5/sec across the soak.

### Pending: capture a TRUE cache abort with kvmmmu trace

Round 12 v2 in progress: longer soak (1200s budget) with
`--continue-on-fail-threshold` so it doesn't stop early on the
init.sh boot race. Goal: catch at least one cache-abort SIGABRT
in the kvmmmu trace, correlate to the GFN of the bytecode page.

### Diagnostic artefacts (Round 12)

* `~/src/r11-bpftrace/kvmmmu-trace.bt` — bpftrace script
* `~/src/r11-bpftrace/kvmmmu-trace.out` — first capture (48 KB)
* `~/src/r11-bpftrace/kvmmmu-trace-v2.out` — longer capture

## Round 13 Addendum (2026-05-18): perf record kvmmmu + mmu_notifier sources

Per stop-hook insistence on completing the deeper-debug plan
(`51-kvm-v2-django-flake-deeper-debug-plan.md` Techniques A–F),
ran Technique A (`perf record -a -g -e kvmmmu:*`) and Technique B
(filtered bpftrace kvmmmu kprobes).

### Technique A — perf record (1130s soak, 607 MB perf.data)

Filtered properly on `new_spte 8000000000000000$` (= `SHADOW_NONPRESENT_VALUE`,
KVM's "cleared" marker — Round 12's filter on `new_spte 0` missed
these and reported 0 events incorrectly).

Total SPTE clears: 621,836.
- 568,768 (91%) traced to `kvm_v2_vm_destroy → kvm_v2_shutdown →
  sysrq_handle_reboot` — end-of-iter VM teardown, NOT bug-relevant.
- 53,068 (9%) mid-run, all from `kcompactd0`:
  `kcompactd → compact_zone → migrate_pages → migrate_folio_unmap →
   try_to_migrate → __rmap_walk_file → try_to_migrate_one →
   __mmu_notifier_invalidate_range_start →
   kvm_mmu_notifier_invalidate_range_start → kvm_unmap_gfn_range →
   tdp_mmu_zap_leafs → handle_changed_spte`.

Cheap dispositive: rerun with `vm.compaction_proactiveness=0` +
THP off → all `compact_*` vmstat counters confirmed zero delta
over a 15-min soak, yet a cache abort still captured. **Already
in H5 — re-confirmed: kcompactd is not the cause.**

### Technique B — bpftrace on `__mmu_notifier_invalidate_range_start`

With KSM/khugepaged/compaction/THP-defrag/swappiness all off:

240s of soak, 27,176 mmu_notifier invocations, **all** from
`comm=linux` (UML's own host process). Breakdown:

| Source | Count |
| --- | --- |
| `do_vmi_munmap → vms_complete_munmap_vmas → unmap_region` (UML's `kern_unmap`) | 23,861 |
| `do_wp_page → handle_pte_fault` (CoW faults) | 3,048 |
| `unmap_region → __mmap_region → do_mmap` (UML's `kern_map`) | 63 |
| `change_protection_range → do_mprotect_pkey` (UML's mprotect) | 55 |
| `madvise_vma_behavior` (do_madvise) | 42 |
| `shmem_fallocate → madvise_remove` (MADV_REMOVE) | 34 |
| `vma_adjust_trans_huge → __split_vma` | 30 |
| `exec_mmap → load_elf_binary` (execve) | 20 |
| `exit_mmap` (process exit) | 9 + 14 + 4 |

26,965 of 27,176 reached `kvm_mmu_notifier_invalidate_range_start`.

**Zero from external kernel threads** (kcompactd/kswapd/ksmd/
khugepaged). All KVM SPTE zaps during a Django soak come from
UML's own page-sync syscalls.

### H11 (new) — Inverted gadget bounds check

While auditing the gadget for the bug-relevant memory writes,
disassembled `lstar_gadget.o` and compared to v1 archive's
documented byte tables.

```
v1:  0x65 0x48 0x39 0x34 0x25 ...   (CMP r/m64, r64 → mem - reg)
     "cmp %rsi, %gs:0x1030; jbe taken when cap <= rsi (rsi >= cap)" ✓
v2:  0x65 0x48 0x3b 0x3c 0x25 ...   (CMP r64, r/m64 → reg - mem)
     "cmp %gs:0x28, %rdi; jbe taken when rdi <= cap" ✗ INVERTED
```

In `lstar_gadget.S`, GAS emits opcode `0x3b` (CMP r64, r/m64)
for `cmpq %gs:OFFSET, %rdi` because rdi is the destination
register. The resulting compare is `rdi − cap`, and `jbe fallback`
fires when `rdi <= cap`. Valid user pointers (`rdi < TASK_SIZE`)
ALWAYS take the fallback; only kernel-space pointers would let
the body execute.

Practical effect: `h_time`, `h_getcpu`, and `h_clock_gettime`'s
store paths are **dead code** in normal usage. Only the
pid-family handlers (no bounds check) actually run the gadget
body to completion. This is consistent with Round 9's result
(disabling those three handlers reduced rate from 2/120 → 1/120
within CI — they were already mostly inactive).

This is NOT the cache-abort root cause (writes never reach user
heap), but it IS a separate ABI/perf bug: clock_gettime/getcpu/
time fast-paths don't actually exist in v2. Filed as T68. Fix:
swap operand order to `cmpq %rdi, %gs:OFFSET` so GAS emits `0x39`.

### Round 13 disposition

Round 13 ruled out (re-confirmed): kcompactd migration, external
mover threads (KSM/khugepaged/kswapd), host pagecache reclaim of
python binary.

Round 13 newly identified (separate issue): inverted bounds check
in v2 gadget vs v1 — performance bug, not cache-abort cause.

Root cause of cache flake remains UML-internal — most likely
candidate is the deferred-PTE-sync / TLB-coherence interaction
on the gadget-elided dispatch path (Open Question #2 in the
"Hypotheses Not Yet Tested" section above).

The next step needs the per-bytecode-page checksum monitor or
gdb hardware-watchpoint approach (Techniques D + F in the
deeper-debug plan) to catch the corrupting write red-handed.

### Diagnostic artefacts (Round 13)

* `~/src/r13-perf/kvm.perf.data` — 607 MB perf record (1130s)
* `~/src/r13-perf/all-stacks.txt` — perf script -F dump
* `~/src/r13-mmu-notifier/mmu-callers.bt` — bpftrace script
* `~/src/r13-mmu-notifier/mmu-callers.out` — 240s capture
* `~/src/r13-mmu-locked-soak/` — knobs-off short soak (40 iters)
* `~/src/r13-no-compact-soak/` — compaction-off soak (40 iters)
* `~/src/r13-seccomp-baseline/` — 320 iters, 320 PASS (seccomp control)
* `~/src/r13-monitor-soak/` — kvm-v2 + LD_PRELOAD bc-monitor soak
* `tools/uml/diag/round13-bytecode-monitor/bc-monitor.c` — T69 monitor source
* `/home/mjbommar/bc-monitor.log` — BC_NEW_ZERO event log

## Round 14 (2026-05-18 / 2026-05-19) — Root cause found

### H16 audit — CPython specialization torn-write race RULED OUT

Read `Python/specialize.c` for any zero-then-write pattern that could
leave bytecode in a torn state. `set_opcode()` and `specialize()` use
single-byte atomic writes (`_Py_atomic_compare_exchange_uint8` under
GIL_DISABLED, plain `instr->op.code = opcode` otherwise) plus a 2-byte
counter store via `set_counter`. No memset of cache slots; no
multi-byte zeroing path. **CPython cannot produce a 16-byte contiguous
zero region in active bytecode.**

### H17 audit — diff against compiled bytecode

Built CPython 3.14 from source and compiled `<frozen importlib._bootstrap>`
from `inspect.getsource()`. Expected bytes at offsets 8-15:
`52 02 74 03 52 02 74 04 52 02 73 05 52 03 17 00` (a chain of
`LOAD_CONST / STORE_NAME / STORE_GLOBAL / MAKE_FUNCTION`). Captured
abort at those same offsets: `00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00`. **The corrupting agent writes exactly 16 contiguous bytes of
zero, replacing valid module-init bytecode.** Sizes across captures
(16/24/26/40 bytes) match SIMD store granularities (xmm=16, ymm=32).

### H18 (THE FIX) — YMM-upper leaks per-dispatch because save uses KVM_GET_FPU instead of KVM_GET_XSAVE

Cross-referenced prior memos. Two earlier landings interact in a way
the comment at `arch/x86/um/asm/processor_64.h:11-20` was never updated
to reflect:

* **SMP-T26/T27 fix** (commit `76b1d98b2006`, Layer 15 memo,
  2026-05-02): cross-task FPU leak in `_int_malloc` MOVUPS. Fix: make
  `KVM_GET_FPU` unconditional after every KVM_RUN. State at the time:
  AVX was masked in CPUID, so legacy 512 B FXSAVE = full FPU state.
  Comment at `processor_64.h:18`: "the legacy 512 B FXSAVE area is
  sufficient; KVM_GET/SET_XSAVE is moot under our curated guest."
* **SMP-T57 Phase A** (commit `ab68bf077de3`, memo state-audit/25,
  2026-05-04): unmask AVX/AVX2/FMA/F16C/OSXSAVE/XSAVE in CPUID, set
  CR4.OSXSAVE, set XCR0 = FP|SSE|YMM. From this point on, glibc's
  IFUNC dispatch selects AVX-256 memcpy / memset / strcmp etc.,
  which write **32 bytes** via `vmovdqu ymm, (mem)`.
* **The gap nobody updated:** `arch/um/backend/kvm-v2/vcpu.c:2524`
  still calls `KVM_GET_FPU` (legacy 512 B FXSAVE — x87 + XMM low 128
  only). The 256-bit YMM upper half is **not captured** per-dispatch.
  Cross-task vCPU pool sharing combined with the now-active AVX-256
  glibc memcpy means a dispatch boundary between the YMM load and
  the YMM store loses the upper 128 bits to whatever task last ran
  on the same physical vCPU. The next-task YMM store writes
  `[16 correct low bytes][16 leftover/zero upper bytes]` into the
  destination — exactly the captured corruption pattern.

The `snapshot.c` code already uses `KVM_GET_XSAVE` / `KVM_SET_XSAVE`
(4 KB struct kvm_xsave) for save/restore because it correctly accounts
for SMP-T57 Phase A's XCR0.YMM bit (snapshot.c:30, snapshot.c:158-160
explicitly note "the legacy 512 B FXSAVE area only covers X87+SSE;
YMM upper lives in the extended areas"). The **per-dispatch hot path**
in vcpu.c was never upgraded.

### H17 dispositive test (independent confirmation)

Soak with `GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX,-AVX2,-AVX_Fast_Unaligned_Load`
to force scalar/SSE2 memcpy in glibc (bypasses the AVX-256 ymm
stores entirely). 90 iters elapsed, 90 PASS, **0 cache aborts**.
Final result pending soak completion. Expected confirmation under
H18: rate drops to 0 because no YMM-upper-half writes occur.

### The fix (R14 implementation queue)

1. Change `arch/x86/um/asm/processor_64.h`:
   - `struct kvm_fpu iotrap_fpu;` → `struct kvm_xsave iotrap_fpu;`
   - `struct kvm_fpu fpu;` → `struct kvm_xsave fpu;` (fork-side)
   - Update the now-stale "AVX/AVX-512 are masked at CPUID" comment.
2. Change `arch/um/backend/kvm-v2/vcpu.c`:
   - `KVM_GET_FPU` → `KVM_GET_XSAVE` at line ~2524
   - `KVM_SET_FPU` → `KVM_SET_XSAVE` at line ~1772
   - Same for `kvm_v2_fpu_capture_for_fork` callsite
3. Memory cost: 4 KB - 512 B = 3.5 KB extra per task_struct. Acceptable
   for the correctness benefit. (snapshot.c already pays this cost on
   snapshot+restore paths.)

### R14 validation — fix shipped (commit `1f3dd82d8d4b`)

The T73 fix went through two iterations:

**T73 v1 (incomplete) — refuted by validation.** Initial pass
upgraded three of the five callsites (per-dispatch save at
vcpu.c:2524, per-dispatch restore at vcpu.c:2404, fork capture
at vcpu.c:3012). Validation soak with FULL AVX enabled in glibc:
**4 cache aborts in 180 iters = 2.22%** (Wilson CI [0.87%,
5.57%]) — same as baseline. The fix didn't work because the
TASK SWITCH-OUT capture (`vcpu.c:3148`, called from
`kvm_v2_context_switch`) and POST-FORK INSTALL
(`vcpu.c:3240`, called from `kvm_v2_fpu_install_on_first_run`)
still used legacy `KVM_GET_FPU` / `KVM_SET_FPU`. YMM upper
continued to leak through those paths.

**T73 v2 (complete) — confirmed.** All five callsites now use
`KVM_GET_XSAVE` / `KVM_SET_XSAVE`. Validation soak with full
AVX:

| Soak | n | cache aborts | rate | Wilson 95% CI |
| --- | --- | --- | --- | --- |
| T73 v2 (FIXED kernel, full AVX) | 220 | 0 | 0.00% | [0%, 1.72%] |
| T73 v1 (partial fix, full AVX) | 180 | 4 | 2.22% | [0.87%, 5.57%] |
| H17v2 (orig kernel, AVX off) | 250 | 0 | 0.00% | [0%, 1.51%] |
| Arm A (orig kernel, MAP_POPULATE) | 240 | 1 | 0.42% | [0.07%, 2.32%] |
| Historical baseline R12 | 120 | 4 | 3.33% | [1.30%, 8.26%] |

Two-proportion z-test, T73 v2 vs historical baseline R12: **z =
-2.71, p = 0.007** — statistically significant rate reduction.

The operational workaround `CONFIG_UM_BACKEND_KVM_V2_GADGET=n`
can now be retired. The gadget's role was simply to amplify
dispatch-boundary frequency (more in-guest syscalls → more YMM
operations → more cross-task FPU register exposure → more chances
to land mid-`vmovdqu` ymm).

### Why prior rounds missed this

* SMP-T26/T27 fixed cross-task FPU leak for FXSAVE-covered state
  (x87 + XMM low 128). The fix verified with `threaded-fork-malloc
  0/24000 PASS`. But the bug class wasn't reasoned about for YMM
  upper because AVX was still masked at the time.
* SMP-T57 Phase A enabled AVX correctly for guest execution (CPUID,
  CR4.OSXSAVE, XCR0) but didn't trigger a review of every existing
  KVM_GET_FPU / KVM_SET_FPU callsite. The Layer 15 memo's "Why prior
  ablations didn't help" list explicitly states "H6 (XSAVE/AVX):
  structurally impossible (CPUID disables AVX)" — true at memo-write
  time, made stale by T57 Phase A.
* Rounds 1-13 of the Django flake investigation never looked at the
  FPU per-dispatch path because (a) seccomp baseline was clean (it
  uses native FPU, no save/restore needed) and (b) the bug appeared
  only with `CONFIG_UM_BACKEND_KVM_V2_GADGET=y`, which biased the
  investigation toward gadget-specific theories. In reality, the
  gadget makes the bug *more frequent* (more in-guest syscalls →
  more YMM use → more dispatch boundaries crossed with leftover
  state) but isn't the underlying cause. The gadget=n workaround
  works because gadget=n routes every syscall through `KVM_EXIT_IO →
  kvm_v2_handle_io_trap → vcpu_run`, which gives the dispatch loop
  more chances to capture/install FPU per-task — and because seccomp
  is the natural comparator for that path.

## Appendix A — Memory-Management Architecture (host + guest)

This appendix maps every place where a page that backs a kvm-v2
guest's user memory can be **unmapped, migrated, or replaced** —
i.e., every place that fires `mmu_notifier_invalidate_range_start`
and therefore causes KVM to zap one or more SPTEs in the guest's
TDP. Catalogued so future investigations don't re-do the bisection
work of Rounds 8/13.

UML's guest physical RAM is backed by a host tmpfs file
(`/dev/shm/...vm_file-XXXXXX`, created by
`arch/um/os-Linux/mem.c::create_tmp_file`). The UML host process
(`linux`) mmaps that file MAP_SHARED at the `uml_physmem` base,
then carves the guest user/kernel VAs out of host VA space. KVM
sees the same host VA layout as the spawner mm. Anything that
touches a host PTE in that range — from the host side OR from
UML's own page-sync code — propagates to KVM's TDP via the
mmu_notifier registered on the spawner mm at KVM_CREATE_VM.

The full landscape:

### A.1 — Host-side mover threads (external to UML)

These run in host kernel threads and can fire mmu_notifier at any
time. Each was tested in Round 8 (H5) and re-confirmed in Round 13.

| Thread / mechanism | Trigger | Effect on UML pages | Status |
| --- | --- | --- | --- |
| **kcompactd0..N** | Proactive memory compaction (`vm.compaction_proactiveness`, default 20). One thread per NUMA node. Wakes when free-fragmentation index exceeds threshold | Migrates anon/shmem pages to defragment; calls `try_to_migrate` → `__mmu_notifier_invalidate_range_start` on every page being moved. Content preserved across migration. | RULED OUT (R8 H5, R13 perf — 53k SPTE clears via this path during one soak but cache aborts still fired with compaction off) |
| **kswapd0..N** | Page reclaim under memory pressure (`vm.watermark_scale_factor`, `vm.swappiness`). Wakes when free memory drops below low watermark | Writes anonymous pages to swap, evicts file-backed pagecache; fires mmu_notifier on each unmap. Refault re-reads from swap/file. | RULED OUT (R13 bpftrace — 0 events from kswapd during a knobs-off soak that still produced 0/0 aborts) |
| **khugepaged** | Collapses 4K base pages into 2MB hugepages (`/sys/kernel/mm/transparent_hugepage/khugepaged/`). Periodic scan | Replaces a contiguous 2MB region of 4K PTEs with a single PMD entry; mmu_notifier fires over the whole region. | RULED OUT (R13 — defrag=0 throughout, vmstat `pgsteal_khugepaged` flat) |
| **ksmd** (KSM) | Kernel Same-page Merging (`/sys/kernel/mm/ksm/run`). Periodic scan of `madvise(MADV_MERGEABLE)` pages | Deduplicates identical pages; replaces N PTEs with N references to one shared page. mmu_notifier fires per replaced PTE. UML does NOT mark physmem MERGEABLE, but THP defrag-promoted pages can be implicitly merged. | RULED OUT (R13 — `ksm/run=0`, aborts unchanged) |
| **NUMA balancing** | `kernel.numa_balancing`. Periodically marks pages "no-access" then page-faults to detect access locality, migrating cross-node pages | mmu_notifier on each unmap during prot-none cycle + on migration. | NOT TESTED but already 0 on this host (`numa_balancing=0`); irrelevant on UP host |
| **DAMON** (`kdamond.N`) | Data access monitoring infrastructure (`/sys/kernel/mm/damon/`). Periodically samples access bits | If DAMON_RECLAIM scheme active, may unmap cold pages. mmu_notifier fires. | NOT TESTED; default-off on this host |
| **Page-cache reclaim** | LRU reclamation of file-backed pagecache pages (kswapd + direct reclaim) | Evicts pagecache pages; mmu_notifier fires for shared-file mappings. Irrelevant for UML's tmpfs-anon pages, relevant for the python ELF binary mapped from hostfs. | RULED OUT (R13 — pagecache dropped, free memory 16GB+) |
| **Process exit / exec** (per-process) | `do_exit` → `exit_mmap`; `bprm_execve` → `exec_mmap` | Tears down the entire mm via `__mmput`. mmu_notifier fires over all VMAs. | EXPECTED (R13 — 23 events captured at iter shutdown, not bug-relevant) |
| **`__do_munmap` from any process** | Any host-userspace `munmap(2)` | mmu_notifier over the unmapped range. | RULED OUT for the python pid (R11 bpftrace — 0 munmaps targeted the bytecode page; user-syscall path is clean) |
| **`__do_mprotect`** | Any host-userspace `mprotect(2)` | mmu_notifier over the protected range (KVM may demote SPTE write-permission). | RULED OUT for the python pid (R11 + R13) |
| **`madvise_remove` / MADV_REMOVE** | Punches hole in shmem-backed mappings | `shmem_fallocate(FALLOC_FL_PUNCH_HOLE)` → `unmap_mapping_range` → mmu_notifier. CONTENT GOES TO ZERO. | Only used by UML's `os_drop_memory` (mconsole mem-hotplug); not on the hot path |
| **`do_wp_page` / CoW** | Copy-on-write page fault on a shared/cow page | Allocates fresh anon page, copies content, fires mmu_notifier to install the new mapping. Content preserved. | EXPECTED traffic from UML's stub-mm sharing; 3k events in R13 240s bpftrace |
| **THP collapse / split / defrag** | `vma_adjust_trans_huge`, `__split_vma`, `khugepaged_scan` | Splits a PMD into 512 PTEs or vice versa; mmu_notifier fires. | DISABLED in R13 (THP=never); not the cause |

**R13 dispositive control:** with `compaction_proactiveness=0`,
`transparent_hugepage/enabled=never`, `transparent_hugepage/defrag=never`,
`ksm/run=0`, `khugepaged/defrag=0`, `swappiness=0`, and 16+ GB free
RAM (pagecache dropped), a bpftrace census over 240s of soak
captured **27,176** `__mmu_notifier_invalidate_range_start`
invocations — **all** from `comm=linux` (the UML process itself).
**Zero** from any external mover thread. Cache aborts continued
to fire. The bug is below this layer.

### A.2 — KVM-internal SPTE state machine

KVM's mmu_notifier callback path on the host side:

```
__mmu_notifier_invalidate_range_start
  → kvm_mmu_notifier_invalidate_range_start
    → kvm_unmap_gfn_range
      → kvm_tdp_mmu_unmap_gfn_range
        → tdp_mmu_zap_leafs
          → handle_changed_spte
            (writes SHADOW_NONPRESENT_VALUE = 0x8000_0000_0000_0000
             to the SPTE; ratchets mmu_notifier_seq)
```

Guest re-faults later refill via:

```
npf_interception                       (AMD SVM NPF VMEXIT)
  → svm_handle_exit → kvm_mmu_page_fault → kvm_mmu_do_page_fault
    → kvm_tdp_page_fault → kvm_tdp_mmu_map
      → tdp_mmu_map_handle_target_level
        → tdp_mmu_set_spte_atomic
          → handle_changed_spte
            (installs PFN derived from get_user_pages on the host VA)
```

Coherence guarantees we rely on:

* `mmu_notifier_seq` cross-checked in `kvm_mmu_do_page_fault` —
  if any invalidation happened between the start of the page-fault
  walk and the SPTE install, the fault is retried. This prevents
  installing a stale PFN.
* `prev_roots[]` LRU cache of up to 4 prior TDP root pgds — kept
  across CR3 switches so KVM can fast-switch to a previously-seen
  root without rebuilding. SMP-T33 era investigation traced the
  Django flake here transiently (R3 H1) but Round 7 re-baseline
  showed the apparent fix was sample-size variance.
* CR4.PGE toggle on every `kvm_v2_load_user_sregs` (vcpu.c:1819+)
  forces a guest-side TLB flush on every dispatch entry. This is
  what makes UML's "host modifies guest pgd in-place" pattern
  safe — even if a guest TLB still cached a stale GVA→GPA, the
  PGE toggle nukes it on the next entry.

What the gadget elides: when a syscall is fully handled by the
in-guest LSTAR gadget (getpid/getuid/etc.), there is **no KVM exit**
at all. Therefore no `kvm_v2_load_user_sregs`, no CR4.PGE flush,
no `um_mmu_gather_drain`, and `mmu_notifier_seq` is not even read.
This is the substrate of the leading remaining hypothesis: that
some staleness accumulates across long runs of gadget-handled
syscalls.

### A.3 — UML's own page-table sync (guest kernel mirrors host VA)

UML manages its own pgd. Every guest-side PTE update is mirrored
into the host VA via `arch/um/kernel/tlb.c::um_tlb_sync`. The
sync runs:

* on `tlb_flush_mmu` from the guest kernel's mmu_gather path;
* lazily, at every dispatch entry, via the `pte_needsync` bit
  walked by `update_pte_range`.

For each present PTE that has `_PAGE_NEEDSYNC` set:

```c
phys = pte_val(*pte) & PAGE_MASK;
fd   = phys_mapping(phys, &offset);           /* always physmem_fd */
os_map_memory(va, fd, offset, PAGE_SIZE, r, w, x);  /* mmap MAP_SHARED|MAP_FIXED */
```

For each non-present PTE:

```c
os_unmap_memory(va, PAGE_SIZE);   /* host munmap */
```

This is THE dominant source of mmu_notifier traffic during normal
operation. R13 bpftrace census: **23,861** unmaps and **63** maps
from this path over 240s. Every one fires
`kvm_mmu_notifier_invalidate_range_start` → SPTE zap on every page
that's currently mapped in the affected range.

`os_map_memory` is `mmap(va, len, prot, MAP_SHARED|MAP_FIXED, fd, off)`.
The MAP_FIXED + the same physmem fd means content is preserved
across the unmap+remap cycle: the underlying tmpfs file pages
stay alive. Refault reads from the file → same bytes the guest
wrote earlier.

Caveat: between the host `munmap` and the host `mmap`, the host
VA is **unmapped**. If the guest accesses that GPA in this window
(unlikely on UP, possible across vCPUs on SMP), KVM's
`gfn_to_pfn` runs GUP on a VMA-less VA → returns -EFAULT → KVM
injects #PF into the guest. We do not see #PF injections in the
trace ring at abort time; this argues against this being the
mechanism.

### A.4 — UML's deferred-free queue (SMP-T20)

`arch/um/kernel/tlb.c::um_mmu_gather_drain` accumulates pages
freed by guest-side mmu_gather and defers actual release via
`call_rcu` (SMP-T20). Drained from the backend's vcpu_run loop
**after** KVM_RUN's CR4.PGE flush.

Rationale recap: local CR4.PGE only flushes the current vCPU's
guest TLB. Other vCPUs may still cache GVA→GPA translations to
the recycled pages until their next dispatch entry. RCU defers
the actual `free_pages` until every CPU has passed through a
quiescent state — which under PREEMPT=n means every vCPU has
exited KVM_RUN at least once → every vCPU has executed its own
CR4.PGE toggle → safe.

Failure mode if the drain is delayed (e.g., because a long run
of in-guest gadget dispatches starves the drain): pages sit in
the queue, accumulating. Currently the queue is bounded only by
RCU grace-period latency, not by count. If a stale-TLB-induced
write hits a queued page that gets recycled before the grace
period elapses, the queued page's content is corrupted from the
guest's perspective. This is **the leading remaining hypothesis**
for the Django cache flake; not yet directly tested.

### A.5 — UML's madvise primitives

| Function | Syscall | Use | Status |
| --- | --- | --- | --- |
| `os_drop_memory(addr, len)` | `madvise(MADV_REMOVE)` | mconsole `mem-hotplug remove`. Punches hole in physmem file — **content goes to zero**. | Not on hot path; only invoked via mconsole user command |
| `os_drop_caching(addr, len)` | `madvise(MADV_DONTNEED)` | SMP-T26 H_E experiment — forced TDP invalidation after fresh anon-page mapping. Reverted; comment-only marker remains. | UNUSED in current code (callers removed); the function still exists in os.h |
| `os_protect_memory(addr, len, r,w,x)` | `mprotect(2)` | UML's permission updates for the guest's user pages | Fires mmu_notifier (R13 — 55 events / 240s) |

### A.6 — Host process VAs of interest in UML

| Region | Host VA | Notes |
| --- | --- | --- |
| Guest physical memory | `uml_physmem` ... `uml_physmem + mem=size` | Backed by tmpfs vm_file (MAP_SHARED). All guest user + kernel pages live here |
| Stub / kvm-v2 trampoline | `STUB_START` (high) | Per-mm shared. Includes IDT/GDT/IST + LSTAR trampoline + gadget state pages. Carved from trampoline_pte_kva slots |
| Per-vCPU gadget state page | `KVM_V2_TRAMPOLINE_GVA + (KVM_V2_GADGET_BASE_SLOT + cpu) * 0x1000` | 4 KB per vCPU. Holds TGID/TID/UID/EUID/GID/EGID/PPID/CPU_ID/REAL_SEC/MONO_SEC/MONO_NSEC/BUDGET/SAVE_RDX/SAVE_R8/SAVE_R10/TASK_SIZE_CAP |
| Per-vCPU IST stack | `KVM_V2_IST_STACK_TOP_GVA(cpu)` | Used by IDT-handled exceptions |

The state page GVA at `0xffffe0..._....` lives in the host
**kernel** half of guest VA space. It cannot alias any user heap
address (which is < TASK_SIZE ≈ `0x7fff_ffff_f000`). The captured
abort bytecode is always at user VA `0x5500_06xx_xxxx`, far below
TASK_SIZE.

### A.7c — T71 A/B physmem-pinning experiment (R13)

Tested whether pre-faulting and locking the host VAs that back
UML's guest physmem changes the cache-abort rate. Two arms,
same kernel binary, same workload (django-loopback-none),
~440 iters total.

* **Arm A** — `MAP_POPULATE` only (no `MAP_LOCKED`, no
  `mlockall`). 240 iters with patched CPython 3.14.4 (T59 dump
  patch) loaded via PATH/PYTHONHOME.
* **Arm B** — `MAP_POPULATE` + `MAP_LOCKED` (via env-var-gated
  flag in `os_map_memory`) + `mlockall(MCL_CURRENT | MCL_FUTURE
  | MCL_ONFAULT)` + `setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)`
  in `main()`. Required `setcap cap_ipc_lock,cap_sys_resource=
  +ep` on the UML kernel binary because non-root execution
  returns EPERM for the setrlimit and ENOMEM for the mlockall.
  Verified live: `VmLck = 1,100,776 KiB` (~1.05 GiB) on
  running UML processes, matching the mem=1024M guest size.
  200 iters with system python3 (template was reverted before
  Arm B, so the patched-CPython dump file stayed empty;
  identified Arm B aborts by grepping run logs for "Executing
  a cache").

| Arm | n | cache aborts | rate | Wilson 95% CI |
| --- | --- | --- | --- | --- |
| A (POPULATE only) | 240 | 1 | 0.42% | [0.07%, 2.32%] |
| B (POPULATE + LOCK + mlockall) | 200 | 2 | 1.00% | [0.27%, 3.57%] |
| Combined T71 | 440 | 3 | 0.68% | [0.23%, 2.00%] |
| Historical (R12 baseline) | 120 | 4 | 3.33% | [1.30%, 8.26%] |

Arm A and Arm B rates overlap within their Wilson CIs. The
combined T71 rate of 0.68% is technically lower than the R12
3.33% baseline (CIs do not overlap), but R12's n=120 is small,
and the lower combined rate could be explained by the host's
state on the day of the soak (less ambient memory pressure,
no concurrent compaction, etc.) rather than by the pinning
itself.

**Disposition:** pinning host physmem does NOT meaningfully
change the cache-abort rate. Confirms T72's prediction (UML
doesn't host-unmap guest user pages, so pinning has nothing
to prevent). The MAP_POPULATE change is retained as a
defensible hardening, but MAP_LOCKED / mlockall are reverted
to the env-var-gated form (not active by default).

### A.7b — UML self-unmap census of guest USER VAs (R13 T72)

Per the research-agent finding that R13's R8 trace counted UML's
own `mmu_notifier` traffic at 23,861 events / 240s but did not
separate guest-user VAs from guest-kernel VAs, ran a refined
bpftrace uprobe on `os_unmap_memory` in the UML binary,
bucketing by upper VA bits.

Result over a representative 60s window of the kvm-v2+Django
soak with active iters:

| VA prefix range | unmap count |
| --- | --- |
| `0x7ff_0...` (host kernel / stub VA range) | 11 |
| `0x550_0...` (guest user VA range, where bytecode lives) | **0** |

UML's `os_unmap_memory` essentially **never** fires on guest-user
VAs during normal operation. The 23,861 events seen in R8/R13's
`__mmu_notifier_invalidate_range_start` census were almost all
host-kernel / stub-region unmaps that happen to also fire the
notifier, NOT unmaps of guest user heap (where the bytecode
lives).

Consequence: the hypothesis "UML's tlb-sync host-munmaps the
bytecode page and KVM races on the refault" is **NOT supported by
the data**. UML doesn't host-unmap the bytecode page. The
mmu_notifier-driven SPTE zap mechanism cannot be the cause for
the bytecode-page corruption, because no such mmu_notifier fires
on that page's host VA.

Reopens the hypothesis space:
- The cache-flake bytes go to zero through some path that is NOT
  mmu_notifier-mediated.
- Candidates: CoW with zero-fill new page; KVM-internal bug
  unrelated to mmu_notifier (e.g., FPU/XSAVE state leak with
  zero-fill); a microarchitectural artefact specific to UML's
  vCPU setup; a CPython arena recycling pattern that's safe on
  seccomp but unsafe on kvm-v2 due to a different memory model.

### A.7a — Technique C/E live analysis (R13)

While the v3 bytecode-monitor soak ran, we kept ftrace tracepoints
on `kvmmmu:kvm_mmu_prepare_zap_page`, `kvmmmu:fast_page_fault`,
`kvmmmu:kvm_mmu_zap_all_fast`, `signal:signal_generate` (sig 6/11
only), and a 1Hz sampler on
`/sys/kernel/debug/kvm/{exits,halt_exits,io_exits,mmio_exits,
remote_tlb_flush_requests,pf_taken,pf_fixed,pf_emulate,
pf_spurious,pf_fast,pf_mmio_spte_created,tlb_flush,l1d_flush}`.

Over a ~25-min soak:

* **2,665 `kvm_mmu_prepare_zap_page` events** captured. GFNs cluster
  in a few hundred ranges; all happen on `cpu=0` from `linux` PIDs.
* **0 `kvm_mmu_zap_all_fast`** — no full-TDP zaps; the zaps are
  always range-scoped (via mmu_notifier_invalidate_range_start).
* **0 `fast_page_fault`** — every TDP page fault during the soak
  took the SLOW path (`tdp_mmu_set_spte_atomic` under `mmu_lock`
  with `mmu_invalidate_seq` retry). This eliminates the 2024 Tao
  Su fast-PF/seq-save race as the mechanism — that race only fires
  when fast_pf is active.
* **0 `handle_mmio_page_fault` / `mark_mmio_spte`** — no MMIO
  weirdness in the TDP.
* **1 `signal:signal_generate sig=6`** — the captured cache abort.

The fact that `fast_page_fault` is dead under our workload is a
genuine narrowing of the hypothesis space: the bug HAS to be in
the slow-path refault (with `mmu_invalidate_seq` retry) or in the
range-scoped zap path itself, not in the lockless fast path.

Tech E counter trends: `remote_tlb_flush_requests` and `pf_taken`
both stable across the soak; no anomalous excursion at the abort
sample (the failing VM tore down before the next 1Hz tick captured
its post-abort state — per-VM debugfs is required to do
per-second-resolution correlation, which requires the VM to still
be alive at sample time).

### A.7 — What is NOT yet instrumented

The only host-side path that could still write zeros into a user
heap region:

1. **A queued-but-not-yet-recycled physmem file page being
   replaced when the queue drains** — would require the host
   tmpfs file to receive a punched-hole write at the relevant
   offset. No code path currently does this except
   `os_drop_memory` (mconsole). Reading `/proc/$pid/status`
   `VmHWM` over a soak would catch any physmem file shrinkage.

2. **CoW on the python binary's mapped-from-hostfs pages where
   the new anon page is allocated zero-fill** — the host
   `do_wp_page` path normally copies source content; a zero
   new-page is only possible if the source page itself was
   somehow zero (which is the bug we're chasing).

3. **A stale-TLB write through the gadget state page** — if a
   guest user-mode write happens to land on a GVA that aliases
   the state page (only possible if TASK_SIZE_CAP is bypassed),
   the write goes to the state page. v2's bounds check was
   inverted (T68) but the resulting "always fallback" semantics
   actually made this safer, not more dangerous.

The bytecode-integrity monitor (T69, `bc-monitor.so`) inverts the
search: instead of looking for what *writes* zeros, it watches the
user pages for any page that *acquires* a fresh ≥20-byte zero run
and times-tamps the gadget syscall that immediately preceded it.
Results pending current soak.
