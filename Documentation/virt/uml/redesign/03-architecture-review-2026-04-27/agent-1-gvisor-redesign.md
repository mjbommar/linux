# Agent 1 — gVisor-pattern redesign for the UML KVM backend

Author: Architecture-review agent #1
Date:   2026-04-27
Branch: uml-redesign-plan
Mandate: STRUCTURAL fix for the 25 % per-trial flakiness on heavy
        long-running Python workloads (no per-trial retries, no
        50 µs `udelay` band-aids). Model on gVisor's KVM platform.

This memo is the answer to "stop playing whack-a-mole — go look at how
a production-grade KVM-as-process-sandbox (gVisor) actually structures
the same problem and copy what it does." It is concrete: it names the
struct fields, the locks, the entry/exit paths, the migration order,
and the engineering effort.

----------------------------------------------------------------------

## 1. gVisor architecture summary

The Sentry runs every sandboxed task on a vCPU. gVisor is **not** a
classic VMM: there is no separate guest kernel image, no virtio
devices, no boot loader. Sentry code itself runs inside the guest;
KVM is just a hardware-enforced address-space + ring isolator. The
key shape:

### 1.1 One `machine` per Platform; vCPUs are a *pool*

`pkg/sentry/platform/kvm/machine.go:189-220` (struct vCPU) and
`549-602` (Get) / `604-610` (Put):

```go
type vCPU struct {
    ring0.CPU                    // first element — must be page-pinned
    id        int
    fd        int
    tid       atomicbitops.Uint64
    state     atomicbitops.Uint32 // vCPUReady / vCPUUser / vCPUGuest / vCPUWaiter
    runData   *runData            // mmap of struct kvm_run
    machine   *machine
    active    atomicAddressSpace
    lastCtx   atomic.Pointer[platformContext]
    ...
}
```

`machine.Get()` (line 549) acquires *some* vCPU for the calling host
thread — fast-path returns the vCPU previously bound to this host TID;
slow path either allocates from `vCPUsByID[]`, or steals one whose
host thread has finished its guest run. Crucially `Get()` calls
`runtime.LockOSThread()` so the calling Go goroutine cannot migrate
while inside the vCPU. `Put()` (604) reverses that.

State bits (153-166): `vCPUReady=0`, `vCPUUser=1<<0`, `vCPUGuest=1<<1`,
`vCPUWaiter=1<<2`. `lock()` (719) is a single `OrUint32(state,
vCPUUser)` — that is the entire entry serialization. `unlock()`
(724-749) is a CAS loop that, when it observes `vCPUWaiter`, calls
`c.notify()` to kick the vCPU out of guest mode.

### 1.2 One `addressSpace` per *guest mm*; pageTables are owned by it

`pkg/sentry/platform/kvm/address_space.go:57-75`:

```go
type addressSpace struct {
    platform.NoAddressSpaceIO
    mu         sync.Mutex
    machine    *machine
    pageTables *pagetables.PageTables
    dirtySet   *dirtySet
}
```

`pageTables` is a real, hardware-walked page-table tree (in the
`pkg/ring0/pagetables` package, lines 25-180). It is built by
`Map()`/`Unmap()` calls. The upper half (kernel half) is *shared*
across every addressSpace via `upperSharedPageTables`
(pagetables.go:47-54) — so kernel-VA mappings (the Sentry's own
text/data, vDSO, IST stack, gadget pages) are installed exactly once
and every guest mm references the same physical PML4 entries.

This is the **most important structural difference** from UML, see §2.

### 1.3 The `dirtySet` — bounce-and-mark TLB invalidation

`address_space.go:18-54`:

```go
type dirtySet struct {
    vCPUMasks []atomicbitops.Uint64        // one bit per vCPU
}

func (ds *dirtySet) mark(c *vCPU) bool {        // line 37
    index := uint64(c.id) / 64
    bit   := uint64(1) << uint(c.id%64)
    if ds.vCPUMasks[index].Load()&bit != 0 {
        return false                            // already dirty
    }
    atomicbitops.OrUint64(&ds.vCPUMasks[index], bit)
    return true
}

func (ds *dirtySet) forEach(m *machine, fn func(c *vCPU)) {
    for index := range ds.vCPUMasks {
        mask := ds.vCPUMasks[index].Swap(0)     // atomic clear-as-we-go
        if mask != 0 {
            for bit := 0; bit < 64; bit++ {
                if mask&(1<<bit) != 0 {
                    fn(m.vCPUsByID[64*index+bit])
                }
            }
        }
    }
}
```

`addressSpace.Touch(c)` (84) is called for *every* page-table mutation
that affects vCPU `c`. `Invalidate()` (84-88) calls the internal
`invalidate()`:

```go
func (as *addressSpace) invalidate() {
    as.dirtySet.forEach(as.machine, func(c *vCPU) {
        if c.active.get() == as {
            c.BounceToKernel()                  // IPI + flush
        }
    })
}
```

`BounceToKernel` is implemented via the **bluepill** mechanism: send
a host signal (SIGCHLD) to the host thread that owns vCPU `c`; that
signal handler is gVisor's `sighandler` which, if the thread was in
guest mode, returns through `bluepillHandler` which exits the guest,
flushes TLB, then re-enters. If the thread was *not* in guest mode
when the signal arrived, the bounce is already a no-op.

This is the keystone: **TLB invalidation is asynchronous, per-vCPU,
and never blocks the writer**. The writer just `mark()`s a bit. The
*next time* that vCPU enters guest mode, or it gets bounced if
already in guest, the flush happens. The writer never has to know
which vCPUs are in which state, never holds a lock across the flush,
and never has to wait.

### 1.4 The bluepill — signal-as-VMENTER

`bluepill_amd64.go` + `bluepill_unsafe.go`:

- `bluepill(c)` is *called* from sentry Go code. It executes a
  ring-0-only instruction (HLT or a write to a magic MSR) which on a
  real host triggers SIGSEGV — but inside the KVM guest it triggers a
  VMEXIT routed back to `bluepillHandler`.
- `bluepillHandler` (in `bluepill_unsafe.go`, around line 115) just
  invokes `RawSyscallErrno(SYS_IOCTL, c.fd, KVM_RUN, 0)` and decodes
  `c.runData.exitReason` (the mmap'd `struct kvm_run`).
- `redpill()` (bluepill.go:63) issues `syscall(-1)` from inside the
  guest — that's the guest→host bounce.

The effect: there is no separate "host scheduler loop calling KVM_RUN
in a tight while-loop". The host goroutine *transparently* moves
between host context and guest context by stepping on a bluepill or
catching a signal. Switching addressSpace is a `MOV %rax, %cr3`
inside the guest. Switching vCPU happens at `Get()/Put()` boundaries
in host context.

### 1.5 The contract for context.Switch

`context.go:50-104` (paraphrased):

```go
func (c *platformContext) Switch(as platform.AddressSpace, ...) {
    cpu := c.machine.Get()                     // host thread bound to vCPU
    defer cpu.Unlock()                         // (Put())

    if cpu.lastCtx.Swap(c) != c { ... }        // detect preemption
    cpu.active.set(localAS)                    // remember current AS
    err := cpu.SwitchToUser(switchOpts{        // <-- the VMENTER
        Registers:        regs,
        FloatingPointState: fpu,
        PageTables:       localAS.pageTables,
        Flush:            localAS.Touch(cpu),  // PCID flush bit
    })
    // err encodes signal / fault / preemption — caller handles.
}
```

`localAS.Touch(cpu)` returns true if this addressSpace had marked
this vCPU dirty since its last entry — that drives the PCID-aware
flush (see §1.6). The `pageTables` field is the real on-CPU page
tables; KVM's CR3 will point at `pageTables.rootPhysical`.

### 1.6 PCID + noflush bit

`pkg/ring0/pagetables/pcids.go:48-77`:

```go
func (p *PCIDs) Assign(pt *PageTables) (uint16, bool) {
    if pcid, ok := p.cache[pt]; ok {
        return pcid, false                     // hit — NO flush
    }
    if len(p.available) > 0 {                  // pool draw
        pcid := pop(); p.cache[pt] = pcid
        return pcid, true                      // flush this entry
    }
    // evict
    return evictOne(), true
}

func (p *PCIDs) Drop(pt *PageTables) {
    if pcid, ok := p.cache[pt]; ok {
        delete(p.cache, pt); push(pcid)
    }
}
```

`SwitchToUser` (machine_amd64.go:298-408) consumes the `(pcid, flush)`
pair: it loads CR3 with the PCID encoded in bits [11:0] and bit 63
set to NOT flush when `flush=false`. So switching back to a recently-
used addressSpace on the same vCPU is **TLB-free** — the hardware
already has its translations cached under that PCID.

`machine.dropPageTables(pt)` (713) is invoked at addressSpace teardown
to evict stale PCID assignments across every vCPU.

----------------------------------------------------------------------

## 2. What gVisor does that UML doesn't

The four specific race classes the playbook has identified
(`Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/19-next-investigation-playbook/00-playbook.md`):

| Race class | gVisor's structural prevention |
|---|---|
| Singleton vCPU contention (`kvm_um.vcpu0_fd` shared across all UML tasks) | Pool of N vCPUs in `m.vCPUsByID[]`, `Get()`/`Put()` hands a vCPU to whichever host thread runs it next, with `LockOSThread` and atomic state bits (lines 549/604/719/724). N defaults to host CPU count; vCPU-fd is never multiplexed under one task at the cost of another. |
| Post-unblock state races (`kvm_run` mmap and IST stack overwritten between `unblock_signals()` and decoding) | `kvm_run` (`runData`) and any IST/scratch state are *per-vCPU*. The vCPU is locked to the calling host thread for the whole `Switch()`. Nothing else can write its `runData` because nothing else owns it. There is no need for the UML-style "snapshot exit_reason / kregs / IST 40-bytes before unblock_signals" workaround (cf. UML commit `b516bee62eb2`). |
| Shadow-PT-vs-direct-sync races (`shadow_dirty`/`synced`/`needs_full_resync` cmpxchg dance) | gVisor has **no shadow PT**. The `pagetables.PageTables` IS the guest's hardware-walked PT. There is no second tree to keep in sync. Every PTE mutation goes through `pageTables.Map()`; that's it — no fill, no clear pass, no transactional re-walk, no seqlock. |
| Host-VA aliasing (UML's user mappings live outside the KVM memslot, so guest reads via shadow → GPA → kernel VA can diverge from host writes via os_map_memory) | `physical_map.go` registers ALL of host physical memory the Sentry can hand out as a single 1:1 KVM memslot (or a small set of slots covering the full Sentry heap). Guest VA → guest PA → KVM memslot → host VA is a single mapping with NO aliases. The "GPA matches PFN matches host-write target" property is structural. |

Three more observations follow directly:

a. **No singleton kernel-half mappings tied to a particular mm**. UML
   has the bootstrap page, IST stack, gadget state page, gadget vvar
   page, and IRETQ frame page all installed *into the active mm's
   shadow PT* on every entry (`thread.c:2049-2174`, `2345-2361`).
   Each shadow_mm needs its own copy and a per-mm install path. gVisor
   solves this once, at addressSpace allocation, by sharing the upper
   half (`pagetables.go:47-54, upperSharedPageTables`). The kernel-
   half mappings are physically the same PML4 entries across every
   addressSpace; cross-mm isolation comes purely from the user half.
   This eliminates the entire class of "memo 18 phase 2 per-mm IRETQ
   frame collision" bugs structurally.

b. **No `kvm_enter_guest` "rebuild everything every entry" path**.
   gVisor enters via a single signal-handler hop into a tight C
   sequence that loads CR3 (often noflush), loads `kregs` from the
   `runData` mmap, and issues `KVM_RUN`. UML's `kvm_enter_guest`
   (thread.c:1952-2731) is ~780 lines: it ensures memslot, ensures
   CPUID, calls bootstrap init, maps 4 bootstrap pages into the
   shadow, runs an MSR program block, allocates gadget state/vvar,
   does a fill-or-skip predicate, holds mmap_read_lock, walks
   mm->pgd, installs the per-mm IRETQ frame, runs SREGS-skip vs full
   SREGS, runs CR4.PGE-toggle for TLB-flush, programs LSTAR/STAR/
   FMASK/EFER MSRs, and *then* calls KVM_RUN. Every one of those
   steps is a possible drift source.

c. **No "another task can run while I'm in `unblock_signals()`"
   problem.** gVisor disables interrupts (`intsema`) inside `Switch`;
   the only signals that fire in that window are gVisor's own bounce
   signal which deliberately yanks the vCPU out. UML, in contrast,
   `unblock_signals()` *immediately* after KVM_RUN returns and BEFORE
   reading the singleton run mmap — a SIGALRM at that instant routes
   to UML's scheduler which can pick a different UML task that grabs
   the same vcpu0_fd / run mmap and runs its own KVM_RUN, blowing
   away the original task's exit state. The current band-aid
   (snapshot exit_reason, kregs, IST stack, etc., before
   `unblock_signals` — landed in commit `b516bee62eb2`) papers
   over this but does not fix it. Per-task vCPU fixes it.

----------------------------------------------------------------------

## 3. Specific UML files / functions to rewrite

| gVisor pattern | UML code that must be replaced |
|---|---|
| `vCPU` pool + `Get()/Put()` with host-thread binding (`machine.go:549-610, 719-749`) | Replace `kvm_um.vcpu0_fd` (`kvm_backend.h:37`), `kvm_backend_vcpu0_fd()` (`lifecycle.c:2201`), and every caller (e.g. `thread.c:1954, 3270`). New: per-task `struct kvm_vcpu_slot` allocated lazily in `kvm_thread_create` (`thread.c:227`). |
| `addressSpace` owning real `pageTables` (`address_space.go:57-75, pagetables/pagetables.go:25-180`) | Replace the entire shadow-PT machinery: delete `kvm_shadow_mm`, `kvm_shadow_pgd_alloc`, `kvm_shadow_fill_from_uml_pgd` (lifecycle.c:1173), `kvm_shadow_invalidate_va_range` (lifecycle.c:1650), `kvm_shadow_sync_pte` (shadow_sync.c:153), `kvm_shadow_sync_va_atomic`, `kvm_shadow_sync_range_atomic`, `kvm_shadow_clear_range_atomic`, `kvm_shadow_pgd_clear_user` (lifecycle.c:1624). Also delete the `dirty/synced/needs_full_resync` state machine documented at `kvm_backend.h:438-538` and the F12 mutation ring (`kvm_backend.h:423-437`). |
| `dirtySet.mark()` + `BounceToKernel` (`address_space.go:37-54, 84-94`) | Replace `kvm_mm_map`/`kvm_mm_unmap` shadow-invalidate hooks (`mm.c:175-193, 239-247`). New: `kvm_as_unmap()` calls `kvm_pt_unmap()` then `kvm_dirty_set_mark(as, vcpu)` for every vCPU in `as->active_vcpus` followed by an IPI to host CPUs running those vCPUs. |
| `upperSharedPageTables` (`pagetables.go:47-54`) | Replace per-mm install of bootstrap, IST stack, gadget state, gadget vvar, and IRETQ frame (`thread.c:2049-2176, 2345-2361`). New: build these once at `kvm_init`, pin in a "kernel-half" PT page that every per-mm root inherits via `pgd[256..511]` slot copies. |
| `bluepill` signal-driven entry (`bluepill_amd64.go`, `bluepill_unsafe.go:115`) | Optional. UML's userspace loop `kvm_run_userspace` (thread.c:3268) is already an explicit `KVM_RUN` call with `block_signals`/`unblock_signals` brackets; we don't need bluepill if we get vCPU ownership right. Bluepill is the gVisor pattern that eliminates "what if a signal hits during the host-side preamble" — UML's equivalent is to keep signals blocked for the entire span where the vCPU's `runData` and friends are uniquely owned. |
| PCID + noflush (`pcids.go`, `machine_amd64.go SwitchToUser`) | Replace the CR4.PGE-toggle TLB-flush hack (`thread.c:2483-2538`). New: `kvm_pcid_assign(vcpu, as)` returns `(pcid, flush)`; the SREGS load uses `cr3 | (pcid & 0xfff) | (flush ? 0 : (1ull<<63))`. |

----------------------------------------------------------------------

## 4. Redesign blueprint

### 4.1 New data structures

```c
/*
 * One per UML task. Replaces the singleton kvm_um.vcpu0_fd.
 *
 * Lifecycle: allocated in kvm_thread_create when CONFIG_UM_BACKEND_KVM
 * tasks are created, freed in kvm_thread_exit. Pinned to the task; not
 * pooled, not stolen. The simpler invariant ("each task has its own
 * vCPU") trades RAM (mmap'd kvm_run is ~12 KiB / vCPU) for the
 * elimination of every singleton-vCPU race class.
 *
 * For UML's typical 50-200 task count this is 0.6-2.4 MiB total —
 * fine. If task count explodes we revisit with a bounded pool +
 * gVisor-style stealing.
 */
struct kvm_vcpu_slot {
    int                 fd;         /* KVM_CREATE_VCPU result */
    struct kvm_run     *run;        /* mmap of KVM_GET_VCPU_MMAP_SIZE bytes */
    size_t              run_size;
    /* Per-vCPU stable kernel-side state */
    void               *iretq_frame_page;  /* host VA, page allocated once */
    u64                 iretq_frame_gpa;
    void               *gadget_state;      /* per-vCPU per-task struct */
    /* Cached MSRs / SREGS — primed once per vCPU */
    bool                msrs_primed;
    bool                cpuid_done;
    /* Active addressSpace (which mm last loaded CR3) for PCID flushing */
    struct kvm_as      *active_as;
    u16                 active_pcid;
    /* IPI dispatch — bluepill-equivalent */
    int                 host_cpu;          /* host CPU last run on */
    atomic_t            state;             /* READY / IN_GUEST / WAITER */
};

/*
 * One per UML mm. Replaces struct kvm_shadow_mm AND the shadow-PT
 * machinery. The pgd here is the REAL guest-CR3 page tables, not a
 * shadow of mm->pgd — mm->pgd IS this tree (we re-use UML's existing
 * pgd allocator + just insert a backend-side install hook).
 *
 * Equivalent to gVisor's addressSpace + pageTables (bound together
 * because UML doesn't need them separable).
 */
struct kvm_as {
    struct mm_struct   *mm;                /* back-pointer */
    struct page        *pgd_page;
    void               *pgd;               /* kernel VA */
    u64                 pgd_gpa;           /* CR3 load value */
    /* Dirty bitmap: bit per vCPU that needs a TLB flush before next entry */
    DECLARE_BITMAP(dirty_vcpus, CONFIG_NR_CPUS);
    /* Active set: which vCPUs currently have this AS as their CR3 */
    DECLARE_BITMAP(active_vcpus, CONFIG_NR_CPUS);
    spinlock_t          mu;                /* serializes pt_map / pt_unmap */
    /* PCID assignment cache (per-vCPU PCID for this AS) */
    u16                 pcid_per_vcpu[CONFIG_NR_CPUS];   /* 0 = unassigned */
};

/*
 * Singleton kernel-half PT — built ONCE at kvm_init. Holds the
 * bootstrap page, IST stack, gadget state, gadget vvar, IRETQ frame,
 * and any other ring-0-side mappings the in-guest shim needs.
 *
 * Every kvm_as->pgd has the kernel half (PGD slots 256..511) pointed
 * at this tree's PUDs via shared PML4 entries — gVisor's
 * upperSharedPageTables pattern.
 *
 * Critically, the IRETQ frame page is per-vCPU (one slot per vCPU
 * at a known offset within the kernel half), not per-mm. Same for
 * gadget state. That eliminates the per-mm-IRETQ-frame race
 * (current memo 18 phase 2) AND the singleton-IST-stack race
 * (current snapshot-before-unblock workaround).
 */
struct kvm_kernel_half {
    struct page        *pml4_kernel_pages[256]; /* slots 256..511 */
    void               *bootstrap_page;
    u64                 bootstrap_gpa;
    /* per-vCPU IST/IRETQ frame array, indexed by vcpu->id */
    struct page        *iretq_frames[NR_VCPUS];
    /* per-vCPU gadget state; gadget vvar can stay singleton (read-only) */
    struct page        *gadget_states[NR_VCPUS];
};
```

### 4.2 Call flow

```
        Task A (host kthread)                Task B (host kthread)
        --------------------------           --------------------------
        kvm_run_userspace(regs)              kvm_run_userspace(regs)
          |                                    |
          v                                    v
        kvm_vcpu_acquire(current)            kvm_vcpu_acquire(current)
          - returns task->kvm_vcpu_slot      - returns task->kvm_vcpu_slot
          - DIFFERENT vcpu_fd                  - DIFFERENT vcpu_fd
          |                                    |
          v                                    v
        block_signals()                      block_signals()
        kvm_as_load(slot, current->mm)       kvm_as_load(slot, current->mm)
          - if slot->active_as != as:          - same routine
            - pcid = pcid_assign(as, slot)
            - flush = first time on this slot ? true : false
            - issue KVM_SET_SREGS(cr3 | pcid | (flush?0:NOFLUSH))
            - check dirty_vcpus bitmap; if set, force flush + clear
          |                                    |
          v                                    v
        ioctl(slot->fd, KVM_RUN)             ioctl(slot->fd, KVM_RUN)
          [enters guest synchronously]         [enters guest concurrently
                                                on a different vCPU]
          [KVM_RUN returns]                    [KVM_RUN returns]
          v                                    v
        decode slot->run->exit_reason        decode slot->run->exit_reason
        no race — slot->run is uniquely      ditto
        owned by task A                       
          v
        unblock_signals()
        dispatch syscall / pf / hlt etc.
        return to userspace() loop


        Producer side (set_pte_at hook):
        ----------------------------------------
        kvm_pt_set_pte(mm, va, pte)
          - install/clear leaf in mm->kvm_as->pgd (real PT)
          - no shadow second tree
          - for each cpu in as->active_vcpus:
                bitmap_set(as->dirty_vcpus, cpu)
                if cpu != current_cpu and slot[cpu].state == IN_GUEST:
                    smp_send_reschedule(host_cpu_of(cpu))   /* IPI */

        On the IPI'd vCPU's host CPU, the in-guest signal handler
        (or the next post-RUN exit) consumes dirty_vcpus[my_id],
        runs INVPCID, and clears the bit. The writer never waits.
```

### 4.3 Migration path — file by file

This is multi-week work. Order matters: the dependency chain dictates
that kernel-half-shared PTs must land BEFORE per-task vCPUs, because
otherwise we'd be allocating per-vCPU IRETQ-frame pages but with no
unified PT to host them.

**Phase 1 (1 week) — kernel-half consolidation**

- New file `arch/um/backend/kvm/kernel_half.c`. Builds the singleton
  upper-half PT at `kvm_init`. Allocates one bootstrap page, one
  gadget vvar page, and a per-CPU array of IST/IRETQ-frame and gadget
  state pages.
- Modify `kvm_shadow_mm_alloc` (`lifecycle.c:656`) to copy the upper-
  half PML4 entries from the singleton kernel-half PT into the new
  shadow tree's PML4. Lower half stays per-mm.
- Delete the per-mm IRETQ-frame-page allocation in
  `kvm_shadow_mm_alloc` and the install-after-fill hack in
  `thread.c:2345-2361`. Replace with: at every entry, the IRETQ
  frame VA is computed from `vcpu->id` against the singleton kernel
  half's per-vCPU IRETQ frame slot.
- Validation: cpython parity gate must not regress vs current 17/21.

**Phase 2 (1.5 weeks) — per-task vCPU**

- Add `struct kvm_vcpu_slot *kvm_vcpu` to `struct thread_struct`
  (`arch/x86/include/asm/processor.h` UM section).
- `kvm_thread_create` (`thread.c:227`) — call `KVM_CREATE_VCPU` and
  `KVM_GET_VCPU_MMAP_SIZE` + `mmap`; store slot pointer on the task.
- `kvm_thread_exit` — close fd, munmap the run page.
- `kvm_run_userspace` (`thread.c:3268`) — replace
  `kvm_backend_vcpu0_fd()` with `current->thread.kvm_vcpu->fd` and
  `kvm_backend_ctx()->run0` with `current->thread.kvm_vcpu->run`.
- Delete the entire `if (rc >= 0 || rc == -EINTR) { snapshot ... }`
  block at `thread.c:3454-3530`. The mmap is uniquely owned now;
  `unblock_signals()` can move ABOVE the snapshot block since there
  is no race.
- Delete the singleton `kvm_run` pointer plumbing throughout.
- Phase 1's per-vCPU IRETQ frame slot now naturally maps to per-task
  (since each task has its own vCPU). The "memo 18 phase 2.4 signal-
  block window" hack in `thread.c:3388` becomes pure leftover and
  can stay or be removed.

**Phase 3 (2 weeks) — kill the shadow PT**

- Stop using a separate shadow tree. Make UML's mm->pgd allocator
  hand out a hardware-walkable PT directly; the backend translates
  UML PTE bits at install time inside `set_pte_at` rather than
  storing a software encoding and translating on fill.
- Concretely: change `_PAGE_RW`/`_PAGE_USER`/etc. to live at the x86
  hardware bit positions for UML+KVM builds, OR add a
  `set_pte_at`-side translator that writes BOTH the UML-encoded PTE
  (for UML's own pgd walks) and the hardware PTE (in a parallel
  page-table tree owned by `struct kvm_as`).
- Delete `kvm_shadow_fill_from_uml_pgd`, `kvm_shadow_map_page`,
  `kvm_shadow_pgd_clear_user`, `kvm_shadow_invalidate_va_range`,
  `kvm_shadow_sync_pte`, `kvm_shadow_sync_va_atomic`,
  `kvm_shadow_sync_range_atomic`, `kvm_shadow_clear_range_atomic`,
  `kvm_shadow_audit_va`, `kvm_shadow_audit_content_va`,
  `kvm_shadow_audit_pgd`. Delete the whole F12 mutation ring.
- Delete the `dirty/synced/needs_full_resync` state machine
  (`kvm_backend.h:439-538`) and every cmpxchg consume site
  (`thread.c:2239-2329, 2406-2574`).

**Phase 4 (3-5 days) — dirtySet + IPI**

- Add `dirty_vcpus` and `active_vcpus` bitmaps to `kvm_as`.
- At `kvm_as_load(slot, as)`: set `bitmap_set(as->active_vcpus,
  slot->id)`; clear from any previous AS's `active_vcpus`.
- At `kvm_pt_set_pte` / `kvm_pt_unmap` (the new equivalents of
  `kvm_mm_map`/`kvm_mm_unmap`): set `as->dirty_vcpus[cpu]` for every
  cpu in `as->active_vcpus`. For each `cpu != smp_processor_id()`,
  if its `slot->state == IN_GUEST`, send a host IPI via
  `smp_send_reschedule(slot->host_cpu)`.
- On `kvm_run_userspace` re-entry: consume `dirty_vcpus[my_id]`
  before KVM_RUN; if set, issue an INVPCID flush via
  KVM_SET_SREGS-with-flush-bit, then clear the bit (cmpxchg-style).
- The IPI handler is a no-op userland signal — its purpose is to
  pop the host thread out of `KVM_RUN` (KVM converts pending host
  signals to KVM_RUN return -EINTR) so the dirty-bit consume runs
  before the next KVM_RUN.

**Phase 5 (3 days) — PCID**

- Probe `KVM_CAP_X86_DISABLE_EXITS` and feature-bit PCID/INVPCID.
- Add `pcid_per_vcpu[CONFIG_NR_CPUS]` to `kvm_as`.
- Helper `kvm_pcid_assign(slot, as)` → returns `(pcid, flush)`.
  First call on a (vcpu, as) pair: pcid = first-free in vcpu's pool
  (16 PCIDs/vCPU), flush=true. Subsequent: pcid = cached, flush=false.
  Pool full: evict LRU, flush=true.
- SREGS load programs `cr3 | (pcid & 0xfff) | (flush ? 0 : (1ULL<<63))`.
- Delete the CR4.PGE-toggle TLB-flush hack (`thread.c:2483-2538`).

**Phase 6 (2 days) — cleanup, documentation**

- Delete `kvm_shadow_audit_*` (no longer applicable). Delete the
  diagnostics that monitored shadow state.
- Update `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  to mark workstreams D-shadow-* as superseded.
- KUnit suite update for new APIs.

### 4.4 Estimated effort

Total: ~5 engineering weeks for one developer; ~3 weeks for two
working in parallel (Phase 1 + Phase 2 are largely independent of
Phase 3, can run concurrently if Phase 1 lands first).

| Phase | Effort | Dependency | Risk |
|---|---|---|---|
| 1 — kernel-half consolidation | 1 wk | — | low |
| 2 — per-task vCPU | 1.5 wk | Phase 1 | medium |
| 3 — kill shadow PT | 2 wk | Phase 1 | high (touches set_pte_at) |
| 4 — dirtySet + IPI | 3-5 d | Phase 2, 3 | medium |
| 5 — PCID | 3 d | Phase 4 | low |
| 6 — cleanup/docs | 2 d | — | low |

----------------------------------------------------------------------

## 5. Tradeoffs & risks

**What we lose**

a. *Memory*. Per-task vCPU costs ~12 KiB (mmap of struct kvm_run) +
   ~8-16 KiB (per-vCPU FPU + scratch) per UML task. For a 200-task
   UML system that's 4-6 MiB. Acceptable.

b. *Cold-start cost*. `KVM_CREATE_VCPU` + `KVM_SET_CPUID2` + initial
   `KVM_SET_SREGS` are ~200-500 µs each. Per task this is one-time
   at thread_create. The hot path (every KVM_RUN) becomes *cheaper*
   because the SREGS-skip cache hits more reliably (no cross-task
   invalidation of cached state).

c. *Loss of the `shadow_dirty/synced` observability counters*. The
   F12 mutation ring is genuinely useful for the bug class we have
   right now; we should reintroduce a simpler "per-vcpu dirty-flush
   count" once Phase 4 lands so we can still see whether IPIs are
   firing.

**What gets harder**

a. *UML's set_pte_at must know about the kvm_as*. Today it goes
   through generic UML mm hooks that eventually call `kvm_mm_map`/
   `kvm_mm_unmap`. The new model needs the set_pte_at hook to
   directly install into the real PT, which means either (i) UML's
   pgd allocator hands out the same physical pages used as guest
   CR3 (cleaner) or (ii) we maintain a parallel hardware PT and
   copy on every set_pte_at (uglier — basically the current shadow
   model with a new name). Option (i) is what gVisor does and is the
   right answer; it requires changing UML's pgd allocator for
   `CONFIG_UM_BACKEND_KVM` builds, which is invasive.

b. *Cross-CPU IPI delivery latency*. IPI on x86 is ~1-3 µs; we have
   to be sure the IPI'd host CPU's KVM_RUN actually returns -EINTR
   promptly. KVM_RUN does honour pending host signals via the
   request_immediate_exit path; needs validation.

c. *Concurrency *to *the same mm* across multiple host CPUs (CLONE_VM
   threads each on their own vCPU)*. Now the dirtySet really matters;
   today the singleton-vCPU model serializes everything. We need a
   careful unit test: two tasks, same mm, one mprotects while the
   other is in KVM_RUN, the running one must see the new permission
   on its next page touch.

**What to validate**

1. cpython parity gate: 21/21 reproducibly across N=20 trials. The
   fix only "works" if the band-aid `udelay(50)` becomes pointless.

2. `import test.test_decimal` reliability: target 50/50.

3. `test_int` long-running corruption (the 12-million-character
   bignum diverging at byte ~2.4 M) — the hypothesis is that it's a
   shadow-PT/TLB staleness issue during long computation; killing
   the shadow + adding the dirtySet + PCID-noflush should make it
   structurally impossible.

4. Multi-task stress: build kernel under UML running CONFIG_UM_BACKEND_KVM
   for >30 minutes without divergence. Today this is a known crasher.

5. Snapshot/forkserver path (`snapshot.c`) — the per-task-vCPU model
   means each fork-server child needs its own vCPU, not the singleton.
   `kvm_snapshot_*` calls need to capture+restore the per-task vCPU
   too. Risk: medium; mitigated by the snapshot path being relatively
   self-contained.

6. SMP UML (`ncpus>1`) — actually becomes *easier* under the new
   model because per-task vCPU is the natural shape for SMP. The
   current singleton-vCPU model is fundamentally broken under SMP.

----------------------------------------------------------------------

## Appendix — gVisor source citations

- `pkg/sentry/platform/kvm/machine.go:189-220` (struct vCPU)
- `pkg/sentry/platform/kvm/machine.go:549-602` (Get())
- `pkg/sentry/platform/kvm/machine.go:604-610` (Put())
- `pkg/sentry/platform/kvm/machine.go:713-721` (dropPageTables)
- `pkg/sentry/platform/kvm/machine.go:719-749` (lock/unlock)
- `pkg/sentry/platform/kvm/address_space.go:18-54` (dirtySet)
- `pkg/sentry/platform/kvm/address_space.go:57-94` (addressSpace)
- `pkg/sentry/platform/kvm/address_space_amd64.go` (invalidate via BounceToKernel)
- `pkg/sentry/platform/kvm/context.go:31-104` (platformContext, Switch)
- `pkg/sentry/platform/kvm/bluepill.go` (sighandler, redpill)
- `pkg/sentry/platform/kvm/bluepill_amd64.go` (bluepillArchEnter/Exit, KernelSyscall, KernelException)
- `pkg/sentry/platform/kvm/bluepill_unsafe.go:115` (KVM_RUN ioctl invocation)
- `pkg/sentry/platform/kvm/machine_amd64.go:73-148` (vCPU.initArchState)
- `pkg/sentry/platform/kvm/machine_amd64.go:298-408` (SwitchToUser + vector dispatch)
- `pkg/ring0/pagetables/pagetables.go:25-180` (PageTables, Map/Unmap, upperSharedPageTables)
- `pkg/ring0/pagetables/pcids.go:48-77` (PCIDs.Assign with noflush)
- `pkg/ring0/kernel_amd64.go:29-60, 87-128, 170-189, 203-233` (Kernel/CPU init, SwitchToUser, SYSCALL setup)

## Appendix — UML source citations

- `arch/um/backend/kvm/kvm_backend.h:34-150` (struct kvm_um — singleton vCPU + caches)
- `arch/um/backend/kvm/kvm_backend.h:418-617` (struct kvm_shadow_mm — to be deleted)
- `arch/um/backend/kvm/kvm_backend.h:438-538` (the dirty/synced/needs_full_resync doc — to be deleted)
- `arch/um/backend/kvm/lifecycle.c:656-783` (kvm_shadow_mm_alloc / free — replace)
- `arch/um/backend/kvm/lifecycle.c:1173-1505` (kvm_shadow_fill_from_uml_pgd — delete)
- `arch/um/backend/kvm/lifecycle.c:1507-1623` (kvm_shadow_map_page — delete)
- `arch/um/backend/kvm/lifecycle.c:1624-1648` (kvm_shadow_pgd_clear_user — delete)
- `arch/um/backend/kvm/lifecycle.c:1650-1730` (kvm_shadow_invalidate_va_range — delete)
- `arch/um/backend/kvm/shadow_sync.c:153-310` (kvm_shadow_sync_pte — delete)
- `arch/um/backend/kvm/shadow_sync.c:393-487` (sync_va/range/clear_range_atomic — delete)
- `arch/um/backend/kvm/mm.c:30-251` (kvm_mm_attach/detach/map/unmap — rewrite to use kvm_as)
- `arch/um/backend/kvm/thread.c:1952-2731` (kvm_enter_guest — substantially rewrite)
- `arch/um/backend/kvm/thread.c:2049-2176` (per-mm shadow installs of bootstrap/gadget/vvar — replace with shared kernel half)
- `arch/um/backend/kvm/thread.c:2345-2361` (per-mm IRETQ frame install — replace with per-vCPU)
- `arch/um/backend/kvm/thread.c:2483-2538` (CR4.PGE-toggle TLB flush hack — replace with PCID)
- `arch/um/backend/kvm/thread.c:3268-end` (kvm_run_userspace — rewrite per-task)
- `arch/um/backend/kvm/thread.c:3454-3530` (snapshot-before-unblock_signals workaround — delete)
- `arch/um/backend/kvm/thread.c:239-345` (kvm_context_switch — rewrite; the FPU save/restore dance becomes unnecessary because per-task vCPUs naturally hold per-task FPU state)

End of memo.
