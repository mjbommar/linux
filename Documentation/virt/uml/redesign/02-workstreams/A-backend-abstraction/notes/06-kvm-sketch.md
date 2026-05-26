# A-01.6 — KVM backend sketch (forward-looking)

Date: 2026-04-17
Owner: claude-code session
Inputs: A-01.0–A-01.5; D-workstream README and 01-kvm-platform-design.md;
gVisor `pkg/sentry/platform/kvm/` (URL referenced; not fetched here).

## Goal

Same as A-01.4/.5 but for the *future* KVM backend (workstream D, 6 EM,
starts month 13). The point of this sketch is not to implement KVM
support — it is to validate that the ops table in `asm/backend.h`
accommodates KVM **without per-backend extensions to the core ops**.

If a KVM op needs a signature that ptrace/seccomp don't, the table is
wrong and we revise A-01.3 *now*, before any implementation locks in.

## Architecture summary

Per `D-kvm-backend/README.md`, the model is gVisor-style:

- One KVM guest VM per UML instance.
- Two ring levels in the guest: ring-0 is the UML kernel; ring-3 is
  guest userspace.
- Per UML vCPU thread, one KVM vCPU.
- Guest userspace `syscall` traps via `MSR_LSTAR` directly into ring-0
  *inside the guest* — no VMEXIT, ~100 cycles.
- Operations the guest kernel can't do (host I/O, scheduling) trigger
  VMEXIT (HLT or VMCALL) and the host-mode UML kernel handles them.

This is fundamentally different from ptrace/seccomp:

- No stub child process. The guest *is* a KVM VM.
- No SIGSYS / signal-based trap delivery. Hardware delivers traps via
  MSR_LSTAR / EPT violations.
- No PTRACE_SETREGS / set_stub_state. Register changes go through
  KVM_SET_REGS / KVM_SET_SREGS ioctls.
- Page table management is shared between the UML kernel page tables
  (which already exist) and the EPT (which the host KVM manages
  through `KVM_SET_USER_MEMORY_REGION`).

Per the contract gating rule, **the ops table must absorb these
differences without exposing them**.

## Per-op mapping

Legend: same as A-01.4. Add **DESIGN** = needs design work in D-01.

### Lifecycle and trap

| Op | KVM impl | Action | Notes |
|---|---|---|---|
| `probe` | `open("/dev/kvm")` + `KVM_GET_API_VERSION` + check `KVM_CAP_X86_LSTAR` (via `KVM_CHECK_EXTENSION`) | NEW | Fast probe; cheap if /dev/kvm exists. |
| `init` | `KVM_CREATE_VM` (one VM per UML instance), set up host-physical memory pool, install identity mapping for the UML kernel pages, `KVM_SET_TSS_ADDR`, `KVM_CREATE_IRQCHIP` if needed | DESIGN | The "no BIOS, construct initial state" path; D-01 spec confirms. |
| `shutdown` | close VM fd, close vCPU fds, free guest memory pool | NEW | Symmetric. |
| `run_userspace` | `ioctl(vcpu_fd, KVM_RUN)` on the current vCPU; on return inspect `kvm_run->exit_reason`; dispatch to `handle_syscall` (LSTAR), `handle_io_inst`, `handle_ept_violation` (page fault), `handle_hypercall`, etc. | DESIGN (D-02) | The hot loop. Single chokepoint, same shape as ptrace/seccomp `run_userspace`. The VMEXIT/VMENTRY *lifecycle* lives inside this op — invisible to the caller. **This confirms the ops table is right.** |

### Memory

| Op | KVM impl | Action | Notes |
|---|---|---|---|
| `mm_attach` | Allocate one guest physical address-space slot for this mm; set up the EPT mappings for the kernel-side identity portion; create a guest CR3 value bound to this mm's pgd | DESIGN (D-03) | Per-mm. No stub child. State stored in `mm_id` (see "Side-channel check" below). |
| `mm_detach` | Tear down memory slots; free guest pgd | NEW | Per-mm. |
| `mm_map` | `KVM_SET_USER_MEMORY_REGION` to map host VMA into guest physical; update guest pgd entries to map `va` → guest physical with `prot` | DESIGN (D-03) | Today's `phys_fd` argument is what the ptrace/seccomp stubs `mmap` from; KVM impl does the host-side `mmap(phys_fd)` itself to get a host VA, then uses that as `userspace_addr` in the slot. Signature matches. **No new arg required.** |
| `mm_unmap` | Punch hole in slot or invalidate slot region; clear guest pgd entries; INVEPT | DESIGN | Symmetric. |

### Scheduling

| Op | KVM impl | Action | Notes |
|---|---|---|---|
| `thread_create` | `KVM_CREATE_VCPU` for a new vCPU; allocate kvm_run shared mmap; bind to this kernel thread | DESIGN (D-04) | The jmp_buf in `task->thread` becomes "vCPU descriptor" instead. The ops table doesn't care; the abstraction is "set up per-thread state". |
| `thread_start_idle` | Same as thread_create for vCPU 0 | NEW | |
| `context_switch` | Save current vCPU state via KVM_GET_REGS (or just rely on KVM saving it on next KVM_RUN); program next mm's guest CR3 into the next vCPU before its next KVM_RUN. The actual "switch" is much cheaper because both threads share the same VM — only CR3 changes. | DESIGN (D-04) | task pointers in, CR3 swap inside. Signature matches. |
| `ipi_send` | `KVM_SIGNAL_MSI` or write the LAPIC ICR; on receiving vCPU, IRQ delivery happens inside the guest | NEW | |

### Time

| Op | KVM impl | Action | Notes |
|---|---|---|---|
| `read_clock_ns` | Either host `clock_gettime(CLOCK_MONOTONIC_RAW)` (if host TSC reliable across vCPUs) or `KVM_GET_CLOCK` (returns guest-relative ns). Plan from D-01 Q2: KVM clock if available, host TSC fallback. | NEW | Hot. |
| `set_timer` | TSC-deadline mode: `KVM_SET_MSRS` with MSR_IA32_TSC_DEADLINE. Periodic: emulate via TSC-deadline + reprogram on each fire. Disable: clear deadline. | DESIGN | Mode tag handles all 3 cases inside the impl. **Signature matches.** |
| `read_persistent_clock_ns` | host `clock_gettime(CLOCK_REALTIME)` | NEW | |

### Debug

| Op | KVM impl | Action | Notes |
|---|---|---|---|
| `init_thread_regs` | KVM_SET_REGS / KVM_SET_SREGS to backend-safe defaults (CS=ring-3 selector, IP=program entry, etc.) | NEW | |
| `read_guest_regs` | KVM_GET_REGS + KVM_GET_SREGS for the vCPU bound to @t | NEW | KGDB consumer. |
| `write_guest_regs` | KVM_SET_REGS + KVM_SET_SREGS | NEW | |

## Side-channel check

KVM cross-op channels:

1. **`task->thread`** — holds the per-vCPU descriptor (pointer to the
   per-thread `struct kvm_vcpu_um` we'll add). Same channel as
   `task->thread.switch_buf` for ptrace/seccomp. ✅
2. **`mm_id`** — holds the per-mm guest pgd value, per-mm memory-slot
   IDs, per-mm CR3 value. Re-uses the existing `stack` field as
   "guest-physical region base", `pid` field as "guest-CR3 cookie",
   plus we need at most 1–2 more fields (`memslot_id`, `kvm_pgd`).
3. **Backend singleton** — VM fd, KVM API version, vCPU table. Stored
   inside `kvm_backend.c` static state. ✅
4. **`kvm_run` shared mmap** — per-vCPU; lives in the per-thread
   descriptor. ✅

**No side channels needed.** The ops table holds.

## On `mm_id` field expansion

KVM needs slightly different per-mm state than seccomp:

| Field | Used by | Notes |
|---|---|---|
| `pid` | ptrace, seccomp | host child PID; KVM doesn't use (vCPU thread is the kernel thread itself) |
| `stack` | ptrace, seccomp | stub_data physical page; KVM repurposes as "memory slot base GPA" |
| `sock` | seccomp | socketpair; KVM unused |
| `syscall_data_len` | seccomp | KVM unused |
| `syscall_fd_num`/`map` | seccomp | KVM unused |
| (new) `kvm_pgd` | KVM | guest CR3 cookie |
| (new) `memslot_id` | KVM | KVM memory slot index for this mm |

Two design options (same Q from A-01.5):

A. **One struct with all fields.** Add 2 ints for KVM. ~16 bytes
   waste in non-KVM builds. Trivial.
B. **Per-backend allocation.** New `mm_id_alloc`/`mm_id_free` ops.

**Recommendation: option A.** Reasons (now confirmed across all three
backends):

- Total waste is ≤24 bytes per mm.
- No allocator round-trip in the hot path.
- Single union approach: rename to `struct um_backend_mm_state`,
  document field ownership in `backend-contract.rst`.
- Backwards compatible — `mm_id` callers in unrelated code (e.g. the
  ptraced-child kill path) need no change.

This makes A-01.8 Q2 resolution: **global per-backend singleton +
shared `struct mm_id` with documented field ownership.** No backend-
private void *.

## On `task->thread` field expansion

Similar story for per-thread state:

| Field | Used by | Notes |
|---|---|---|
| `switch_buf` | ptrace, seccomp | jmp_buf |
| `regs` | all | uml_pt_regs |
| `arch.faultinfo` | all | populated per-trap |
| (new) `kvm_vcpu` | KVM | pointer to per-vCPU descriptor (allocated separately because of per-vCPU `kvm_run` mmap) |

Plan: union the fields in `struct thread_struct`. Done in D-04.

## Things the KVM backend introduces that the ops table doesn't expose

These all live *inside* KVM impls of the core ops; they don't escape:

- **VMENTRY/VMEXIT lifecycle.** Internal to `run_userspace`. Each
  KVM_RUN exits with `kvm_run->exit_reason`; the impl dispatches.
- **Hypercalls (VMCALL).** Used for guest→host requests. Internal to
  `run_userspace` exit handling.
- **TSC offset management.** Internal to `read_clock_ns` / `set_timer`.
- **Nested-virt detection.** Internal to `probe`. Per D-05, if nested
  virt is detected and KVM is slower than seccomp, `probe` returns
  failure; `init_backend` arbiter falls back to seccomp.
- **EPT invalidation.** Internal to `mm_unmap` and `mm_map` if
  re-mapping. Not exposed.

All of these are KVM-specific concerns that the architecture doc
warned would be "KVM-specific extensions, not part of the core
contract." **They are correctly invisible to the caller.**

## What if a future KVM-specific op IS needed?

The contract supports adding ops without bumping `UM_BACKEND_CONTRACT_VERSION`
(only signature changes bump the version). New ops can be added to
the end of `struct um_backend_ops`; old backends report `NULL` and
the dispatch macro returns `-ENOSYS`.

Concrete examples that *might* need an op someday but don't today:

- `set_breakpoint` / `add_watchpoint` — for hardware-breakpoint KGDB
  via KVM debug ioctls (KVM_SET_GUEST_DEBUG). Future C-11 work.
- `inject_irq` — for direct IRQ injection via KVM_INTERRUPT, instead
  of going through `ipi_send`. Optimization path; not needed today.
- `pause_all_vcpus` / `resume_all_vcpus` — for snapshot/restore in
  fuzz profile. Future C-09.

Each can be added without touching ptrace or seccomp impls.

## Risk: KVM may need a different signature for `mm_map`

The current signature passes `phys_fd` + `offset`. KVM doesn't use
file descriptors directly — it uses host VAs. The KVM impl can mmap
the fd to get a host VA, but that's a per-call cost.

**Resolution:** the KVM impl mmaps the fd *once* per slot and caches
the host VA in `mm_id->memslot_userspace_addr` (added to the union
state). Subsequent `mm_map` calls on the same fd reuse the cached
mapping. No signature change.

If profiling later shows the cache lookup dominates, we can add a
`mm_map_va(mm_id, va, len, prot, host_va)` op that takes a pre-mapped
host VA, used only by KVM and skipped by ptrace/seccomp. Non-breaking
addition.

## Validation criterion

Per A-01 spec:
> Three implementation sketches (ptrace, seccomp, KVM) fit the
> contract without per-backend extensions to the core ops.

KVM fits. Every op maps to a KVM construct without changing the
op signature. The KVM-specific complexity (VMEXIT dispatch,
EPT management, nested virt) is internal to the ops impls.

**Verdict: contract is sound for KVM.**

## What this *doesn't* validate

- Whether the KVM impl is feasible at the cost target (~100 ns
  syscall). That is workstream D's `06-conformance.md` to prove via
  bookend benchmark.
- Whether single-backend KVM-only build (`CONFIG_UM_BACKEND_KVM_ONLY`)
  produces a smaller / faster binary than dynamic. Confirm in A-04.
- Whether KASAN works with KVM EPT mappings. Per D-README, it might
  not — that's a profile constraint (prod-fast doesn't include KASAN
  anyway), not an ops-table issue.

## Next subtask

A-01.7: walk the inventory and verify every site has a destination op.
