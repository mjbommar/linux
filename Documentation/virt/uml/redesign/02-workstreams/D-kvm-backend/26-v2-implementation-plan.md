# Memo 26 — v2 KVM backend: detailed implementation plan

**Date:** 2026-04-28
**Audience:** Whoever is implementing v2 after memo 25's prerequisite
refactors land
**Companion memos:** 23 (v1 fix plan), 24 (clean-slate items + ELI5),
25 (mechanical restart + ARCH=um refactors)

This memo picks up where memo 25 leaves off: the 12 ARCH=um core
refactors are done, v1 is archived, the v2 stub directory exists. Now
we actually build v2.

The plan is **10 phases (A–J) over ~12 weeks** producing a
~1500-LoC v2 backend that hits all 7 success criteria from memo 25
Part 4 and the project vision items from
`Documentation/virt/uml/redesign/01-vision-and-goals.md`.

---

## Vision recap (what "done" looks like)

From memo 24 + memo 25, the v2 backend is:

- **TDP-based**: KVM walks `mm->pgd` directly via per-mapping memslots.
  Zero shadow PT machinery. KVM's mmu_notifier handles cross-vCPU
  coherence.
- **Per-CPU vCPUs**: N vCPUs equal to host CPU count. UML scheduler
  picks tasks onto vCPUs like Linux schedules threads onto CPUs.
- **Per-mm host worker process**: each guest mm is its own host process
  (memo 25 refactor 4). Cross-mm collisions structurally impossible.
- **VMCALL hypercalls**: guest syscalls trap via `vmcall` →
  `KVM_EXIT_HYPERCALL`. No OUT-to-port magic, no LSTAR trampoline.
- **Standard memory layout**: `uml_physmem` at PML4[256+] (memo 25
  refactor 1). User mms have empty kernel-half except for shared
  per-VM IDT/TSS pages.
- **First-class observability**: ftrace tracepoints on every guest
  exit, every memslot op, every hypercall (memo 25 refactor 7).
- **Standard threading**: per-mm worker process forks for new mms;
  inside each worker, pthreads for vCPUs.
- **SMP from day one**: real synchronization, real cross-CPU IPIs via
  KVM's request mechanism.

Target size: **~1500 LoC** in `arch/um/backend/kvm-v2/` (vs v1's ~6000).

---

## Module architecture

```
arch/um/backend/kvm-v2/
├── Kconfig                   # CONFIG_UM_BACKEND_KVM_V2
├── Makefile
├── README.md                 # quickstart + memo pointers
├── kvm_v2.h                  # internal header (struct definitions)
│
├── init.c          (~150)    # backend probe + register ops
├── context.c       (~200)    # per-VM context lifecycle
├── memslot.c       (~250)    # per-region memslot + mmu_notifier
├── vcpu.c          (~200)    # per-CPU vCPU pool + KVM_CREATE_VCPU
├── sched.c         (~200)    # task → vCPU dispatch + CR3 swap
├── hypercall.c     (~150)    # KVM_EXIT_HYPERCALL → syscall dispatch
├── exception.c     (~200)    # IDT setup + #PF/#GP/#UD/#DE/#BP/#OF
├── signal.c        (~100)    # SIGALRM preemption + sigmask
├── smp.c           (~100)    # KVM_REQ_TLB_FLUSH cross-vCPU IPIs
└── trace.h         (~100)    # KVM-specific tracepoints
                  ─────
                   ~1650
```

**Module boundaries:** each .c file owns a clearly-defined subsystem.
No cross-file shared mutable state without an explicit handoff via
the internal header. No "thread.c" mega-file — that pattern was v1's
~5000-LoC single point of complexity.

---

## Phase A — KVM context bring-up (1 week)

**Goal**: backend can be selected, opens `/dev/kvm`, creates a VM, and
returns from `init` without doing anything else.

### A.1 — `init.c`: backend probe + ops registration (DONE — `1a83522e3ea4`)

- Replace the v2 stub with a real init.
- Probe `/dev/kvm` via `os_open_file`. Fail gracefully if absent
  (return `-ENODEV`, system falls back to seccomp).
- Capability negotiation: `KVM_CHECK_EXTENSION` for required caps
  (`KVM_CAP_SYNC_REGS`, `KVM_CAP_SET_GUEST_DEBUG`, `KVM_CAP_HYPERV`
  if used). Fail if any required cap is missing; log which one.
- Register the v2 ops table (from memo 25 refactor 2) via
  `um_register_backend("kvm-v2", &kvm_v2_ops)`.
- ftrace: `TRACE_EVENT(kvm_v2_init)` with cap bitmap.

**Exit criteria:** boot with `backend=force=kvm-v2`. Boot logs show
"kvm-v2: probed, vm_fd=N, caps=0x...". UML continues without crashing.
No actual guest execution yet — system uses fallback for ops not
implemented.

### A.2 — `context.c`: per-VM context lifecycle (DONE — `427f1d88cc42`)

- `struct kvm_v2_vm`: `int vm_fd`, `u64 caps`, `struct list_head
  memslots`, `spinlock_t lock`. One per UML kernel invocation.
- `kvm_v2_vm_create`: `KVM_CREATE_VM`, set up CPUID via
  `KVM_SET_CPUID2` (host-passthrough with curated mask matching v1's
  filter — RDRAND/RDSEED/XSAVE/AVX family).
- `kvm_v2_vm_destroy`: close vm_fd, free memslot list, free struct.
- `KVM_SET_TSS_ADDR` to a fixed gpa (e.g., `0xfffd0000`) — KVM Intel
  requires this for unrestricted-guest mode.
- `KVM_SET_IDENTITY_MAP_ADDR` to another fixed gpa — same reason.

**Exit criteria:** unit test creates a VM, destroys it, no resource
leaks.

### A.3 — vCPU placeholder (DONE — `1066947fd4d3`)

- Create one vCPU at init time as a placeholder (Phase C will replace
  with the per-CPU pool).
- `KVM_CREATE_VCPU` slot 0, mmap the kvm_run, log run_size.

**Exit criteria:** vcpu_alloc visible in dmesg.

---

## Phase B — Memory model: TDP + memslots (2 weeks)

**Goal**: any guest VA the user task expects to be mapped is reachable
via TDP walking `mm->pgd` directly, with `KVM_SET_USER_MEMORY_REGION`
adding memslots per backing-file region.

### B.1 — `memslot.c`: memslot allocator + lookup (3 days)

- `struct kvm_v2_memslot`: gpa, hpa-equivalent (host_va), size, slot_id.
- Per-VM memslot list, indexed by gpa for lookup.
- Slot ID allocator: bitmap of free slots up to `KVM_USER_MEM_SLOTS`
  (typically 32768).
- ftrace: `TRACE_EVENT(kvm_v2_memslot_add/del)`.

### B.2 — Backend op `mm_region_added` → `KVM_SET_USER_MEMORY_REGION add` (3 days)

- Allocate a slot id from the per-mm worker's bitmap (Phase D's worker
  process owns the bitmap; for now use per-VM until the worker model
  lands in Phase D).
- `KVM_SET_USER_MEMORY_REGION` with `userspace_addr = host_va of region`
  and `guest_phys_addr = host_va` (identity mapping host→guest in
  the giant slot model; v2 may switch to non-identity later).
- Insert into per-VM list.

**Important:** under the per-mm host worker model (memo 25 refactor 4),
each worker has its OWN VM context with its own memslot table. Memslot
entries don't cross worker process boundaries. This is what gives us
the natural cross-mm isolation v1's shadow-PT model lacked.

### B.3 — Backend op `mm_region_removed` → memslot delete (2 days)

- Look up slot by gpa.
- `KVM_SET_USER_MEMORY_REGION` with `memory_size = 0` (KVM's delete
  syntax).
- Free the slot id, remove from list.
- KVM's mmu_notifier auto-invalidates EPT.

### B.4 — Backend op `mm_region_protected` → memslot flag update (1 day)

- KVM memslot flags include `KVM_MEM_READONLY`. For prot transitions
  involving R/W toggle, delete + re-add with new flags.
- For RWX-only changes (e.g., toggling NX), KVM doesn't enforce at the
  memslot level — guest pgd determines US/RW/NX. Just regenerate the
  guest pgd entry via the standard kernel mm path; KVM walks it next
  access.

### B.5 — CR3 = `__pa(mm->pgd)` direct swap (3 days)

This is where memo 25 refactor 1 (uml_physmem at PML4[256+]) becomes
load-bearing. After R1, `mm->pgd`:
- PML4[0..255]: user mappings (was: also kernel direct map; now: clean)
- PML4[256..511]: kernel direct map + kernel text + per-VM IDT/TSS
  page

Setting guest CR3 to `__pa(mm->pgd)` walks the user-half cleanly
(US=1 entries). The kernel-half has US=0 entries the user CPL=3 walks
never reach (canonical-sign-extended boundary at bit 47).

- New `kvm_v2_load_cr3(vcpu, pgd)`: builds an SREGS update with
  `cr3 = __pa(pgd)`.
- Called from Phase C's task→vCPU dispatch.

### B.6 — mmu_notifier validation (3 days)

KVM registers an mmu_notifier on the VM's mm at `KVM_CREATE_VM`. When
the host mm changes (mmap/munmap/mprotect via `os_map_memory` etc.),
the notifier fires and KVM invalidates affected EPT entries. This is
how cross-vCPU coherence happens automatically.

- Verify the notifier is firing: instrument `mmu_notifier_invalidate_*`
  with a tracepoint, run `single_dlopen` from
  `tools/testing/selftests/um/cpython-parity/repros/`, confirm the
  notifier hits the right ranges.

**Exit criteria:** A trivial guest binary that does `mmap(NULL, 4096,
PROT_READ|PROT_WRITE, MAP_ANONYMOUS, -1, 0); *p = 0xdeadbeef; munmap`
runs to completion with no shadow PT, no DIVERGE warnings.

---

## Phase C — Per-CPU vCPU pool + task dispatch (2 weeks)

**Goal**: replace v1's per-task vCPU model with N vCPUs (= host CPU
count). UML scheduler picks tasks onto vCPUs.

### C.1 — `vcpu.c`: per-CPU vCPU pool (3 days)

- `struct kvm_v2_vcpu`: `int fd`, `void *run`, `int cpu`, `pthread_t
  thread`, `atomic_t state`.
- At `init`: create `nr_cpu_ids` vCPUs via `KVM_CREATE_VCPU`. mmap
  each kvm_run.
- Pin each vCPU's pthread to its host CPU via `sched_setaffinity`.
- ftrace: `TRACE_EVENT(kvm_v2_vcpu_create)`.

### C.2 — `sched.c`: task → vCPU dispatch (4 days)

- UML scheduler calls `backend->vcpu_run(regs)` from the cooperative
  scheduler (or from the new pthread-based scheduler if memo 25
  refactor 4 lands the standard threading model).
- vcpu_run picks the current host CPU's vCPU.
- Sets up vCPU state for this task:
  - CR3 ← `__pa(current->active_mm->pgd)`
  - kregs ← from `regs` (use sync_regs if available)
  - sregs.fs.base ← `regs->gp[HOST_FS_BASE]` (TLS)
  - sregs.gs.base ← `regs->gp[HOST_GS_BASE]`
- `KVM_RUN`.
- On exit: marshal vCPU state back into `regs`, dispatch by exit reason.

### C.3 — Sync regs fast path (2 days)

- Use `KVM_CAP_SYNC_REGS` to read/write GPRs/SREGS via the mmap'd
  kvm_run struct rather than ioctls. Saves ~2 ioctls per syscall.
- Set `run->kvm_valid_regs |= KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS`
  at entry.
- Read `run->s.regs.regs` / `run->s.regs.sregs` at exit.

### C.4 — Per-vCPU FPU state (3 days)

- KVM manages FPU state on the vCPU. UML's per-task FPU buffer (from
  `arch_thread.fpu`) is loaded into the vCPU on dispatch.
- `KVM_SET_XSAVE` + `KVM_GET_XSAVE` (or `KVM_SET_FPU` if XSAVE
  unavailable).
- Save back to `arch_thread.fpu` on dispatch-out.
- This replaces v1's `kvm_fpu_capture_for_fork` / `kvm_fpu_install_on_first_run`.

**Exit criteria:** A guest binary that uses XMM/AVX (e.g.,
`memset(buf, 0xa5, 4096)` which glibc compiles to `vmovaps`) produces
correct results across context switches.

---

## Phase D — VMCALL hypercall syscall path (2 weeks)

**Goal**: guest syscalls trap via `vmcall` (KVM_EXIT_HYPERCALL), not
via SYSCALL+OUT trampoline. Eliminates the entire LSTAR/bootstrap
machinery.

### D.1 — Guest syscall ABI design (2 days)

UML guest kernel's syscall entry currently uses standard x86_64
SYSCALL. For v2, change to:

- User code does `syscall` as normal.
- UML kernel's LSTAR points at a hypercall trampoline (not the v1
  bootstrap one).
- Trampoline:
  ```asm
    swapgs                    ; standard x86_64 SYSCALL entry
    mov %rsp, %gs:cpu_temp   ; save user RSP
    mov %gs:kernel_rsp, %rsp ; switch to kernel stack
    push %r11                ; save user flags
    push %rcx                ; save user RIP
    mov $UM_KVM_HC_SYSCALL, %rax  ; hypercall number
    vmcall                    ; -> KVM_EXIT_HYPERCALL
    ; return path:
    pop %rcx                  ; restore user RIP
    pop %r11                  ; restore user flags
    mov %gs:cpu_temp, %rsp   ; restore user RSP
    swapgs
    sysretq
  ```

This is much shorter than v1's ~600-byte LSTAR trampoline (which had
gadget paths, bootstrap GDT/IDT references, etc.). Estimated ~50
bytes.

### D.2 — `hypercall.c`: KVM_EXIT_HYPERCALL dispatch (3 days)

- `enum um_kvm_hc { UM_KVM_HC_SYSCALL = 1, UM_KVM_HC_PF = 2,
  UM_KVM_HC_GP = 3, ... }`.
- Hypercall handler reads `run->hypercall.nr` and dispatches.
- For `UM_KVM_HC_SYSCALL`: marshal kregs → uml_pt_regs, call generic
  `handle_syscall`, marshal result back, return to vmcall site.

### D.3 — Hypercall return semantics (2 days)

After the hypercall, vmcall returns to the guest at the next instruction.
The trampoline pops user RIP/RSP/RFLAGS and `sysretq`'s back to user.

- For syscalls that don't return immediately (e.g., signal delivery),
  the hypercall handler can modify the return state via
  `run->s.regs.regs` (sync_regs) before re-entering KVM_RUN.

### D.4 — Replace LSTAR programming (2 days)

- `KVM_SET_MSRS` for `MSR_LSTAR` points at the new trampoline (which
  lives in a per-VM kernel-half memslot, not in user mm pgd).
- `MSR_STAR` and `MSR_FMASK` programmed once at vCPU init.

### D.5 — Validate against the gate (3 days)

- Run cpython-parity gate. Expect ≥ 21/21 since the syscall path is
  now standard x86_64-style with no bootstrap aliases.
- Run `single_dlopen × 100`. Expect 0 flakes (Bug B's mechanism — user
  RIP loaded with corrupt pointer — should be impossible since the
  user never sees the trampoline VA).

**Exit criteria:** gate at 21/21 across 10 trials. `single_dlopen`
0/100 flakes.

---

## Phase E — Exception handling: IDT/TSS without bootstrap pages (2 weeks)

**Goal**: handle guest exceptions (#PF, #GP, #UD, #DE, #BP, #OF, #DF)
via standard KVM mechanisms, with no per-mm pages installed in user
mms.

### E.1 — IDT in dedicated guest page (3 days)

- Allocate a dedicated guest physical page at boot (e.g., gpa
  `0xfffe0000`).
- Map via memslot at a known kernel-half guest VA (e.g.,
  `0xffffffff_ffff_e000`).
- Build IDT contents in the page: vector table pointing at exception
  handlers (which live in the same page or adjacent ones).
- `KVM_SET_SREGS.idt.base` = the guest VA, `idt.limit = 256*16 - 1`.

### E.2 — Per-vCPU IST stacks (2 days)

- N pages allocated at boot (one per vCPU).
- Mapped via memslot at known VAs, indexed by vCPU id.
- TSS IST1 = vCPU's IST stack top.
- `KVM_SET_TSS_ADDR` for the per-VM TSS region (set at Phase A.2).

### E.3 — Exception handler dispatch (5 days)

For each exception we care about:

- **#PF (vector 14):** handler does vmcall(UM_KVM_HC_PF, cr2, ec).
  UML's #PF handler in `arch/um/kernel/trap.c` maps to either
  `do_page_fault` (kernel #PF, panic) or `segv_handler` (user #PF,
  deliver SIGSEGV).
- **#GP (13):** vmcall(UM_KVM_HC_GP, ec). Deliver SIGSEGV to user
  task.
- **#UD (6):** vmcall(UM_KVM_HC_UD). Deliver SIGILL.
- **#DE (0):** vmcall(UM_KVM_HC_DE). Deliver SIGFPE.
- **#BP (3):** vmcall(UM_KVM_HC_BP). Deliver SIGTRAP.
- **#OF (4):** vmcall(UM_KVM_HC_OF). Deliver SIGSEGV (rare).
- **#DF (8):** panic. Should never happen post-Phase-E.

Each handler is small (~30 bytes of asm), all in the same dedicated
guest page.

### E.4 — Validate exception handling (4 days)

- Test that user code triggering each exception delivers the right
  signal.
- Test recovery from #PF (lazy mmap fault → fill page → continue).
- Test that #DF doesn't fire under normal operation.

**Exit criteria:** all of v1's SEC.1, SEC.2, BUG.1-5 audit cases
covered by v2's exception path. Every signal delivery validated by
a focused test.

---

## Phase F — Signal/preemption (1 week)

**Goal**: SIGALRM (UML's timer-driven preemption) cleanly preempts
KVM_RUN; no other host signal interferes with KVM_RUN.

### F.1 — `signal.c`: per-vCPU sigmask (3 days)

- At vCPU init, install `KVM_SET_SIGNAL_MASK` with everything blocked
  except `SIGALRM`.
- v1 also unblocked `KVM_UM_KICK_SIGNAL` (SIGRTMIN+5) for cross-vCPU
  shootdown — **v2 doesn't need this** because TDP + mmu_notifier
  handle cross-vCPU coherence.
- Per-vCPU pthread sigmask set via `pthread_sigmask` at thread start.

### F.2 — Preemption-on-SIGALRM (2 days)

- KVM_RUN exits with -EINTR when SIGALRM arrives.
- Dispatch loop catches -EINTR, marshals current state, calls
  `interrupt_end()` (which runs UML's scheduler), re-enters KVM_RUN.
- Standard pattern, well-trodden in v1.

### F.3 — No bootstrap-window special handling (2 days)

v1 needed special handling for "EINTR fired mid-bootstrap-IRETQ"
(memo 22 §"EINTR-bootstrap-RIP-preservation"). v2 has no bootstrap
sequence (Phase D and E eliminated it), so no special case. EINTR
just re-enters KVM_RUN with the same kregs.

**Exit criteria:** SIGALRM-driven preemption works; UML's scheduler
runs other tasks; original task resumes correctly.

---

## Phase G — SMP (1 week)

**Goal**: SMP guest (multiple guest CPUs) works correctly, parallel
workloads inside the guest scale.

### G.1 — Multi-vCPU coordination (3 days)

- Phase C already created N vCPUs. Now make them runnable concurrently.
- UML scheduler dispatches different tasks to different vCPUs (memo 25
  refactor 4 + Phase C cooperate here).
- KVM's mmu_notifier handles cross-vCPU TLB shootdown automatically
  via `kvm_make_all_cpus_request(KVM_REQ_TLB_FLUSH)`.

### G.2 — `smp.c`: cross-vCPU IPI (if needed) (2 days)

For UML's intra-guest IPIs (e.g., `smp_call_function`), use a
hypercall: `vmcall(UM_KVM_HC_IPI, target_cpu, action_nr)`. The
backend dispatches to the target vCPU via signal or by setting a
flag the vCPU checks on next entry.

### G.3 — SMP validation (2 days)

- Boot UML with `--with-cpus=4`.
- Run `make ARCH=um -j4` of the kernel inside the guest. No corruption,
  no hang, no divergence.

**Exit criteria:** SMP build inside SMP guest works.

---

## Phase H — Performance (1-2 weeks)

**Goal**: cpython-parity gate runs at ≤ 1.2× the seccomp wall-clock
time. Identify and fix any pathological hot paths.

### H.1 — Baseline measurements (2 days)

- Instrument syscall count, vmexit count, time-per-syscall, time-per-
  vmexit.
- Run cpython gate, capture profiles.

### H.2 — Optimize hot paths (5-7 days)

Likely candidates (from v1 experience):
- SREGS-skip cache: skip KVM_SET_SREGS when nothing changed.
- Hypercall fast path: for common syscalls (read/write/mmap/munmap),
  use a tight dispatch table.
- Memslot caching: keep a per-vCPU last-used-memslot pointer to avoid
  list walk on every fault.
- Sync regs (Phase C.3 already did this).

### H.3 — Target validation (2 days)

- Re-run gate, confirm ≤ 1.2× seccomp wall-clock.
- If slower, profile and identify the bottleneck.

**Exit criteria:** gate at ≤ 1.2× seccomp wall-clock.

---

## Phase I — Polish + documentation (1 week)

### I.1 — `trace.h`: comprehensive ftrace (2 days)

- Tracepoint per exit reason, per memslot op, per hypercall.
- Standard `TRACE_EVENT()` boilerplate; integrate with refactor 7's
  generic tracepoints.

### I.2 — KUnit tests (2 days)

- Unit test for memslot allocator.
- Unit test for hypercall dispatch table.
- Unit test for IDT setup.
- Unit test for vCPU pool.

### I.3 — Documentation (3 days)

- `arch/um/backend/kvm-v2/README.md` — full design doc.
- `Documentation/virt/uml/backends.rst` update — v2 promoted from
  EXPERT to default-y.
- Headerdoc on every public function.
- Architecture diagram in `Documentation/virt/uml/kvm-v2-arch.svg`.

### I.4 — Lift `EXPERT` gate (1 day)

`Kconfig` change: `CONFIG_UM_BACKEND_KVM_V2` no longer requires EXPERT,
defaults `y` if `KVM` is available. UML's auto-select picks v2 when
`/dev/kvm` is present.

---

## Phase J — Validation + tier widening (4 weeks)

### J.1 — 24h continuous gate (1 day setup, 24h running)

- Loop the cpython-parity gate for 24 hours.
- Zero flakes required. Any failure → diagnose, fix, restart the 24h.

### J.2 — Tier 1: third-party Python libs (1 week)

- Run pytest against `requests`, `cryptography`, `numpy` under v2.
- 100% pass rate required.
- These exercise C-extension load paths that stressed v1.

### J.3 — Tier 2: pip install + pytest (1 week)

- Full `pip install` of a real package set inside guest.
- Network via loopback (requires `CONFIG_UML_NET_VECTOR=y`).
- pytest the installed packages.

### J.4 — Tier 3: Django/FastAPI server (1 week)

- Real web server inside guest, requests via loopback.
- Validates v2 for production-like workloads.
- Stress: 1000 req/s over an hour, no errors.

### J.5 — Soak test: kernel build under guest (1 week)

- `make ARCH=um -j4` of the kernel itself, inside the guest.
- 30+ minutes, no divergence.
- Validates SMP + heavy mm churn.

**Exit criteria for J:** all 5 sub-tasks pass. v2 declared production-
ready.

---

## Cross-cutting concerns

### Observability (per memo 25 refactor 7)

Every phase adds tracepoints. By Phase I, `trace-cmd record -e
'um_kvm_v2:*'` shows:
- Every memslot add/del with timestamps.
- Every vCPU enter/exit with reason + RIP.
- Every hypercall with nr + args.
- Every signal delivered to a vCPU.
- Every cross-vCPU IPI.

### Backwards compat

- v2 implements the same `um_backend_ops` table as seccomp (memo 25
  refactor 2). UML core code doesn't know which backend is active.
- Boot command line `backend=force=seccomp` always works as fallback.
- The 5 C reproducers stay valid; v2 must pass them.

### v1 archive lifecycle

- `arch/um/backend/kvm-v1-archive/` stays in-tree but `obj-` is empty
  (not built).
- Periodic CI run can compile it to detect bitrot.
- After v2 ships and 6 months of zero issues, propose deletion via
  LKML.

### Risk classes that v2 STRUCTURALLY ELIMINATES

By design, v2 has no:
- Shadow PT staleness (no shadow PT)
- DIVERGE pattern (no shadow vs UML pgd)
- Cross-vCPU TLB shootdown gap (KVM handles)
- Bootstrap-leaf US-violation (no bootstrap pages in user mms)
- Use-after-munmap on stale shadow leaf (mmu_notifier fires)
- Per-task vCPU lifecycle bugs (per-CPU vCPU pool)
- Cross-mm host-VA collision (per-mm worker process)
- LSTAR trampoline complexity (vmcall direct)
- Bootstrap PML4[0] aliasing (no bootstrap install at all)

These are the bug classes that consumed memos 14-22. They don't have
analogues in v2's architecture.

---

## Phase summary

| Phase | Topic | Duration | Deliverable |
|---|---|---|---|
| A | KVM context bring-up | 1 wk | VM created, vcpu0 placeholder |
| B | TDP + memslots | 2 wk | Anonymous mmap works, no shadow PT |
| C | Per-CPU vCPU pool | 2 wk | Task dispatch via vCPU pool, FPU correct |
| D | VMCALL hypercalls | 2 wk | All syscalls via vmcall, no LSTAR trampoline |
| E | Exception handling | 2 wk | IDT in memslot, all exception classes work |
| F | Signal/preemption | 1 wk | SIGALRM preemption clean, no kick signal |
| G | SMP | 1 wk | Multi-vCPU works, intra-guest IPIs |
| H | Performance | 1-2 wk | ≤1.2× seccomp wall-clock |
| I | Polish + docs | 1 wk | ftrace + KUnit + README + Kconfig promotion |
| J | Validation + tiers | 4 wk | 24h soak + Tier 1/2/3 + kernel build |
| **Total** | | **~17-18 weeks (4 months)** | **v2 production** |

Plus memo 25's prerequisites (~12 weeks) → **total ~7 months** from
"start the restart" to "v2 in production." Shorter than v1's actual
calendar time (which has been ~6 months and isn't done) because v2
builds on a clean substrate instead of patching mismatched layers.

---

## What v2 looks like to the user

- `make ARCH=um defconfig && make ARCH=um -j$(nproc)` produces a UML
  binary.
- Boot it: auto-selects v2 if `/dev/kvm` present, else seccomp.
- Boot logs show `um: kvm-v2: vm_fd=N caps=0x... vcpus=8`. No
  reams of debug spam.
- Performance ≈ seccomp + 20% on syscall-heavy, ≈ seccomp + 80% on
  compute-heavy (KVM lets the guest run real instructions).
- `trace-cmd record -e 'um_kvm_v2:*'` gives full visibility for
  debugging.
- Tier 1/2/3 workloads work. Snapshot/record-replay (vision items
  V.1/V.2) port from v1 cleanly because they don't depend on shadow PT.

---

## Vision items unblocked by v2

From `Documentation/virt/uml/redesign/01-vision-and-goals.md`:

- **V.1** (KVM-aware snapshot): straightforward on v2 because vCPU
  state is small + standard.
- **V.2** (record/replay extensions): same.
- **V.3** (syzkaller backend): v2's clean syscall dispatch makes this
  easier.
- **V.4** (profile builds: research/fuzz/prod-fast/sandbox): v2's
  smaller code surface makes profile-specific tuning tractable.
- **V.5** (ARM64 port): v2's ~1500 LoC is much easier to port than
  v1's ~6000.
- **V.6** (RISC-V port): same.
- **V.7** (cross-host CI matrix): v2's standard architecture means CI
  finds bugs in days instead of months.

---

## When to declare v2 done

When all of the following hold simultaneously for two consecutive
weeks:

1. Phase J.1 (24h continuous gate) passes.
2. Phase J.2 (Tier 1) passes.
3. Phase J.5 (kernel build inside guest) passes.
4. v2 implementation is < 2000 LoC (allow 33% over the ~1500 target).
5. ftrace tracepoints cover every exit reason.
6. No open-issue-tagged-blocker in the project tracker.
7. LKML series 7 (kvm-backend-series, the v2 announcement) has at
   least one Reviewed-by from a non-author maintainer.

When 1-7 hold: ship v2, announce on LKML, mark v1-archive for deletion
after 6 months.

---

## What this memo doesn't cover

- ARM64-specific concerns (defer to V.5).
- RISC-V-specific concerns (defer to V.6).
- The strategic "is this even the right product" question (memo 24
  item #10) — v2 assumes "yes, KVM-backed UML is the right shape for
  containers/sandboxes."
- gVisor-style sentinel backend or Firecracker integration (would be
  a different project entirely).

---

## Reading order if you're picking up v2 implementation

1. This memo (26).
2. Memo 25 (prerequisite refactors — must land first).
3. Memo 24 (clean-slate items — strategic context).
4. Memo 23 (v1 fix plan — what NOT to repeat).
5. Memo 22 (v1 bug diagnoses — Bug A is fixed in v2 by design,
   Bug B is fixed in v2 by design).
6. Memo 20 (Stage B design — many ideas reused in v2).
7. v1 archive at `arch/um/backend/kvm-v1-archive/` for "how did v1
   handle X" lookup.
8. Standard KVM API docs:
   `Documentation/virt/kvm/api.rst`.

Welcome to v2. Build something cleaner this time.
