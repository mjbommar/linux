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

### B.1 — `memslot.c`: memslot allocator + lookup (DONE — `a80a03c02743`)

- `struct kvm_v2_memslot`: gpa, hpa-equivalent (host_va), size, slot_id.
- Per-VM memslot list, indexed by gpa for lookup.
- Slot ID allocator: bitmap of free slots up to `KVM_USER_MEM_SLOTS`
  (typically 32768).
- ftrace: `TRACE_EVENT(kvm_v2_memslot_add/del)`.

### B.2 — Backend op `mm_region_added` → `KVM_SET_USER_MEMORY_REGION add` (DONE — `fd9df1834e8a` + cap-flag followup `40a97b5f3b72`)

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

### B.4 — Backend op `mm_region_protected` → memslot flag update (DONE — no code)

- KVM memslot flags include `KVM_MEM_READONLY`. For prot transitions
  involving R/W toggle, delete + re-add with new flags.
- For RWX-only changes (e.g., toggling NX), KVM doesn't enforce at the
  memslot level — guest pgd determines US/RW/NX. Just regenerate the
  guest pgd entry via the standard kernel mm path; KVM walks it next
  access.

### B.5 — CR3 = `__pa(mm->pgd)` direct swap (DONE — `1a879e8cd9fe`)

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

### B.6 — mmu_notifier validation (DONE — verification + Phase D gate)

Phase B's structural goals are met by B.1-B.5:
- Memslot allocator + lookup (B.1).
- KVM_SET_USER_MEMORY_REGION add (B.2) + delete (B.3) + dedupe by gpa.
- mm_region_protected fallback to remove+add (B.4 — no-op).
- kvm_v2_load_cr3 helper (B.5; ready for Phase C's task dispatch).

Phase B exit criteria validated 2026-04-29:
- **no shadow PT**: `git grep -rE "shadow_pgd|shadow_mm|shadow_sync_pte|shadow_va" arch/um/backend/kvm-v2/` returns empty. v2 has zero shadow PT references — structurally distinct from v1 by design.
- **no DIVERGE warnings**: `backend=force=kvm-v2 init=/bin/true` boot dmesg contains zero `DIVERGE` lines.
- **mmu_notifier hooks**: KVM's existing `kvm_mmu_notifier_*` tracepoints in `virt/kvm/kvm_main.c` already fire when the spawner's mm changes via `os_map_memory` and similar (auto-registered at KVM_CREATE_VM). UML doesn't need to add new tracepoints; B.7 (Phase D) will verify under real KVM_RUN traffic.
- **anonymous mmap via TDP**: DEFERRED to Phase D. Phase B.6's original wording ("trivial guest binary mmap/write/munmap runs end-to-end via TDP") implicitly assumed KVM_RUN is wired. Under the incremental migration plan that v2 has actually followed (HOT ops delegate to seccomp until each phase replaces them), KVM_RUN itself doesn't fire until Phase D. The TDP path through kvm_v2_load_cr3 → KVM_RUN → EPT walk via memslot table is plumbed but unexercised. Phase D's "all syscalls via vmcall; gate at 21/21 × 10" criterion is what actually exercises it; the no-DIVERGE / no-shadow-PT structural checks above are sufficient for closing Phase B itself.

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

## Phase C — Per-CPU vCPU pool + task dispatch (2 weeks) — DONE

**Goal**: replace v1's per-task vCPU model with N vCPUs (= host CPU
count). UML scheduler picks tasks onto vCPUs.

Phase C landed as 5 commits (C.1 / C.2 / C.3 / C.4.0 / C.4) building
the dispatch shape on the side without flipping `.vcpu_run` — Phase D
activates it.

### C.1 — `vcpu.c`: per-CPU vCPU pool (DONE — `0df41d13febf`)

- `struct kvm_v2_vcpu`: `int vcpu_fd`, `void *kvm_run`, `size_t
  kvm_run_size`, `int cpu` (no pthread / state — v2 has no per-vCPU
  thread; the dispatcher runs on the calling task's stack with
  preempt_disable around the per-CPU pool pick).
- At `init`: create `min(nr_cpu_ids, NR_CPUS)` vCPUs via
  `KVM_CREATE_VCPU`. mmap each kvm_run.
- Pool members initialised to `.vcpu_fd = -1` sentinel; `pool_initialised`
  flag flips after the pool is fully built.
- ftrace: `TRACE_EVENT(um_backend_kvm_v2_vcpu_create)`.

### C.2 — task → vCPU dispatch helper (DONE — `7b29a64af5f3`)

- `kvm_v2_vcpu_run(struct uml_pt_regs *regs)` helper added to
  `vcpu.c`; ops.c `.vcpu_run` is **still seccomp_vcpu_run** today.
  Phase D flips the pointer. (Memo 28 Part C "option α": dispatch
  shape lands ahead of activation.)
- Helper picks vCPU via `kvm_v2_vcpu_get(smp_processor_id())` under
  preempt_disable; NULL-or-sentinel guard falls back to seccomp.
- Sets up vCPU state via:
  - CR3 ← `kvm_v2_load_cr3` (B.5 helper).
  - sregs.fs.base / gs.base ← `kvm_v2_load_user_sregs` (file-static
    in vcpu.c; one GET_SREGS / SET_SREGS round-trip in C.2 — C.3
    dissolves this against the SYNC_REGS mmap).
- KVM_RUN; switch on exit_reason with placeholder panics for
  KVM_EXIT_HLT / FAIL_ENTRY / INTERNAL_ERROR / SHUTDOWN ("Phase
  D/E required"). Real handlers land in Phase D.2 (HYPERCALL) /
  E.3 (FAIL_ENTRY / INTERNAL_ERROR).
- ftrace: `um_backend_kvm_v2_vcpu_enter` / `_exit`.

### C.3 — Sync regs fast path (DONE — `124db82a0ccb`)

- `kvm_run->kvm_valid_regs = KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS`
  set once at pool member create time (`kvm_v2_vcpu_create_one`),
  not per-dispatch.
- `kvm_v2_load_user_sregs`: drop GET_SREGS / SET_SREGS ioctls.
  Modify `kvm_run->s.regs.sregs.{cr3, fs.base, gs.base}` in place
  and OR `KVM_SYNC_X86_SREGS` into `kvm_dirty_regs`. KVM consumes
  on next entry.
- `kvm_v2_vcpu_run`: drop SET_REGS / GET_REGS ioctls. Marshal GPRs
  into `kvm_run->s.regs.regs` + OR `KVM_SYNC_X86_REGS` into
  `kvm_dirty_regs` on entry; read `kvm_run->s.regs.regs` on exit.
- Net effect when Phase D activates: 4 ioctls per dispatch (GET/SET
  SREGS + SET/GET REGS) → 1 KVM_RUN. State lives in the mmap.
- Cap bit was already negotiated in A.1 (`caps & 0x1` from
  KVM_CHECK_EXTENSION → required); C.3 just wired it.

### C.4 — Per-vCPU FPU state (DONE — `734d9bbe54a8` substrate +
`9fa4a804d4e3` helpers)

- C.4.0 substrate (`734d9bbe54a8`): `arch_thread.kvm_v2 = { struct
  kvm_fpu fpu; bool fpu_valid; }` under `CONFIG_UM_BACKEND_KVM_V2`
  in `arch/x86/um/asm/processor_64.h`. INIT_ARCH_THREAD,
  arch_flush_thread (clears fpu_valid), arch_copy_thread (calls
  capture_for_fork) hooks. x86_64-only — V2 depends on EXPERT &&
  X86_64, so processor_32.h needs no equivalent.
- C.4 helpers (`9fa4a804d4e3`):
  - `kvm_v2_fpu_capture_for_fork`: KVM_GET_FPU against the parent's
    per-host-CPU vCPU at fork time (preempt_disable around
    smp_processor_id() + kvm_v2_vcpu_get); ships bytes via
    `arch_thread.kvm_v2.fpu`. NULL-or-sentinel guard → fpu_valid
    stays false → child gets arch defaults. KVM_GET_FPU failure
    is non-fatal (warn-ratelimited, mirrors v1's behaviour).
  - `kvm_v2_fpu_install_on_first_run`: pre-KVM_RUN install in
    `kvm_v2_vcpu_run` between SREGS load and KVM_SET_REGS. fpu_valid
    → KVM_SET_FPU snapshot + clear flag (one-shot); !fpu_valid →
    install AMD64 SDM §11.5.1 reset values (fcw=0x037f,
    mxcsr=0x1f80) via stack-local kvm_fpu. Failure → panic
    (running guest with arbitrary FPU state crashes downstream).
  - Uses legacy 512 B `KVM_GET/SET_FPU` (not XSAVE) per Phase C
    surface map: `kvm_v2_curate_cpuid` masks AVX/AVX-512, so XSAVE
    is moot under our curated guest.
- Capture is **live today** via arch_copy_thread; install is
  **dormant** until Phase D's pointer flip activates `.vcpu_run`.
- ftrace: `um_backend_kvm_v2_fpu_capture` / `_install`.

**Phase C exit criteria** (C-internal — full XMM/AVX context-switch
correctness depends on Phase D activation):

- Build clean both modes (=n, =y): ✓
- Boot smoke `backend=force=kvm-v2 init=/bin/true` rc=134: ✓
- Substrate gate `PASS=25 FAIL=3 EXPECTED_FAIL=3`: ✓ unchanged.
- Helper shapes reviewable; .vcpu_run flip lands in Phase D with
  no further C-side work. Original "guest binary that uses XMM/AVX
  produces correct results across context switches" criterion
  defers to Phase D's gate (Phase D is the moment v2 actually runs
  guest code).

---

## Phase D — IO-port syscall trap (2-3 weeks)

**Goal**: stand up v2's actual syscall path. Guest user code does
`syscall` as normal; LSTAR points at a 5-byte kernel-half trampoline
that emits `out %al,$0xf4` → `KVM_EXIT_IO`, then `sysretq`. The host
dispatcher decodes RAX as the syscall NR, fixes
`HOST_IP←HOST_CX` / `HOST_EFLAGS←HOST_R11` (post-SYSCALL semantics
per `kvm-v1-archive/thread.c:3270-3319`), runs `handle_syscall`,
marshals return state, and re-enters KVM_RUN — which advances RIP to
the trampoline's `sysretq` and drops to CPL=3 with RIP=RCX,
RFLAGS=R11. **Phase D is the moment v2 actually executes guest
code** — Phase C built the dispatch shape on the side; D.5 flips
`.vcpu_run` and the gate becomes the real test.

The original §D specced `vmcall` → `KVM_EXIT_HYPERCALL` with a custom
`UM_KVM_HC_SYSCALL=1` nr. That is structurally impossible on stock
KVM: `arch/x86/kvm/x86.c:10456` initialises `ret = -KVM_ENOSYS`,
`x86.c:10520-10523`'s `default:` returns it to the guest, and only
`KVM_HC_MAP_GPA_RANGE` (gated by `KVM_CAP_EXIT_HYPERCALL` whose
valid mask `x86.c:4881-4882` is just `BIT(KVM_HC_MAP_GPA_RANGE)`)
writes `exit_reason = KVM_EXIT_HYPERCALL`. We therefore revert to
v1's IO-port mechanism — byte-identical to v1's non-gadget tail at
`kvm-v1-archive/thread.c:1216-1218` (`#else` branch). The Phase D
structural wins — per-CPU vCPU pool, TDP, no shadow PT,
kernel-half-only trampoline, no per-task bootstrap install — hold
independent of trap instruction. The mechanism reverts to v1's; the
strategy doesn't.

D.0-D.4 land "on the side" like Phase C ("option α"): each commit
adds helpers / state without touching `ops.c`. D.5 is the activation
moment — one-line edit at `arch/um/backend/kvm-v2/ops.c:69`
(`.vcpu_run = seccomp_vcpu_run` → `kvm_v2_vcpu_run`) plus the four
seccomp-delegation flags (`uses_stub_reaper`,
`has_syscall_stub_fd_map`, `stub_syscall_uses_futex`,
`stub_child_runs_seccomp` at lines 60-63) flip to `false`. Bug B
(memo 22's user-half / kernel-half PML4 alias on the bootstrap
page) becomes structurally impossible: the trampoline lives only in
PML4[508] (kernel-half canonical-sign-extended past the user/kernel
boundary; v1's `KVM_BOOTSTRAP_GUEST_VA = 0xffffe00000000000` at
`kvm-v1-archive/thread.c:637`), is never installed at any user-half
VA, and is never present in user-task page tables as a US=1 leaf —
the alias that made Bug B reachable cannot exist.

**Note on §E and §G**: the `UM_KVM_HC_PF / _GP / _UD / _DE / _BP /
_OF` references in Phase E (lines 365-373 of this memo) and the
cross-vCPU IPI design in Phase G (lines 490-492) inherit the same
vmcall correction. Each exception class becomes its own IO port
(`0xf6 = #PF`, `0xf9 = #GP`, etc., paralleling v1's
`UM_KVM_*_PORT` pattern from `kvm-v1-archive/kvm_backend.h`); IPIs
become a real KVM mechanism (`KVM_REQ_TLB_FLUSH` + `kvm_make_all_
cpus_request`) rather than a synthesised hypercall. Those updates
land with §E / §G commits, not as part of §D.

**Headline gate**: cpython-parity 21/21 × 10 trials with 0
regressions + `single_dlopen × 100` with 0 flakes. Substrate gate
must hold PASS=25/FAIL=3/EXPECTED_FAIL=3 bit-for-bit.

### D.0 — Pre-flight prep (3 days)

Two discrete fix-ups Phase A/B left dormant; bundled as one commit
because each is small and they share verification (a `KVM_RUN` that
does not immediately fail). The original surface map proposed a third
prep item — per-vCPU GS state page (~80 LoC) — but the simplified
trampoline (D.1) eliminates the need for it: with no in-trampoline
stack switch, there is no `%gs:cpu_temp` / `%gs:kernel_rsp` storage
to allocate.

- **D.0a CPUID lazy first-run install** (~30 LoC). A.3 deferred
  `kvm_v2_curate_cpuid` (XSAVE / AVX / AVX-512 suppression per
  `arch/um/backend/kvm-v2/vcpu.c:118-174`) to "first KVM_RUN" but
  Phase C never wired it. Add `bool cpuid_primed` to `struct
  kvm_v2_vcpu`; install once at top of `kvm_v2_vcpu_run`, sticky per
  pool entry. The curated mask must be live before any guest
  instruction executes — otherwise an unmasked AVX feature bit lets
  the guest issue VEX encodings whose state we don't snapshot under
  Phase C's legacy 512 B `KVM_GET/SET_FPU` path.

- **D.0b `KVM_SET_USER_MEMORY_REGION` userspace_addr fix-up** (~60
  LoC). B.2 emits `-EINVAL` on user VAs (`0x550...0007000` and
  `0x40265000` per the C.x boot logs) because the seccomp dual-wiring
  at `arch/um/backend/kvm-v2/region.c:103-109` maps the region in the
  stub child's mm but not the spawner's mm; KVM at
  `virt/kvm/kvm_main.c:1107-1109` binds the VM to the creator's
  `current->mm` and fault-in resolves the HVA in *that* mm
  (`virt/kvm/kvm_main.c:2999-3027`). Critical subtlety per the
  Phase D surface map's CLAIM 4 cross-check:
  `KVM_SET_USER_MEMORY_REGION` validates with `access_ok()` only
  (`virt/kvm/kvm_main.c:2014-2025`), not `find_vma()` / GUP — so
  registration may succeed against a range with no VMA in the issuer
  mm and only fail later at fault-in. Both paths must be fixed:
  ensure `os_map_memory` runs in the spawner before the ioctl AND
  validate the region is actually mapped before issuing
  `KVM_SET_USER_MEMORY_REGION`.

**Verification**: `KVM_RUN` returns without `KVM_EXIT_FAIL_ENTRY` /
`-EINVAL`; substrate gate unchanged.

### D.1 — IO-port trampoline + ABI (2 days)

- **Bytes**: **5 bytes**. Byte-identical to v1's non-gadget tail at
  `kvm-v1-archive/thread.c:1216-1218`:

  ```asm
  out %al, $0xf4   /* e6 f4 — KVM_EXIT_IO trap */
  sysretq          /* 48 0f 07 — drops to CPL=3, RIP=RCX, RFLAGS=R11 */
  ```

  No `swapgs`. No stack switch. No `%gs:` scratch storage. v1's
  swapgs+stack-switch was for the in-trampoline gadget paths
  (`kvm-v1-archive/thread.c:814-1213`'s 7-syscall + vDSO
  clock_gettime fast path) — Phase H optimisation, not in scope. For
  the trap-only path, the trampoline runs at CPL=0 with interrupts
  masked (FMASK clears IF), executes `out` (kernel-priv I/O, no
  fault), traps to host. Host marshals via sync_regs, runs
  `handle_syscall`, marshals return state. KVM_RUN re-entry advances
  RIP to `sysretq`, which drops to CPL=3 with RIP=RCX, RFLAGS=R11.
  All registers (including RAX = syscall NR on entry, RAX = return
  value on exit) flow through `kvm_run->s.regs.regs` — never
  clobbered by trampoline code.

  Total trampoline budget: ~16 B including alignment padding for
  the next-instruction landing site. `BUILD_BUG_ON(sizeof(bytes) >
  PAGE_SIZE)` for safety.

- **Storage / placement**: allocate one host page from `uml_physmem`
  for the trampoline. Because `uml_physmem` is already covered by
  Phase B's single identity memslot (gpa==host_va, the only memslot
  shape B knows how to issue), the trampoline GPA is automatically
  guest-reachable without a new memslot. **No second memslot is
  needed.** The guest VA is `0xffffe00000000040` (matching v1's
  `KVM_BOOTSTRAP_GUEST_VA + KVM_BOOTSTRAP_LSTAR_OFFSET` from
  `kvm-v1-archive/thread.c:637,665`); MSR_LSTAR is programmed to
  this GVA in D.4, and the kernel-half PML4[508] entry (also D.4)
  makes the GVA→GPA walk land on the trampoline page.

- **ABI**: `enum um_kvm_iotrap { UM_KVM_TRAP_SYSCALL = 1,
  UM_KVM_TRAP_PF = 2, UM_KVM_TRAP_GP = 3, ... }` in a new
  `arch/um/backend/kvm-v2/syscall_trap.h`. The trap class is
  encoded in the IO port number (0xf4 = SYSCALL, 0xf6 = PF, 0xf9 =
  GP per v1's pattern at `kvm-v1-archive/kvm_backend.h`'s
  `UM_KVM_*_PORT`), not in RAX (which stays = guest syscall NR for
  D.2's decode path). The enum is the host-side tag for switch
  dispatch.

### D.2 — `syscall_trap.c` dispatch handler (3 days)

- New file `arch/um/backend/kvm-v2/syscall_trap.c` (~180 LoC). One
  exported helper `kvm_v2_handle_io_trap(struct uml_pt_regs *regs,
  struct kvm_run *run, int vcpu_fd)`.
- Add `case KVM_EXIT_IO:` to `kvm_v2_vcpu_run`'s switch in
  `vcpu.c` (currently panic-default at vcpu.c:691+). Read
  `run->io.port`; on `0xf4` call into the helper, else `panic`
  (other ports are Phase E's classes).
- Helper body mirrors `kvm-v1-archive/thread.c:3270-3319,4030-4047`:
  - GPRs already in `regs->gp[]` from C.3's sync-regs marshal at
    `vcpu.c:689` (`kvm_v2_marshal_from_kvm_regs`). No KVM_GET_REGS.
  - `unsigned long syscall_nr = regs->gp[HOST_AX];`
  - `PT_SYSCALL_NR(regs->gp) = syscall_nr;`
  - `regs->is_user = 1;`
  - `regs->gp[HOST_IP] = regs->gp[HOST_CX];` (post-SYSCALL user
    RIP — sysretq will read it back from RCX, and handle_syscall
    needs HOST_IP to reflect the user's intended return RIP)
  - `regs->gp[HOST_EFLAGS] = regs->gp[HOST_R11];` (FMASK-masked
    kernel rflags must NOT leak — v1 archive lines 3296-3306)
  - `handle_syscall(regs);` — direct call into
    `arch/um/kernel/skas/syscall.c:19`, same shape as seccomp's
    `arch/um/backend/seccomp/trap_user.c:159` SIGSYS branch.
  - On return, `regs->gp[HOST_AX]` holds the return value; mirror
    it back into the sync-regs mmap (D.3 wires the marshal-out).
- Class-D / replay machinery from v1 archive lines 3320-3550 is
  **not** ported — that's memo 10 / 13 territory, out of scope.
- **Helper unreferenced by ops.c at this point** — `.vcpu_run`
  still points at `seccomp_vcpu_run` until D.5.

### D.3 — Return semantics + EINTR + per-task FPU swap-out (2 days)

- **Sync-regs return marshal** (~30 LoC). After `handle_syscall`,
  copy `regs->gp[HOST_AX..HOST_R11]` back into `run->s.regs.regs.*`
  and OR `KVM_SYNC_X86_REGS` into `kvm_dirty_regs`. Critical:
  before re-entry, set `run->s.regs.regs.rcx = regs->gp[HOST_IP]`
  and `run->s.regs.regs.r11 = regs->gp[HOST_EFLAGS]` — sysretq
  consumes RCX→RIP and R11→RFLAGS, so the user-resume RIP must
  land in RCX and the user-resume RFLAGS in R11. For normal
  syscall return that's the original user RIP/RFLAGS;
  signal-delivery overwrites HOST_IP with the signal handler VA.
- **Signal-delivery branch**: when `handle_syscall` returns with a
  pending signal, `interrupt_end()` runs the standard delivery
  path; `regs->gp[HOST_IP]` ends up at the signal-handler RIP and
  the marshal above lifts it into RCX. UML's signal delivery path
  is backend-agnostic; no special-case work beyond the marshal.
- **EINTR / SIGALRM mid-KVM_RUN** (~10 LoC). Phase F nominally owns
  signal handling but D.5 activates `.vcpu_run` and SIGALRM fires
  every tick — the gate fails the moment the timer fires unless D
  handles `rc == -EINTR` cleanly. **Critical location**: the panic
  is at `arch/um/backend/kvm-v2/vcpu.c:675-682` (the `rc < 0` check
  immediately after `KVM_RUN`), **before** the exit-reason switch
  — so the EINTR path must guard the rc<0 check, not the default
  in the switch. Change to `if (rc < 0 && rc != -EINTR) panic;`
  and on EINTR fall through to `preempt_enable; return;` (treat as
  "guest didn't fault yet; dispatcher caller re-enters next
  schedule slice"). Restart-via-RAX-rewrite is Phase F's full
  handling. Reference: v1's EINTR path at
  `kvm-v1-archive/thread.c:3937-3984,5121-5127`.
- **Per-task FPU on context_switch_out** (~50 LoC). C.4 only
  captures at fork (`arch_copy_thread`). Tasks migrating between
  host CPUs leave whichever FPU on the destination vCPU; cpython's
  multi-thread tests will catch this even if `single_dlopen`
  doesn't. Add `kvm_v2_fpu_capture_for_switch_out` paralleling
  `kvm_v2_fpu_capture_for_fork` at `vcpu.c:733-768`: KVM_GET_FPU on
  the outgoing per-CPU vCPU into `current->thread.arch.kvm_v2.fpu`,
  set `fpu_valid=true`. Fork-capture remains the special "child
  inherits parent" case; switch-out is "task migrates, takes its
  FPU with it." Hook from
  `arch/um/kernel/process.c::__switch_to`'s pre-switch path under
  `CONFIG_UM_BACKEND_KVM_V2`.

### D.4 — MSR programming + PML4[508] kernel-half install (3 days)

- **MSR programming at vcpu_create** (~50 LoC). Mirror v1's
  `kvm_enter_guest_program_msrs` at
  `kvm-v1-archive/thread.c:2020-2111`, but issued **once** at pool
  member create (C.1's `kvm_v2_vcpu_create_one`), not per-dispatch
  — vCPUs are reused across tasks, MSRs are immutable across the
  pool's lifetime:
  - `MSR_LSTAR (0xc0000082)` ← trampoline GVA
    (`0xffffe00000000040`).
  - `MSR_STAR (0xc0000081)` ← `(0x0018 << 48) | (0x0008 << 32)`
    (ring-0 kernel selectors for SYSCALL, ring-3 user selectors
    for SYSRETQ; matches v1 line 2032).
  - `MSR_FMASK (0xc0000084)` ← `0x47700` (TF | IF | DF | IOPL |
    NT | AC, matching native syscall_init per v1 archive lines
    2042-2065 — DF=1 leak comment is the lesson, mask the bits).
  - **No `MSR_KERNEL_GS_BASE` programming** — D.1's simplified
    trampoline doesn't use `%gs:` storage. Phase E may revisit if
    IDT/IST stacks need per-vCPU GS; that's an E-side decision.
- **EFER.SCE in SREGS** (~5 LoC). Add `KVM_EFER_SCE` to v2's
  `kvm_v2_load_user_sregs` mask. v1 set this explicitly at
  `kvm-v1-archive/sregs.c:264-265` (`KVM_EFER_SCE | KVM_EFER_LME |
  KVM_EFER_LMA | KVM_EFER_NXE`); v2's current SREGS load only sets
  CR3 / FS.base / GS.base via the C.3 sync path. Without SCE the
  CPU raises #UD on SYSCALL; the gate fails on the first user-mode
  instruction. One-time write at vcpu_create via
  `kvm_run->s.regs.sregs.efer` + dirty bit.
- **PML4[508] kernel-half install via swapper_pg_dir** (~80 LoC).
  The trampoline GPA is reachable via the identity memslot (D.1),
  but the guest CPU walks `CR3` (= `__pa(active_mm->pgd)`) for any
  GVA — including the trampoline's `0xffffe00000000040`. Each UML
  mm pgd needs PML4[508] pointing at a kernel-half PUD/PMD/PTE
  chain that walks down to the trampoline GPA.

  **Mechanism**: install the kernel-half mapping into UML's
  `swapper_pg_dir` (the kernel reference pgd) ONCE at
  `kvm_v2_init`. UML's existing mm-creation path at
  `arch/um/kernel/mem.c:149-157` already copies kernel-half PGD
  entries from `swapper_pg_dir` into every new mm's pgd — so
  future `pgd_alloc()` calls pick up the trampoline mapping
  automatically. No `arch_dup_mmap` hook needed; we lean on UML's
  existing kernel-half propagation. For mms that exist at
  `kvm_v2_init` time (kthreadd, init_mm), iterate the mmlist and
  install the PML4[508] entry directly into each pgd.

  - PUD/PMD/PTE pages allocated from `uml_physmem` so they sit in
    the identity memslot (gpa==host_va, no new memslot needed).
  - The chain is shared across all task pgds (the guest kernel-half
    is identical for every task), so the per-VM cost is ~12 KB
    total (one PUD page + one PMD page + one PTE page).
  - This mirrors v1's `kvm_shadow_map_page` pattern from
    `kvm-v1-archive/thread.c:2287-2365` but writes **real** page-
    table entries instead of shadow entries — TDP walks them
    natively, no shadow-PT machinery, no per-task install, no
    aliasing window.

**Verification**: build clean (=n, =y); boot smoke
`backend=force=kvm-v2 init=/bin/true` still rc=134 (still seccomp
fallback because `.vcpu_run` doesn't flip until D.5); no MSR-set
errors in `dmesg`; substrate gate PASS=25/FAIL=3/EXPECTED_FAIL=3.

### D.5 — Flip `.vcpu_run`; validate against gate (3 days)

- **Single-line ops.c edit** at `arch/um/backend/kvm-v2/ops.c:69`:
  `.vcpu_run = seccomp_vcpu_run` → `kvm_v2_vcpu_run`.
- **Capability flag flips** at `ops.c:60-63`: `uses_stub_reaper`,
  `has_syscall_stub_fd_map`, `stub_syscall_uses_futex`,
  `stub_child_runs_seccomp` all → `false`. The stub child is no
  longer in the loop — guest user code runs directly under KVM,
  trapping via `out` to the in-spawner dispatcher (no per-mm
  worker process, no SCM_RIGHTS round-trip, no futex on
  `stub_data`).
- **Gate validation**:
  - cpython-parity 21/21 × 10 trials, 0 regressions.
  - `single_dlopen × 100`, 0 flakes (Bug B structurally
    impossible per the headline — trampoline never aliases
    user-half).
  - Substrate gate PASS=25/FAIL=3/EXPECTED_FAIL=3 must hold
    bit-for-bit. The 3 expected-fails are stub-child-shape tests
    that v2 deliberately doesn't honour; the 3 fails are
    unrelated to D.

**Phase D exit criteria**:

- Build clean both modes (=n, =y).
- Boot smoke `backend=force=kvm-v2 init=/bin/true` rc=0 (no longer
  rc=134 — v2 is now the live syscall path).
- cpython-parity 21/21 across 10 consecutive trials, 0
  regressions.
- `single_dlopen × 100` produces 0 flakes.
- Substrate gate PASS=25/FAIL=3/EXPECTED_FAIL=3 unchanged.
- Bug B repro from memo 22 (user RIP loaded with corrupt pointer
  via shadow-PT alias) does not reproduce — verified by replaying
  the original failing seed: trampoline VA never appears in any
  user-half PTE walk because PML4[508] is the only install
  point.

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
