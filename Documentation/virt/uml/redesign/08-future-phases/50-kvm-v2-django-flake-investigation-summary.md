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
