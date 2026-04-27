# Stage B — Memslot-per-mm + TDP/EPT design

Author: Stage A team
Date:   2026-04-27
Status: Design memo, pre-implementation

Inputs:
- `03-architecture-review-2026-04-27/00-synthesis.md` (the Stage A→B→C plan).
- `03b-memslot-policy.md` (current Policy A: single physmem memslot).
- Empirical state post-Stage A: cpython-parity ~75% per-trial, single-mm.
  test_pylong_roundtrip_huge (the heaviest-memory module) is the
  single most reliable repro of remaining shadow-PT staleness.

## 1. Objective

**Eliminate the shadow page table.** Replace the multi-writer
`kvm_shadow_mm` apparatus (lazy fill, direct sync, range invalidate,
dirty/synced/needs_full_resync state machine, F12 mutation ring,
~3-4k LOC) with KVM's hardware Two-Dimensional Paging (TDP/EPT).
KVM walks `mm->pgd` directly via Stage 2 page tables; UML's pgd IS
the guest CR3 root.

This closes Race classes B (direct-sync vs full fill), C (host-VA
aliasing via os_map_memory), and D (set_pte_at lock-free vs fill)
from the memo-19 playbook. Race A (singleton vCPU) and Race E
(per-mm IRETQ frame collision) are already closed by Stage A.

After Stage B, the KVM backend has zero multi-writer correctness
windows on the memory-management bridge.

## 2. The shape of the change

```
PRE-STAGE-B (today):                   POST-STAGE-B:

UML pgd ───┐                           UML pgd  ───────► KVM TDP/EPT
           │                                              walker
           ▼                                              │
  shadow_mm.pgd ─► KVM walks                              ▼
   (mirror tree)   shadow as CR3                    physical pages
           │
           ▼
   memslot covers physmem
```

Three structural shifts:

1. **`mm->pgd` becomes guest CR3.** No mirror tree. No
   `shadow_sync_pte`, no `shadow_invalidate_va_range`, no
   `shadow_fill_from_uml_pgd`. UML's logical pgd IS what the guest
   CPU walks.

2. **Memslots become per-mm and per-mapping.** Today: one giant
   memslot covers `[uml_physmem, uml_physmem+physmem_size)` (Policy
   A). After Stage B: memslot per `kvm_mm_map(virt, len, phys_fd,
   offset)` call, registered via `KVM_SET_USER_MEMORY_REGION`.
   Deregistered (or split) on `kvm_mm_unmap`. KVM resolves
   guest-VA → memslot-userspace_addr → host PFN through TDP.

3. **Kernel-half (PML4 slots 256..511) is shared across mms.**
   Allocated once at `kvm_init`. Bootstrap page, IST stack, gadget
   state, gadget vvar, IDT/TSS, IRETQ-frame staging — all live here.
   Each mm's pgd PML4[256..511] = copies of the shared template.
   Per-vCPU IRETQ frame slot is addressed by vcpu_id.

## 3. Memslot management strategy

KVM supports up to `KVM_USER_MEM_SLOTS` (32k on modern Linux x86_64)
distinct memslots per VM. UML processes typically have 50-500
distinct user-VA mappings (per `cat /proc/$$/maps`); even a heavy
Python or browser stays well below 1k. Cap is not a concern in the
common case.

### 3.1 Granularity

**One memslot per `kvm_mm_map` call.** This is the natural unit:
each call maps `(virt, len)` user-VA range to `(phys_fd, offset)`
backing storage. A KVM memslot is exactly the same shape:
`(guest_phys_addr, memory_size, userspace_addr)`. Direct
correspondence.

### 3.2 Coalescing

When two adjacent `kvm_mm_map` calls produce contiguous user-VA
ranges with contiguous backing storage, coalesce into one memslot
(via `KVM_SET_USER_MEMORY_REGION` extending the existing slot).
This keeps the slot count well below the cap on programs with many
small mmap()s (e.g. Python with many .so files, each producing a
file-backed region).

Heuristic: coalesce when `prev_slot.guest_phys_addr +
prev_slot.memory_size == new.virt` AND
`prev_slot.userspace_addr + prev_slot.memory_size == backing_va`.
Coalescing is opportunistic; never required for correctness.

### 3.3 Splitting

`kvm_mm_unmap(virt, len)` removes `[virt, virt+len)` from a
memslot. Three cases:

- Full slot match: deregister via `memory_size=0`.
- Range at slot start: shrink the slot's `guest_phys_addr` and
  `userspace_addr` upward.
- Range at slot end: shrink `memory_size`.
- Range in the middle: split into two slots covering the surviving
  prefix and suffix.

The per-mm slot bitmap tracks which slot IDs are in use; freed IDs
go back to the pool. KVM's slot-id space is per-VM, but UML uses
one VM for all mms — so we manage the bitmap globally.

### 3.4 Overflow handling

If `slot_count >= KVM_USER_MEM_SLOTS` (pathological mm
fragmentation), fall back to a "shared overflow slot" covering all
of `uml_physmem`. The few overflowing user-VA ranges in the affected
mm get TDP entries through the overflow slot's GPA mapping. Cost:
overflow ranges incur a guest-VA → physmem-GPA translation step
(via UML's pgd encoding the GPA in the leaf), losing the
single-walk benefit. Tracked via a per-mm `overflow_slot_count`
counter for telemetry.

This is rare (UML processes don't typically hit 32k mappings) but
the path must exist and be tested.

### 3.5 Per-vCPU CR3 selection

Today: `KVM_SET_SREGS` writes `mm->shadow_pgd_gpa` as guest CR3.

After Stage B: `KVM_SET_SREGS` writes `__pa(mm->pgd)` as guest CR3.
TDP walks the user pgd directly. CR3 changes (cross-mm context
switch) trigger natural KVM TLB flush via the `mmu_reset_needed`
detection in `__set_sregs_common`.

Per-vCPU `cached_cr3_gpa` (already in struct kvm_vcpu_handle from
Stage A) tracks the last-programmed CR3 for the vCPU's skip-fast-
path predicate. With shadow PT gone, the only TLB-flush trigger is
CR3 change — same-CR3 entries don't need the CR4.PGE-toggle hack
that Stage A retains as defensive code.

## 4. Kernel-half shared PGD

### 4.1 Layout

PML4 slots 256..511 (upper half) cover canonical kernel
`0xffff_8000_0000_0000 .. 0xffff_ffff_ffff_ffff`. Allocate one
shared PUD/PMD/PT tree at `kvm_init` time covering all kernel-half
mappings:

| Range                          | Use                       |
|--------------------------------|---------------------------|
| `0xffff_8000_0000_0000`        | Bootstrap page (LSTAR/IDT/TSS) |
| `+ 0x1000`                     | Gadget state (per-mm refresh) |
| `+ 0x2000`                     | Gadget vvar                |
| `+ 0x3000`                     | IST stack page             |
| `0xffffc000_0000_0000` + vcpu_id*PAGE_SIZE | Per-vCPU IRETQ frame |

Per-vCPU IRETQ-frame slot is one page per vCPU, indexed by
`vcpu_id`. With KVM_MAX_VCPU_IDS=4096, this carveout needs 16 MiB
of guest VA — cheap.

### 4.2 mm attach

When a new UML mm is created (`kvm_mm_attach`), copy PML4 entries
[256..511] from the shared template into the new mm's pgd. After
this, the new mm sees all kernel-half mappings without per-mm
mapping work.

### 4.3 Why kernel-half sharing matters

Pre-Stage-B, every entry to `kvm_enter_guest` does
`kvm_shadow_map_page` for the bootstrap page, gadget state, gadget
vvar, IRETQ frame — that's ~5 idempotent writes to the per-mm
shadow PT, every entry. With Stage B's shared kernel-half, those
mappings are programmed once at `kvm_init` and inherited by every
new mm. Saves ~5 producer events per `kvm_enter_guest`.

## 5. Validation strategy (side-by-side gating)

Per the synthesis section 3.B.a, ship Stage B as a **dual-path**
implementation first:

1. **B.1 — TDP path computed alongside shadow path.** Both are
   produced on every `kvm_enter_guest`; the shadow path is the
   production source of truth. The TDP path's leaf-PTE walk for
   any `cr2` (or random sample VAs from the active mm's vmas) is
   compared against the shadow path's. Divergences logged via
   `kvm_shadow_audit_content_va` (already in tree from task #91).

2. **B.6 — Promote TDP path to canonical.** When zero divergences
   are observed across ≥3 consecutive cpython-parity-gate trials,
   flip the production path to TDP. Shadow PT remains compiled in
   for one rolling window of validation (1-2 weeks of soak).

3. **B.7-B.11 — Delete the shadow PT.** Remove
   `shadow_sync.c`/`kvm_shadow_*` exports/state machine /
   mutation ring / hooks in `pgtable.h`. ~3000-4000 lines net
   delete.

4. **B.12-B.13 — Heavy validation.** test_pylong_roundtrip_huge
   × 50 trials (the bignum-stress sentinel for PT-staleness
   bugs). 30+ minute kernel-build-under-UML for sustained
   mmap/munmap churn.

## 6. Hybrid fallback (if memslot churn is too expensive)

If per-mm memslot churn under heavy mmap/munmap (Python startup,
gcc compile) makes the gate slower than seccomp baseline, fall
back to a hybrid:

- **One physmem memslot** for UML's backing memory (Policy A
  retained).
- **Direct mm->pgd walks via TDP** still — but the leaves point to
  `physmem-GPA` instead of `host-PFN` directly. UML's pgd
  encoding is responsible for the user-VA → physmem-GPA mapping;
  KVM's TDP walks pgd → leaf and the leaf already names the
  physmem GPA, which KVM resolves via the single physmem memslot.
- Loses the host-VA aliasing benefit (memslot's userspace_addr
  doesn't cover individual user mappings) but eliminates the
  multi-writer shadow PT.

This is the "no shadow, no per-mm slots" minimum that still kills
Race B/D. Race C (host-VA aliasing via os_map_memory) requires
real per-mm host-VA isolation, which is Phase 4 Option B from the
synthesis (~multi-week per-mm host worker process).

Decision point: ship Stage B with full per-mm memslots first;
measure perf vs seccomp; fall back to hybrid only if perf gate
fails.

## 7. Sequencing

The dependency chain matches the task list:

| Task | Description                                          |
|------|------------------------------------------------------|
| B.0  | This memo                                            |
| B.1  | TDP path side-by-side; content-audit divergences     |
| B.2  | kvm_mm_map → KVM_SET_USER_MEMORY_REGION add          |
| B.3  | kvm_mm_unmap → KVM_SET_USER_MEMORY_REGION delete/split |
| B.4  | Shared kernel-half PGD                                |
| B.5  | Per-vCPU IRETQ slot in kernel-half                   |
| B.6  | Validate TDP=shadow on full cpython parity           |
| B.7  | Delete kvm_shadow_sync_pte hooks (pgtable.h)         |
| B.8  | Delete shadow_sync.c                                  |
| B.9  | Delete shadow PGD apparatus from lifecycle.c          |
| B.10 | Delete dirty/synced/needs_full_resync state machine  |
| B.11 | Delete F12 mutation ring                              |
| B.12 | test_pylong_roundtrip_huge × 50 stress validation    |
| B.13 | 30-min kernel-build-under-UML soak                    |

Estimated effort: 4-6 weeks. Risk: high (TDP walks of UML's
software pgd may surface encoding mismatches; per-mm memslot
overhead may regress perf vs Policy A).

## 8. What's explicitly NOT in Stage B

- **Per-mm host worker process** (Race C structural fix). That's
  Phase 4 Option B from the synthesis — closes the host-VA
  aliasing race that Stage B's memslot model still has under
  CLONE_VM thread groups. Multi-week refactor on top of Stage B.

- **PCID + noflush** (synthesis 4 / agent 1 phase 5). Optional
  perf optimization. Defer until Stage B closes the bug class
  and we measure whether TLB-flush cost is the bottleneck.

- **SAUCE / PKU intra-process isolation** (agent 4). Park as v2
  consideration after Stage B+C ship and we have ≥3 months of
  production data.

- **SMP UML support**. Requires Stage B's per-vCPU IRETQ slot
  + the kernel-half sharing already; the missing pieces are
  per-CPU vCPU host-thread pinning and a `pthread_kill(KICK)`
  sender. Tracked separately in tasks SMP.1-SMP.3.

## 9. Why this is the right shape

Every production VMM (qemu, cloud-hypervisor, Firecracker, kvmtool,
gVisor's KVM platform) uses TDP + memslots. Nobody runs a shadow
PT in 2026 because hardware EPT/NPT is universally available and
the multi-writer race class is structural to any "kernel mirrors
hardware" design.

UML's shadow PT was a workaround for the historical
`os_map_memory`-based host-VA aliasing model. With a per-mm
memslot per `kvm_mm_map` call, the host-VA aliasing IS the memslot
mapping, and KVM's TDP does the walk that the shadow tree was
duplicating in software. The right shape is what production VMMs
already do; UML can use it because UML already has the per-mm
KVM context (kvm_um.vm_fd is per-process).

## 9.5. Empirical blocker found 2026-04-27 (read this first)

**The naive "swap CR3 from `shadow->pgd_gpa` to `__pa(mm->pgd)`"
shortcut DOES NOT WORK and triple-faults the guest.** The reason
is structural to UML's current memory layout, not a Stage A bug:

- UML's `uml_physmem` lives at host VA `0x60000000`, size `0x20000000`
  (512 MiB), in PML4 slot 0 PUD slot 1 (covers
  `[0x40000000, 0x80000000)`).
- UML's kernel direct map for that range uses a **1GB huge page**
  in `init_mm.pgd` PUD[1] with the supervisor-only flag (US=0).
  Standard arch/x86 `setup_arch` populates this for the kernel
  direct map.
- UML user processes also map their text/data/stack at user VAs
  in the `0x4xxxxxxx` range — also covered by PML4[0] PUD[1].
- The shadow PT splits the same range into 4 KiB pages with
  per-page flags (US=1 for user mappings, US=0 for kernel/bootstrap).
  This is what makes shadow PT correct.
- Setting guest CR3 = `__pa(mm->pgd)` makes the guest CPU walk the
  1GB huge page. CPL=3 user code reads its own VA → US=0 access
  → #PF → triple-fault → KVM_EXIT_SHUTDOWN.

**Two ways forward, both substantial UML-core work:**

1. **Move `uml_physmem` out of PML4[0]** to a canonical kernel-
   half slot (PML4[256+]). User-VAs stay in PML4[0]; kernel
   direct map moves to PML4[256+]. After that, `mm->pgd`'s
   user-half (PML4[0]) is safely walkable for user code, and
   the bootstrap pages can be installed in PML4[256+] which
   user mms inherit via fork. This is the right end-state but
   requires changing the `uml_physmem` allocation in
   `arch/um/kernel/mem.c` + every `__pa`/`__va` site that assumes
   the current layout.

2. **Break the 1GB huge page** in `init_mm.pgd` to 4 KiB pages
   with per-VA flags matching what the shadow PT currently does.
   Less invasive but requires walking + rewriting init_mm.pgd
   at boot time, plus careful flag management. Defeats some of
   the perf benefit of the huge page (TLB pressure, walk depth).

(2) is the smaller change but still invasive. (1) is the right
strategic shape and matches what other arches do.

**A third option — the per-mm host worker process** (synthesis
section 6 hybrid) — sidesteps this entirely by giving each mm
its own host process with its own VA space, so the user-VA range
is private to each mm and the kernel direct map doesn't conflict.
That's the "stub-child host process" model the seccomp backend
uses; pulling it into the KVM backend means routing KVM_RUN
through a per-mm helper process. Different shape entirely.

For now: shadow PT stays as guest CR3. The B.1 diagnostic knob
(`kvm_use_mm_pgd`) is preserved for walk-and-print only;
activating the CR3 swap WILL crash the host UML kernel.

## 10. Open questions

- **PML4 sharing safety**: must verify that copying PML4[256..511]
  entries to a new mm doesn't break Linux's existing
  kernel-half-pgd-management invariants. Linux's `clone_pgd_range`
  + `pgd_alloc` for x86_64 native does roughly this; need to
  confirm UML's Kconfig + uml-specific arch_pgd_*() match.

- **`__pa(mm->pgd)` validity as guest CR3**: KVM's
  `kvm_vcpu_is_legal_cr3` checks reserved bits + PCID/LAM. The
  UML pgd's physical address is host-physical; KVM expects
  guest-physical. Under Policy A's identity memslot, host-phys
  IS guest-phys for the physmem range, so this works as long as
  `mm->pgd` lives in the physmem range. Audit `pgd_alloc` to
  confirm.

- **EFER/CR4 setup at first KVM_RUN**: the current
  `kvm_setup_production_sregs` puts the pgd_gpa as CR3 + sets
  CR4.PAE/PSE/PGE/etc + EFER.LMA/LME/NXE for long mode. With
  TDP, EPT enables itself when the guest has CR0.PG + CR4.PAE +
  EFER.LME — which we already program. Should Just Work.

- **Audit sites for cleanup**: `kvm_shadow_audit_va` /
  `kvm_shadow_audit_content_va` / `kvm_shadow_audit_pgd` are
  already useful for B.1's side-by-side comparison; reuse them
  before deletion in B.7+.

---

End of design memo. Implementation starts at B.1; this document
is the reference for review and for the LKML cover letter when
Series 7 (kvm-backend-series) is refreshed post-Stage-B.
