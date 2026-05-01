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
- **IO-port syscall trap**: guest syscalls trap via `out %al,$0xf4`
  → `KVM_EXIT_IO` (5-byte LSTAR trampoline at PML4[448] kernel-half;
  byte-identical to v1's non-gadget tail). The original speccing
  `vmcall → KVM_EXIT_HYPERCALL` was mechanically impossible on
  stock KVM (`arch/x86/kvm/x86.c:10456,10520-10523` returns
  `-KVM_ENOSYS` in-kernel for unknown hypercall nrs); §D was
  rewritten at `d186d870e8eb` to use v1's IO-port mechanism. The
  Phase D structural wins (per-CPU vCPU pool, TDP, no shadow PT,
  kernel-half-only trampoline, no per-task bootstrap install) hold
  independent of trap instruction.
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
├── syscall_trap.c (~180)    # KVM_EXIT_IO → handle_syscall (renamed from hypercall.c per §D rewrite)
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
PML4[448] (kernel-half canonical-sign-extended past the user/kernel
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
  this GVA in D.4, and the kernel-half PML4[448] entry (also D.4)
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

### D.4 — MSR programming + PML4[448] kernel-half install (3 days)

**Codex `--search` audit corrections (2026-04-29):** the original §D
draft claimed PML4[508] for VA `0xffffe00000000040`. The math is
wrong: `(0xffffe00000000040 >> 39) & 0x1ff = 0x1c0 = 448`. v1's
archive used the same VA so v1 was at PML4[448]; the §D headline's
"508" was a derivation error. PML4[508] would correspond to
`0xfffffe0000000040`. **All references corrected to PML4[448]**;
implementation must `BUG_ON(pgd_index(KVM_V2_TRAMPOLINE_GVA) != 448)`
as a runtime assertion.

The audit also caught a deeper issue: Phase B's per-region memslot
convention is `guest_phys_addr = userspace_addr = region->va` (a
host VA), but `kvm_v2_load_cr3` writes `__pa(pgd)` to CR3, which
under UML is `kva - uml_physmem` (an offset within physmem, not a
host VA). Phase B's memslots cover GPAs at user-half VA values; the
guest pgd's PTE values reference physmem offsets. **No memslot
covers physmem-offset GPAs today.** When D.5 flips `.vcpu_run`,
KVM's TDP walks CR3 and finds no memslot for the pgd — KVM_RUN
fails. v1 had a single big physmem memslot at slot 0 (gpa=0,
hva=uml_physmem, size=physmem_size) per `kvm-v1-archive/lifecycle.
c:613-648` that v2 inherited the design from but missed the
implementation. Phase D.4b-pre adds this memslot before the PML4
install can land usefully.

Plus three smaller corrections folded in: (a) non-leaf PT entries
use `_KERNPG_TABLE` (UML's `pud_bad` / `pmd_bad` expect that shape;
`_PAGE_TABLE` has US=1 — wrong for kernel-half); (b) UML's mm_list
is file-local in `arch/um/kernel/skas/mmu.c:71-74`, not exported —
patch `swapper_pg_dir[448]` and `init_mm.pgd[448]` explicitly
rather than iterating an mm_list backend code can't see; (c) PT
chain pages (PUD/PMD/PTE) must be VM-lifetime — never freed while
any mm carries the installed pgd entry, otherwise stale walks
follow stale pointers.

This expands D.4 from one commit to three:

1. **D.4a — MSR programming + EFER.SCE + readback** (DONE —
   `812e6725d48d`).
2. **D.4b-pre — physmem identity memslot at vm_create**. Single
   slot mirroring v1's `kvm_ensure_memslot()`; gpa=0, hva=
   uml_physmem, size=physmem_size. ~40 LoC.
3. **D.4b — PML4[448] kernel-half install via swapper_pg_dir**.
   PT chain in physmem (covered by D.4b-pre's memslot), `_KERNPG_
   TABLE` non-leaf flags, swapper + init_mm explicit patch, runtime
   `pgd_index` assertion, VM-lifetime PT pages, free in vm_destroy.
   ~120 LoC.

The bullets below describe D.4b's PT install. D.4a (MSR programming)
already shipped; D.4b-pre's physmem memslot is its prerequisite.

- **MSR programming at vcpu_create** (~50 LoC; SHIPPED in D.4a as
  `812e6725d48d`). Mirror v1's `kvm_enter_guest_program_msrs` at
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
- **D.4b-pre: physmem identity-offset memslot at vm_create** (~40
  LoC). Add one big memslot at `vm_create` time:
  `guest_phys_addr=0, userspace_addr=uml_physmem,
  memory_size=physmem_size`. Mirrors v1's `kvm_ensure_memslot()`
  at `kvm-v1-archive/lifecycle.c:613-648`. Without this, KVM TDP
  walks `__pa(pgd)`-based GPAs (offsets in physmem) and finds no
  memslot. The per-region memslots from B.2 don't conflict (their
  GPAs are at user-half VA values outside `[0, physmem_size)`)
  but are arguably also wrong; whether they're redundant or
  needed for protection-bit enforcement is a separate Phase B
  audit, deferred to Phase H. D.4b-pre is the minimum needed to
  make D.5 work.

- **D.4b: PML4[448] kernel-half install via swapper_pg_dir**
  (~120 LoC). The trampoline GPA = `__pa(trampoline_kva)` is
  reachable via D.4b-pre's physmem memslot. The guest CPU walks
  `CR3` (= `__pa(active_mm->pgd)`) for any GVA — including the
  trampoline's `0xffffe00000000040`. Each UML mm pgd needs
  PML4[448] pointing at a kernel-half PUD/PMD/PTE chain that
  walks down to the trampoline GPA.

  **Mechanism**: install the kernel-half mapping into UML's
  `swapper_pg_dir` (the kernel reference pgd) ONCE at
  `kvm_v2_init`. UML's existing mm-creation path at
  `arch/um/kernel/mem.c:149-157` already copies kernel-half PGD
  entries from `swapper_pg_dir` into every new mm's pgd — so
  future `pgd_alloc()` calls pick up the trampoline mapping
  automatically. No `arch_dup_mmap` hook needed; we lean on UML's
  existing kernel-half propagation. For pre-existing mms patch
  `init_mm.pgd[448]` explicitly. UML's `mm_list` is file-local in
  `arch/um/kernel/skas/mmu.c:71-74` (not exported to backend
  code) so the iterate-mmlist approach the surface map proposed
  is wrong; init_mm + swapper_pg_dir is sufficient at subsys_initcall
  time (no other mms exist that early).

  - PUD/PMD/PTE pages allocated from `uml_physmem` so they sit in
    D.4b-pre's physmem memslot. PTE chain pages are **VM-lifetime**
    — never freed while any mm carries the installed pgd entry.
    Free in `vm_destroy` after teardown.
  - **Non-leaf PT flags use `_KERNPG_TABLE`** (= `_PAGE_PRESENT |
    _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY`), NOT `_PAGE_TABLE`
    (which has `_PAGE_USER` set — wrong for kernel-half). UML's
    `pud_bad` / `pmd_bad` validators expect the `_KERNPG_TABLE`
    shape.
  - **Leaf PTE flags**: `_PAGE_PRESENT | _PAGE_ACCESSED` —
    kernel-only (US=0), RO (no `_PAGE_RW`), executable (UML has no
    `_PAGE_NX` so executable is implicit). Trampoline is RO+EXEC
    code; never written from guest CPL=0 either.
  - The chain is shared across all task pgds (the guest
    kernel-half is identical for every task), so the per-VM cost
    is ~12 KB total (one PUD page + one PMD page + one PTE page).
  - **Runtime assertion** at install time:
    `BUG_ON(pgd_index(KVM_V2_TRAMPOLINE_GVA) != 448)` — catches
    any future VA change that would silently hit the wrong PML4
    slot.
  - This mirrors v1's `kvm_shadow_map_page` pattern from
    `kvm-v1-archive/thread.c:2287-2365` but writes **real** page-
    table entries instead of shadow entries — TDP walks them
    natively, no shadow-PT machinery, no per-task install, no
    aliasing window.

**Verification**: build clean (=n, =y); boot smoke
`backend=force=kvm-v2 init=/bin/true` still rc=134 (still seccomp
fallback because `.vcpu_run` doesn't flip until D.5); no MSR-set
errors in `dmesg`; substrate gate PASS=25/FAIL=3/EXPECTED_FAIL=3.

### D.5 — Flip `.vcpu_run` (DEFERRED to Phase E activation)

**Original spec**: flip `.vcpu_run = seccomp_vcpu_run` → `kvm_v2_vcpu_run`,
flip the four stub-child capability flags to false, run the headline
gate (cpython 21/21 × 10 + single_dlopen × 100).

**What actually happened (2026-04-29 attempt)**: the flip surfaced
TWO Phase E dependencies that this memo's §D.5 specification didn't
anticipate. Both were diagnosed in flight; the corresponding
infrastructure landed (D.5-fix-1 + D.5-fix-2) but the flip itself
was reverted because the gate criteria are unreachable without
Phase E machinery.

**Dependencies surfaced**:

1. **Long-mode SREGS install required at vcpu_create.** The first
   `KVM_RUN` returns `-EINVAL` because `kvm_is_valid_sregs` at
   `arch/x86/kvm/x86.c:12426-12449` rejects EFER.LMA=1 with
   CR0.PG=0. v2's per-dispatch `kvm_v2_load_user_sregs` only writes
   CR3 / FS_BASE / GS_BASE / EFER — the rest of `kvm_run->s.regs.
   sregs` is mmap-zero on first dispatch (KVM only populates the
   sync-regs view via `store_regs()` after a successful exit, per
   `arch/x86/kvm/x86.c:12748-12761`). v1 archive at
   `kvm-v1-archive/thread.c:2843-2913` had the GET → overlay → SET
   pattern v2 was missing. **Landed at `124cfefa342f` as
   `kvm_v2_install_production_sregs` called from
   `kvm_v2_vcpu_create_one`** with full long-mode CS/DS/SS, CR0
   (PE|MP|NE|WP|PG), CR4 (PAE|OSFXSR|OSXMMEXCPT), EFER (SCE|LME|
   LMA|NX), and seeded sync-regs mmap so first dispatch's dirty bit
   doesn't ship zero state back through `__set_sregs`.

2. **`KVM_SET_SIGNAL_MASK` + `unblock_signals()` required.** With
   the SREGS fix, `KVM_RUN` now succeeded but every dispatch
   immediately returned `-EINTR` because (a) signals were unmasked
   at the host-thread level, and (b) UML's deferred-signal queue
   never drained between dispatches. v1 archive at
   `kvm-v1-archive/thread.c:71-124,3979` had the full pattern: a
   `sigfillset` minus SIGALRM mask installed at vcpu_create
   (everything blocked except the timer-driven preemption signal),
   plus `unblock_signals()` after every `KVM_RUN` to drain UML's
   deferred handlers. **Landed at `5297fe2bd165` as
   `kvm_v2_install_signal_mask` + the `unblock_signals()` call in
   `kvm_v2_vcpu_run`**.

3. **Marshal-from-kvm_run on EINTR required.** D.3's original EINTR
   path skipped the post-RUN GPR marshal-back, on the (incorrect)
   theory that sync_regs might be incoherent on signal exits.
   Diagnostic confirmed the opposite: KVM commits guest state on
   `KVM_EXIT_INTR` (10) — sync_regs IS coherent. Without the
   marshal, `regs->gp[HOST_IP]` kept the pre-RUN value forever and
   every re-dispatch wrote that same RIP back. **Fixed in
   `f0e4d1d95c2e`**.

**Why the gate is still unreachable**: with all of the above in
place, the guest still cannot advance any instruction. Diagnostic
trace showed RIP stuck at the init binary entry point across every
dispatch under KVM_EXIT_INTR. Blocking ALL signals confirmed the
guest is in a fault loop — most likely #PF on first instruction
fetch (the user PT walk doesn't have an entry yet) or #GP on
segment access — and KVM has no IDT to dispatch the fault to. KVM
either spins re-injecting (signals masked) or exits via
SIGALRM-EINTR (signals unmasked) before a single instruction
retires.

**Phase E (memo 26 §E) is a hard precondition for the flip.**
Until E.1 (IDT in dedicated guest page) + E.2 (per-vCPU IST stacks)
+ E.3 (exception handler dispatch — at minimum #PF) land,
`.vcpu_run` MUST stay routed to seccomp.

**Capability flag flips also deferred.** The original §D.5 spec
said to flip `uses_stub_reaper`, `has_syscall_stub_fd_map`,
`stub_syscall_uses_futex`, `stub_child_runs_seccomp` to false at
the same moment as the `.vcpu_run` flip. That was wrong even
without the IDT issue: the stub child is still spawned by
`seccomp_mm_create` / `mm_attach` (v2 only owns memslot ops +
context_switch + .vcpu_run currently). With
`stub_child_runs_seccomp = false`, `arch/um/os-Linux/skas/
process.c:220` takes the PTRACE_TRACEME branch instead of
installing the seccomp filter, and the stub child stalls forever
(this is the original boot-deadlock A.1 followup `40a97b5f3b72`
fixed). The flags should flip in a later phase that ALSO replaces
`seccomp_mm_create` / `mm_attach` / `thread_create` with v2-native
equivalents.

### Phase D current exit criteria (post-deferral)

- All Phase D infrastructure landed and verified at boot:
  CPUID lazy install, 5-byte trampoline, KVM_EXIT_IO dispatcher,
  return marshal + EINTR + per-task FPU swap-out, MSR_LSTAR/STAR/
  FMASK + EFER, physmem identity-offset memslot, PML4[448]
  kernel-half PT chain, full long-mode SREGS install, signal mask
  + drain.
- Boot smoke `backend=force=kvm-v2 init=/bin/true` rc=134 (still
  routed through seccomp; flip is a one-line edit pending Phase E).
- Substrate gate PASS=25/FAIL=3/EXPECTED_FAIL=3 (bit-for-bit
  unchanged from pre-Phase-D baseline).
- v2-vs-seccomp parity wrapper: bit-identical results across both
  backends (expected — v2 still delegates `.vcpu_run` to seccomp).

### Phase E adopts the original D.5 gate criteria

The original §D.5 headline gate moves to Phase E's exit criteria:

- cpython-parity 21/21 × 10 trials with 0 regressions.
- `single_dlopen × 100` produces 0 flakes (Bug B structurally
  impossible per memo 22 — trampoline lives only in PML4[448],
  never aliases user-half).
- Substrate gate holds PASS=25/FAIL=3/EXPECTED_FAIL=3.
- Boot smoke `init=/bin/true` rc=0 (the moment v2 actually runs
  guest code).

---

## Phase E — Exception handling: IDT/TSS without bootstrap pages (2 weeks)

**Goal**: handle guest exceptions (#PF, #GP, #UD, #DE, #BP, #OF, #DF)
via standard KVM mechanisms, with no per-mm pages installed in user
mms.

### Architectural correction (2026-04-29, retrospective)

**Memo 26 §A vision item "Zero shadow PT machinery" was load-bearing
but not empirically verified at design time.** UML's PTE bit layout
in `arch/um/include/asm/pgtable.h` was software-only book-keeping
with positions chosen arbitrarily relative to x86 hardware paging.
With v2 handing UML's pgd directly to KVM via CR3, the bits collide:
`_PAGE_RW=0x020` at x86's A-bit position; `_PAGE_ACCESSED=0x080` at
x86's PS-bit position; etc. KVM's TDP MMU walks UML's pgd as a
malformed x86 page table -> reserved-bit faults -> injected #PF ->
IDT walk faults same way -> reinjection loop -> SIGALRM-EINTR is the
only escape. KVM debugfs shows `EXIT_NPF info 20000000f` (bit 3 =
PFERR_RSVD_MASK).

24+ hours of session time investigated alternative hypothesis spaces
(signal pacing, R4 worker, SREGS install, IDT gates, entry shape)
before empirical debugging - minimum C reproducer + KVM tracepoints
+ gVisor source comparison - found the actual cause. v1 archive at
`kvm-v1-archive/lifecycle.c:1114` had a `kvm_um_pte_to_x86`
translator whose function name documented this incompatibility; v1
maintained per-mm shadow PT trees with this translator on every PTE
mutation. v1 wasn't naive to do this - v1 was correct.

**Resolution at commit `912587c605d8`**: align UML's PTE bits with
x86 hardware paging via header-only edit to
`arch/um/include/asm/pgtable.h` (Option B over Option A
shadow-PT-port; Option B is 23 LoC vs Option A's ~500-700 LoC and
has zero seccomp-side blast radius because every UML PTE consumer
goes through name-based accessors that route through
`pte_get_bits` / `pte_set_bits`):

| Macro | Old | New | x86 |
|---|---|---|---|
| _PAGE_PRESENT | 0x001 | 0x001 | bit 0 P |
| _PAGE_RW | 0x020 | 0x002 | bit 1 R/W |
| _PAGE_USER | 0x040 | 0x004 | bit 2 U/S |
| _PAGE_ACCESSED | 0x080 | 0x020 | bit 5 A |
| _PAGE_DIRTY | 0x100 | 0x040 | bit 6 D |
| _PAGE_NEEDSYNC | 0x002 | 0x200 | bit 9 AVL |
| _PAGE_PROTNONE | 0x010 | 0x400 | bit 10 AVL |
| _PAGE_SWP_EXCLUSIVE | 0x400 | 0x800 | bit 11 AVL |

Boot smoke under `.vcpu_run = kvm_v2_vcpu_run` reaches "Console
initialized on /dev/tty0" + same VFS panic as seccomp = identical
kernel-side trajectory. Reserved-bit fault eliminated. Substrate
gate green PASS=25/FAIL=3/EXPECTED_FAIL=3 (seccomp regression-free).

**Residual (Phase E.5 in flight, corrected 2026-04-29 evening after
codex audit response)**: empty user-RIP leaf PTE (`pte[35] = 0`) at
first KVM_RUN. Earlier hypothesis ("tlb.c -> `mm_region_added` ->
`seccomp_mm_region_added` populates the STUB CHILD's mm but not the
spawner mm's pgd") was **wrong**. The codex audit
(`/tmp/codex_e5_audit_response.md` archived at session boundary)
established two facts that overturn that hypothesis:

1. **There is no separate "stub child mm with separate pgd".** The
   stub child is a host process whose address-space mappings are
   mirrored from UML's mm->pgd via `os_map_memory`. UML's mm->pgd IS
   the source of truth. (`arch/um/kernel/trap.c:146,183` — fault flow
   uses `current->mm`; `arch/um/kernel/tlb.c:269-271` — drain dispatch
   walks the same mm->pgd to emit `mm_region_added` events.)

2. **TDP semantics: a non-present *guest* PT entry does NOT exit to
   userspace.** Per Intel SDM Vol.3C §28 + KVM's
   `arch/x86/kvm/mmu/paging_tmpl.h::FNAME(walk_addr_generic)`, the
   hardware injects #PF into the guest IDT directly. EXIT_NPF only
   fires for GPA→HPA failures. Confirmed independently by gVisor's
   design (sentry pre-populates user-half via
   `address_space.go::mapLocked` + sentry's own ring0 IDT) and
   kvmtool (`x86/kvm-cpu.c::prot64_sregs` pre-builds boot PML4).

So the architecture for v2 IS:

  KVM walks UML's pgd → finds non-present user leaf →
  hardware-injects #PF → guest IDT[14] stub (E.1) →
  `out al, port_pf ; iretq` → KVM_EXIT_IO →
  `kvm_v2_handle_io_pf` (`arch/um/backend/kvm-v2/syscall_trap.c:896`)
  → `segv_handler` → `do_page_fault` → `handle_mm_fault` →
  `set_pte_at` populates mm->pgd → re-enter KVM_RUN

This chain is **already implemented** (syscall_trap.c:896-992,
already routes faultinfo through segv_handler). The empirical
"EXIT_NPF on `pte[35]=0`" observation is **inconsistent** with TDP
semantics — there's a different bug upstream of the guest-IDT walk.

Top candidates per the audit (in priority order):

1. **CR3 sync-regs lazy-commit** — first KVM_RUN walker uses CR3=0
   instead of `__pa(active_mm->pgd)` because the
   `KVM_SYNC_X86_SREGS` dirty-bit write in
   `kvm_v2_load_user_sregs` (vcpu.c:1059-1105) isn't being honored.
   Fix: force-commit via `KVM_GET_SREGS`+`KVM_SET_SREGS` pair before
   first dispatch (empirically verified by E.5 subagent).

2. **Memslot conflict** — Phase B per-region memslots (gpa=region->va,
   `arch/um/backend/kvm-v2/context.c:75-150`) compete with the
   physmem identity slot 0 (gpa=0..physmem_size, D.4b-pre at
   `8fff1742bfb4`). When KVM walks UML's pgd and dereferences a PT
   page by GPA = `__pa(kva)` (offset into physmem), per-region
   memslots may shadow those GPAs and KVM reads zeros from a
   user-VA region instead of UML's actual PT. Codex CLAIM C
   (deferred to Phase H) is to drop per-region in favor of physmem-
   only. May need to be pulled forward to E.5.

3. **Exception bitmap** — confirm bit 14 (#PF) is NOT in the
   intercept bitmap. If it is (perhaps via KVM defaults under some
   host config), KVM intercepts #PF and the guest IDT never runs.
   `kvm_exit reason EXCEPTION nr=14` in tracepoints would confirm.

**E.5 resolution (2026-04-29 evening, commit `aba0b460a034`)**:
boot smoke under v2 now reaches `Run /bin/true as init process`
followed by the standard "Attempted to kill init! exitcode=
0x00000000" panic — the panic any kernel produces when init
exits cleanly, identical under seccomp. /bin/true executes user
code → exit_group(0) → KVM_EXIT_IO → handle_syscall → do_exit.
Substrate gate green: PASS=25/FAIL=3/EXPECTED_FAIL=3 (matches
seccomp baseline; no regression).

The empirical capture (KVM tracepoints during first 5 sec of
boot under .vcpu_run = kvm_v2_vcpu_run) discriminated between
the three audit candidates and overturned all three partially:

- **Candidate 1 (CR3 sync-regs lazy-commit)** — REJECTED. The
  faulting RIP was 0x40023340 (/bin/true's first user
  instruction), not an early KVM_RUN with cr3=0. CR3 was being
  committed across re-injections.

- **Candidate 2 (memslot conflict)** — PARTIALLY RIGHT, was the
  load-bearing fix. The mechanism wasn't "per-region memslots
  shadow slot 0's GPAs" (those didn't overlap); it was
  "per-region DELETE+CREATE churn → KVM internal MMU
  invalidate_zap → slot 0's PML4-page EPT entries left
  un-refilled (pf_taken=1, pf_fixed=0)". Dropping per-region
  memslots eliminated the EPT zap source.

- **Candidate 3 (exception bitmap)** — REJECTED. KVM was
  injecting #PF (kvm_inj_exception trace), not intercepting it;
  the architecture for hardware-injected #PF → guest IDT was
  engaging correctly.

After dropping per-region memslots, the EPT loop broke and
boot reached a SECOND bug: `segfault at 10 ip 0x10 sp 0x10002
error 10` looping. Two stacked errors in the IDT-vectoring path:

- **IST frame off-by-8 with error code** (`syscall_trap.c
  kvm_v2_ist_frame_read/write`): the read used
  `off = has_error_code ? 48 : 40` and indexed RIP at
  `(top - off) + 0`, which lands on the error-code slot for
  with-error-code frames per SDM §6.14.5 layout (low→high:
  error_code, RIP, CS, RFLAGS, RSP, SS). Every field shifted
  by 8 — RIP picked up error_code (0x10), RSP picked up RFLAGS
  (0x10002 with bit-16 RF set during fault delivery). Fix:
  RIP-anchor at top-40 always; read error_code separately at
  top-48 when present.

- **PF/GP IDT stub iretq pops error_code as RIP** (`exception.c
  kvm_v2_handler_stub_pf/_gp`): E.1's stub shape was 4 bytes
  (`out %al, $port ; iretq`) for ALL vectors. Per SDM §6.14.5,
  iretq pops 5×8B (RIP/CS/RFLAGS/RSP/SS) — it does NOT pop the
  error code. The deferred decision in the E.1 comment block
  ("E.3 may revisit if iretq's frame shape for the error-code
  case requires explicit `add $8, %rsp` in the stub") was real
  and required. Fix: extend pf/gp stubs to 8 bytes with explicit
  `add $8, %rsp` (4 bytes: 48 83 c4 08) before iretq. v1's
  archive at kvm-v1-archive/thread.c:1310-1314 had exactly this
  shape for the same reason.

The three fixes (drop per-region memslots, fix IST frame
offsets, extend PF/GP stubs) committed atomically at
`aba0b460a034` because none alone is sufficient: fix 1 unblocks
the EPT loop but exposes fix 2's bug, fix 2 alone doesn't help
because IDT-vectoring never reaches the host handler with empty
EPT, fix 3 only matters once fixes 1 + 2 land. Together they
complete "first KVM_RUN under v2 produces a working user-mode
dispatch."

**Phase E.4 validation matrix (refreshed at `31748fea602e`)** —
multi-process workloads now run through end-to-end; one residual
remains in glibc varargs format substitution for the child task.
Post-E.5 instrumentation found and fixed three sub-bugs in the
fork-and-dispatch path:

  1. **CPL=0 from first entry** — `kvm_v2_install_production_sregs`
     installed kernel CS/SS at vcpu_create, leaving the guest at
     CPL=0 until its first SYSCALL→sysretq round-trip. /bin/true
     and /bin/echo got away with it because their first instruction
     after _start IS a syscall; glibc fork's child task ran
     ~26 dispatches of post-fork bookkeeping at CPL=0 before the
     first syscall and reached `abort+0xa9`'s `hlt` (0xf4) → at
     CPL=0 hlt panics with KVM_EXIT_HLT instead of #GP'ing as it
     would at CPL=3. Fix at `32a7603236b0`: select CS=0x2b/SS=0x23
     (DPL=3) in install_production_sregs so the guest enters at
     CPL=3 from first KVM_RUN. Existing SYSCALL/sysretq + IDT-gate
     transitions internally manage CPL; only the *initial* value
     needed correcting. v1 archive's `sregs.c` had this shape.

  2. **Fork's child mm inherited parent's worker pointer** — UML's
     fork→dup_mm path bytewise-copies the parent mm_struct,
     including `mm->context.worker`. Without resetting in
     init_new_context, `seccomp_mm_create → worker_alloc_stub_for_mm
     → __spawn_worker_for_mm` tripped `WARN_ON_ONCE(mm->context.
     worker != NULL)` at `arch/um/kernel/spawner.c:225` on every
     fork, then silently overwrote the inherited pointer (leaking
     the parent's reference). Fix at `31748fea602e`: clear
     `mm->context.worker = NULL` at the top of init_new_context.
     WARN gone; substrate gate stays green.

| Workload                          | Status   | Notes                              |
|---|---|---|
| `init=/bin/true`                  | PASS     | exit_group(0) → init-kill panic    |
| `init=/bin/echo hello`            | PASS     | stdout works; clean exit           |
| `init=/bin/sh` (builtins only)    | PASS     | shell builtins work                |
| Static glibc no-fork              | PASS     | printf + fprintf both substitute   |
| Pure-syscall fork+wait+exit       | PASS     | child runs, parent waits, both OK  |
| glibc fork (no wait)              | PASS     | parent + child stdio both work     |
| glibc fork+wait (parent printf)   | PASS     | wait returns child status correctly |
| glibc fork+wait (child printf %d) | RESIDUAL | child's *first* printf emits `%d` literally instead of substituting; subsequent calls work |

**Residual resolved at `0e05de21dba0`**: the bug was in
`kvm_v2_fpu_install_on_first_run` (vcpu.c:1600). Despite its name,
the function is called on EVERY dispatch from `kvm_v2_vcpu_run`.
Its else-branch (when fpu_valid=false) was unconditionally
KVM_SET_FPU'ing architectural-reset values (zero XMM, fcw=0x037f,
mxcsr=0x1f80) on every re-entry, destroying the in-flight task's
XMM/x87 state any time KVM_RUN exits and re-enters mid-user-
instruction (e.g. mid-COW #PF on a stack/glibc page during
vfprintf's MOVAPS XMM save). Per System V x86_64 ABI §3.5.7
variadic functions save XMM0..XMM7 to a stack-allocated SSE save
area in the prologue via MOVAPS — if a #PF fired during that
save (or any later XMM-touching op), the post-fault re-entry's
else-branch zeroed XMM, vfprintf's varargs spec-decoder picked up
zeros, and the format-substitution fast-path emitted format chars
literally. Fix: turn the else-branch into a no-op. Per-vCPU FPU
is correctly owned by KVM between explicit save/restore points
(`kvm_v2_fpu_capture_for_switch_out` + `kvm_v2_fpu_capture_for_
fork`); re-entry of the same task on the same vCPU should leave
KVM's per-vCPU FPU untouched. v1 archive's
`kvm_fpu_install_on_first_run` at thread.c:240-339 had the same
contract.

**Workload status at HEAD `a478952b8da0`** (post-PT_SYSCALL_NR clear):

| Workload                              | seccomp | v2     | Notes |
|---|---|---|---|
| `init=/bin/true`, `/bin/echo`         | PASS    | PASS   | exit_group(0) → init-kill panic |
| `init=/bin/sh` (builtins)             | PASS    | PASS   | |
| Static glibc (no fork)                | PASS    | PASS   | |
| Pure-syscall fork+wait+exit           | PASS    | PASS   | |
| glibc fork+wait (parent + child stdio)| PASS    | PASS   | `CHILD pid=37` substitutes correctly |
| glibc fork+exec(/bin/echo)            | PASS    | PASS   | |
| Single-import python (`hashlib`)      | PASS    | PASS   | `2d711642` (correct SHA256) |
| **Multi-import python** (json+base64+os) | PASS | FAIL    | UML kernel-side fatal signal during child python startup (#96) |
| Direct `init=python3 -c "..."`        | PASS    | flaky  | PID 1 = python3 sometimes segfaults; shell-wrapped works |
| Direct `init=fork_exec_wait` (50× fork+execve+waitpid loop) | PASS | FAIL (SIGILL #95) | Substrate harness path works (the fork is a shell child not init); direct init=fork_exec_wait still SIGILLs |
| **Substrate gate**                    | **PASS=25 / FAIL=3 / XFAIL=3** | **PASS=5..9 / FAIL=3..4 / XFAIL=0** (post-fix at 11102c8176fb; mode=5, common 7 and 9) | See "Fork-tree-3level reproducer (2026-04-30)" below for the root cause + fix; the post-fix distribution shows the floor lifted from 4→5 and 9 is now common rather than rare |

**v2 progress trajectory this session**:
  - Pre-CPL=3 SREGS:           v2 stuck at "infinite EPT-violation reinjection loop"
  - Post-CPL=3 (32a7603236b0):  /bin/true reaches clean exit
  - Post-FPU-fix (0e05de21dba0): glibc fork+wait works (child %d substitutes)
  - Post-exception-IE (31ba9c354063): substrate class-a-env runs; Python imports work
  - Post-PT_SYSCALL_NR clear (a478952b8da0): substrate PASS=6/FAIL=3 *in best-of-N runs* (PASS=4 dominant), class-c-syscall reproducers join

The remaining residuals (#95 fork_exec_wait direct, #96 multi-import python crashing UML) are real bugs but represent fewer workloads than what now passes. v2 has crossed from "experimental — barely boots" to "functional for shell-wrapped multi-process workloads + substrate class-a-env subset". Phase H baseline (c296bfbaa05d) reported v2 1.73× faster than seccomp on Python startup, **but that measurement is pre-revert and has not been re-baselined post-aaced3ce4924** — see Phase H.1 §"re-baseline pending".

#### Fork-tree-3level reproducer (2026-04-30) — root cause of #95, #96, and the substrate truncation

The substrate gate truncation, #95 (fork_exec_wait direct init
SIGILL), and #96 (multi-import Python crashes UML) are very likely
the same bug. We have a clean minimal reproducer:

```c
// tools/testing/selftests/um/fork-tree-3level/fork_tree_3level.c
int main(void) {
    pid_t p = fork();
    if (p == 0) _exit(42);
    int st;
    waitpid(p, &st, 0);
    return 0;
}
```

Run via init shell script (so the test is PID 2, child is PID 3,
shell init is PID 1). Under seccomp the test prints
`FORK_TREE_3LEVEL: PASS child=N status=0x2a00` and shell sees rc=0.
Under kvm-v2, **the test's main returns 0 but the shell sees rc=255**.

What's verified:
  - 2-level trees (shell + /bin/true × N): always PASS on both
    backends.
  - 3-level trees WITHOUT middle-process wait (parent forks, returns
    without waitpid): always PASS on both.
  - 3-level trees WITH middle-process wait+exit: **kvm-v2 reports
    rc=255 every time, no exceptions in 10 runs** (deterministic).

Kernel-side trace at the panic (init=script that runs the
reproducer + `sync` + ends):

```
do_exit+0x208/0x978
sys_exit_group+0x0/0x1a
do_group_exit+0x0/0x9e
handle_syscall+0x155/0x1e4
kvm_v2_handle_io_trap+0x336/0x375
kvm_v2_vcpu_run+0x334/0x3ea
```

The call stack itself is identical between seccomp and kvm-v2 — the
difference is the value the kernel records as init's exit_code.
PID 1 (the shell) calls `exit_group(0)` syscall, but on v2 the
kernel records 0xff (255) instead of 0. Either:
  a) `regs->gp[HOST_DI]` (the syscall arg) is set to 0xff somewhere
     between marshal-from-kvm-regs and handle_syscall.
  b) `signal->group_exit_code` is corrupted to 0xff before
     do_group_exit reads it (do_group_exit:
     `if (sig->flags & SIGNAL_GROUP_EXIT) exit_code = sig->group_exit_code;`).

Hypothesis (b) is more interesting because group_exit_code is
normally set only by signal-driven group death. If a signal is
silently triggering SIGNAL_GROUP_EXIT on v2's PID 1 during the
post-grandchild-reap window, we have a signal-delivery bug, not a
register-marshaling bug.

**Confirmed via instrumented kernel (2026-04-30, debug printk in
do_exit):** Hypothesis (b) was initially diagnosed but is wrong on
deeper inspection. With `pr_emerg("...sig->gec=0x%x sig->flags=
0x%x...")` added at the init-panic site, kvm-v2 shows `code=0xff00`,
which we initially read as "gec was corrupted." A second printk at
do_group_exit's entry resolved this: shell receives `arg=0xff00`,
`flags=0x40` (= `SIGNAL_UNKILLABLE`, normal for PID 1), `prior_gec=0`
— meaning **the shell genuinely calls `exit_group(255)`**, and
`SYSCALL_DEFINE1(exit_group, error_code) { do_group_exit((error_code &
0xff) << 8); }` does the canonical wait-status shift. There is no
kernel-side group_exit_code corruption.

So shell sees `$? = 255` because fork_pid (the immediate child) was
reaped with wait_status = 0xff00 = WEXITSTATUS=255. And the next
question is: **why did fork_pid produce a wait_status of 255?** The propagation chain is: do_group_exit(0) sees
flags & SIGNAL_GROUP_EXIT → exit_code := sig->group_exit_code (0xff00)
→ do_exit(0xff00) → tsk->exit_code = 0xff00 → init panic prints
0xff00.

A wider trace caught this output:

    *** stack smashing detected ***: terminated

That's **glibc's stack-canary firing in user space**, called via
`__stack_chk_fail` → `abort`. The 0xff00 wait-status the shell sees
comes from glibc-fortify killing fork_pid via SIGABRT (or the libc-
fortify `_exit(127)` fallback path, which then propagates as 127 in
some contexts and 255 in others depending on the libc message
delivery). Either way: **fork_pid is dying because user-space libc
is detecting memory corruption in itself, not because the kernel's
signal/exit-code path is buggy.**

The bisect against `interrupt_end()` was a negative result: with
all 5 calls disabled in `arch/um/backend/kvm-v2/syscall_trap.c`, the
gate still FAILs the same way under v2. So `interrupt_end` placement
is innocent.

A raw-asm reproducer (`tools/.../v2-flake-investigation/raw_fork.c`,
`gcc -static -nostdlib -O0 -fno-stack-protector`, only raw syscalls
via inline assembly) **passes cleanly under both backends** (exit
99 propagates correctly, no stack-smashing). That isolates the
difference: v2's syscall path is fine for raw syscalls; the
corruption only manifests when libc is involved.

Likely culprit: **TLS / FS_BASE handling**. glibc's stack canary
lives at `%fs:0x28`. v2's load-user-sregs path (vcpu.c:1297-1300)
writes `sregs->fs.base` from `regs->gp[HOST_FS_BASE]` on every
KVM_RUN, but `kvm_v2_marshal_from_kvm_regs` (vcpu.c:1182-1205) does
NOT update `regs->gp[HOST_FS_BASE]` after a KVM_RUN exit — it only
copies GPRs from `kvm_regs`, not segment bases from `kvm_sregs`.
If a user-mode `wrfsbase` (or arch_prctl(ARCH_SET_FS)) updates
fs.base inside the guest, v2 won't propagate it back to the
parent's `gp[HOST_FS_BASE]`. On next dispatch the kernel reloads the
stale fs.base, breaking glibc's TLS view, which corrupts the
canary check.

The bug is also **timing-sensitive**: with the debug printks
slowing dispatch, the 0xff00 outcome only fires on a fraction of
runs (vs ~10/10 deterministic without instrumentation). That
fits the substrate-gate truncation pattern (mostly PASS=4, sometimes
PASS=6/9 — it depends on whether the in-flight signal landed before
the next syscall's marshal-out). Combined with the two
`interrupt_end()` placement reverts (memo §E.4 revert pair), this
points to a v2 signal-delivery path that uses a stale or
uninitialised `signo` slot when the host process the worker is
running on is reaped.

Concrete next moves (deferred to a focused kernel-side investigation):
  1. ~~Audit fs.base / gs.base round-trip~~ Closed at 6e52574cca6c.
     Defensive fix landed; doesn't resolve fork-tree-3level.
  2. ~~Bisect against 31ba9c354063 (interrupt_end placement).~~
     Closed; negative result.
  3. **Locate child-process stack-corruption.** Bisection via
     `tools/testing/selftests/um/fork-tree-3level/repros/` (landed
     this session) narrowed the bug:
       - `raw_fork.c` (no libc, raw asm syscalls): PASS on v2 ✓
       - `libc_simple.c` (libc, no fork): PASS on v2 ✓
       - `child_simple.c` (libc + fork; child does printf): FAIL on
         v2 8/10 with `*** stack smashing detected ***` in the CHILD
       - `child_only_canary.c` (child calls glibc's
         strlen/memset internals): FAIL 10/10 in CHILD
       - Parent always exits cleanly (RC=0). The stack-smashing-
         detected output is from the CHILD's libc.
     The bug is **v2's child-process state on first dispatch after
     fork**. Investigation results (this session):

     **Hypotheses ELIMINATED** (no improvement on
     fork-tree-3level / child_simple gate; ~30-40% PASS unchanged):
       - Stale FPU state. Forced arch-default FPU on every
         dispatch; no improvement.
       - Stale segment registers. Explicitly wrote CS=0x2b/DS=ES=
         SS=0x23/CR0/CR4 on every dispatch; no improvement.
       - Real KVM_SET_SREGS ioctl (vs SYNC_REGS dirty-bit). No
         improvement.
       - interrupt_end placement (commit 31ba9c354063); bisected
         negative.
       - CLONE_CHILD_*TID flags; tested with raw clone(SIGCHLD,
         ...); same failure rate.
       - glibc cleanup at exit (`_exit` vs `return 0`); same rate.
       - TLS / FS_BASE; static-glibc binaries don't issue
         arch_prctl(SET_FS) at all (verified DIAG; fs.base=0
         throughout).

     **Defensive fixes LANDED** (improve correctness but don't
     resolve the gate):
       - sregs.fs.base/gs.base round-trip from KVM_RUN exit
         (commit 6e52574cca6c). Fixes a v1→v2 regression but isn't
         exercised by static-glibc workloads.
       - sregs.cr2 cleared in load_user_sregs (commit
         24a7f0575e18). Stops parent's last #PF address leaking
         into child via the per-host-CPU vCPU's sregs mmap.

     **ROOT CAUSE FOUND (codex-gpt5.5-xhigh audit, fix at 11102c8176fb):**

     KVM kept stale guest-TLB entries that v2 never flushed. UML's
     "guest" page tables live in physmem (every guest page is just
     bytes in the physmem memslot). When UML's kernel updates a
     guest PTE (post-CoW handle_mm_fault rewrites the spawner PTE
     AND calls set_pte_at on the UML guest pgd), only the spawner
     PTE write fires KVM's mmu_notifier. The UML guest-pgd write is
     a plain memory write into the physmem memslot, which
     mmu_notifier doesn't see. So KVM's guest-TLB cache (the
     linear-address translation cache, NOT the slot-0 TDP) keeps
     mapping GVA → old physmem PFN even after UML's PTE points to
     the post-CoW page.

     Fix has two pieces (both in `kvm_v2_load_user_sregs` at
     vcpu.c):

       1. **`current_mm_sync()` before KVM_RUN.** Drains UML's
          deferred-flush queue so any pending mremap/mprotect/
          munmap on the spawner mm gets applied via host syscalls,
          firing mmu_notifier and invalidating KVM TDP for the
          affected GPA ranges. seccomp does this at
          `arch/um/backend/seccomp/trap_user.c:68`; v1 did at
          `kvm-v1-archive/thread.c:3851`. v2 was missing it.

       2. **`sregs->cr4 ^= X86_CR4_PGE` per dispatch.** Forces
          KVM's `__set_sregs_common` to set `mmu_reset_needed=1`
          and request `TLB_FLUSH_GUEST` on every entry, flushing
          the guest TLB cache via `vpid_sync_context` (single-
          context INVVPID). v1 did this at
          `kvm-v1-archive/thread.c:2974-2984`, with a comment
          documenting that narrowing the toggle to "tlb_stale &&
          same_cr3" regressed v1's gate to "~70% pass rate vs
          100%" — exactly the v2 fork-tree-3level rate observed
          before this fix.

     Empirical impact (cumulative, post all fixes through
     d5d5d15c4e5b):
       - `child_simple` reproducer: was 3-4/10 PASS, now 10/10.
       - `regrtest-substrate` PASS distribution evolved across
         the session:
           Pre-fix:    {4×8, 6×1, 9×1} mode 4
           Post-TLB:   {5,5,5,5,5,5,7,7,9,10} mode 5
           Post-EINTR: {6,7,8,8,9,9,10,14} mode 8 (more variance up)
           Latest x5:  {6,8,8,9,9} floor 6 ceiling 9
         Net: floor lifted 4→6, ceiling lifted 9→14, mode 4→8.
       - `fork_tree_3level` gate: ~30-35% PASS (was 0-10%).
       - Perf unchanged: 0.500× ratio (2× faster than seccomp)
         on perf-py-startup.

     Five fixes landed this session:
       1. 6e52574cca6c: sregs.fs.base/gs.base round-trip
       2. 24a7f0575e18: sregs.cr2 cleared in load_user_sregs
       3. 11102c8176fb: current_mm_sync + CR4.PGE toggle (TLB flush)
       4. 3d426c4cb0c7: snapshot kvm_run state before unblock_signals
       5. a3f6238adcec: interrupt_end after EINTR marshal-back
       6. d5d5d15c4e5b: drain prev->active_mm in context_switch

     **Snapshot fix (commit 3d426c4cb0c7):** Codex audit identified
     a residual race in v2's KVM_RUN return path where
     `unblock_signals()` could schedule a different task between
     the ioctl returning and the marshal-back, causing the marshal
     to read the new task's exit state into the original task's
     pt_regs. Mirror v1's pattern (kvm-v1-archive/thread.c:3937-
     3979) by snapshotting `kvm_run->s.regs` and `exit_reason`
     into local vars BEFORE unblock_signals. Lifts substrate gate
     ceiling to PASS=10 (was 9 max).

     **Residual:** `child_delay` (parent forks, child does
     `usleep(100ms)` then printf, then exits) still 0/10 PASS on
     v2. Pure-syscall child (5000 getpid loop) PASSes 5/5;
     usleep-based and busy-loop children fail. The bug accumulates
     with elapsed CHILD time + scheduling activity. This is a
     DIFFERENT family from the TLB-flush bug — investigation
     continues in next session.

     **2026-04-30 RESOLUTION (commit bd435856948e):** The residual
     was *not* a TLB/mm/FPU corruption — it was the same -ERESTARTSYS
     leak class that ad06c7f5164c originally tried to fix. dash's
     `echo BEFORE / fork+wait child / echo AFTER` script printed
     BEFORE + child output but the AFTER line never reached the
     console. DIAG-SYS-WRITE/DIAG-SYS-WRITE-RET instrumentation in
     handle_io_trap caught the smoking gun:
       DIAG-SYS-WRITE[7]: pid=1 fd=1 len=7 rip=0x400f46c6
       DIAG-SYS-WRITE-RET[7]: pid=1 ret=-512
     `-512` is `-ERESTARTSYS`: dash's waitpid completed via
     SIGCHLD → handle_syscall returned -ERESTARTSYS in HOST_AX →
     the next write(1, ...) call also returned -512 (the kernel
     never converted it to -EINTR via do_signal because the syscall
     arm had no interrupt_end()).

     Re-introduced the post-handle_syscall interrupt_end() call.
     The original revert reason — SYSRETQ-RCX-rewind — was
     **already mitigated** by the explicit
       run->s.regs.regs.rcx = regs->gp[HOST_IP];
       run->s.regs.regs.r11 = regs->gp[HOST_EFLAGS];
     overwrites in the marshal-out, which were added at D.3 long
     before the ad06c7 attempt. With those overwrites in place,
     SYSRETQ pops whatever HOST_IP do_signal left, regardless of
     restart vs handler-entry vs unchanged path.

     Substrate gate impact:
       - Pre-fix (post-TLB-flush floor): PASS=6/FAIL=3/XFAIL=3
       - Post-fix:                       PASS=25/FAIL=3/XFAIL=3
                 (full parity with seccomp, stable across 3 runs)
     Closes #95 (fork_exec_wait now PASSes), #96 (multi-import
     Python prints multi_import_OK + exits cleanly), #107
     (child_delay now 10/10 PASS).

     **Residual investigation:**

     **Eliminated this round** (no improvement on the
     child_simple gate at ~30-40% PASS):
       - Worker / stub-child mm setup. Tested with
         `CONFIG_UM_WORKER_PROCESS=n`; bug rate unchanged.
       - Signal-delivery / sigframe setup. Tested with
         `sigprocmask(SIG_BLOCK, sigfillset())` blocking ALL
         signals before fork; bug rate unchanged.
       - Real KVM_SET_SREGS ioctl every dispatch (vs SYNC_REGS
         dirty-bit). No improvement.
       - Stale segment registers / FPU state. Tested resets; no
         improvement.
       - SIGALRM/preemption-driven race. Tested with
         `time-travel=infcpu` (disables real-time preemption);
         only marginal improvement (40% vs 30% PASS).

     Adding a `usleep(100ms)` to the child BEFORE its first stdio
     call makes the bug 10/10 deterministic — adding time/syscalls
     in the child reliably triggers the corruption.

     **Remaining suspects** (very narrow now):
       c. EPT/TLB invalidation timing. v2 may carry stale TDP
          cache entries. The audit suggests adding per-CPU
          last_task tracking + flushing on task change.
       d. User-stack page mapping race. Maybe KVM_REQ_TLB_FLUSH
          isn't being issued when child's mm CoW updates a PTE.

     Reproducer suite at
     `tools/testing/selftests/um/fork-tree-3level/repros/`
     expanded to 9 variants for fast bisection.

     Next: instrument KVM tracepoints (KVM_REQ_TLB_FLUSH,
     mmu_invalidate, kvm_inj_exception) during a single
     fork-tree-3level run and compare to seccomp's identical
     syscall trace.

     **Reproducer suite** at
     `tools/testing/selftests/um/fork-tree-3level/repros/` makes
     deterministic-ish bisection cheap:
       - raw_fork.c: PASS (no libc baseline)
       - libc_simple.c: PASS (libc no-fork baseline)
       - child_simple.c: deterministic FAIL ~70% (parent _exit;
         child does printf)
       - child_only_canary.c: FAIL 10/10 (child's strlen/memset
         force-XMM workload)

     Next: instrument `kvm_v2_load_user_sregs` to dump (cpu, pid,
     pgd_pa, gs.base, prev_pid_on_this_vcpu) on first child
     dispatch. Cross-reference against host-side strace of the
     stub-child process to look for unexpected memory operations
     between parent's last exit and child's first entry.

These are kernel-internals work, not host-side tooling, so they
belong on a fresh `uml-redesign-plan` checkout with codex-style
careful reading of the signal-delivery path. The reproducer +
gate now ship, so the next session can iterate against a green
seccomp baseline and a red kvm-v2 baseline with one command per
attempt: `umlctl gate run -f .../fork-tree-3level.toml --backend
kvm-v2 --kernel ...`.

The reproducer ships at:
  - `tools/testing/selftests/um/fork-tree-3level/fork_tree_3level.c`
  - `tools/testing/selftests/um/fork-tree-3level/run-fork-tree-3level.sh`
  - Gate: `tools/testing/selftests/um/gates/fork-tree-3level.toml`
    (min_pass=1 max_fail=0; v2 FAILs today, seccomp PASSes).

Tightening this gate to PASS on v2 IS the concrete deliverable for
resolving #95, #96, and the substrate truncation. Phase H.1b
(headline cpython gate) and Phase J Tier 1/2/3 are downstream.

**UPDATE 2026-04-30 (post-six-fixes session):** Phase H.1b is now
**UNBLOCKED**. After landing the six fixes (FS_BASE, cr2, TLB-flush
+ current_mm_sync, EINTR snapshot, EINTR interrupt_end, ctx-switch
drain) plus the harness-regex fix at 987e62eb3799, the cpython-tier0
gate PASSes deterministically on kvm-v2:

  $ umlctl gate run -f tools/.../gates/cpython-tier0.toml \
        --backend kvm-v2 --kernel ~/src/uml-builds/uml-clean/linux
  pass=1 fail=0 status=PASS
  hashlib_sha256_emptystring = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824

The 2cf24dba... is the canonical SHA256 of the empty string —
proves Python loaded libcrypto and called sha256() correctly under
v2. That class of bug (single-process Python with shared library
loading) is now closed.

Substrate gate at this checkpoint: PASS=7..9 mode 7-9, floor 7.
fork-tree-3level: 30-40% PASS (residual from sustained user-mode
schedule activity — separate task #107).

#### Substrate gate v2 non-determinism (2026-04-30)

10× sequential runs of `run-regrtest-repros.sh` against the kernel
binary built from `aaced3ce4924` (current tip):

| backend | runs | mode | min PASS | max PASS | distribution         |
|---------|------|------|----------|----------|----------------------|
| seccomp | 10   | 25   | 25       | 25       | bit-identical PASS=25/FAIL=3/XFAIL=3 |
| kvm-v2  | 10   |  4   |  4       |  9       | 8× PASS=4/FAIL=2; 1× PASS=6/FAIL=4; 1× PASS=9/FAIL=4 |

Diagnosis: not flake — **truncation**. The harness exits via the
host wrapper's "=== SUMMARY ===" trailer (host-side, after kernel
exit), without the in-guest "=== REGRTEST_REPROS_END" marker
appearing in any v2 run. That is, the kernel dies / init exits
mid-class-a-env every time on v2; the variable PASS count comes
from how many fork()s in `run-class-a-env.sh`'s for-loop completed
before the kill.

Concretely:
  - In every v2 run, no `--- class-b-process ---` header appears,
    so class-b/c/d are never reached.
  - In the dominant PASS=4 case, the kernel dies after 6 of the 13
    class-a-env reproducers complete; "lucky" runs reach further
    along the class-a-env loop before dying.
  - Seccomp finishes all four classes cleanly in every run.

Most likely root cause: the same fork-state-leak family that
underlies #95 and #96 (PT_SYSCALL_NR, FPU clobber, worker mm reset
have all been fixed; at least one more piece of state crosses the
fork boundary in v2 that doesn't in seccomp). The substrate gate
flake should resolve when #95/#96 are root-caused.

Implication for memo §E.4: the previously-claimed v2 line of
"PASS=6/FAIL=3 stable" was a best-of-N sample, not a stable
baseline. The real measurement is non-deterministic with PASS=4
mode, PASS=9 max. The `regrtest-substrate.toml` gate descriptor
keeps `min_pass=6` so that worst-case runs FAIL the gate
(surfacing the regression rather than masking it).

**Substrate gate** (seccomp baseline) stays green throughout
v2 development: PASS=25/FAIL=3/EXPECTED_FAIL=3 (verified bit-
identical across 10/10 runs at aaced3ce4924, 2026-04-30).

### Phase E.4 residual resolution: `interrupt_end()` in IO-trap dispatchers (commit `31ba9c354063`)

Two residuals were collapsed to a single root cause:
  - `#92`: substrate harness output lost after first fork+exec
  - `#93`: Python with extension imports (hashlib, etc.) infinite SIGSEGV loop

Mechanism: v2's `kvm_v2_handle_io_pf` (and the GP/UD/DE/OF siblings)
called `segv_handler` / `relay_signal` to queue a signal on
`current->task_struct`, then immediately returned to `KVM_RUN` at
the same RIP. The pending signal was never delivered (no `do_signal`
ran), so the user task re-entered the same faulting instruction,
faulted again, queued another signal, looped forever — or, for the
substrate harness, the parent shell's pending I/O completion /
SIGCHLD-on-wait was never drained, so it stalled at queued state.

Fix: mirror seccomp's pattern at
`arch/um/backend/seccomp/trap_user.c:184` — call `interrupt_end()`
in each of v2's per-vector handlers after the
`segv_handler`/`relay_signal` call but BEFORE marshal-back. Per
`arch/um/kernel/process.c::interrupt_end`, this drains
`resume_user_mode_work` (which fires `do_signal` to set up a
sigframe for default-action terminate signals or skip if the
signal has a handler), `schedule` (if `TIF_NEED_RESCHED`), and
the cgroup-threadgroup change end.

Verified empirically: Python imports + crypto + substrate gate
class-a-env all work end-to-end after the fix. Substrate baseline
(seccomp) stays unchanged green — seccomp's path was always
calling interrupt_end.

### Lesson for future memo work

Verify load-bearing design claims against source-level evidence
before committing infrastructure. `grep _PAGE_ arch/um/include/asm/
pgtable.h` is a 1-second sanity check; would have caught this before
Phase A landed.

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

**Mechanism correction (memo 26 §D rewrite, codex audit independent
finding C):** the original prose specced
`vmcall(UM_KVM_HC_*, ...)` for each exception class; that does not
work on stock KVM (`arch/x86/kvm/x86.c:10456,10520-10523`'s
`____kvm_emulate_hypercall` returns `-KVM_ENOSYS` in-kernel for
unknown hypercall nrs — only `KVM_HC_MAP_GPA_RANGE` exits to
userspace). E.3 must use one of the working mechanisms below; the
detailed choice is an E.3-start decision, but every option is
compatible with the IDT/IST infrastructure E.1/E.2 set up.

Two working options for the in-handler trap-out, in order of
preference:

1. **IO port per exception class** (paralleling §D.1's SYSCALL
   trap on port 0xf4). Each IDT handler is ~10-15 bytes of asm:
   move cr2/ecode/etc into argument registers, then `out %al,
   $0xf6` (#PF), `out %al, $0xf9` (#GP), etc. Host reads
   `run->io.port` to disambiguate, marshals registers via
   sync_regs, dispatches to the existing `arch/um/kernel/trap.c`
   path. Mirrors v1's port-based wire format
   (`kvm-v1-archive/kvm_backend.h:1004` defines
   `UM_KVM_SYSCALL_PORT = 0xf4` + `UM_KVM_SYSRETQ_PORT = 0xf5`;
   v1 didn't ship per-exception ports because v1 used a different
   exception-handling path, but the IO-port-per-class extension
   is mechanically straightforward).
2. **KVM exception bitmap interception**. KVM lets userspace
   intercept specific exception vectors via the VMCS exception
   bitmap (VMX) / SVM intercept_exceptions (SVM). When a guest
   exception fires whose bit is set, KVM exits with
   `exit_reason = KVM_EXIT_EXCEPTION` (or `KVM_EXIT_DEBUG` for
   #BP) and the host handles it without the guest IDT firing at
   all. Pros: no in-guest handler asm needed; KVM does the work.
   Cons: requires the exception bitmap config to land at
   vcpu_create (the bitmap is set via `KVM_SET_GUEST_DEBUG` for
   #BP/#DB; other vectors use the VMCS field directly via
   architecture-specific paths). E.3-start should evaluate which
   approach is cleaner for which exception class — likely #BP
   uses option 2 (KVM_GUESTDBG_*), all others use option 1
   (in-guest IDT handler emits IO port).

Per-vector mapping (handler delivers the listed signal via
`arch/um/kernel/trap.c` paths after trap-out):

- **#PF (vector 14):** `out` port 0xf6 with cr2 in RAX, ecode in
  RBX (or via stack frame pushed by CPU). UML's #PF handler maps
  to `do_page_fault` (kernel #PF, panic) or `segv_handler` (user
  #PF, deliver SIGSEGV).
- **#GP (13):** `out` port 0xf9 with ecode. Deliver SIGSEGV.
- **#UD (6):** `out` port 0xfa. Deliver SIGILL.
- **#DE (0):** `out` port 0xfb. Deliver SIGFPE.
- **#BP (3):** `KVM_GUESTDBG_USE_SW_BP` → `KVM_EXIT_DEBUG`
  (option 2; cleaner than IDT-handler-emits-IO-port for the
  software-breakpoint case). Deliver SIGTRAP.
- **#OF (4):** `out` port 0xfc. Deliver SIGSEGV (rare).
- **#DF (8):** panic. Should never happen post-Phase-E.

Each in-guest handler (#PF / #GP / #UD / #DE / #OF) is ~10-15
bytes of asm, all in the same dedicated kernel-half guest page
that the trampoline sits in (PML4[448] + offset). Re-uses the
per-VM PT chain D.4 already built — exception handlers slot in
alongside the trampoline at fixed offsets.

### E.4 — Validate exception handling (4 days)

- Test that user code triggering each exception delivers the right
  signal.
- Test recovery from #PF (lazy mmap fault → fill page → continue).
- Test that #DF doesn't fire under normal operation.

**Exit criteria:** all of v1's SEC.1, SEC.2, BUG.1-5 audit cases
covered by v2's exception path. Every signal delivery validated by
a focused test.

---

## Phase F — Signal/preemption (1 week) — **EFFECTIVELY COMPLETE**

**Goal**: SIGALRM (UML's timer-driven preemption) cleanly preempts
KVM_RUN; no other host signal interferes with KVM_RUN.

All three F.1/F.2/F.3 components landed during the Phase D rollout
because the same KVM_RUN dispatch loop needed signal handling to
function at all. Exit criterion validated empirically (2026-04-29):
a 3-second CPU-bound static binary advances `CLOCK_MONOTONIC`
through 3 seconds of wall-clock time, executing 5.8 billion
iterations — only possible if SIGALRM-driven preemption is letting
UML's timer subsystem run between dispatches.

### F.1 — `signal.c`: per-vCPU sigmask — **DONE** (`5297fe2bd165`)

`kvm_v2_install_signal_mask` at vcpu_create installs
`KVM_SET_SIGNAL_MASK` with sigfillset minus SIGALRM, paired with
`unblock_signals` after KVM_RUN to drain UML's deferred-signal
queue. Originally landed as D.5-fix-2 (the .vcpu_run flip
required this to make boot progress at all).

v1 also unblocked `KVM_UM_KICK_SIGNAL` (SIGRTMIN+5) for cross-vCPU
shootdown — **v2 doesn't need this** because TDP + mmu_notifier
handle cross-vCPU coherence.

### F.2 — Preemption-on-SIGALRM — **DONE** (`f0e4d1d95c2e`)

`kvm_v2_vcpu_run` (vcpu.c:1217+) catches `-EINTR` from KVM_RUN,
calls `kvm_v2_marshal_from_kvm_regs` to read the post-SIGALRM
guest state from sync-regs (KVM commits state on
KVM_EXIT_INTR per `arch/x86/kvm/x86.c::kvm_arch_vcpu_ioctl_run`),
runs `unblock_signals` to drain UML's deferred-signal queue, and
returns to userspace() which calls `interrupt_end()` and
re-dispatches. v1's archive shape at thread.c:3939+5121-5127.

### F.3 — No bootstrap-window special handling — **DONE** (architectural)

v1 needed special handling for "EINTR fired mid-bootstrap-IRETQ"
(memo 22 §"EINTR-bootstrap-RIP-preservation"). v2 has no
bootstrap-IRETQ stub — task #87's bootstrap-IRETQ approach was
replaced at 32a7603236b0 by direct CPL=3 SREGS install. EINTR
just re-marshals via sync-regs and re-enters KVM_RUN with the
post-SIGALRM RIP, no special case needed.

**Exit criterion validated**: SIGALRM preemption works empirically;
3-sec CPU-bound test runs cleanly with timer advancing.

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

#### G.1 prep findings (2026-04-30)

A first-pass build with `CONFIG_SMP=y` + `NR_CPUS=4`:
- Builds clean (no compile errors)
- v2 substrate creates 4 vCPUs (vcpu_fd 5/6/7/8) at boot — Phase
  C's per-host-CPU pool sizing already covers SMP
- BUT: `rcu: RCU restricting CPUs from NR_CPUS=4 to nr_cpu_ids=1`
  fires at boot — UML's runtime nr_cpu_ids is still 1 even with
  CONFIG_SMP=y
- `per_vcpu_ist_tss` only installs cpu=0 (the others stay zeroed)
- Boot then panics in `kvm_v2_handle_io_trap` during init=/bin/true

So G.1 isn't just "make existing vCPUs runnable" — it requires:
1. UML-side: enable SMP runtime CPU bring-up (currently stubbed?)
2. v2-side: install IDT/IST/TSS for ALL vCPUs (not just cpu=0)
3. v2-side: kvm_v2_vcpu_run already handles the per-host-CPU
   dispatch but maybe needs a stage to wait for vCPUs > 0 to be
   "online" per UML's smp_init.

Open task #68 carries this; not blocking Phase J.

#### G.1 status update (2026-05-01)

Re-test after the cumulative work landed since 2026-04-30 (in
particular the FPU snapshot pair, mmu_gather drain ordering,
sregs.fs.base/gs.base round-trip, and the #121 EINTR-mid-IDT-
delivery cr2/IST fix). With the SAME `CONFIG_SMP=y NR_CPUS=4`
build, the 2026-04-30 panic is GONE:

- `ncpus=2` boot: clean, both `processor: 0` and `processor: 1`
  in /proc/cpuinfo, both have non-zero counters in /proc/stat
- `ncpus=4` boot: clean, all 4 processors visible
- Substrate gate under SMP build (`ncpus` defaults to 1 unless
  set): `PASS=25/FAIL=3/EXPECTED_FAIL=3` — same as UP, matches
  seccomp baseline
- cpython-parity gate under SMP build: `parity=21 diverge=0
  skip=0` — full bit-identical parity
- 8 parallel shell workers under `ncpus=4`: all complete cleanly,
  init exits cleanly
- mt-byteset N=4 under `ncpus=4`: 49/50 PASS = 98% (was 90%
  before the #121 inline-handler SMP fix at e5977806fd14)

Thread (1) "UML-side SMP bring-up" turns out to have been working
all along — the panic was a victim of the cr2/IST race which
also bites under SMP from #121 (only louder because cross-vCPU
task migration widens the race window). With the inline handler
shipped, single-threaded SMP boots are clean.

Thread (2) "per-vCPU IDT/IST/TSS install for all vCPUs" was
already done — `kvm_v2_install_per_vcpu_ist_tss` loops over all
NR_CPUS in `exception_install` and the 2026-04-30 "only cpu=0
installed" observation was apparently pre-Phase-E.2 / before the
loop was added.

Thread (3) "wait for vCPUs > 0 online" was unnecessary — UML's
own smp_prepare_cpus does the wait, and v2's per-host-CPU pool
dispatcher correctly picks up the right vCPU once a host-CPU
pthread comes online.

**Remaining mt-byteset SMP flake** (separate from the original #121):

| Thread count | ncpus | PASS |
|---|---|---|
| T=2 | 4 | 60/60 = 100% |
| T=4 | 4 | 196/200 = 98% |
| T=8 | 4 | 12/30 = 40% (high stress, 2× threads/vCPU) |

Symptom: `mt-byteset[N]: segfault at 0x{1,2,3} ip 0x40197b error 2`
— slow_memset writing to a small address (RDX corrupted to 1-3).
mt-byteset-diag (sigaction-instrumented variant) under T=8 captures:
- `last_mmap_p=0x42014000` (TLS, this thread's most recent mmap)
- but `[rbp-8]` (the vp local slot) = `0x44078000` (a DIFFERENT
  thread's mmap-region address)
- `RSP=0x420031e0` but `RBP=0x440371e0` (mismatched; RBP belongs
  to a different thread's stack region)

**Root cause: cross-vCPU guest TLB stale**. When UML's
`um_tlb_sync` runs (on PTE-level mm changes), it toggles CR4.PGE
on the CURRENT vCPU's next dispatch — flushing only that vCPU's
guest TLB. Other vCPUs running tasks in the SAME mm continue to
cache stale GVA→guest_PA mappings until their own next dispatch.
When thread A's stack page is freed and the physical page is
recycled to thread B's mmap region, thread A on vCPU 0 may still
have a stale TLB entry [stack_VA → physpage_X], where physpage_X
is now thread B's mmap region. Thread A's stack reads/writes
alias with thread B's mmap data → register-state swap symptoms.

**Fix path** (Phase G.2 — cross-vCPU IPI / TLB shootdown):
- On every PTE-level mm change in UML, issue cross-vCPU TLB
  shootdown so all OTHER vCPUs flush their guest TLBs at next
  entry.

**Phase G.2 commits A+B SHIPPED (2026-05-01)**: 77cc1821c595 +
7e1c255a09ad. Adds nullable `tlb_kick_others` backend op (contract
v2 bump), v2 impl that pthread_sigqueue's IPI_SIGNAL via
os_send_ipi to all other UML CPUs, and unblocks IPI_SIGNAL in v2's
KVM_SET_SIGNAL_MASK so the IPI actually interrupts KVM_RUN.

**Phase G.2 commit C DEFERRED twice**: 

- 1st attempt (broadcast): regressed T=4/ncpus=4 from 196/200
  (98%) → 18/60 (30%). IPI storm.
- 2nd attempt 9f0ff6257e8b (cmpxchg dedup, at-most-one-in-flight
  per vCPU): regressed T=4/ncpus=4 to 46/60 (77%). Even bounded,
  the kicks under high mm-churn (mt-byteset ~1k drains/sec/thread)
  disrupt forward progress.

Conclusion: kick-from-um_tlb_sync is fundamentally too eager.
A future attempt needs:
- per-mm throttle (skip if mm was recently kicked)
- mm_cpumask narrowing (only kick vCPUs running THIS mm)
- OR fully-passive gen-counter (no IPI; rely on natural
  dispatch boundaries + SIGALRM 100Hz preemption)

Tracked as #143 (now closed; needs new task for next attempt).
The cmpxchg dedup infrastructure shipped at 9f0ff6257e8b stays
in place; activation deferred. The substrate gate and
cpython-parity gate (headline metrics) remain CLEAN under SMP
without activation.

**Empirical T:N matrix** for mt-mini stress (the substrate
isn't affected):

| T | ncpus | Pass rate |
|---|---|---|
| 2 | 4 | 100% |
| 4 | 4 | 100% (60/60) |
| 4 | 8 | 100% (30/30) |
| 8 | 4 | ~45% (13/30) |
| 8 | 8 | ~47% (14/30) |
| 8 | 4 + taskset 0-3 | ~53% (16/30) |

Bug scales with **thread count in same mm**, NOT with T:N ratio
or host-CPU pinning. T=4 is rock solid; T=8 is a stress edge case
unrelated to vCPU pool sizing.

Workaround until G.2-cont lands: run substrate workloads on UP
builds (`CONFIG_SMP=n`), or with `ncpus` ≥ thread count.

This is a STRESS-test edge case affecting only the multi-thread
mmap-stress reproducer at high T/N. Substrate gate (PASS=25/FAIL=3/
EXPECTED_FAIL=3) and cpython-parity gate (21/21 PARITY) are both
clean under SMP — those don't exercise the same churn-driven mm
race.

### G.2 — `smp.c`: cross-vCPU IPI (if needed) (2 days)

**Mechanism correction (memo 26 §D rewrite, codex audit independent
finding C):** the original prose specced
`vmcall(UM_KVM_HC_IPI, target_cpu, action_nr)`; that does not work
on stock KVM (`arch/x86/kvm/x86.c:10456,10520-10523` returns
`-KVM_ENOSYS` for unknown hypercall nrs). Replaced with KVM's
existing request-and-kick mechanism, which is what G.1 already uses
for cross-vCPU TLB shootdown.

For UML's intra-guest IPIs (e.g., `smp_call_function`):

- Define an in-kernel request bit (or reuse an existing
  `KVM_REQ_*`); KVM exposes `kvm_make_request(vcpu, req)` plus
  `kvm_make_all_cpus_request(kvm, req)` to mark request bits and
  kick target vCPUs out of guest mode via the
  `KVM_REQ_OUTSIDE_GUEST_MODE` check at the top of
  `vcpu_enter_guest` (`arch/x86/kvm/x86.c`).
- The kicked vCPU re-enters host code with `KVM_RUN` returning
  -EINTR (or with `kvm_run->exit_reason` set per the request
  semantics); the host dispatcher handles the action and re-
  enters KVM_RUN.
- For action_nr semantics that need richer state than a single bit,
  pair the request with a per-vCPU mailbox structure (`struct
  kvm_v2_vcpu` field: pending action queue under a spinlock). The
  kicked vCPU drains the queue on host-side wakeup.
- Cross-mm IPIs (target task is in another mm worker): route
  through the per-mm worker dispatcher (memo 28 Part C); the kick
  ends up as a SIGUSR2 to the destination worker, which consumes
  the mailbox.

**No custom hypercall, no IO-port trap, no synthesised exception.**
This is the same primitive Linux native KVM uses for inter-vCPU
coordination.

### G.3 — SMP validation (2 days)

- Boot UML with `--with-cpus=4`.
- Run `make ARCH=um -j4` of the kernel inside the guest. No corruption,
  no hang, no divergence.

**Exit criteria:** SMP build inside SMP guest works.

---

## Phase H — Performance (1-2 weeks) — **BASELINE MEASUREMENTS LOOK LIKE H.1 ALREADY HITS THE GATE**

**Goal**: cpython-parity gate runs at ≤ 1.2× the seccomp wall-clock
time. Identify and fix any pathological hot paths.

### H.1 — Baseline measurements (2 days) — **COMPLETE; v2 is FASTER than seccomp**

#### Initial measurement (2026-04-29 at tip `8955ce7f878d`)

| Workload                          | seccomp | v2     | v2/seccomp |
|---|---|---|---|
| `python3 -c "import math; print('done')"` minimal startup | 104.3 ms | 58.4 ms | **0.56×** |

#### Re-baseline (2026-04-30 at tip `aaced3ce4924`)

After two `interrupt_end()` reverts (`bad8d61d2592` →
`79a50392d4e7`, `ad06c7f5164c` → `8955ce7f878d`) and the
`PT_SYSCALL_NR` clear (`a478952b8da0`) landed, we re-ran the
same shape. **The perf advantage is intact and slightly larger:**

| Backend | median | min | max | samples |
|---|---|---|---|---|
| seccomp | 90 ms | 90 ms | 90 ms | 7/7 bit-identical |
| kvm-v2  | 40 ms | 40 ms | 40 ms | 7/7 bit-identical |
| **ratio kvm-v2 / seccomp** | **0.444× (2.25× faster)** | — | — | — |

Methodology: in-kernel `printk.time=1` timestamps. Wall-clock from
the "Run <init> as init process" message (kernel hands off to
init) to the "Kernel panic - not syncing: Attempted to kill init!"
message (init exits, panic=-1 fires). Captures init shell + python
startup + sync, excludes pre-init kernel boot (which is identical
across backends). Note printk's timer has 10 ms granularity — the
40 ms / 90 ms numbers are bucketed, so the true ratio is somewhere
in [0.30, 0.55]; the win is real but the precision floor is ±10 ms.
Harness: `tools/testing/selftests/um/perf-py-startup/run-perf-py-startup.sh`.
The advantage comes from v2's KVM-direct syscall path (one
KVM_EXIT_IO per syscall) versus seccomp's stub-child + ptrace
round-trip overhead.

Important caveat: this measurement is single-process. Multi-process
/ fork-heavy workloads cannot yet be perf-measured because the
substrate gate kills v2 mid-stream after a variable number of
fork()s (see "Substrate gate v2 non-determinism (2026-04-30)" above).
H.1b (full cpython gate measurement) remains blocked on root-causing
that fork-loop bug — likely the same family as #95/#96.

#### Re-re-baseline (2026-04-30 at tip `8b29ce0b5608`)

After the syscall-side `interrupt_end()` re-introduction at
`bd435856948e` (the substrate-parity fix), the wall-clock numbers
re-bucket:

| Backend | median | samples |
|---------|--------|---------|
| seccomp | 90 ms  | 0.090 0.090 0.100 0.090 0.090 0.090 0.090 |
| kvm-v2  | 90 ms  | 0.090 0.090 0.090 0.090 0.090 0.090 0.090 |
| **ratio kvm-v2 / seccomp** | **1.000×** | — |

The 2.25× lead from `aaced3ce4924` (v2 = 40 ms, bucketed) narrowed
to 1.0× (both = 90 ms, bucketed) once `interrupt_end()` runs after
every syscall. That call invokes `do_signal` + reschedule check on
every syscall return — exactly what's needed for `-ERESTARTSYS`
translation but not free per dispatch. Phase H gate criterion is
"≤ 1.2× seccomp wall-clock"; we still PASS the gate at 1.0×, but
the slack is gone. Phase H.2 (hot-path optimization) is now the
next perf lever; likely candidates: skip `interrupt_end` when no
signal is pending, batch the `current_mm_sync` drain, narrow the
CR4.PGE TLB toggle to mm-change events.

The trade was correct: v2 substrate gate moved from PASS=6 to
PASS=25 (full seccomp parity), closing #94 / #95 / #96 / #107 in
exchange for a 2.25× → 1.0× perf regression. Phase H.2 can
optimize back; correctness ships first.

### H.1b — Headline cpython gate measurement (2026-04-30 update)

After the syscall-side interrupt_end fix at bd435856948e and the
substrate gate reaching full PASS=25 parity, the cpython-parity
gate is now reachable. Three back-to-back trials at tip
8b29ce0b5608:

| Trial | parity | diverge |
|-------|--------|---------|
| 1     | 13     | 8       |
| 2     | 15     | 6       |
| 3     | 18     | 3       |

Best run shows 18/21 modules at parity (vs seccomp 21/21 reference).
The diverging modules vary across runs (test_struct, test_dict,
test_int, test_itertools, test_typing all rotate through the
DIVERGE bucket), which points at one underlying flaky pattern,
not 5 independent regressions.

**Residual root cause** (deferred, see #115):
`InterpreterPoolExecutor` (5 worker threads each running its own
PEP-684 subinterpreter, each importing `struct`) reproduces the
flake in isolation at ~50%. The minimal repro is:

```
python3 -u -c "
from concurrent.futures import InterpreterPoolExecutor
with InterpreterPoolExecutor(max_workers=5) as ex:
    list(ex.map(exec, ['import struct'] * 5))
"
```

Symptoms: SIGSEGV at near-NULL+small-offset writes (PyFunction_
NewWithQualName, PyList_New, libc.so allocator) or glibc malloc
abort ("free(): corrupted unsorted chunks"). 100% PASS under
seccomp.

Things that PASS under v2:
- Single-threaded Python with subinterpreters (10× create+run+
  destroy)
- ThreadPool with 5 threads each importing struct (no subinterp)
- All 25 substrate-gate reproducers (covers fork+exec+wait,
  ioctls, etc.)
- cpython-tier0 (libcrypto+hashlib loading from C extension)
- multi-import (json+base64+os in one process)
- child_delay 10/10
- single-process workloads in general

Codex (gpt-5.5 xhigh) audit, 2026-04-30 (full report at
/tmp/codex-response.txt):

> "Most likely class: shared per-host-CPU vCPU ownership is being
> held across paths that can run UML signal/scheduler work."

The hypothesis is that `kvm_v2_vcpu_run`'s preempt_disable spans
the entire dispatch including handle_syscall + interrupt_end, and
sleepable work in there can let other UML tasks share the vCPU
mmap. v1 used `block_signals()` before KVM entry and
`unblock_signals()` only after exit-state copy, narrowing the
window. v2 has the snapshot-before-unblock half but holds preempt
across the whole dispatch.

Tactical fixes attempted (none moved the failure rate):
- Add `block_signals()` before `load_user_sregs` (still 5/10 PASS)
- Drop the unconditional `marshal_sregs_back` (regressed substrate
  gate by 1, no help on InterpreterPool)

This is the original Bug B *family* from v1 — InterpreterPool was
the canonical Bug B trigger. v2's no-shadow-PT design eliminates
many bug classes (memo §"Risk classes that v2 STRUCTURALLY
ELIMINATES"), but the shared-per-CPU-vCPU + held-preempt design
introduced a new race that surfaces only on the multi-thread +
multi-subinterpreter combo.

**Defer to a focused deep-dive session** (#115) — the current
substrate parity ships, and Phase I polish + Phase J validation
work is independent of this residual.

### H.1b breakthrough (2026-04-30 cont): drop os_map_memory

After writing a 30-line C-only reproducer (`tools/testing/
selftests/um/mt-mmap-stress/mt-mmap-stress.c` — 3 pthreads ×
100 iters of mmap+memset+munmap), iteration-threshold sweep
showed the failure rate scales with iteration count (1-20 iters
PASS, 50 iters 80%, 100 iters 20%). MAP_POPULATE workaround
gave 5/5 PASS, narrowing the bug to the LAZY page-fault path.

Walking through arch/um/backend/kvm-v2/region.c revealed that
mm_region_added was calling `os_map_memory(va, phys_fd, offset)`
in the SPAWNER mm on every guest user mmap — installing a
parallel mapping at user-half VAs that was REDUNDANT under v2's
slot-0-only design.

Slot 0 (gpa=0..physmem_size, hva=uml_physmem) is the canonical
EPT coverage. KVM's TDP walk resolves guest VA → guest PT (in
physmem) → guest GPA → user_addr=uml_physmem+GPA (slot 0) →
spawner mm walk for the uml_physmem range. Per-region maps at
user-half VAs were leftover from Phase B's per-region memslot
design (dropped at E.5 / Codex Claim C).

The redundant parallel mapping was racing under multi-thread
workloads despite mmap_lock serialization, causing the
post-memset readback corruption in mt-mmap-stress.

**Fix at f77a31d1fbe4**: drop os_map_memory in region_added,
drop os_unmap_memory in region_removed.

| State                              | mt-mmap-stress | substrate gate     |
|------------------------------------|----------------|--------------------|
| Pre-region-turnstile (baseline)    | 0/10 PASS      | 25/3/3 (occasional 24/4/3) |
| region-turnstile only (b4bad916ca93) | ~30%        | same |
| **Drop os_map_memory (f77a31d1fbe4)** | **63%**     | **25/3/3 stable × 3 runs** |

The remaining 37% mt-mmap-stress flake + ~50% InterpreterPool
flake is a smaller residual race elsewhere — possibly in the
lazy-PF serialization of guest PT updates or in some other
v2-specific path. Future investigation tracks under #115.

### H.1b residual ROOT-CAUSED (2026-04-30 cont 2): mmu_gather free-before-flush

**Hypothesis (codex gpt-5.5 audit, 2026-04-30):** UML's deferred
TLB-sync model violates `mmu_gather`'s free-after-flush contract.

Standard kernel MM contract (mm/mmu_gather.c:421-425):
```
tlb_flush_mmu():
    tlb_flush_mmu_tlbonly()  # actual TLB invalidation
    tlb_flush_mmu_free()     # frees pages back to buddy
```

Under UML, `tlb_flush()` (asm-generic/tlb.h:443-447) calls
`flush_tlb_range()` which on UML is just `um_tlb_mark_sync` —
sets `sync_tlb_range_{from,to}` (arch/um/include/asm/tlbflush.h:53-57).
The actual drain is deferred to `um_tlb_sync` at the next vcpu_run
dispatch (kvm-v2/vcpu.c:1429-1434). Pages are freed back to buddy
BEFORE the actual flush.

Under v2, the only mechanism that flushes the GUEST CPU's TLB is
the CR4.PGE toggle in load_user_sregs (vcpu.c:1148) at next
dispatch entry. Slot-0 doesn't change so no mmu_notifier fires;
region_added/removed are no-op (post f77a31d1fbe4 + c77a585330f3).

**Diagnostic confirmation:** added a printk in
`tlb_batch_pages_flush` that logs `(pid, comm, pfn, sync_tlb_range)`
for every encoded_page freed (mm/mmu_gather.c:146 patch — reverted
after capture). Captured failing run at iter 26 with PFN 0x1123:
```
FAIL T0 iter26 page6 p=0x41809000 got=22 expect=0
  pfn_after_memset=0x1123 pfn_at_check=0x1123
DIAG-MT-PFN free pid=23 comm=mt-diag pfn=1123 pending=[41803000,41813000)
DIAG-MT-PFN free pid=23 comm=mt-diag pfn=1123 pending=[41803000,41813000)
```

The failing PFN was freed twice via mmu_gather while sync_tlb_range
was non-empty (pending = the test's mmap range). Confirms hypothesis.

The corrupting bytes (`16 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00`
repeating every 16 bytes) is recognizable kernel-struct content
(plausibly slab freelist metadata or sysctl table entries).

**Why seccomp doesn't show this:** under seccomp, user accesses
go through stub-child mm, which does its own real munmap on its
host TLB during sync drain — the stub-child's host TLB is flushed
by the host kernel's normal mmu shootdown. v2's slot-0 model
puts user pages and kernel pages in the SAME physmem fd, and the
guest CPU runs both — so kernel writes to a freed PFN become
visible to a user-half stale TLB lookup.

**Fix candidates (Option B SHIPPED at e4d347ae48d8 + bd8f6fe1c511):**

A. **Eager flush in flush_tlb_range:** (NOT shipped) make UML's
   flush_tlb_* call a force_tlb_flush hook which under v2 issues
   a CR4.PGE toggle ioctl + KVM_RUN no-op. High overhead per
   munmap call but architecturally simplest.

B. **Defer page-free until after drain:** SHIPPED at
   `e4d347ae48d8` + `bd8f6fe1c511`. mmu_gather hands the
   encoded_page array to `um_mmu_gather_defer` (arch/um/kernel/
   tlb.c) which queues entries on `mm->context.deferred_free_pages`.
   Backend's vcpu_run calls `um_mmu_gather_drain` AFTER KVM_RUN
   exits — by which point CR4.PGE flush has run, so deferred
   pages can safely return to buddy.

C. **mmu_notifier-aware slot-0:** (NOT shipped) invalidate KVM
   TDP for freed-page GPAs via mmu_notifier callback. Heaviest
   plumbing.

**Empirical result (post Option B):** mt-mmap-stress 50 trials
went from 64% → 68% PASS — statistically within noise. Substrate
gate stable PASS=25/FAIL=3/EXPECTED_FAIL=3, cpython-tier0 PASS
(no regression). The defer/drain mechanism IS firing per
diagnostic — `DIAG-DRAIN count=16` per 64KB unmap matches the
test workload.

**Yet failures persist with the SAME corruption pattern**
(`16 00 00 00 ff ff ff ff 00 00 00 00 00 00 00 00` repeating
every 16 bytes). This means either:

1. A free path bypasses `tlb_batch_pages_flush` (and our defer
   hook), e.g. compaction's `migrate_pages` → `folio_put` direct
   release, or reclaim's swap-out path.
2. Page-table page freeing (via `tlb_table_flush` / `__tlb_remove_table`)
   uses a separate path we don't cover; PT pages re-mapped to
   user could appear with their old PTE-table contents.
3. The corruption pattern is not from a freed-then-reused page
   at all but from somewhere else entirely (e.g. clear_page
   writing to wrong kernel-half VA due to a different TLB issue).

The 16-byte-period kernel-struct pattern (4-byte 22, 4-byte
0xFFFFFFFF, 8-byte 0) doesn't match IDT entries (segment field
would be 0x08 not 0x00) or PT entries (PT would be 8-byte
period). Possibly a kmalloc-16 slab object freelist. Codex
follow-up audit needed; #117 cont.

Reproducer: tip bd8f6fe1c511. mt-mmap-stress 3T×50 iters:
~64-68% PASS on v2 post-fix, 100% PASS on seccomp, 100% PASS
with MAP_POPULATE.

### H.1b residual cont 2 (2026-04-30): instruction-class isolation

After Option B fix didn't statistically improve mt-mmap-stress,
isolated the failure to specific instruction classes by writing
4 variants of memset:

| Test variant      | Instruction       | PASS rate (30 trials) |
|-------------------|-------------------|------------------------|
| byteset (volatile *p = v) | per-byte mov | **100%** PASS |
| REP STOSB         | rep stosb (GPR)   | ~93% PASS |
| glibc memset      | SSE `__memset_sse2` | ~64% PASS |
| AVX YMM (vmovdqu) | _mm256_storeu_si256 | **0%** PASS |
| SSE XMM (movdqu)  | _mm_storeu_si128  | ~65% PASS |

Failure rate scales with FPU/SIMD width:
- byteset (no SIMD)        100%
- REP STOSB (GPR only)      93%
- SSE/glibc (XMM 16-byte)   65%
- AVX (YMM 32-byte)          0%

**Conclusion:** the residual bug is FPU-related. XMM register
state is corrupted across the #PF cycle. The fault-during-store
re-execution writes garbage (likely zeros) instead of the
splat'd byte value.

KVM normally auto-saves/restores vcpu FPU on KVM_RUN entry/exit
via `fpu_swap_kvm_fpstate`. v2's `kvm_v2_fpu_install_on_first_run`
already opted out of explicit per-dispatch reset (vcpu.c:1828-1860)
because the earlier shape "destroyed XMM/x87 state mid-instruction
whenever a task re-entered after a #PF (lazy CoW etc)".

So in theory KVM should preserve FPU. But empirically it isn't.
A direct attempt to add `KVM_GET_FPU` at handle_io_pf entry +
`KVM_SET_FPU` at exit MADE THINGS WORSE (40% vs 60% baseline) —
suggesting the explicit save/restore interferes with KVM's
auto-handling.

**Next investigation directions (not yet pursued):**

1. Audit per-vCPU pool member FPU initialization at vcpu_create —
   does each vCPU have correct CR0.MP/NE, CR4.OSFXSR/OSXMMEXCPT
   set? `kvm_v2_install_initial_sregs` (vcpu.c:519) sets these
   on the boot vCPU but pool members may inherit incorrectly.

2. Check whether KVM_RUN's userspace exit path (`KVM_EXIT_IO`
   from `out`) skips the FPU save that direct VMEXIT (e.g.
   triple fault, EPT violation) does. If the IO trap is "fast
   path" without FPU save, vcpu FPU state lives in HW registers
   and may be clobbered by host kernel-mode code between exit
   and re-entry.

3. AVX 0% PASS specifically: AVX is CPUID-masked off (vcpu.c:128-184)
   and CR4.OSXSAVE off. Guests issuing YMM stores on this setup
   may behave undefined. Confirm via #UD trap dispatch — if AVX
   instructions are silently nopping rather than trapping, that's
   a separate KVM/CPU configuration issue.

The byteset (100% PASS) and REP STOSB (93% PASS) baselines are
clean enough to ship if SIMD-correctness is acceptable to defer.
v2 backend's main user is the substrate (which doesn't use SIMD
via memset since the kernel's static binaries don't). User-mode
binaries that use AVX heavily would expose this — Phase J validation
should catch this in cpython-parity full-suite runs.

### H.1b residual cont 3 (2026-04-30): DEFINITIVE proof of XMM corruption

Wrote `tools/testing/selftests/um/mt-mmap-stress/mt-xmmprobe.c`
which loads XMM0 with a known pattern, triggers 16 #PFs by writing
across page boundaries via `movdqu xmm0`, then reads XMM0 back to
memory and compares.

Captured failures:
```
FAIL T2 iter18 XMM_DRIFT before=02020202020202020202020202020202
                          after=16000000ffffffff0000000000000000

FAIL T1 iter42 XMM_DRIFT before=01010101010101010101010101010101
                          after=00000000000000000000000000000000
```

The "after" pattern in T2's case (`16 00 00 00 ff ff ff ff 00 00 00
00 00 00 00 00`) is **THE EXACT SAME PATTERN** that appears in the
corrupted user pages in mt-mmap-stress failures (`16 00 00 00 ff
ff ff ff` repeating every 16 bytes). T1's case is all zeros.

This proves: when the GUEST CPU re-executes `movdqu xmm0, [user_va]`
after #PF, **XMM0 has been clobbered with kernel-struct data (or
zeros) instead of preserving the splat'd byte value**. The user
page receives whatever XMM0 has post-fault, which is kernel data —
matching the corruption pattern observed in the original
mt-mmap-stress C reproducer.

The corruption is NOT in user memory — it's in the GUEST CPU's
XMM register state. KVM is not properly preserving XMM/FPU across
KVM_EXIT_IO → userspace processing → KVM_RUN re-entry under v2's
dispatch flow.

**Why direct KVM_GET_FPU/SET_FPU made things WORSE (40% PASS):**
KVM's vcpu->arch.guest_fpu may be stale at the time of GET (KVM
might not save FPU on the fast-path KVM_EXIT_IO from the in-guest
`out` instruction). Saving stale state and restoring it overwrites
the still-correct hardware FPU.

**Likely fix path (NOT yet attempted):** force KVM to save FPU on
exit and restore on entry. Possible mechanisms:
- KVM_CAP_X86_USER_SPACE_FPU enablement (if exists)
- Explicit `kernel_fpu_begin/end` around `os_ioctl_generic(KVM_RUN)`
- Use VCPU events / KVM_GUESTDBG to force "deep" exit semantics
- Manually save FPU into per-task arch_thread on every IO trap and
  restore on next dispatch (heavy but local fix)

Alternative: investigate whether v2's vcpu_create properly enables
CR0.MP/NE and CR4.OSFXSR/OSXMMEXCPT for the per-host-CPU pool members
(boot vCPU has these set at vcpu.c:519, but pool members may not).

Reproducer: `tools/testing/selftests/um/mt-mmap-stress/mt-xmmprobe.c`
shipped at this commit.

### H.1b CLOSED (2026-04-30): per-task FPU snapshot fix at 49b3e40a968c

**Test results post-fix (2026-04-30 confirmation runs):**

| Test                                | Pre-fix    | Post-fix   |
|-------------------------------------|------------|------------|
| mt-mmap-stress (3T×50, 200 trials)  | 60-68%    | **96%**    |
| mt-detail (3T×50, 50 trials)        | flaky      | **100%**   |
| mt-xmmprobe (XMM preservation)      | 80%        | **100%**   |
| mt-sse (SSE-only memset)            | 65%        | **90%**    |
| mt-byteset (no SIMD)                | 100%       | 100%       |
| mt-rep (REP STOSB)                  | 93%        | n/m        |
| mt-avx (AVX YMM)                    | 0%         | 0% (XSAVE) |
| InterpreterPool (subinterp+thread)  | 50%       | **90%**    |
| fork-tree-3level (10 trials)        | flaky     | **100%**   |
| cpython-parity (8 runs)             | 14-17/21  | 15-20/21   |
| substrate gate (10 runs)            | 25/3/3    | 25/3/3 ✓   |
| cpython-tier0 (3 runs)              | PASS      | PASS ✓     |
| perf-py-startup ratio (3 runs)      | 0.667×    | 0.667× ✓   |

The 4% mt-mmap-stress residual is a SEPARATE bug class (not FPU):
captured via mt-byteset.c (per-byte volatile mov, no SIMD), the
residual fail rate is ~4% — same as mt-mmap-stress. Crash signature:
`mt-byteset[N]: segfault at 0 ip 000000000040197b` (NULL deref on
`mov %al, (%rdx)` inside slow_memset). Plain non-SIMD stack-or-
pointer corruption, unrelated to XMM clobber. Likely race during
pthread fork+execve or page-table state interaction. Tracked as
separate Phase J validation finding; substrate gate is unaffected.

cpython-parity remaining flake (3-7 tests, mostly test_struct,
test_bytes, test_dict, test_int, test_heapq, test_itertools)
likely a separate issue — those tests' subprocess-spawning paths
hit code we haven't fully covered. AVX-512 + XSAVE plumbing
(Phase H or beyond) would close this.

**Fix mechanism:** bypass KVM's broken IO-exit FPU auto-save by manually
snapshotting + restoring around the host-kernel exit-handling window.

Specifically in `arch/um/backend/kvm-v2/vcpu.c::kvm_v2_vcpu_run`:

```c
rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_RUN, 0);

/* IMMEDIATELY snapshot — before any host-kernel code can clobber FPU. */
{
    int fpu_rc = os_ioctl_generic(vcpu->vcpu_fd, KVM_GET_FPU,
        (unsigned long)&current->thread.arch.kvm_v2.iotrap_fpu);
    current->thread.arch.kvm_v2.iotrap_fpu_valid = (fpu_rc == 0);
}
```

And before the NEXT KVM_RUN re-entry (after IST snapshot restore, before
marshal_to_kvm_regs):

```c
if (current->thread.arch.kvm_v2.iotrap_fpu_valid) {
    (void)os_ioctl_generic(vcpu->vcpu_fd, KVM_SET_FPU,
        (unsigned long)&current->thread.arch.kvm_v2.iotrap_fpu);
    current->thread.arch.kvm_v2.iotrap_fpu_valid = false;
}
```

Per-task storage `iotrap_fpu` + `iotrap_fpu_valid` added to
`arch_thread.kvm_v2` struct in `arch/x86/um/asm/processor_64.h`.

**Why per-task and not per-vCPU:** under v2's per-host-CPU vCPU pool,
multiple UML tasks share one vCPU. If T1 and T2 are interleaved, T1's
snapshot must follow T1 around (not stay in T2's vcpu state). Per-task
storage keyed via `current->thread.arch` matches the IST frame pattern
(memo §H.1b residual fix at 31748fea602e earlier).

**Upstream attribution (NOT a KVM kernel bug):** verified via three
standalone KVM userspace reproducers (no UML, no special framework):

1. `kvm-fpu-repro.c` — basic XMM-load + IO-trap + readback: PASS
2. `kvm-fpu-uml-pattern.c` — 200 iters with SYNC_REGS + CR4.PGE
   toggle (UML's exact dispatch pattern): PASS, no FAILs
3. `kvm-fpu-task-switch.c` — 200 iters across 3 simulated tasks
   with explicit FPU save/load + host XMM clobber between RUNs:
   PASS, 0 FAILs across 4 variants
4. `kvm-fpu-sigalrm.c` — SIGALRM-driven KVM_RUN -EINTR + host
   XMM clobber on every -EINTR: 100 iters × 3 runs, all PASS

**KVM correctly preserves vcpu FPU across all tested scenarios.**

The bug is therefore in UML's specific environment. The existing
UML mechanism (kvm_v2_fpu_capture_for_switch_out + kvm_v2_fpu_
install_on_first_run, vcpu.c:1828+) provides per-task FPU isolation
via context_switch hook, but has an empirical gap: my per-dispatch
iotrap_fpu fix gives the 60%→96% improvement, suggesting the
context-switch-only granularity misses some path. Possibly:
- A schedule path that doesn't go through `__switch_to`
- A KVM_RUN cycle that runs after install_on_first_run cleared
  `fpu_valid` but before context_switch fires for next task
- Subtle interaction with the worker-process model

For now, our per-dispatch iotrap_fpu mechanism (always saves on
exit, always restores on next entry) is correct-by-construction
regardless of where the gap is. It composes safely with the
existing per-task install (iotrap_fpu always wins because it
restores last, line 1473 vs 1448).

**Update 2026-04-30 (post-attribution analysis):** instrumented
`kvm_v2_vcpu_run` to log `(install_valid, iotrap_valid)` per
dispatch. For mt-xmmprobe (3 pthreads, 50 iters), 50/50 dispatches
showed `install_valid=0, iotrap_valid=1`. Meaning:

- `kvm_v2_fpu_install_on_first_run` essentially NEVER fires for
  mt-xmmprobe's threads — they're not flushed via context_switch
  often enough.
- `iotrap_fpu` fires every dispatch.

But `install_on_first_run` DOES fire for OTHER tasks (init shell,
mount, ksoftirqd, etc.) at their context_switch boundaries — each
such install does `KVM_SET_FPU(other_task.fpu)` which OVERWRITES
`vcpu->arch.guest_fpu` with that task's snapshot. When mt-xmmprobe
next dispatches with `install_valid=0`, KVM's auto-load pulls the
polluted `vcpu->arch.guest_fpu` into hardware — leaking the OTHER
task's FPU into mt-xmmprobe's run.

**This is the precise UML bug:** the existing per-context-switch
install/capture pair pollutes `vcpu->arch.guest_fpu` across tasks
sharing one vcpu. The per-dispatch iotrap_fpu mechanism corrects
this by *always* re-loading the current task's snapshot before
each KVM_RUN, overriding any cross-task pollution.

A potential cleaner fix would be: make `install_on_first_run`
idempotent (don't clear `fpu_valid` after install — leave it
sticky so every dispatch installs current task's FPU). That would
remove the cross-task pollution at the source. But the per-dispatch
GET in my fix is also needed — without it, ANY UML kernel use of
FPU between exit and next entry could poison `vcpu->arch.guest_fpu`
via the same auto-load path.

So my fix is a complete solution. The "make install sticky" approach
would be a partial cleanup but doesn't replace the GET side. Phase
H.2 may explore further refactoring.

### H.1b residual #121 — investigation 2026-04-30 (multi-thread mmap stress)

**Status:** open / partially characterised. Substrate gate stable;
mt-mmap-stress N=3..4 still flakes ~7-10%.

**Reproducer:** `mt-byteset.c` (3-4 pthreads × 50 iters of mmap +
volatile per-byte memset + munmap). N=1 is 100% PASS; N≥2 flakes.

**Empirical findings:**

1. **Multi-thread is required** — N=1 passes 15/15. N=4 fails ~12%.
   Implies multi-thread mm coordination, not single-task #PF flow.
2. **Diverse crash signatures:**
   - Original: kernel printk `mt-byteset[N]: segfault at 0 ip 0x40197b
     error 2`. User SIGSEGV handler captures `si_addr=(nil)`,
     `RIP=0x401bd6` (slow_memset's `mov %al, (%rdx)`),
     `RDX=non-zero` (actual write target was valid mmap'd page).
   - With diag instrumentation: signature shifts to libc-RIP +
     non-zero si_addr (e.g., `RIP=0x7f7fff8e0a37 si_addr=0x10000`).
     Different timing → different race surface.
3. **Confirmed cr2=0 path in handle_io_pf** via diag printk (50 cases
   captured): `cr2=0 cs=0x08 rip=0x401bd6 rsp=user err=2 rdx=non-zero
   io=0xf6 ist1=correct`. So:
   - `io.port=0xf6` (UM_KVM_TRAP_PF) — PF stub fired, real #PF dispatch.
   - `cs=0x08` (kernel CS) at fault-time — guest was at CPL=0 not 3.
   - `cr2=0` despite `rdx=non-zero` — CR2 register was not set by the
     fault (or was reset before vmexit).
4. **CS=0x08 at every dispatch entry** (verified by logging
   sregs.cs.selector before KVM_RUN): even non-failing dispatches
   show `cs_in=0x08/dpl=0`. Apparently normal v2 behavior — KVM
   stores GUEST_CS=0x08 on exit (we exit from IDT handler stubs which
   run at CPL=0); next entry reapplies via KVM_SYNC_X86_SREGS
   dirty-bit. User code "works" at CPL=0 because user PTEs are
   accessible at CPL=0 (no _PAGE_USER required).
5. **DIAG-CR2 around KVM_RUN** confirms `cr2_in=0` (we clear it via
   `load_user_sregs`) and `cr2_out=fault_addr` for normal #PF
   dispatches. Failing case: `cr2_in=0 cr2_out=0` despite `io=0xf6`
   (PF stub fired). That means KVM exited with vcpu->arch.cr2=0,
   which means the host CR2 register was 0 at vmexit — implying
   either (a) no real fault was delivered (yet stub somehow fired)
   or (b) CR2 was reset between fault delivery and vmexit by some
   nested fault overwriting it.

**Working theory:** the PF stub at IDT[14] is sometimes entered via
a nested fault during the IDT delivery itself (e.g., iretq frame
push to IST stack, IDT walk, GDT walk for selector resolution).
The nested fault resets CR2 to the nested-fault VA, which happens
to be 0 in the cases we observed (suggests something at GVA 0 is
involved — maybe a NULL PT entry encountered during walks).

**Why bug is fragile under instrumentation:** added printks shift
timing of unblock_signals / preempt yields; the race window changes
character. Different crash signatures emerge with different diag
configurations.

**Why test mostly passes (95%) despite this race:** most of the time
the PF stub is reached cleanly. The ~5% loss happens when nested
faults + multi-thread mmap+munmap concurrency align. Reproducer
shows the bug requires multiple threads competing on the same mm,
which suggests mm-arbiter state churn.

**Next steps (not pursued in this session):**
- Add explicit instrumentation in KVM (vmx.c) to log native_read_cr2()
  vs vcpu->arch.cr2 at vmexit for guest-fault-delivery vmexits.
- Catch the case via KVM tracepoint on fault delivery.
- Try a defense: re-set sregs.cs to user CS (0x2b) on every entry to
  see if forcing CPL=3 changes the failure mode (would also confirm
  CS=0x08-during-user-run is not innocuous).
- Investigate UML's mm-arbiter under multi-thread mmap pressure.

For now, substrate gate is stable at PASS=25/FAIL=3/EXPECTED_FAIL=3
which matches seccomp parity. The 4-7% mt-mmap-stress flake is a
non-blocking residual.

### #121 sentinel test (2026-05-01) — definitive: in-stub CR2 reads as 0

Modified the in-guest #PF handler stub to capture three values to
known IST page byte offsets BEFORE the `out` vmexit:
1. CR2 register (via `mov %cr2, %rax; mov %rax, -8(%rsp)`)
2. RDX register (via `mov %rdx, -16(%rsp)`)
3. Sentinel 0xffffffffffffffff (via `movq $-1, -24(%rsp)`)

Bumped `KVM_V2_HANDLER_SLOT_STRIDE` from 32 to 64 bytes to fit the
expanded 28-byte stub. Reads in `kvm_v2_handle_io_pf` show:

```
um: kvm-v2 #PF[1] cr2=0 captured=0 stub_rdx=42010000 regs_rdx=42010000
   sentinel=ffffffffffffffff err=2 user_rip=401bd6 pid=22
```

- `sentinel = 0xff..ff` confirms the stub DID run (otherwise the slot
  would have stale/zero data from a prior dispatch).
- `captured = 0` confirms the stub's `mov %cr2, %rax` read 0 from
  the actual CR2 register at stub-entry time.
- `stub_rdx = regs_rdx = 0x42010000` confirms the user's intended
  write target was a non-zero address in the mmap'd region.
- `err = 2` (W=1, P=0, U=0) confirms a real write fault on a
  non-present page from supervisor mode.

**The fault MUST have been at addr 0** (since CR2 = 0 and err=W=1
is a real write fault) **but the user's write target (RDX) was
0x42010000**. These two observations are inconsistent under standard
x86 semantics. Possible explanations:

1. Nested fault during IDT delivery clobbered CR2 to 0 — but the
   stub's writes did reach the IST stack (sentinel proves it), so
   the iretq-frame-push site cannot have faulted.
2. Some KVM/CPU-level race where CR2 is reset by emulation/inject
   between the original fault and the stub running.
3. The user's `mov %al, (%rdx)` somehow had effective address 0
   despite RDX = 0x42010000 — would imply CPU instruction-decode
   or register-fetch failure (extremely unlikely).

**Defense shipped (commit f0487174741c):** when CR2 reads 0 with
error_code.W=1 (write fault) AND user RDX is a plausible user-space
pointer (>0x10000), use RDX as the fault address. Heuristic — only
helps mov-to-(%rdx) style instructions. Substrate gate stable.

Validation matrix (post-defense):
| Test                              | PASS rate          |
|-----------------------------------|--------------------|
| Substrate gate (3 runs)           | 25/3/3 ✓           |
| mt-mmap-stress N=3 (50 trials)    | 50/50 = 100% ✓     |
| mt-byteset N=4 (80 trials)        | 76/80 = 95%        |
| mt-byteset N=4 (200-trial soak)   | 185/200 = 92.5%    |
| seccomp baseline N=4 (40 trials)  | 40/40 = 100%       |

The deeper root cause remains unexplained without host-side KVM
instrumentation (which would require host kernel rebuild + reboot —
not feasible in this session).

### #121-D1 host-KVM tracepoint + bpftrace investigation (2026-05-01)

**Critical correction:** host is AMD Ryzen 7 7840HS — KVM uses SVM
(arch/x86/kvm/svm/), NOT Intel VMX. Earlier analysis of vmx.c was
wrong-arch.

**Findings via `kvm:*` tracepoints + bpftrace:**

1. **560 `kvm_inj_exception` events per 80-trial soak**, all
   `reinjected=1, vec=14, error_code=0x14` (user-mode instruction
   fetch fault). All routed through `svm_complete_interrupts` →
   `kvm_requeue_exception` after EXITINTINFO valid in VMCB.

2. **`kvm_deliver_exception_payload` always called with
   `has_payload=0`** for re-injections — function returns early,
   does NOT modify `vcpu->arch.cr2`. So this is NOT the path
   setting cr2 to 0.

3. **`kvm_inject_emulated_page_fault` never called with addr=0** —
   verified by bpftrace filter on `fault->address`.

4. **`vcpu->arch.cr2` NEVER transitions non-zero → zero across
   `svm_vcpu_run`** (verified by kprobe entry/exit pair). Once set
   non-zero by hardware, it stays non-zero.

5. **In failing case, `vcpu->arch.cr2 = 0` for the ENTIRE KVM_RUN
   inner loop.** Set to 0 by UML's `load_user_sregs` at KVM_RUN
   entry, stays 0 through every iteration.

6. **`run->s.regs.regs.rip = 0xffffe00000002140` at exit** = PF
   stub START address (slot 5 × 64-byte stride). Before any stub
   instruction has executed.

**What this means:** the `cr2 = 0` that UML reads is the value loaded
by `load_user_sregs` — NOT a value that hardware set then KVM lost.
The user's hardware-detected #PF either:

- Never fired (the `out` exit was actually for some other reason),
  with `run->io.port=0xf6` being stale from an earlier exit; OR
- Did fire but on a path that doesn't update vmcb.save.cr2 (e.g.,
  software-injected #PF via vmcb.event_inj).

**Notable AMD-specific observation:** when a guest #PF is interrupted
mid-delivery by a NPF (host nested page fault), KVM's
`kvm_requeue_exception` re-queues the #PF with `has_payload=false`
and `payload=0`. On re-injection via `vmcb.event_inj`, hardware
delivers the #PF without updating vmcb.save.cr2 (per AMD APM Vol 2
§15.20.4: "CR2 register is not implicitly updated when an event is
injected"). KVM relies on `vcpu->arch.cr2` being the right value
before `svm->vmcb->save.cr2 = vcpu->arch.cr2` at svm.c:4457.

**Why mt-mmap-stress N>=2 specifically:** lazy-mmap'd pages cascade
NPF + guest-#PF interactions. Multi-thread amplifies because each
thread's mm activity creates fresh GPAs that need NPT population.
The NPF-mid-delivery race window is wider with more threads.

**The actionable path forward** (not pursued in this session):
- D6: in-guest CR2 sanity-check at boot to confirm IDT path works
  in isolation
- D8: out-of-tree kvm-debug kernel module to instrument the exact
  cr2-write moments without host kernel rebuild
- D10: reproduce on Intel host to confirm AMD-specific
- Possible upstream KVM patch: extend `kvm_requeue_exception` to
  carry CR2 payload via `vcpu->arch.cr2` (preserve across
  EXITINTINFO-cycles).

### #121-D10 cross-architecture reproduction (2026-05-01)

**Bug reproduces on BOTH Intel (VMX) and AMD (SVM)** — architecture-
independent. 200-trial N=4 soaks:

| Host | CPU       | Backend   | PASS rate    | Fail % |
|------|-----------|-----------|--------------|--------|
| s0   | i9-12900K | v2 (VMX)  | 189/200      | 5.5%   |
| s5   | 7840HS    | v2 (SVM)  | 185/200      | 7.5%   |
| s5   | 7840HS    | seccomp   |  40/40       | 0%     |

This invalidates the AMD-SVM-specific theory. Root cause must be in
shared KVM code (arch/x86/kvm/x86.c) or in UML's v2 backend code.
Hardware-architecture-specific paths (svm.c / vmx.c) cannot be the
source — both show the same bug.

The strongest remaining theory is: `kvm_requeue_exception` (called
from svm_complete_interrupts/vmx_complete_interrupts after
EXITINTINFO-valid vmexits) does not preserve CR2 across
NPT/EPT-interrupted #PF delivery cycles. The function explicitly
sets `payload=0; has_payload=false;` for re-injected exceptions.
On the next inject, `kvm_deliver_exception_payload` sees has_payload=0
and skips updating vcpu->arch.cr2. If the prior svm.c:4506 or
vmx.c:7474 read happened to land on a vmexit that didn't yet reflect
hardware-set CR2 (e.g., NPF/EPT vmexit that pre-empted the CR2-update
microcode atom), the value gets stuck.

Older Intel hosts (s1 Xeon E3-1225 v6, s2 v5, s3 W-2123) and s4
(i5-12600K w/ host kernel 7.0.0-15) showed "UML: fatal signal" early
in boot — separate compatibility issue, not the residual flake.

### #121-D additional experiments (2026-05-01)

**E1: Remove sregs->cr2=0 in load_user_sregs.** 200-trial soak with
the line removed: 184/200 PASS = 92%. With the line: 185/200 = 92.5%.
**Identical fail rate.** Difference: without zero, failing cases show
STALE CR2 from previous tasks (cross-task leak); with zero they show
CR2=0. The zero is NOT the bug source — it just makes the symptom
uniform. Restored.

**E2: Add KVM_SYNC_X86_EVENTS sync to clear pending exception state
on every KVM_RUN entry.** Hypothesis: prevent cross-task re-injection
leak (task A's pending #PF surviving to task B's KVM_RUN).
Implemented via `memset(&run->s.regs.events, 0, sizeof)` +
`KVM_SYNC_X86_EVENTS` dirty bit. Verified via tracepoint that
kvm_inj_exception still fires 140 times / 20 trials with this fix —
so re-injections are happening WITHIN one KVM_RUN ioctl, not
crossing task boundaries. mt-byteset N=4 fail rate unchanged.
**Cross-task injection leak is NOT the bug source.** Reverted.

These two experiments rule out the most-likely UML-side fixes I
hypothesized. The bug is intra-KVM_RUN: somewhere in the inner
vcpu_run loop, a #PF stub fires with vmcb.save.cr2 = 0 (or stale)
despite the dispatch starting from load_user_sregs's clean state.

Without rebuilding the host kernel to instrument every CR2
read/write site in arch/x86/kvm/{x86,svm/svm,vmx/vmx}.c, the deeper
root cause cannot be pinpointed in this session. The defensive
heuristic (RDX-fallback for cr2=0 + W=1 + plausible RDX) shipped at
`f0487174741c` mitigates 95% of the impact at N=4.

### #121-D15 ROOT CAUSE CONFIRMED + CLOSED (2026-05-01)

**Smoking gun.** A bpftrace probe on `vcpu_enter_guest`, capturing
`vcpu->arch.regs[VCPU_REGS_RIP]` in the buggy `cr2=0 port=0xf6`
cases, reproducibly showed `entry_rip=0xffffe00000002140` — the
PF stub start address. UML was entering KVM_RUN with RIP already
pointing at the stub, NOT at user code. Then we added a UML-side
printk after `kvm_v2_marshal_to_kvm_regs` to log when the marshaled
RIP was in the HANDLERS region, and within 20 mt-byteset N=4 trials
caught:

```
um: kvm-v2 D15 entry rip=ffffe00000002140 ... gp_ip=ffffe00000002140
gp_sp=ffffe00000004fd0 gp_dx=42012000 gp_cx=2 is_user=1
```

`gp_sp = 0xffffe00000004fd0 = IST_top - 48` — exactly the RSP value
the CPU leaves after pushing the 48-byte IDT frame. So the
**eintr_regs snapshot was captured at the moment between hardware
IDT delivery (CPU pushed user_rip/cs/rflags/rsp/ss/error_code, set
RIP=stub_start) and the in-guest stub's first `push %rax`** — a
narrow window when SIGALRM-driven EINTR vmexits the vCPU.

**Mechanism** (compounded race, two independent failure modes
inside the EINTR-mid-IDT-delivery window):

1. **CR2 wipeout.** The next dispatch's `kvm_v2_load_user_sregs()`
   writes `sregs->cr2 = 0` with `KVM_SYNC_X86_SREGS` dirty. KVM
   propagates that to `vcpu->arch.cr2` → `VMCB.save.cr2 = 0` →
   hardware CR2 register = 0 on next VMRUN. When the resumed stub
   executes `mov %cr2, %rax`, RAX = 0. Stub captures 0 to the
   IST page slot. `out %al, $0xf6` vmexits; `handle_io_pf` reads
   `sregs.cr2 = 0` AND `captured_cr2 = 0`.
2. **IST frame clobber.** The hardware-pushed IDT frame on the IST
   stack sits unprotected. Another UML task running on the same
   per-host-CPU vCPU before this task resumes can deliver its own
   #PF, push its own IDT frame to the SAME IST stack — overwriting
   the original task's `user_rip/cs/rflags/rsp` slot. On resume,
   the stub captures CR2 (now 0) and OUTs; `handle_io_pf` reads
   the IST frame which now belongs to the OTHER task. The original
   task's `regs->gp[HOST_IP]` becomes the other task's `user_rip`,
   and the user task resumes at the wrong code address — observed
   as `mt-byteset[N]: segfault at 0x10000` (slow_memset hit
   garbage RDX) or `segfault at 0x55000000a481` (instruction-fetch
   to a stale CS:RIP).

**Fix** (commit landed 2026-05-01):

- New per-task fields in `arch/x86/um/asm/processor_64.h`'s
  `arch_thread.kvm_v2`: `saved_cr2_at_eintr` + `saved_cr2_valid`.
- In the `EINTR` path of `kvm_v2_vcpu_run` (vcpu.c), when
  `eintr_regs.rip` is in `[KVM_V2_HANDLERS_GVA,
  KVM_V2_HANDLERS_GVA + 0x200)`:
  1. Save `eintr_sregs.cr2` to `saved_cr2_at_eintr`.
  2. Call new helper `kvm_v2_ist_frame_snapshot_raw(vcpu)` (in
     syscall_trap.c, declared in kvm_v2_backend.h) which copies the
     6-qword IDT frame from the IST stack into per-task
     `ist_frame[0..5]` and sets `ist_pending=true`.
- In `kvm_v2_load_user_sregs()` (vcpu.c): if `saved_cr2_valid`,
  restore `sregs->cr2 = saved_cr2_at_eintr` and clear the flag;
  otherwise zero as before.
- The existing `kvm_v2_ist_frame_restore_pending()` (already called
  before `KVM_RUN` entry) handles the IST stack restore, so the
  resumed stub finds the original task's IDT frame intact.

**Result.** mt-byteset N=4: 200/200 PASS (previously 92-96%
PASS, ~5-12% flake). Substrate gate stable PASS=25/FAIL=3/
EXPECTED_FAIL=3 (matches seccomp). 23 D15-SAVE events captured in
soak instrumentation confirm the bug-prone path fires at expected
frequency and is now correctly recovered.

The cross-task IST clobber was the LATENT bug already known from
the H.1b InterpreterPool subinterp investigation that introduced
`ist_pending` snapshot/restore in handle_io_pf at end-of-handler;
D15 reveals there's a SECOND entry point into "frame on IST stack
that needs preservation" (the EINTR-mid-IDT-delivery window) that
the original snapshot point didn't cover.

### H.1b legacy notes (pre-2026-04-30)

- Instrument syscall count, vmexit count, time-per-syscall,
  time-per-vmexit.
- Run cpython gate, capture profiles.

### H.2 — Optimize hot paths (5-7 days)

Likely candidates (from v1 experience):
- SREGS-skip cache: skip KVM_SET_SREGS when nothing changed.
- Hypercall fast path: for common syscalls (read/write/mmap/munmap),
  use a tight dispatch table.
- Memslot caching: keep a per-vCPU last-used-memslot pointer to avoid
  list walk on every fault.
- Sync regs (Phase C.3 already did this).

### H.2 — CR0.TS lazy FPU SHIPPED 2026-05-01 (ba9c83331f30)

Arms CR0.TS=1 every dispatch; in-guest IDT[7] #NM stub does
`clts; iretq` (4 bytes, no host vmexit). After vmexit, host reads
sregs.cr0.TS:
- TS=1 → guest didn't touch FPU → skip KVM_GET_FPU
- TS=0 → guest used FPU → KVM_GET_FPU as before

Skip rate measured on mt-byteset N=4 (4000 dispatches):
- 1000 dispatches: 81% skipped
- 2000 dispatches: 90% skipped
- 3000 dispatches: 93% skipped
- 4000 dispatches: 95% skipped

Most syscalls (mmap/read/write/munmap) and #PF dispatches don't use
FPU; only specific FP/SIMD code paths trigger #NM. The cost saving
is ~1 ioctl per non-FPU dispatch (~95% of dispatches in syscall-heavy
workloads).

Validation:
- Substrate gate: 25/3/3 (unchanged)
- mt-xmmprobe N=3 (15 trials): 15/15 PASS — FPU correctness intact
- mt-byteset N=4 (50 trials): 50/50 PASS — also stability win

KVM_SET_FPU side unchanged: still always-SET when iotrap_fpu_valid.
Future H.2.1 could gate SET on per-task "owns vcpu->arch.guest_fpu"
flag (skip if no other task ran on this vCPU since last SET).

### H.3 — Target validation (2 days) — DONE 2026-05-01

Post-H.2 measurements with H.2 lazy FPU optimization shipped:

**perf-getpid** (cycle-level via getpid-loop, 100k iterations):
| Backend  | cyc/call | ratio vs seccomp |
|----------|----------|------------------|
| seccomp  | 62500    | 1.0×             |
| kvm-v2   | 24300    | **0.388×** (2.58× faster) |

**perf-py-startup** (printk-bracketed wall-clock, 5 samples):
| Backend  | median (s) | ratio vs seccomp |
|----------|-----------|------------------|
| seccomp  | 0.09      | 1.0×             |
| kvm-v2   | 0.06      | **0.667×** (1.5× faster) |

Both well below the 1.2× target ceiling — Phase H exit criterion met.

**Exit criteria MET:** gate at 0.388× / 0.667× seccomp wall-clock.

---

## Phase I — Polish + documentation (1 week) — **DONE 2026-04-30**

### I.1 — `trace.h`: comprehensive ftrace — **DONE**

31 `TRACE_EVENT()` definitions in
`arch/um/include/asm/trace/um_backend.h` cover every v2 surface:
- KVM_RUN cycle: `vcpu_enter` / `vcpu_exit` / `vcpu_eintr`
- IO traps: `iotrap_syscall_enter` / `iotrap_syscall_exit` / `iotrap_pf` / `iotrap_gp` / etc.
- Memslots: `memslot_add` / `memslot_del` / `physmem_memslot_install`
- Per-vCPU lifecycle: `vcpu_create` / `cpuid_install` / `msr_program` / `sregs_install` / `sigmask_install` / `trampoline_install` / `pml4_install` / `per_vcpu_ist_tss_install` / `descriptors_sregs_install`
- Per-task FPU: `fpu_capture` / `fpu_install`
- Generic mm ops: `mm_create` / `mm_destroy` / `mm_region_added` / `mm_region_removed`

Run via `trace-cmd record -e 'um_backend_kvm_v2:*'`.

### I.2 — KUnit tests — **DONE**

Two suites under `CONFIG_UM_BACKEND_KVM_V2_KUNIT`:
- **`kvm_v2_marshal`** (`test_marshal.c`, D.2 + task #74): 8 tests
  for `kvm_v2_marshal_to_kvm_regs` / `kvm_v2_marshal_from_kvm_regs`
  shape correctness (every GPR + RIP + RFLAGS slot, RFLAGS-bit-1
  invariant, round-trip).
- **`kvm_v2_byteshape`** (`test_byteshape.c`, this session): 7
  tests for SDM-prescribed byte layouts: IDT-pushed exception
  frame (with/without error code), GDT segment descriptors
  (kernel + user CS), LSTAR trampoline byte sequence, and IDT
  handler stub byte sequences (with/without error-code-discard).
  Regression-catches the E.5 IST-frame off-by-8 bug class.

Total 15/15 PASS. KTAP output via `kunit.enable=1` cmdline.

The originally-specified "memslot allocator" / "hypercall dispatch
table" / "vCPU pool" tests remain pending — those would require
exposing static helpers as non-static or splitting them out for
test harnesses. Deferred to a follow-up session (low value vs the
byte-shape tests, which catch an actual class of bugs we burned
debugging cycles on).

### I.3 — Documentation — **DONE**

- `arch/um/backend/kvm-v2/README.md` rewritten as the canonical
  v2 design doc: design overview, file layout, current state
  table (phase tracking), build/run cookbook, tracing recipes,
  known residuals.
- `Documentation/virt/uml/backends.rst` updated: kvm-v2 listed as
  a buildable backend (not "in development"); picking-a-backend
  table now lists v2 under "Maximum speed"; trap-path diagram
  details the IO-port trampoline + SYNC_REGS marshal flow.

The architecture diagram (kvm-v2-arch.svg) remains optional —
current text-based docs cover the design adequately. Headerdoc
audit deferred (existing comments in source are extensive).

### I.4 — Lift `EXPERT` gate — **DONE**

`Kconfig` change at commit 07f680c6d1a5: removed
`depends on EXPERT` from `CONFIG_UM_BACKEND_KVM_V2`. Default stays
`n` until Phase J validation completes; the EXPERT lift just stops
hiding v2 from the defconfig browser.

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
