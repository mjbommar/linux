# Memo 23 — Plan to ACTUALLY fix the cpython-parity gate to 21/21

**Date:** 2026-04-28
**Goal:** cpython-parity gate at 21/21 single-pass reliably (zero flakes across 100+ trials)
**Current:** Mean 19.4/21 with A.4i applied (uncommitted); Bug B accounts for the residual ~5-10% per-module flake

This memo is the explicit plan. No more "do another diagnostic and report
back". Each phase has concrete deliverables, dependencies, and exit
criteria. Total work: 6-8 weeks if done sequentially; some phases can
parallelize.

---

## Phase 1 — Land A.4i + commit working tree (1 day)

**What it means:** Take the verified gate-improving fix and commit it.
The diagnosis and fix already exist; the only blocker is committing.

**Why necessary:** Gate moves from mean 18.6/21 to 19.4/21 with this
commit alone. Eliminates the worst-case 16-17/21 trials. test_struct
(InterpreterPool subinterpreter test, the most-flaky module pre-A.4i)
becomes clean.

**Concrete deliverable:** Three commits on `uml-redesign-plan`:
1. `arch/um/backend/kvm/{thread.c,kvm_backend.h}`: A.4i — install
   bootstrap pages in shadow PT at PML4[508] (kernel-half), not at
   PML4[0] (user-half). Fixes the US=0 leaf alias that user CPL=3
   walks could hit.
2. `tools/testing/selftests/um/cpython-parity/repros/`: 5 minimal C
   reproducers. `single_dlopen.c` is the 30s deterministic Bug B repro.
3. `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/{21,22,23}-*.md`:
   Investigation memos covering the failed kick attempts (21), the
   bug-class isolation + A.4i diagnosis + Bug B characterization (22),
   and this plan (23).

**Exit criteria:** A.4i in `uml-redesign-plan` HEAD. Single-pass gate run
post-commit shows mean ≥19/21.

---

## Phase 2 — Find and fix Bug B (3-7 days)

**What Bug B is:** Post-A.4i, single_dlopen still flakes ~80%. Failure
signature is `pf_unrecoverable: va=0x80038___ shadow=0x0 EQUAL synced=1`
— the user task's RIP is loaded with a value in
`[0x80000000, 0x80700000)`, all `shadow=0x0`, all `um_pte=0x0`. The user
CPU branches to a never-mapped VA. Diagnostic instrumentation proved
NO actual mappings exist in that range, so this is NOT a use-after-munmap.

**Mechanism (working hypothesis):** glibc's dynamic loader builds an
in-memory link map containing function pointers (GOT entries, symbol
table). One of those pointers is being **served wrong content** by the
KVM backend — probably from some path that returns stale page bytes. The
user code then dereferences the wrong pointer and crashes.

**Phase 2 sub-tasks:**

### Phase 2.1 — Capture the call-site (1-2 days)

Extend the `#PF unrecoverable` handler in `arch/um/backend/kvm/thread.c`
(currently at the `kvm pf_mini_regs` print site) to ALSO:
- Read the top 8 bytes of the user's stack (RSP) — that's the return
  address pushed by the immediately-preceding CALL instruction.
- Disassemble the bytes at `[return_address - 8, return_address)` —
  identifies which call instruction targeted the bad VA.
- Read the user code at `[return_address, return_address + 16)` — shows
  what the call site expected to do post-call.
- Print all of the above so a single failing run gives us the full
  context.

Run `single_dlopen × 30`, capture all 24 failures' new diagnostic. The
return-address pattern across failures will reveal the call site.

**Deliverable:** Memo 22 update with the call site identified by
disassembly. Should localize Bug B to a specific glibc/cpython function
or a specific UML KVM code path.

### Phase 2.2 — Identify which page is corrupted (1-2 days)

Once we know the call site, we know the function pointer that's wrong.
Trace backward:
- Where does that pointer come from? (PyTypeObject field, GOT entry,
  C++ vtable, etc.)
- What page holds it?
- When was that page mapped, and from where?

Add per-page integrity check to that specific VA: read the page bytes
right after dlopen finishes and right before the call. Compare. If
bytes differ between dlopen-time and call-time, we have the smoking
gun.

**Deliverable:** Memo 22 update naming the corrupted page's VA and
the timing of corruption.

### Phase 2.3 — Find the source of corruption (1-2 days)

Three sub-hypotheses to test in priority order:

**H1: KVM_RUN serves wrong page bytes for a specific VA.** Test by:
- Read host VA bytes in UML kernel
- Read same bytes via guest VA from inside the guest
- Compare. If they differ, KVM/EPT/shadow PT has a translation bug for
  that specific VA.

**H2: A separate UML task (helper thread, signal handler) overwrites
the page.** Test by adding a guard byte before/after the corrupted page
and watch for guard-byte changes.

**H3: The page is a hostfs file mmap and the file content was modified
on the host between dlopen and call.** Test by `ls -la` the file
inode mtime mid-run.

**Deliverable:** Root cause for Bug B identified.

### Phase 2.4 — Apply fix (1 day)

Depends entirely on Phase 2.3 outcome. Could be:
- Fix specific kvm_shadow_sync_pte path that loses a bit
- Add missing barrier between page write and KVM_RUN
- Fix MAP_FIXED handling in os_map_memory
- Or something else entirely

**Exit criteria:** `single_dlopen × 100` shows 0 flakes. Full
cpython-parity gate × 10 trials shows 21/21 every time.

---

## Phase 3 — Stage B (TDP + memslots) — STRUCTURAL FIX (4-6 weeks)

**Why do this even after Phase 2?** Because the shadow PT model is
fundamentally fragile. We've found 4 bugs in it (A.4d/A.4e/A.4i +
Bug B); there will be more as workloads get heavier. The TDP path
(KVM walks `mm->pgd` directly) eliminates ~3,500 LoC of shadow PT
machinery and inherits KVM's well-tested invalidation logic via
mmu_notifier.

This is the right long-term shape. Phase 1+2 buys us a working gate;
Phase 3 makes the architecture sustainable.

### Phase 3.1 — Resolve 1GB huge-page blocker (1 week)

**What it means:** Currently `init_mm.pgd` PUD[1] is a 1GB huge page
covering `[0x40000000, 0x80000000)` with US=0. This range overlaps
both UML kernel direct map AND user VAs. Setting guest CR3 =
`__pa(mm->pgd)` triple-faults because user code at CPL=3 hits US=0.

**Two options:**

A. **Move uml_physmem to PML4[256+].** Right shape; matches standard
   x86_64. Touches `arch/um/kernel/mem.c` (uml_physmem allocation) plus
   every `__pa`/`__va` site that assumes the current layout. Substantial
   UML-core change but enables all of Stage B cleanly.

B. **Break the 1GB huge page into 2MB or 4KB pages with proper US
   bits.** Smaller change, walks + rewrites init_mm.pgd at boot. Loses
   some perf (TLB pressure, walk depth). Doesn't address the PML4[0]
   mixed-use issue; Stage B works but per-mm pgds are still cluttered.

**Recommendation:** (A). One-time UML-core surgery for the right end-state.

**Deliverable:** UML boots, user code can access VAs in
`[0x40000000, 0x80000000)` via per-mm pgds without hitting kernel
direct map. KUnit test: assert PML4[0] PUD[1] in `init_mm.pgd` is NOT
a huge page; user mappings work in that range.

### Phase 3.2 — Wire `kvm_mm_map` → `KVM_SET_USER_MEMORY_REGION add` (3-5 days)

**What it means:** Currently UML registers ONE giant memslot at
init covering `[0, physmem_size)`. Every guest user mapping goes
through shadow PT. In Stage B, each `kvm_mm_map` call adds a NEW
memslot for the specific `[virt, virt+len)` range. KVM walks `mm->pgd`
directly and translates GPA via the per-mapping memslot.

**Touch points:** `arch/um/backend/kvm/mm.c` — `kvm_mm_map` adds a
memslot via `KVM_SET_USER_MEMORY_REGION`. Need a per-mm memslot ID
allocator (KVM has 32768 slots; recycle on unmap).

**Deliverable:** Per-mapping memslots active under a `CONFIG_*` knob;
shadow PT path coexists for fallback.

### Phase 3.3 — Wire `kvm_mm_unmap` → `KVM_SET_USER_MEMORY_REGION delete` (1-2 days)

Mirror of 3.2. On unmap, set the memslot's `memory_size = 0` to
delete. KVM's mmu_notifier handles EPT invalidation automatically.

**Deliverable:** Per-mapping memslot deletion working; no leaks.

### Phase 3.4 — Shared kernel-half PGD across all mms (already done — needs verification)

Per memo 20 §3, PML4[256..511] is shared across all UML mms (kernel
direct map, vmalloc, etc.). This was done at task #116. Verify it's
still correct after Phase 3.1's uml_physmem relocation.

**Deliverable:** PML4[256..511] consistent across all mm pgds.

### Phase 3.5 — Per-vCPU IRETQ frame slot in kernel-half (1 day)

The IRETQ frame already lives in PML4[256+] (shadow code at
`lifecycle.c:738`). Under TDP, it needs to be in `mm->pgd` directly,
NOT in shadow. Move the install into `init_new_context` or similar.

**Deliverable:** Per-mm IRETQ frame in `mm->pgd` PML4[256+].

### Phase 3.6 — Side-by-side TDP validation (3-5 days)

Add a `CONFIG_UM_BACKEND_KVM_TDP` build knob. With knob off → shadow PT
(current). With knob on → TDP via per-mm pgd CR3 + per-mapping memslots.

Run cpython-parity gate × 10 with both. Demand 21/21 with TDP. If TDP
diverges, debug; do NOT proceed to deletion until TDP is verified.

**Deliverable:** TDP path verified; matches shadow PT correctness on
21/21 gate.

### Phase 3.7 — Delete shadow PT machinery (3-5 days)

Once TDP is verified:
- Delete `arch/um/backend/kvm/shadow_sync.c` (~487 lines)
- Delete shadow PGD apparatus from `lifecycle.c` (~2000 lines)
- Delete dirty/synced/needs_full_resync state machine
- Delete F12 mutation ring + remaining shadow telemetry
- Delete `kvm_shadow_sync_pte` hook from `arch/um/include/asm/pgtable.h`

Net: ~3,500 LoC removal.

**Deliverable:** Shadow PT gone; TDP is sole path. Gate still 21/21.

---

## Phase 4 — Stabilization (1 week)

### Phase 4.1 — Memslot churn perf gate (2 days)

`test_int` big-int math triggers heavy mmap/munmap. Run it 50 times
under TDP; assert no memslot leak, no slow-down past 2× pre-A.4i.

### Phase 4.2 — Long-running soak (3-5 days)

`make ARCH=um` of the kernel itself, under UML KVM, for >30 minutes.
No divergence, no panics.

### Phase 4.3 — 24h continuous parity gate (1 day setup, 24h running)

Run the cpython-parity gate in a loop for 24 hours. Zero flakes
required. If any failures, diagnose and fix before declaring Stage B
complete.

---

## Phase 5 — Tier widening (gated on Phase 4)

### Phase 5.1 — Tier1: third-party Python libs (1 week)

Run pytest against `requests`, `cryptography`, `numpy` under UML KVM.
Network-free, in-process. Gate: 100% pass = Tier1 ready.

### Phase 5.2 — Tier2: pip install + pytest (1 week)

Full network + tarball + subprocess workflow. Requires
`CONFIG_UML_NET_VECTOR=y` for working loopback.

### Phase 5.3 — Tier3: Django/FastAPI server (1 week)

Real web server over loopback. Validates UML KVM for production-like
workloads.

---

## Architectural changes required (summary)

1. **uml_physmem relocation** (Phase 3.1A): kernel direct map moves
   from PML4[0] to PML4[256+]. Required for clean TDP CR3 swap.

2. **Per-mapping memslots** (Phase 3.2/3.3): one giant memslot →
   per-mmap memslots. Required for KVM's mmu_notifier to handle
   cross-vCPU coherence automatically.

3. **TDP CR3 = mm->pgd**: drop shadow PT; let KVM walk the real
   pgd. Required to eliminate the entire shadow PT race class.

4. **Shared kernel-half PGD** (already done): PML4[256..511] is
   identical across all mms. No change needed but verify under (1).

---

## Decision points

- **Phase 1 → Phase 2 vs Phase 1 → Phase 3:** Phase 2 is faster
  (3-7 days) and may get us to 21/21 with just shadow PT. Phase 3 is
  the structural cure. **Recommend doing both:** Phase 2 first to
  unblock the gate, then Phase 3 for the long-term right shape.
- **If Phase 2 doesn't find a fixable Bug B in 1 week:** skip to
  Phase 3. The structural change in Phase 3 obsoletes Bug B (it
  lives in shadow PT machinery; deleting that machinery deletes the
  bug class).

## Risks

1. **Phase 3.1 (uml_physmem relocation) is invasive.** UML's
   `__pa`/`__va` are pervasive. Estimated 50-200 site changes plus
   careful boot-order verification. Could overrun the 1-week estimate
   to 2-3 weeks if UML's mm code has hidden assumptions.

2. **Phase 3.2 (per-mapping memslots) hits KVM_USER_MEM_SLOTS limit.**
   32768 slots is plenty for typical workloads but a memslot leak
   could exhaust them. Need leak-detection telemetry.

3. **Phase 3.7 (shadow PT deletion) may surface tests/scripts that
   depend on shadow PT internals.** KUnit suite + any out-of-tree
   diagnostic tools need updating.

## Success criteria

- **End of Phase 1:** gate at mean 19.4/21, band 19-20, no worst cases.
  test_struct clean.
- **End of Phase 2:** single_dlopen 0/100 flakes; gate 21/21 across
  10 trials.
- **End of Phase 3:** Stage B in production; ~3500 LoC of shadow PT
  removed; gate still 21/21.
- **End of Phase 4:** 24h continuous gate run, zero flakes.
- **End of Phase 5:** Tier1/2/3 workloads passing under UML KVM.

This is the plan. Phase 1 is ready to execute now.
