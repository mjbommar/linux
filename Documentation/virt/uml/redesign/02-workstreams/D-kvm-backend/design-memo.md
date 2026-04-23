# D-workstream design memo: naive `um_backend_kvm`

**Status:** draft (2026-04-23)
**Companion to:** `01-kvm-platform-design.md` (status + spike
result summary), `measurements.md` (timing data), spike 01/02
(the empirical floor this memo is designed around).

This is the implementation-shaping memo for the first-phase
KVM backend. Scope: replace the ptrace/seccomp trap mechanism
with one KVM vCPU per UML CPU, servicing guest syscalls via
`VMEXIT → handle → VMRESUME` in userspace. Per D56, the
systrap gadget layer (D-04) is a second-phase deliverable and
is **not** designed into the first-phase backend. This memo
covers the shape that ships ~4 µs/call on Skylake and ~800
ns/call on Alder Lake i9 without tricks.

The emphasis here is **"what shape does the code want?"** not
"how do we hit the vision." The vision is a function of what
hardware the user runs on, and the spike results already told
us what's achievable. Our job is a clean backend that doesn't
preclude the D-04 gadget layer when we get to it.

## Summary for busy readers

1. One `struct kvm_um` per UML process. One `kvm_vcpu_fd`
   per UML CPU. All vCPUs share the same `kvm_vm_fd`.
2. Guest physical memory is the UML kernel's own address
   space, registered whole-hog into KVM via
   `KVM_SET_USER_MEMORY_REGION`. No copy; the guest runs
   the UML kernel image in-place.
3. Guest runs in **long mode, ring 0**, with `LSTAR` set to
   a tiny "bounce" trampoline that issues a known-magic
   instruction (e.g. `vmcall`) to force VMEXIT. That VMEXIT
   is where `um_backend_kvm_syscall_entry()` runs, looks at
   the vCPU registers, dispatches the syscall through
   UML's normal `sys_*()` path, restores the register
   frame, and resumes.
4. Guest userspace (UML's notion — programs running under
   UML) is ring 3 in KVM's sense. Page tables are UML's
   existing kernel/user split, handed to KVM via
   `KVM_SET_SREGS.cr3`. No new page-table layer.
5. Interrupts, timers, MMIO are emulated on VMEXITs that
   KVM routes to `KVM_EXIT_{INTR, MMIO, IO}` plus the
   exit-event handler we already have for vhost-user
   backend shutdown — same machinery, different caller.
6. The existing `struct um_backend_ops` table gets a third
   entry `kvm_ops`. ptrace and seccomp backends stay
   unchanged; dispatch at runtime via the A-01 table.
7. All of the above ship without a systrap gadget. The
   gadget is a ring-0 guest-side intercept of `SYSCALL`
   that avoids the VMEXIT for common fast paths and lands
   as D-04.

## Terminology

Two overloaded words. Fix them here.

- **"vCPU"** = KVM virtual CPU. One per UML CPU. Not the
  Linux `cpu_struct` / `cpu_present_mask` notion; that's
  handled separately by UML's SMP code.
- **"guest"** = what's running inside the KVM VMCS. For UML
  on KVM, "guest kernel" = the UML kernel binary running in
  ring 0 inside KVM; "guest userspace" = UML programs
  running in ring 3 inside KVM.
- **"host"** = the outer Linux host UML itself runs on.
  Unchanged by this memo.
- **"syscall"** = always the guest-side syscall (UML
  program → UML kernel), unless explicitly "host syscall".

## Shape: gVisor → UML mapping

gVisor's `pkg/sentry/platform/kvm/` is the clearest prior
art. We borrow the shape; we deviate where gVisor had to
solve problems that don't exist for UML.

### What we borrow unchanged

| gVisor | UML equivalent | Why it carries over |
|---|---|---|
| `Machine` struct (kvm_fd + vm_fd + memslots) | `struct kvm_um` | Per-process KVM context; straightforward. |
| `vCPU` per goroutine | `kvm_vcpu_fd` per UML CPU | SMP mapping is 1:1 with UML CPUs. |
| `addressSpace` (per-thread mm with memslot registrations) | UML `mm_struct` + a single memslot covering the UML process's whole VA | Same shape, simpler because no threading. |
| VMEXIT reason dispatch table | `switch (kvm_run->exit_reason)` in C | Same idea; C instead of Go. |
| `host.Run` via goroutine pool | Per-vCPU kernel thread (already exists in UML SMP) | UML already runs one kernel thread per UML CPU. We attach the KVM vCPU to that thread. |

### What we drop

| gVisor mechanism | Why UML doesn't need it |
|---|---|
| Sentry (Go userland kernel) | UML *is* Linux. The guest runs `vmlinux`. |
| Userspace-managed page tables | UML's kernel already owns kernel + user page tables. KVM inherits them via `CR3`. |
| `bounce` goroutine for preemption | UML SMP uses real kernel preemption + signals; no per-call goroutine scheduling. |
| `archVirtual` address-space tricks | We identity-map the host `mm` into the guest. |

### What's different but needed

| gVisor pattern | UML variant | Notes |
|---|---|---|
| `SYSENTER` on 32-bit | `SYSCALL` (x86_64 only for v1) | No 32-bit UML in v1 scope. |
| Sentry-managed TLS / FS-base | UML-managed via `WRMSR(MSR_FS_BASE)` on guest-entry | UML already sets this for the signal-based backend; reuse. |
| Go finalizer-driven cleanup | Standard Linux kref/refcount on `kvm_um` | Reference-counted via `kvm_um_put()`. |

## Concrete shape

### Data structures

```
struct kvm_um {
    struct kref             ref;
    int                     kvm_fd;     /* /dev/kvm */
    int                     vm_fd;      /* KVM_CREATE_VM */
    struct mutex            lock;

    /* guest physical memory = UML process's own address space. */
    void                    *mem_base;
    size_t                  mem_size;
    struct kvm_userspace_memory_region memslot;

    /* per-vCPU state. */
    struct kvm_um_vcpu      *vcpus;
    int                     nr_vcpus;

    /* exit-path event counters (tracing hooks). */
    atomic64_t              stat_syscall_exits;
    atomic64_t              stat_mmio_exits;
    atomic64_t              stat_intr_exits;
};

struct kvm_um_vcpu {
    struct kvm_um           *km;
    int                     cpu_id;     /* UML CPU id, 0-based */
    int                     vcpu_fd;
    struct kvm_run          *run;       /* mmap'd shared state */
    size_t                  run_size;

    /* current vCPU dirty state — consumed by syscall entry. */
    struct pt_regs          cached_regs;
    bool                    regs_dirty;
};
```

### Entry points (maps to `struct um_backend_ops`)

Existing ops table (from A-01) has entries for ptrace +
seccomp backends. KVM adds a third:

```
static const struct um_backend_ops kvm_ops = {
    .name           = "kvm",
    .init           = kvm_backend_init,
    .fini           = kvm_backend_fini,
    .cpu_create     = kvm_backend_cpu_create,
    .cpu_run        = kvm_backend_cpu_run,
    .cpu_halt       = kvm_backend_cpu_halt,
    .syscall_hook   = kvm_backend_syscall_hook,
    /* A-01's other hooks: mmap_notify, signal_deliver, etc. */
};
```

Key lifecycle:

- **init:** open `/dev/kvm`, create VM, configure memslots.
- **cpu_create:** per UML CPU, `KVM_CREATE_VCPU`, mmap
  `kvm_run`, set up long mode + paging state from UML's
  current `mm`, prime LSTAR to the bounce trampoline
  (see below).
- **cpu_run:** the vCPU thread's main loop.
  `while (!need_halt) { KVM_RUN; dispatch_exit(run); }`.
- **syscall_hook:** called from `dispatch_exit` when the
  guest's `SYSCALL` triggered a VMEXIT (via the bounce).
  Pulls regs → builds a `pt_regs` → invokes
  `sys_call_table[nr]` → writes result back into the vCPU
  regs → returns (main loop VMRESUMEs).

### The bounce trampoline

The single cleverness needed for v1. LSTAR is set to a 1-page
hard-coded trampoline in guest memory that contains exactly:

```
bounce:
    vmcall                # VMEXIT; exit_reason=KVM_EXIT_HYPERCALL
    sysretq               # unreachable on VMCALL path
```

The `vmcall` instruction triggers `KVM_EXIT_HYPERCALL` which
we route to `kvm_backend_syscall_hook`. On return,
`KVM_RUN` resumes, the trampoline's `sysretq` restores
`%rcx → %rip` (the guest userspace return address saved by
the `SYSCALL` instruction itself) and jumps back.

Why `vmcall` and not just `ud2` + `KVM_EXIT_EXCEPTION`:
`vmcall` is the documented userspace/hypervisor escape
hatch. `ud2` works but routes through the exception
delivery path, which on some CPUs is slower. Measure on
the spike follow-on.

One 4 KiB trampoline page per UML process, write-protected
after init. No per-vCPU copies; vcpus share it.

### Address-space model

Guest physical memory is the UML process's own
`mem_base..mem_base + mem_size`. One `KVM_USER_MEMORY_REGION`
slot covers it. Guest virtual addresses are UML virtual
addresses; `CR3` is whatever UML's current `mm->pgd` points
to, translated to a guest-physical offset via the same
arithmetic UML already uses.

**Implication:** no page-table duplication, no shadow paging,
no EPT/NPT management from userspace. KVM's hardware-assisted
paging (EPT on Intel, NPT on AMD) walks UML's page tables
directly. This is what makes the naive backend simple: UML
already owns its page tables, KVM just points at them.

**Shared page-table concern:** if UML's kernel writes a page
table entry between vCPU runs, the vCPU sees the new value
on the next `KVM_RUN`. No TLB management needed beyond what
UML already does (it invalidates TLB on its own via the
seccomp/ptrace path today).

**Failure mode worth calling out:** if UML ever starts
using `MADV_DONTFORK` on page-table pages (e.g. under
snapshot/forkserver), KVM memslots need re-registering. The
C-09 snapshot code already tracks these; hook into
`um_register_mmap_region()` to keep memslots in sync.

### Signal / interrupt delivery

Two paths:

1. **Host → guest signals.** UML's kernel wants to deliver
   a signal to a guest userspace thread (e.g. timer tick,
   user-space SIGSEGV). Under seccomp, UML writes the
   signal into the guest thread's frame and resumes. Under
   KVM, we do the same: interrupt the vCPU run loop via a
   host-side signal (`SIGUSR1` on a per-vCPU pipe-fd the
   runloop polls), recover the vCPU regs with `KVM_GET_REGS`,
   inject the guest-visible signal into the guest pt_regs,
   resume.
2. **Guest asynchronous exits.** Timer interrupts from the
   host clock arrive as real signals to the vCPU thread
   (same mechanism UML uses today). KVM makes them visible
   as `KVM_EXIT_INTR`; we already have a handler for this
   pattern.

Neither path is speculative — both are how seccomp backend
delivers signals today. The KVM backend reuses the same
entry points with different plumbing.

### Syscall trace + observability

The three counters in `struct kvm_um` map to debugfs:

```
/sys/kernel/debug/um/kvm/syscall_exits
/sys/kernel/debug/um/kvm/mmio_exits
/sys/kernel/debug/um/kvm/intr_exits
```

Not on by default in prod-fast; gated on `CONFIG_DEBUG_FS`
as the rest of UML's debugfs already is.

Per-syscall tracing: `syscall_hook` is tracepoint-friendly.
Expose `kvm:syscall_enter` and `kvm:syscall_exit` so
`bpftrace` can count + histogram VMEXITs by syscall nr
without a rebuild.

## Sequence: guest syscall

```
time     guest (vCPU)                        host (runloop)
----     -------------------------------     -------------------------
t0       user: syscall (movl $NR, %eax)
t1       CPU: ring3 → ring0, RIP = LSTAR
t2       trampoline: vmcall
t3       CPU: VMEXIT, reason=HYPERCALL
t4                                           KVM_RUN returns
t5                                           dispatch_exit()
t6                                           kvm_backend_syscall_hook()
t7                                             KVM_GET_REGS
t8                                             sys_call_table[rax](...)
t9                                             KVM_SET_REGS (write %rax)
t10                                          KVM_RUN
t11      trampoline: sysretq
t12      CPU: ring0 → ring3, RIP = %rcx
t13      user: next instruction
```

t0..t13 is one round-trip. From the spike, that's ~800 ns
on Alder Lake i9, ~5 µs on Skylake.

Open question: steps t7 + t9 are `ioctl(KVM_GET_REGS)` and
`ioctl(KVM_SET_REGS)` — each is a host syscall. gVisor
avoids both by reading/writing the vCPU state directly from
the mmap'd `kvm_run` page. We should do the same in v1;
dropping 2 ioctls per guest syscall saves ~200 ns on modern
silicon per spike-extrapolation.

## Sequence: guest page fault

```
time     guest                               host
----     -------------------------------     -------------------------
t0       user: ld from unmapped VA
t1       CPU: page fault → kernel entry
t2       guest kernel: walk page tables
t3       find no mapping, demand page
t4       guest kernel: vmcall into host-provided "page_in"
         helper, OR fall through to normal UML page fault
         handler (which on seccomp calls mmap via a host
         syscall)
t5                                           HYPERCALL exit
t6                                           page_in_hook()
t7                                             mmap into UML mem region
t8                                             KVM_SET_USER_MEMORY_REGION
                                                (if region grew)
t9                                           resume
```

The interesting question: should guest page faults exit
normally (the full UML page-fault handler runs in-guest, it
sees the fault via CR2, it calls UML's own allocator, which
then calls `mmap` via vmcall-as-syscall on t4) — OR should
they short-circuit directly into a host-side `page_in_hook`?

**Recommendation: use the normal path.** The whole point of
UML is that the Linux kernel runs as a normal program. If
we start special-casing page faults, we're reinventing
gVisor's Sentry. Take the 1 extra VMEXIT; the infrequency
of guest page faults (after warmup) makes this cheap in
aggregate.

## Commit plan for D-02..D-06

Bisectable; each compiles + boots the seccomp backend
unchanged.

**D-02: KVM backend skeleton.** `arch/um/kernel/kvm/`
directory, Kconfig `UM_BACKEND_KVM` gated on X86_64, the
`kvm_ops` stub returning `-ENOSYS` from every op except
`init` which just opens `/dev/kvm` and returns. Register
in `um_backends[]` from A-01. No runtime effect unless
`CONFIG_UM_BACKEND_KVM=y` + `uml ... backend=kvm` cmdline.
Target: ~300 LOC.

**D-03: page-table + memslot plumbing.** Implement the
guest memory region setup + `CR3` programming. Enough to
boot a kernel-only guest that never returns from the
initial jump. Verified via `KVM_EXIT_HLT` on a carefully-
placed HLT instruction in UML's init path. Target:
~400 LOC.

**D-04 (renamed: core entry/exit loop, not systrap):** the
`cpu_run` main loop + `dispatch_exit` + the bounce
trampoline + syscall_hook. Boot reaches `init_task`. Target:
~600 LOC + the bounce trampoline as assembled bytes
(~30 bytes total).

**D-05: signal delivery + interrupts.** Hook the existing
signal paths. Guest userspace reachable; basic shell boots.
Target: ~300 LOC.

**D-06: conformance.** Reuses the existing A-05 conformance
test suite (20 KUnit tests) run under `backend=kvm`. Perf
measurement via `uml-perf-compare.sh` using the existing
baselines. Target: 0 kernel LOC; selftest + docs + Status
flip.

Total: ~1600 LOC kernel-side + ~50 bytes of x86 assembly
for the trampoline. 4-6 weeks for one focused engineer.

## Failure modes + mitigations

1. **VMEXIT cost on syzbot's fleet** (Skylake-era) is 5 µs,
   not 800 ns. syzbot cares about `iter/s`; at 5 µs per
   syscall, a syscall-heavy workload (which is most of
   fuzz) runs at ~200k syscalls/sec per CPU. That's **2×
   QEMU-KVM's ~100k on the same hardware**, not 10×. Still
   a win, but less dramatic than the vision-headline reads.
   **Mitigation:** measure early. If the perf win is < 2×
   on a representative workload, revisit the systrap
   gadget priority.

2. **Nested KVM on cloud / CI hosts.** GHA runners, AWS
   EC2 t2/t3, GCP e2 — all KVM guests, most with nested
   virt off. Our D-01 spikes 02/03 will quantify. If
   nested doesn't work on target CI, we need a dedicated
   bare-metal runner pool for D-gating.
   **Mitigation:** detect at runtime, fall back to seccomp
   backend with a warning. Same Kconfig, runtime
   dispatch.

3. **CR3 tearing under concurrent vCPU updates.** If UML's
   scheduler switches mms on one CPU while another CPU's
   vCPU is mid-`KVM_RUN`, the second vCPU's CR3 might
   point into a torn page table.
   **Mitigation:** the vCPU run loop is a kernel thread
   with preemption disabled across `KVM_RUN`. UML's
   existing SMP invariants already assume this pattern.

4. **KVM API drift.** `kvm_run`'s shape is stable but has
   grown fields across kernel versions. Building against a
   6.x host + running on a 5.x host could break.
   **Mitigation:** `KVM_GET_API_VERSION` check at init;
   refuse to enable if < our minimum (6.1 LTS is fine).

5. **`KVM_EXIT_INTERNAL_ERROR`.** The silent killer. KVM
   has conditions where it can't proceed (e.g. illegal
   combo of guest state after our `KVM_SET_SREGS`) and
   returns INTERNAL_ERROR. No useful debug info.
   **Mitigation:** panic loudly with the full vCPU state
   dumped to dmesg; treat as a D-backend bug, not a
   guest-kernel bug.

## Not in first-phase scope (explicit deferrals)

Each deferred to protect the commit-plan's size:

- **Systrap-equivalent ring-0 gadget (D-04 successor).**
  The ~100 ns path. Second phase, measured on modern
  silicon first.
- **Live migration.** QEMU does this; we don't need to.
- **SMP vCPU hot-plug.** UML's SMP is `ncpus=` at boot
  only; no runtime hotplug plumbing.
- **Nested KVM support.** The backend assumes bare metal
  or a hypervisor that exposes VMX/SVM through nested.
  Explicit probe at init.
- **KVM_FEATURE paravirt extensions** (kvmclock,
  steal-time, IPIs). Use host TSC for timekeeping via
  UML's existing clocksource. Revisit if timer accuracy
  matters for a real workload.
- **Guest SMP over a single KVM VM with > max_vcpus.**
  KVM caps vCPUs per VM; UML's SMP is capped by that.
  Acceptable: UML has never advertised ≥ 256 CPUs.

## Review checklist

Before this memo moves from draft → final:

- [ ] Anton Ivanov / UML maintainer read-through (LKML
      shape + upstream plausibility).
- [ ] gVisor KVM-platform reader confirms we haven't
      misread the Sentry model we're borrowing from.
- [ ] A-01 maintainer (whoever currently owns
      `struct um_backend_ops`) signs off on kvm_ops fit.
- [ ] KMSAN / KASAN / KCSAN interaction covered — the
      sanitizers run in-guest via normal kernel builds;
      no new instrumentation points the KVM backend
      creates that we haven't already covered in the
      C-02..C-07 work.
- [ ] One review pass from a KVM-internals person (kvm
      maintainer mailing list is the right venue).

## Cross-references

- `01-kvm-platform-design.md` — Status + spike-result
  summary; this memo's parent.
- `02-msr-lstar-trap.md` — Per-file design detail for
  the LSTAR/vmcall trampoline.
- `03-page-table-mgmt.md` — Per-file design detail for
  the memslot + CR3 plumbing.
- `04-ring-transition.md` — What becomes the systrap
  gadget in phase 2.
- `05-nested-virt-fallback.md` — Runtime probe +
  seccomp-fallback story.
- `06-conformance.md` — Tests shared with A-05.
- `measurements.md` — The timing-log this memo's
  performance claims rest on.
- `spikes/01-getpid-roundtrip/` — The empirical spike.
- gVisor `pkg/sentry/platform/kvm/` — prior art.
- KVM API docs: `Documentation/virt/kvm/api.rst`.
