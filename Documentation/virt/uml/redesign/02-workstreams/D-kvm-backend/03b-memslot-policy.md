# D-03c design note: memslot policy for `mm_map` / `mm_unmap`

**Status:** landed (2026-04-22) — Policy A shipped as D-03c
           (`8be68ed2ca20`, "one giant memslot at init"
           covering UML's VA) plus D-03d host-side
           `mm_map`/`mm_unmap` wiring (`184471f40ec9`). Memslot
           registration itself deferred to first
           `run_userspace` via D-04b.0 (`153017d8eeda`) so
           `backend=kvm` probe phase stays silent on hosts
           where early-boot VA layout would otherwise race
           registration. See `03-page-table-mgmt.md` for the
           per-op landing table.
**Follows:** D-03a (probe) ✓ 64aa06142e8f, D-03b (kvm_um +
           mm_attach) ✓ b27088ca79e9
**Companion to:** `03-page-table-mgmt.md`,
           `design-memo.md` §"Address-space model"

D-03b is the last D-03 step that can land in isolation of
D-04's CR3 programming. The remaining two hot ops (`mm_map`,
`mm_unmap`) have a shape that depends on a design call we
haven't made yet. This note records the options so the code
commit doesn't bake in the wrong one by accident.

## What `mm_map` / `mm_unmap` mean under the KVM-platform model

`mm_map(struct mm_id *id, unsigned long virt, unsigned long len,
int prot, int phys_fd, unsigned long long offset)` is called
from `arch/um/kernel/tlb.c::um_tlb_sync()` as UML kernel walks
its own pgd and finds pmd / pgd entries flagged for sync. The
op's contract: make the region `[virt, virt+len)` of the UML
guest process's address space visible with backing from
`phys_fd @ offset` and protection `prot`.

Under ptrace / seccomp, "visible to the guest" means visible
in the stub child's process VA: the op queues an `mmap(virt,
len, prot, MAP_FIXED|MAP_SHARED, phys_fd, offset)` that the
stub child will execute, which updates the child's page tables
in the host kernel. The stub child IS the UML guest process
address space — their PTEs are literally the guest PTEs.

Under KVM-platform there is no stub child. The "guest" runs
as ring-3 under the UML kernel's vCPU thread, using CR3 that
points at UML's own `mm->pgd` (translated to guest-physical).
"Visible to the guest at `virt`" then decomposes into two
orthogonal questions:

  - (a) **EPT visibility.** The host-physical frames backing
    `phys_fd @ offset` need to be reachable from the guest
    at some guest-physical address. That's what
    `KVM_SET_USER_MEMORY_REGION` expresses: one memslot
    maps a range of guest-physical to a range of host-VA
    (where the host is UML's own process, so the host-VA
    is within UML's mm).
  - (b) **Guest PTE visibility.** UML's own `pgd` for the
    target mm needs a PTE at `virt` pointing to the right
    guest-physical frame, with the right protection bits.
    That PTE is what the guest CPU will walk once CR3 is
    loaded in D-04.

Under ptrace / seccomp both are fused into the stub child's
`mmap()` syscall. Under KVM-platform they are separate:
memslots live on the VM fd (host-side, one per VM); PTEs
live in guest memory (per-mm, touched by UML as it walks
`pgd`).

## Two memslot policies worth considering

### Policy A — one giant memslot, registered at init

Per the design memo §"Address-space model":

> Guest physical memory is the UML process's own
> `mem_base..mem_base + mem_size`. One
> `KVM_USER_MEMORY_REGION` slot covers it. Guest virtual
> addresses are UML virtual addresses; `CR3` is whatever
> UML's current `mm->pgd` points to, translated to a
> guest-physical offset via the same arithmetic UML already
> uses.

Implementation shape:

  - `kvm_init()` runs `KVM_SET_USER_MEMORY_REGION` once
    after `KVM_CREATE_VM`. guest_phys_addr = 0,
    userspace_addr = UML kernel's min addressable VA,
    memory_size = `task_size` (roughly 2^47 on x86_64).
  - `mm_map(virt, len, prot, phys_fd, offset)`: mmap the
    `phys_fd @ offset` range into UML's own VA at some
    address (either `virt` directly via `MAP_FIXED` — but
    UML already owns `virt` in its own VA, so this works
    — or a parallel "guest-physical arena"). Then UML's
    normal PTE update path (the tlb.c walker that
    invoked us) installs the guest PTE at `virt`.
  - `mm_unmap(virt, len)`: munmap the host-VA range.
    Memslot is untouched; guest PTE gets cleared by the
    caller's tlb-walker.

Pros: matches the design memo; fewest `KVM_SET_USER_MEMORY_
REGION` calls (once); host-VA == guest-VA lets UML's existing
`mm->pgd` arithmetic work unchanged.

Cons: the single giant slot assumes the entire UML-kernel
host-VA range is legitimate guest-physical memory, which
means UML's own kernel code + data lives inside the guest
physical-memory window. That's fine (the guest running ring-3
under this CR3 can't walk into kernel-protected pages; ring-0
is the UML kernel itself, which is supposed to see kernel
memory). EPT shadowing overhead scales with the slot size at
setup, not per-map, so one slot of 128 TiB is cheap to
register.

Failure mode: the memslot is registered against the UML
kernel's _current_ memory layout. If UML later mmaps new
ranges outside the initial slot (unlikely — UML fixes its
address space at boot), those would be invisible to the
guest. Mitigation: cover 2^47 from the get-go; sparse
host-VA regions are fine because KVM's EPT is demand-paged.

### Policy B — per-map memslot, registered lazily

Alternative: each `mm_map` call registers a new memslot of
length `len` for guest-physical `[slot_base, slot_base+len)`
mapped to host-VA `[mmap'd_host_addr, mmap'd_host_addr+len)`.
`mm_unmap` removes the slot.

Pros: no initial sweep; slots exactly mirror live mappings
(nice for observability via `kvm_stat`); cleaner resource
accounting if UML ever wants to drop a region eagerly.

Cons: KVM caps slots at `KVM_USER_MEM_SLOTS` (509 on
x86_64 in recent kernels) and registration is a host
syscall each time. UML applications can easily have
thousands of mmap regions. Would overflow the slot limit
within a typical workload.

### Recommendation: Policy A

The slot-limit concern in Policy B is disqualifying on its
own. Policy A matches the design memo, is cheaper at
run-time, and lines up with gVisor's prior-art shape.

## What this means for D-03c code

  1. `kvm_init()` grows one call: after `KVM_CREATE_VM`,
     issue `KVM_SET_USER_MEMORY_REGION` for the whole UML
     address space. The slot covers the UML kernel's host
     VA range (tracked via the existing `task_size` global
     populated by `linux_main()`). guest_phys_addr = 0
     (or UML's min VA, matters for CR3 arithmetic; settle
     alongside D-04).
  2. `kvm_mm_map()` becomes a host-side `mmap(virt, len,
     prot, MAP_FIXED|MAP_SHARED, phys_fd, offset)` on the
     UML kernel process. No KVM ioctls on the hot path.
  3. `kvm_mm_unmap()` becomes a host-side `munmap(virt,
     len)`. No KVM ioctls on the hot path.
  4. mm.c's refcount on `kvm_um` stays; memslot state is
     static per VM, not per mm.

This lands cleanly without needing D-04's CR3 programming:
the pieces exist and are wired, but nothing under the
current stub set actually dispatches a vCPU run, so the PTE
visibility (point (b) above) is moot until D-04. The D-03c
commit makes `mm_map` / `mm_unmap` side-effect-complete
from the host's perspective; D-04 adds the vCPU that
actually exercises the resulting guest state.

## Open questions (resolve in code review)

  1. **`KVM_SET_USER_MEMORY_REGION` vs `KVM_SET_USER_MEMORY_
     REGION2`.** The `2` variant (added in 6.10) supports
     guest-memfd flags we don't need. Stick with the
     original ioctl; add `#ifdef KVM_USER_MEMORY_REGION2`
     guards only if we find a reason.
  2. **Initial slot size.** `task_size` can be up to
     `PTRS_PER_PGD * PGDIR_SIZE` (~128 TiB). KVM's internal
     overhead scales with populated guest-physical, not
     with slot size, so covering the whole thing is fine.
     Use `task_size` as computed by `linux_main()` rather
     than hardcoding.
  3. **MAP_FIXED semantics.** UML's own VA at `virt` is
     already allocated to the UML process (that's how
     UML "addresses" a guest mm). `mmap(MAP_FIXED)` over
     it will replace whatever was there. Fine in principle
     but it's worth a KUnit test to confirm no leaks on
     replace.
  4. **Error ordering.** If `mmap()` succeeds but the
     caller's PTE update later fails, we have a dangling
     host-VA mapping. Either (a) make mm_map idempotent
     so a retry heals it, or (b) roll back with `munmap`
     on failure. (a) is simpler; KVM's model doesn't care
     about stale host-VA pages (they're just demand-paged
     from `phys_fd`).

## Not in D-03c scope

- CR3 programming on `context_switch()` — D-04.
- vCPU creation / `KVM_RUN` loop — D-04.
- LSTAR trampoline, syscall dispatch — D-04 / D-05.
- Signal delivery through vCPU — D-05.

## Cross-references

- `design-memo.md` — overall shape.
- `03-page-table-mgmt.md` — parent task.
- decisions-log D57 — why one kvm_um per UML process.
- gVisor `pkg/sentry/platform/kvm/machine_amd64.go` —
  prior art, particularly `MapPhysical` and
  `mapPhysical` which implement an analogous slot-based
  mapping layer.
