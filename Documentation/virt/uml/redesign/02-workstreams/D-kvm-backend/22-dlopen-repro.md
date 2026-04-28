# Memo 22 — dlopen-style repro: minimal UML KVM crash, no Python

**Date:** 2026-04-27 evening
**Prereq:** Memo 21 §Update 4 (A.4f abandoned after 3 failed attempts)
**Source artifacts:** `tools/testing/selftests/um/cpython-parity/repros/`

## Headline

A 75-line C program (5 pthreads, each `dlopen("libm.so.6") → dlsym("cos") →
dlclose` × 2000) **reproduces the cpython-parity flake at 25% per run** under
UML KVM and **0%** on bare metal. cpython is innocent; the bug is in UML KVM's
handling of file-backed PROT_EXEC mmap from sibling threads sharing the mm.

## Reproducers (committed to tree)

| Name | Workload | Bare metal | UML KVM (×20 trials) |
|---|---|---|---|
| `mt_mmap_repro.c` | 5 threads × 20000 anon mmap+verify+munmap | 0/30 fail | **0/20 fail** |
| `mt_dlopen_repro.c` | 5 threads × 2000 dlopen+dlsym+call+dlclose | 0/30 fail | **5/20 fail (25%)** |

The anon-mmap baseline being clean is critical — it rules out generic cross-vCPU
TLB shootdown as the bug class. The flake requires **file-backed mmap**, the
**PROT_EXEC** flag, the **dlopen sequence** (multiple maps per dlopen, ld.so
internal state), or some combination thereof.

## Crash signatures (5 trials, run_4/6/10/16/18)

All five hit `pf_unrecoverable` with `um_pte=0x0 expected=0x0 shadow=0x0
EQUAL synced=1` — UML's mm has no entry, shadow agrees. The user accessed an
unmapped page. Shared landmarks across the regs:

- `0x440008b0` appears as RIP in run_4/6, as `r14` in run_10/16/18 — same
  function involved in all five.
- `0x40041000` appears as `rdi` (1st arg) in run_4/6 and as the faulting
  `va=0x40041000` in run_6 — same buffer pointer is being passed/dereffed.
- Faulting VAs cluster low (0x0, 0x4127, 0x40041000) — NULL deref or near-NULL
  offset, consistent with a freed-and-zeroed pointer field being followed.

The pattern matches a use-after-munmap: thread A is mid-call on a buffer that
thread B's dlclose just unmapped, OR ld.so's internal link-map / ref-count is
read-then-found-stale across the dlopen lock window.

## Why earlier hypotheses were wrong

Memo 21 (A.4f) hypothesized cross-vCPU TLB shootdown. With this repro:

- Anon-mmap workload is clean → TLB shootdown of arbitrary user pages is fine.
- The flake requires the dlopen-specific pattern → the bug is in a more
  specific path: file-backed mmap, PROT_EXEC, ref-counted, or ld.so-shared
  state that crosses thread boundaries.

Three failed A.4f kick attempts make sense: kicks would have only helped if
the underlying issue were generic TLB staleness, which it isn't.

## Possible root causes (to investigate)

1. **File-backed mmap accounting** in `kvm_mm_map` — when two threads on the
   same mm map the same file simultaneously, the shadow PT install for the
   second thread may collide with the first.
2. **PROT_EXEC + W^X** — UML KVM's shadow PT may handle PROT_EXEC differently
   than PROT_READ|WRITE. `kvm_um_pte_to_x86` translation could lose a bit.
3. **ld.so / glibc futex behavior under UML** — if `pthread_mutex_lock` (used
   by glibc to serialize dlopen/dlclose) doesn't actually serialize, both
   threads enter the critical section concurrently. UML's futex implementation
   would be the suspect; could be checked with a futex-only stress repro.
4. **Cross-thread vmas** — when one thread munmaps a vma another thread is
   currently faulting on, the race between mmap_lock release and shadow PT
   teardown could leave a stale shadow leaf pointing at freed pages.

## Strategic value

This repro reduces the iteration cycle from **5 minutes (cpython gate)** to
**30 seconds (single dlopen run)** with **deterministic 25% rate** rather than
a 17–21/21 band that takes multiple trials to characterize. Any future
attempted fix can be validated against `MOD=dlopen N=20 → 0 flakes` quickly.

## Update — orthogonal bisection narrows the bug class

Three additional repros isolate the bug to a single axis:

| Repro | Workload | UML KVM (×10) |
|---|---|---|
| `mt_mmap_repro.c` (multi-thread anon mmap) | 5 thr × 20000 anon mmap | **0/20** |
| `mt_anon_exec.c` (multi-thread anon mmap PROT_EXEC) | 5 thr × 20000 anon mmap PROT_EXEC | **0/10** |
| `mt_file_noexec.c` (multi-thread file mmap PROT_READ) | 5 thr × 5000 file mmap | **2/10 (20%)** |
| `single_dlopen.c` (single-thread dlopen) | 1 thr × 5000 dlopen | **8/10 (80%)** |
| `mt_dlopen_repro.c` (multi-thread dlopen) | 5 thr × 2000 dlopen | 5/20 (25%) |

What this tells us:
- **Multi-thread is NOT necessary.** Single-thread dlopen flakes 80%.
- **PROT_EXEC alone is NOT the trigger.** mt_anon_exec is clean.
- **File-backed mmap IS the trigger.** mt_file_noexec (PROT_READ-only file
  mmap) flakes 20% even with multi-thread.
- **Volume amplifies.** dlopen does ~5 maps per call; that's ~17 shadow PT
  mutations per iteration (mut_dump head_seq=85490 after 5000 iters).
  More mutations per iter → higher per-trial flake rate.

## Refined hypothesis

UML's `os_map_memory` always uses `MAP_SHARED | MAP_FIXED` against a `phys_fd`
(usually UML's own physmem-backing fd). The host doesn't see "anon vs file"
from the guest workload — but the **guest mm path** that drives down to
`kvm_mm_map` differs. File-backed guest mmap goes through hostfs-style
read-into-physmem-page paths plus extra `kvm_shadow_invalidate_va_range` +
re-fill cycles compared to anon mmap.

The flake rate scaling with mutation volume points at **shadow PT
correctness under heavy churn** — likely:

1. `kvm_shadow_invalidate_va_range` racing its own re-install when the same
   VA is invalidated and re-mapped within the same `um_tlb_sync` drain.
2. `kvm_shadow_sync_pte`'s atomic-context fast path missing a barrier when a
   leaf is rewritten with a different physical page (mmap → munmap → mmap of
   the same VA range, which file-backed mmap exercises).
3. Stale shadow leaf retained across the seq-coherency window
   (shadow->invalidate_seq) when a producer re-installs immediately after
   invalidate without yielding to the consumer.

The `cr2=0xffffffff rip=0xffffffff` flake is **strongly indicative** — the
user RIP got loaded with UINT_MAX, which is a freed/uninitialized sentinel
that some glibc internal table likely sets when the dlopen state is being
torn down. The user code dereffed a pointer that should have been refreshed
between dlclose and the next dlopen — meaning the shadow PT served stale
contents (the OLD page's bytes, with the sentinel) instead of the NEW page's
contents (the freshly-installed VA).

## Next investigation step (deterministic 30s/trial loop)

Single-thread `single_dlopen` at 80% flake rate is the fastest deterministic
repro available. With it:

1. Add per-VA logging in `kvm_shadow_sync_pte` and
   `kvm_shadow_invalidate_va_range` for the address range the dlopen-loaded
   .so lands at. Compare what UML installs vs what user reads back.
2. Try **disabling the SREGS-skip-cache fast path** in `kvm_enter_guest`.
   If forcing every entry to do `KVM_SET_SREGS` (full TLB flush) eliminates
   the flake, the bug is in the cache predicate (A.4d's tlb_gen check).
3. Try **forcing `needs_full_resync = true` on every `kvm_shadow_sync_pte`
   call**. If full re-fill on every leaf write eliminates the flake, the bug
   is in the per-PTE incremental sync path (kvm_shadow_sync_pte itself).
4. Both 2 and 3 are coarse "force the slow correct path" diagnostics — neither
   is a real fix, but they narrow the bug to which subsystem owns it.

If neither 2 nor 3 helps, the bug is in shadow PT walk/invalidate at a deeper
level and Stage B (TDP) is the right answer.

## Update — diagnostic 1 (force-full-resync) RESULT: no change

Patched `kvm_shadow_sync_pte` to short-circuit: every PTE write sets
`needs_full_resync=true` + `kvm_shadow_mark_dirty` and returns. This forces
the next `kvm_enter_guest` to perform a full pgd walk + leaf re-fill instead
of relying on the per-PTE incremental update.

Result on `single_dlopen × 10`: **10/10 flakes (100%)**. Same `pf_unrecoverable`
signature (`cr2=0x80038930 rip=0x80038930 ec=0x14`). The bug is **NOT** in the
per-PTE incremental sync path.

What this rules out:
- `kvm_shadow_sync_pte`'s atomic-context fast path
- Per-PTE leaf write barrier ordering
- Hypothesis 2 from above (incremental sync race)

What's left:
- The full-fill code (`kvm_shadow_fill_from_uml_pgd`) — but this is the
  fallback we just forced; if it had a bug we'd see different signatures
- The shadow PT walk at guest fault time (probably OK; same crash sig)
- KVM's own EPT translation (gpa→hpa) — if KVM caches a stale EPT entry
  pointing at the OLD physmem page after `os_map_memory(MAP_FIXED)` overwrites
  the host VA, the guest reads stale data via correct shadow PT but stale EPT
- `os_map_memory`'s `MAP_FIXED` semantics: when host VA X is currently mapped
  to physmem and we do `mmap(X, ..., MAP_FIXED, file_fd, off)`, the host
  kernel atomically swaps the mapping. KVM's mmu_notifier *should* fire and
  invalidate EPT entries pointing at the OLD HPA. If the mmu_notifier doesn't
  fire (or fires too late), the guest sees stale data via cached EPT.

This last point is the strongest remaining hypothesis. The fix would be either:
- Force KVM to re-walk: explicit `KVM_SET_USER_MEMORY_REGION` add/del around
  the mmap (this is essentially Stage B)
- Or find a mmu_notifier hook UML's mm path is missing

## Update — diagnostic 2 (EPT-flush-every-entry) RESULT: no change

Patched `kvm_ensure_memslot` to do `KVM_SET_USER_MEMORY_REGION(size=0)` then
re-add on every `kvm_enter_guest`. Forces KVM to throw away the entire EPT
each entry.

Result on `single_dlopen × 10`: **10/10 flakes (100%)**.

But the failure SIGNATURE changed in an interesting way. Pre-diag2 we saw
`shadow=0x0` (UML mm and shadow agree both empty). Post-diag2 we saw
**`shadow=0xaca021 DIVERGE synced=1`** — the shadow PT has a stale leaf
pointing at kernel page 0xaca000 (gpa) while UML's mm correctly has no
entry. The "synced" flag is True yet they DIVERGE. The user RIP =
`0x60aca4d0` is an attempted instruction fetch into UML's kernel direct map
(uml_physmem at 0x60000000 + 0xaca4d0); ec=0x15 = U + I + P (user-mode
instruction fetch hit a present-but-US=0 page).

Implications:
- EPT is not the staleness layer — flushing it every entry didn't help.
- **Shadow PT itself retains stale leaves across mm operations.** The
  per-PTE sync (`kvm_shadow_sync_pte`) installed entries based on UML's
  pgd at the time of mutation; subsequent unmaps weren't propagated.
- The `synced=1` flag is **lying** — the shadow doesn't actually mirror
  the mm. This is consistent with the F12 (mut_ring) data showing per-PTE
  installs, but no clear matching unmap when the user munmap'd.

## Verdict on shadow PT approach

Three independent diagnostics rule out the obvious culprits:
1. Cross-vCPU TLB shootdown (A.4f, three attempts) — **not the bug**.
2. Per-PTE incremental sync correctness (diag1, force-full-resync) — **not the bug**.
3. KVM EPT staleness (diag2, force-flush-every-entry) — **not the bug**.

The remaining bug is in **shadow PT walk/install/invalidate at a deeper
level than per-PTE — likely the synced/needs_full_resync state machine
itself, or the interaction between full-fill clear+install passes and
concurrent per-PTE mutations.** Patching it further is unlikely to be
productive given the prior three iterations all failed.

**Stage B (delete shadow PT entirely; use TDP via per-mmap memslots) is the
right path forward.** The fundamental architecture of "mirror UML's pgd
into a parallel shadow tree, then keep them sync'd" has accumulated more
edge cases than we can patch incrementally. KVM's TDP path uses the host
mm's actual page tables via mmu_notifier, eliminating the entire shadow PT
concept and its accompanying race classes.

The 1GB huge page at PML4[0] PUD[1] in init_mm.pgd (memo 20 §9.5) is the
known blocker for direct CR3=mm->pgd. Stage B's design memo should pivot
to either:
- Move uml_physmem out of user-VA reach (TASK_SIZE shrink), OR  
- Per-mm host worker process (kvmtool-style address space split)

Both are larger UML-core changes but unblock the structural fix.

## Session summary (2026-04-27)

Three failed kick-shootdown attempts (A.4f v1, v2, v2-with-sigprocmask)
followed by three failed shadow-PT diagnostics (force-resync, EPT-flush,
and the implied control of cross-vCPU TLB) collectively prove the bug is
not in any individual layer we can patch.

**Artifacts produced this session:**
- 5 minimal C reproducers in `tools/testing/selftests/um/cpython-parity/repros/`
  (single_dlopen at 80% deterministic flake, 30s/trial)
- Memo 21: TLB shootdown investigation (3 failed attempts documented)
- Memo 22: dlopen reproducer + 3 diagnostic results (this file)

**Confirmed bug class:**
- File-backed mmap path under heavy churn (mmap → munmap → remap)
- Reproduces single-thread, no PROT_EXEC needed, no cpython needed
- 0% on bare metal, 80% under UML KVM (deterministic)
- The user reads stale page contents (or kernel-half VAs) after a remap

**Confirmed NOT the bug class:**
- Per-PTE incremental sync (`kvm_shadow_sync_pte`) — diag1
- KVM EPT staleness — diag2  
- Cross-vCPU TLB shootdown — A.4f triple-failed
- glibc/cpython internal race — bare metal clean

**Path forward: Stage B.** The shadow PT machinery has accumulated more
edge cases than incremental patching can fix. The 1GB huge page blocker
(memo 20 §9.5) needs to be unblocked via either uml_physmem relocation
or huge-page split. Stage A artifacts (per-task vCPU, audit fixes) remain
committed; Stage B work begins with the huge-page resolution.

## Update — backend comparison closes the case (2026-04-27 evening)

Same UML binary, same workload, just swapped the backend:

| Backend | `single_dlopen × 10` flakes |
|---|---|
| Bare metal (no UML) | 0/30 |
| UML `backend=force=seccomp` | **0/10** |
| UML `backend=force=kvm`     | 8/10 (80%) |

The seccomp backend uses a stub-child host process and ptrace to drive each
mm; it has none of the shadow PT machinery. It runs identical UML kernel
code paths for `set_pte`, `flush_tlb_range`, `kvm_mm_map/unmap` (the
backend dispatch sees seccomp's ops, not KVM's). cpython parity gate's
`s=True/k=False` divergences fire on KVM only because seccomp's mm-arbiter
side is effectively a passthrough.

This **definitively rules out** every UML-core path:
- UML's pgtable hooks (set_pte, pte_clear, pmd_clear, flush_tlb_range)
- UML's mm-arbiter (um_tlb_sync, um_backend_dispatch)
- UML's task model (clone, signals, schedule, exit)
- glibc / dlopen / mmap host-side
- Host kernel mmap / mmu_notifier path

The bug **must be in `arch/um/backend/kvm/`** — the shadow PT walk,
install, invalidate, fill, or seq-coherency logic. Three diagnostics
already isolated it to "not in any single patchable layer." Stage B
(delete shadow PT entirely; let KVM's TDP walk `mm->pgd` directly via
memslots that mirror host VAs) is the structurally correct fix.

## ROOT CAUSE FOUND (2026-04-27 evening, sub-agent diagnosis)

An Opus Explore sub-agent traced every shadow-PT-leaf-writing call site
and pinpointed the source of the `va=0x60aca000 shadow=0xaca021 (US=0)`
DIVERGE entry seen in diag2.

**`thread.c:2287` (`kvm_shadow_map_page` for `kvm_bootstrap_page`).**

`kvm_bootstrap_va` is the host kernel-direct-map VA of `kvm_bootstrap_page`,
allocated via `alloc_page()` (`thread.c:1701-1705`). The page lives in
`[uml_physmem, uml_physmem+physmem_size)` = `[0x60000000, 0x80000000)`
which sits in **PGD slot 0 = user-half** (TASK_SIZE goes up to
`uml_physmem`).

The install at line 2287 writes the bootstrap page into shadow PT at
`kvm_bootstrap_va` with hardcoded flags `KVM_X86_PTE_P` only — **no US
bit**. After the hardware sets the A bit on first walk, the leaf becomes
`0x21` exactly. The PFN matches `0xaca` because
`__pa(kvm_bootstrap_page) = bootstrap_va − uml_physmem`.

The same applies to:
- `thread.c:2296` (IST stack page, `+3*PAGE_SIZE`) — `P | RW | NX`, no US
- `thread.c:2366` (gadget state page, `+1*PAGE_SIZE`) — `P | NX`, no US
- `thread.c:2403` (gadget vvar page, `+2*PAGE_SIZE`) — `P | RW | NX`, no US

All four leaves sit at user-half VAs (PML4[0]) with US=0. The user CPU at
CPL=3 walks PML4[0] for any VA — including `0x60aca___` — and finds the
US=0 leaf → US-violation #PF (`ec=0x15` for instruction-fetch case, `ec=4/6`
for read/write). Result: `pf_unrecoverable: va=0x60aca___ um_pte=0x0
shadow=0xaca021 DIVERGE synced=1`. The `synced=1` flag does not lie —
the fill/clear passes (`lifecycle.c:1338-1342, 1433-1437`) explicitly
**preserve** this 4-page alias range as "design intent."

The `mm.c:204` `kvm_mm_map_collides_kernel` shield silently returns 0
without preventing UML's mm-pgd from being populated by the prior
`set_pte_at`. So `kvm_shadow_sync_pte` overwrites the bootstrap leaf
with a US=1 user leaf transiently, then the next `kvm_enter_guest`
re-installs the US=0 bootstrap leaf, and so on. After enough churn,
the user task's RIP can land anywhere in the 4-page alias window
(via ASLR, function pointer stored in mmap'd page, etc.) → crash.

**Why bare metal and seccomp don't see this:** they don't install a
"bootstrap page" in any per-task page table. Only KVM does this, because
only KVM needs an in-guest IDT/GDT/LSTAR/TSS payload.

**Why dlopen at 80%:** dlopen does many mmap+munmap cycles per call (5+
maps per dlopen). Over 5000 iterations that's ~25,000 mmap/munmap calls,
plenty of opportunity for ASLR or pointer values to land in the bootstrap
alias window. ASLR randomization granularity in UML is small enough that
hit rate ~80% per 5000-iter run.

**FIX**: install the bootstrap pages in shadow PT at a kernel-half guest
VA (PML4[256+], `0xffff800000000000+`), not at `kvm_bootstrap_va`. The
per-mm IRETQ frame already follows this pattern
(`iretq_frame_va_guest = 0xffffc00000000000 | (shadow_ptr_low_24bits)`,
see `lifecycle.c:738`). All five "kernel-half alias" pages (bootstrap
code, IST stack, gadget state, gadget vvar, IRETQ frame) should use a
PML4[256+] guest VA. Update sregs setup (`idt.base`, `tr.base`), MSR_LSTAR,
the IDT handler-address entries (which encode the handler VA), and the
TSS IST pointer to use the new guest VA.

The host-side `kvm_bootstrap_page` allocation stays as is; only the
**shadow PT install VA** + the 4 places that read it for sregs/MSR/IDT
move to PML4[256+]. Estimated diff: ~150 LoC, contained to thread.c and
the ASM constants for IDT.

**This obsoletes the "Stage B is the only path" verdict** above. With this
fix landed, shadow PT may stabilize. Stage B remains the right long-term
strategic shape but is no longer urgent for parity-gate progress.

## Update — A.4i landed, partial fix (2026-04-27 evening)

Implementation complete (~150 LoC across thread.c + kvm_backend.h):
- New constant `KVM_BOOTSTRAP_GUEST_VA = 0xffffe00000000000` (PML4[508])
- All 4 shadow PT installs (`shadow_map_page` calls in `kvm_enter_guest`) now
  use `KVM_BOOTSTRAP_GUEST_VA + offset` instead of `kvm_bootstrap_va + offset`
- All consumers (sregs.idt.base, sregs.tr.base, MSR_LSTAR, kregs.rip,
  IDT-handler-address encodings, IST-stack offset computations) updated
- `kvm_bootstrap_va_get()` now returns 0 (alias preservation becomes no-op
  since bootstrap is in kernel-half, fill walks user-half only)

Result on `single_dlopen × 10`:

| Variant | Flakes |
|---|---|
| Pre-A.4i baseline | 8/10 (80%) |
| **Post-A.4i** | **7/10 (70%)** |

**The DIVERGE pattern is GONE.** All 7 post-A.4i flakes show
`shadow=0x0 EQUAL synced=1` (no stale leaves in user-half). Bootstrap
fix is verified.

But the dlopen flake CONTINUES, with new failure VAs:
- run_1: `va=0x80038930` (rip=cr2, ec=0x14)
- run_4: `va=0x80483940`
- run_6: `va=0x8029c2a0`
- run_8: `va=0x80272350`
- run_10: `va=0x806b4270`

All clustered in [0x80000000, 0x80700000) — just past physmem (which
ends at 0x80000000). All `shadow=0x0` (correctly empty), all `um_pte=0x0`.
The user CPU branches to a completely unmapped VA → PF unrecoverable.

**Verdict: A.4i fixed Bug A (kernel-VA bootstrap leaves) but Bug B
(stale-pointer-into-just-past-physmem) was hiding behind it.** The two
bugs share a class — both manifest as user RIP landing on wrong
addresses — but have different mechanisms.

Hypotheses for Bug B:
1. UML's mm-arbiter occasionally returns mmap addresses that include
   the just-past-physmem range, and glibc/dlopen stores those in its
   link map. Later dereferences crash because the VA is outside both
   user mappings AND kernel direct map.
2. `os_map_memory(MAP_FIXED, fd=physmem_fd)` overflows: when offset+len
   exceeds physmem size, the host VA target falls past physmem's end.
   Subsequent guest VA → gpa → memslot → host VA translation may pick
   up garbage from the unmapped range.
3. Glibc's link map relocation arithmetic produces an off-by-physmem-base
   pointer due to some address-space-layout assumption.

Next investigation: instrument `kvm_mm_map` to log when virt+len crosses
the `[uml_physmem, uml_physmem+physmem_size)` boundary. If hits found in
the failing run, that's Bug B.

## Update — A.4i full gate impact (2026-04-27 evening)

5-trial canonical cpython-parity gate post-A.4i:

| Trial | parity | failed modules |
|---|---|---|
| 1 | 20/21 | test_int |
| 2 | 19/21 | test_decimal, test_bytes |
| 3 | 19/21 | test_set, test_int |
| 4 | 19/21 | test_hashlib, test_float |
| 5 | 20/21 | test_hashlib |

**Mean 19.4/21, band 19-20.** Compared to pre-A.4i baseline (mean 18.6/21,
band 16-21), A.4i delivers:

1. **Eliminates worst cases**: no more 16/21 or 17/21 trials.
2. **+0.8/trial mean** (~4% improvement).
3. **test_struct now fully clean across all 5 trials.** test_struct's
   InterpreterPool-subinterpreter test was the original Bug A trigger
   (memo 22 §"Update 2"); A.4i fixed it specifically.
4. Residual flakes shifted to: test_int, test_hashlib, test_decimal,
   test_bytes, test_set, test_float — all big-data modules with heavy
   GC / dict resize / arena pool churn. All hit Bug B (just-past-physmem
   stale RIP).

**Verdict: A.4i is a real win, ready to commit.** Reduces gate variance
and eliminates the worst-case bottoms. Bug B's residual ~5-10% per-module
flake rate is the next investigation — likely a glibc/cpython internal
pointer that gets stale when its backing mmap is recycled, but the
mechanism isn't yet localized.

Strategically: ship A.4i; instrument `kvm_mm_map`/`kvm_mm_unmap` for
Bug B in parallel; don't pivot to Stage B yet.

## Update — Bug B is NOT a use-after-munmap (2026-04-28)

Added a transient diagnostic to `kvm_mm_map` and `kvm_mm_unmap` to log
any mapping/unmapping operation whose `[virt, virt+len)` range touches
the post-A.4i failure cluster `[0x80000000, 0x80800000)`. Ran 10
single_dlopen trials, 8 failed.

**Result: ZERO actual mappings near the crash VA.** The only diagnostic
hit was the init_new_context bulk-clear (`virt=0 len=0x7fffffffc000`)
which is short-circuited at `mm.c:307` and does nothing. So the crash
VAs are addresses that **were never mapped** in this UML kernel run.

This rules out:
- Use-after-munmap (no maps existed there to begin with)
- mmap address-allocator returning out-of-bounds VAs (no actual mmap
  in that range)

What's left: **the user task's RIP is loaded with a value the user
code never legitimately had.** Sources for such a value:
- Wrong syscall return value: e.g. `mmap` return marshaled as
  sign-extended 32-bit → 0xffffffff80038930, low 32 bits 0x80038930.
- Wrong register restore on KVM_EXIT_IO → IRETQ-frame round-trip:
  if RAX/RIP/RSP get sign-extended or truncated wrongly somewhere.
- glibc's allocator/dlopen reads 32-bit value from a structure that
  should be 64-bit, gets sign-extended.

**Strong evidence**: cr2 values cluster on bit 31 set (0x80000000+) and
one was 0xffffffff (UINT_MAX). Both are 32-bit sign-extension artifacts.
The 4 GB boundary is suspicious.

Next investigation: audit `kvm_regs_to_uml_regs` and `uml_regs_to_kvm_regs`
in `arch/um/backend/kvm/thread.c` for accidental sign-extension or
32-bit truncation of any GPR. Specifically focus on RAX (syscall return
value) and RIP. Compare against seccomp's marshal in `arch/um/os-Linux/skas/`.

Bug B may be a smaller, more local fix than expected — not a structural
shadow-PT issue but a register-marshal sign-extension bug.

## Update — A.4j register-marshal audit: NO sign-extension found

Inspected `kvm_regs_to_uml_regs` (thread.c:2139) and `kvm_uml_regs_to_kvm_regs`
(thread.c:2164). Both use `unsigned long` (64-bit on x86_64) for every GPR
field — no narrow casts. `uml_pt_regs.gp[]` is also `unsigned long[MAX_REG_NR]`.
Replay-path syscall return at thread.c:3393 (`(unsigned long)replay_ret`) where
`replay_ret` is `long*` — preserves 64 bits.

The marshal path is not the bug. Bug B's source is elsewhere:

Refined hypotheses:
1. **glibc/cpython internal pointer corruption** — some data structure
   in user space has a pointer field that's been clobbered. The clobber
   could come from another bug in UML KVM (e.g., copy_to_user serving
   wrong bytes for a specific VA), or from a cpython issue that only
   manifests under UML KVM's timing.
2. **Stack frame return-address corruption** — the user's CALL/RET
   sequence has a return address that's been overwritten with a value
   in [0x80000000, 0x80700000). RSP at crash is in normal user-stack
   range (0x4xxxxxxx, 0x547xxxxx); if a previous CALL pushed a wrong
   return address, RET would jump to it.
3. **Heap pool reuse with stale pointer fields** — cpython's PyMem
   allocator reuses freed memory. If a freed block's pointer field
   doesn't get cleared, later it's mistaken for a live pointer.

Catching this requires capturing the **previous instruction** before
the CALL/JMP that jumped to 0x80038___. That needs either:
- Per-vCPU call-trace recording (intrusive)
- Disassemble at `cr2 - 5` to see what ended up in this branch (only
  works if the previous instruction range is mapped — which it should
  be, since user code was running)

Bug B remains open. A.4j conclusion: the marshal isn't where the bug
lives. Need a deeper diagnostic — likely a sub-agent investigation
of cpython/glibc memory model under UML.