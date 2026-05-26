# D-KVM memo 10 — syscall + IRQ classification for `run_userspace`

**Status:** DESIGN + INVENTORY. Companion data file:
`syscall-inventory.tsv` in this directory (generated from
`arch/x86/entry/syscalls/syscall_64.tbl`; re-run the awk
recipe at the bottom of this memo to refresh).

**Precedes:** sub-commit #5c of memo 08 (`arch_prctl` +
`MSR_FS_BASE`/`MSR_GS_BASE` round-trip) and the D-06
getpid() perf bookend (task #192), both of which consume
this classification.

**Motivates:** the ongoing question — "do we need to
special-case 350 syscalls + 32 IDT vectors, or is the real
working set small?" Answer below.

## Why this memo exists

The KVM backend's `kvm_run_userspace` catches every
`SYSCALL` from ring-3 via the LSTAR trampoline, decodes
the syscall number + SysV-ABI args in the host, and
dispatches. Current default dispatch is a single line:
`sys_call_table[nr](args)`. That works for the vast
majority of syscalls because UML's kernel logic is already
the canonical implementation.

Some syscalls mutate state that lives **in the vCPU, not
in `task_struct`** — notably `arch_prctl(ARCH_SET_FS)`,
which seccomp-backend UML handles by writing the host
task's FSGSBASE MSR, but which under KVM has to be pushed
into the vCPU via `KVM_SET_MSRS`. Without this propagation
the guest's next FS-relative load traps at a static
guest VA — the current `/bin/true` block in
`kvm_run_userspace` (see sub-commit #6 / task #192).

Enumerating those "needs CPU-state propagation" syscalls
*systematically* — not ad hoc per boot failure — is the
work this memo captures.

## The five classes

Each syscall goes in exactly one of five boxes:

| Class | Post-dispatch delta | Count on x86_64 |
|---|---|---|
| **A — passthrough** | none; default dispatch of `sys_call_table[nr](args)` | 363 of 385 |
| **B — vCPU-state propagate** | dispatch, then push an MSR/SREG delta to the vCPU via `KVM_SET_MSRS` / `KVM_SET_SREGS` | 1 (arch_prctl only, post-F10) |
| **C — signal-frame** | `KVM_GET_REGS` → rebuild frame in guest memory → `KVM_SET_REGS` | 1 |
| **D — deny** | return `-ENOSYS` or deliver `SIGSYS` without dispatching | 10 (includes modify_ldt + set_thread_area demoted under F10) |
| **E — gadget-handled** | in-guest LSTAR gadget fast path (no VMEXIT); fallback behaves like class A | 10 (G7 + G6-follow-on, minus sched_yield demoted by G5) |

That's 22 non-A entries total — exhaustive. Every other
syscall, including every syscall Linux will add next
release, inherits A by default. No per-release maintenance
treadmill on the KVM backend's dispatcher.

Class E repurposes the original "hypercall" slot. The 2026-04-24
round-4 review confirmed we don't need a hypercall class for UML
(bare glibc userspace, not Linux-as-guest), so E is recycled for
the in-guest gadget fast path landed in memo 11 G4-G6. See
§"Class E — gadget-handled" below.

## Class B — vCPU-state propagation (1 entry, post-F10)

The complete list. Source-of-truth grep:
`SYSCALL_DEFINE.*arch_prctl` across `arch/x86/kernel/*.c`
and `arch/x86/um/syscalls_64.c`.

| NR | Name | State touched | Propagation |
|---|---|---|---|
| 158 | `arch_prctl` | MSR_FS_BASE (option `ARCH_SET_FS` / `ARCH_GET_FS`), MSR_GS_BASE (`ARCH_SET_GS` / `ARCH_GET_GS`) | `KVM_SET_MSRS` after dispatch on the "set" path; for the "get" path, UML's sys_arch_prctl already writes the user-pointer before returning, and the MSR_GS_BASE value round-trips via handle_syscall → regs[HOST_GS_BASE] → KVM_SET_MSRS. Live dispatcher branch in arch/um/backend/kvm/thread.c::kvm_decode_syscall. |

`arch_prctl` alone covers ~90 % of real-world breakage —
every glibc-linked binary calls `ARCH_SET_FS` from
`_start` before `main()` runs.

### Previously classified B, demoted by audit round-5 F10

| NR | Name | Reason for demotion |
|---|---|---|
| 154 | `modify_ldt` | UML KVM backend does not virtualize the LDT through KVM. The previous B classification implied a KVM_SET_SREGS refresh after sys_modify_ldt, but that refresh was never wired — syscall silently passed through handle_syscall with no vCPU update. Demoted to class D (return -EPERM) so the behaviour matches the declaration; re-promote when a proper LDT-virtualization branch lands. |
| 205 | `set_thread_area` | 32-bit compat syscall, unreachable from x86_64 glibc. Class B classification was aspirational for a 32-bit guest support path that never materialized. Demoted to D. |

### MSR dance the B handler implements

```
  kvm_decode_syscall(vcpu, nr, args):
      if nr == __NR_arch_prctl:
          ret = sys_arch_prctl(args)     /* real dispatch */
          switch (args[0]) {              /* args[0] = option */
          case ARCH_SET_FS:
              kvm->cached_msr.fs_base = args[1]
              kvm->msr_dirty = true
              break
          case ARCH_SET_GS:
              kvm->cached_msr.gs_base = args[1]
              kvm->msr_dirty = true
              break
          }
          return ret
```

Then at the next `KVM_RUN` entry:

```
  kvm_enter_guest():
      if (kvm->msr_dirty) {
          struct kvm_msrs msrs = { .nmsrs = 2, .entries = {
              { .index = MSR_FS_BASE, .data = kvm->cached_msr.fs_base },
              { .index = MSR_GS_BASE, .data = kvm->cached_msr.gs_base },
          }}
          ioctl(vcpu_fd, KVM_SET_MSRS, &msrs)
          kvm->msr_dirty = false
      }
      ioctl(vcpu_fd, KVM_RUN, ...)
```

`kvm_setup_production_sregs` today clobbers FS/GS base to
zero on every entry — that's the bug. The cached MSR
state needs to be threaded through from the per-mm
`struct kvm_um` so re-entries preserve whatever
`arch_prctl` last set.

## Class C — signal-frame (1 entry)

| NR | Name | What the host has to do |
|---|---|---|
| 15 | `rt_sigreturn` | the guest is on a signal-delivery stack frame it built via `KVM_SET_REGS`; on return, `sys_rt_sigreturn` reads back the saved ucontext from guest memory and writes restored regs. Under KVM the restored regs have to land in the vCPU via `KVM_SET_REGS`, not just in `task_pt_regs(current)`. |

Signal *delivery* into the guest (the outbound path) is
not a class-C syscall — it's a host-initiated action from
UML's own signal dispatch. But it uses the same
frame-construction primitives. Budget the two paths
together as one coherent body of work.

## Class D — deny (10 entries, post-F10)

Not a security wall — UML's `sys_call_table` already
enforces CAP_SYS_ADMIN and similar on the privileged
ones, so most "denials" happen naturally as `-EPERM`
without any special handling at the dispatcher layer. The
short D list below captures the ones we additionally trap
at dispatch time because their *successful* execution
would be semantically wrong for a bare-userspace guest,
regardless of capability state.

| NR | Name | Why deny |
|---|---|---|
| 101 | `ptrace` | UML emulates ptrace at its own kernel layer; a guest-issued ptrace could target a sibling UML task in a way UML's ptrace model doesn't expect. Dispatcher-layer trap keeps the model honest. |
| 169 | `reboot` | privileged in principle; in practice a successful reboot would attempt to reset the host vCPU, which UML's scheduler isn't structured to survive mid-flight |
| 175 | `init_module` | module loading into UML's kernel from guest userspace is meaningless + a privilege-escalation vector |
| 176 | `delete_module` | same |
| 246 | `kexec_load` | meaningless: the "kernel" the guest would kexec into has no ring-0 path |
| 313 | `finit_module` | same as `init_module` |
| 320 | `kexec_file_load` | same as `kexec_load` |
| 321 | `bpf` | BPF attach to host kernel structures via a guest syscall is the obvious bypass vector; UML's BPF JIT port (workstream C-06) runs at the UML-kernel layer, not the guest's |
| 154 | `modify_ldt` | UML KVM backend does not virtualize LDT through KVM. Returning -EPERM is the accurate answer for a bare-userspace guest; can be re-promoted to a real class-B branch if a workload ever needs it (audit round-5 F10). |
| 205 | `set_thread_area` | 32-bit compat syscall that 64-bit glibc never calls. Class B classification was aspirational and unreachable (audit round-5 F10). |

Denial shape: return `-EPERM`. That's the accepted "we
saw the syscall and refuse it at this layer" convention
in the kernel — `capable()` / `ns_capable()` failures
return EPERM, and checkpatch specifically warns against
using `-ENOSYS` for anything other than "unknown
syscall NR." Future tightening can surface SIGSYS via
signal delivery (class C) for audit visibility.

## Class E — gadget-handled (9 entries)

The in-guest systrap gadget (memo 11 G4-G6) handles a small
set of hot-path syscalls entirely inside the LSTAR trampoline,
with no VMEXIT. Per-syscall implementation cost dropped from
~13 µs (class A passthrough via VMEXIT) to ~28 ns (class E
gadget) — a 460× speedup.

The complete list. Mirror of the LSTAR dispatch in
`arch/um/backend/kvm/thread.c::kvm_bootstrap_lstar_bytes`
and the classifier in `arch/um/backend/kvm/syscall_class.c`:

| NR | Name | Gadget source | Landed |
|---|---|---|---|
| ~~24~~ | ~~`sched_yield`~~ | demoted to class A by G5 (audit round 6); see "Removed from class E" below | G6 → demoted G5 (2026-04-24) |
| 39 | `getpid` | `%gs:TGID` via per-vCPU state page | G4 (2026-04-24) |
| 102 | `getuid` | `%gs:UID` | G4 |
| 104 | `getgid` | `%gs:GID` | G4 |
| 107 | `geteuid` | `%gs:EUID` | G4 |
| 108 | `getegid` | `%gs:EGID` | G4 |
| 110 | `getppid` | `%gs:PPID` | G4 |
| 186 | `gettid` | `%gs:TID` | G4 |
| 201 | `time`         | vvar REAL_SEC (no seqlock retry — 1-sec resolution) | G6-follow-on (2026-04-24) |
| 228 | `clock_gettime` (CLOCK_MONOTONIC) | seqlock vvar page | G5 (2026-04-24) |
| 309 | `getcpu`       | `%gs:CPU_ID` via per-vCPU state page; node always 0 | G6-follow-on (2026-04-24) |

### Fallback semantics

Every gadget handler ends in `sysretq`. If the gadget chose
the fallback path (seqlock retry budget exhausted, syscall
arg outside the fast path e.g. `clock_gettime(CLOCK_TAI)`,
or `!CONFIG_UM_BACKEND_KVM_GADGET`), the handler jumps to
the dispatch-table fallback `out $0xf4; sysretq`. That
generates a KVM_EXIT_IO on the SYSCALL port; the host-side
`kvm_decode_syscall` then reaches the classifier and:

- sees `CLASS_GADGET` for this NR,
- falls through to `handle_syscall` exactly like `CLASS_PASSTHROUGH`.

The effect is that class E is an A-path fast-path overlay:
gate-off / fallback / disable-the-gadget all degrade
gracefully to A semantics. This is why
`arch/um/backend/kvm/thread.c::kvm_decode_syscall` only
short-circuits on `CLASS_TRAP` — every other non-A class
still reaches the main dispatch.

### When to promote a syscall to class E

Two criteria, both required:

1. **High call rate in realistic workloads.** A glibc-
   linked guest calling `getpid` 1M times / sec is
   worth ~13 ms/s saved; a syscall called < 1k/s is
   noise.
2. **Gadget-expressible semantics.** The handler must
   fit in LSTAR reach (currently ≤ 64 B per handler,
   shrinking as the dispatch table grows), need only
   per-vCPU state page + vvar page, and have a trivial
   fallback criterion (bounds check on one or two
   arg bits).

If both hold, land the handler + promote the inventory
row from A to E. Every gadget handler has a KUnit
cross-check asserting the classifier agrees with the
live dispatch table (see `kvm_syscall_classification_test`
in `arch/um/backend/contract/test_ops.c`).

### Removed from class E (audit round-6 G5)

`__NR_sched_yield` was originally landed as class E with a
trivial 8-byte handler returning 0. POSIX says sched_yield
is advisory but the gadget's bypass meant a guest sched_yield
loop could starve other UML tasks for up to ~10 ms (one host
timer tick) before SIGALRM-driven preemption fired. Demoted
back to class A by G5 (decisions-log D94): the LSTAR dispatch
entry now redirects to the fallback path so handle_syscall →
sys_sched_yield → schedule() runs and UML's scheduler gets
immediate notice. The original 8-byte handler bytes at LSTAR
offset +168..+175 stay in place but are now unreachable; a
future LSTAR compaction can remove them.

### Follow-on (G6 deferred)

`__NR_time` (201) and `__NR_getcpu` (309) are tracked for
G6-follow-on; both hit the LSTAR rel8-reach limit when G6
tried to land them alongside `sched_yield`. Deferred until
the LSTAR page is split or the gadget is reorganized for
reach. See decisions-log D74.

## Class A — everything else (364 entries)

See `syscall-inventory.tsv` for the authoritative,
per-NR list. The table starts at NR 0 (`read`) and runs
through NR 471 (`rseq_slice_yield`) with holes for removed
syscalls. Default dispatch:

```
  ret = sys_call_table[nr](args);
```

Pointer args in `RSI`/`RDX`/… point into **guest VA**,
which under UML's identity-memory model *is* the kernel
VA UML's `__user` accessors expect. The shadow-PT lazy
fault-in (memo 09, step 3) guarantees those VAs are
populated before the kernel touches them. No per-syscall
extra wiring.

Memory-map-mutating syscalls in class A (`mmap`,
`munmap`, `mprotect`, `brk`, `mremap`, `execve`) trigger
`mm_map` / `mm_unmap` ops which in turn call
`kvm_shadow_invalidate_va_range` (landed 2026-04-24
under audit round-5 F6). So even this subset needs no
syscall-specific handling at the dispatcher — the mm
layer's existing hooks are the right place. Note: the
earlier claim that the invalidator "landed in memo 08
sub-commit #5b" was incorrect — no such code existed
until F6; audit round 5 caught the doc/implementation
drift.

## IDT / exception classification

Much smaller table — 32 architectural exception vectors,
but we only care about the ~5 that a userspace ring-3
binary can actually trigger:

| Vector | Name | Translate to | Status |
|---|---|---|---|
| 6 | `#UD` — invalid opcode | SIGILL via `handle_trap` | **pending** (~50 LOC) |
| 13 | `#GP` — general protection | SIGSEGV (privileged-instruction faults) | **pending** (~50 LOC) |
| 14 | `#PF` — page fault | UML fault path via `trap.c::segv()` | **LANDED** (memo 08 sub-commit #5b) |
| 1 | `#DB` — debug trap | SIGTRAP; needed only for guest-side ptrace/kprobes | parking-lot (post-v1) |
| 3 | `#BP` — breakpoint | SIGTRAP | parking-lot (post-v1) |

All other vectors (`#DE`, `#NMI`, `#OF`, `#MF`, `#XM`,
etc.) are either ring-0-only (never reached from guest
ring-3) or architecturally impossible under KVM's EPT
isolation.

Exception decoder is a single switch in the
`KVM_EXIT_EXCEPTION` arm of `kvm_run_userspace`, parallel
to the `KVM_EXIT_IO` arm that dispatches syscalls.

## Signals + IRQs

Three distinct paths, all small:

| Path | Direction | Status |
|---|---|---|
| **Host signal interrupts KVM_RUN** (SIGALRM / SIGIO / SIGCHLD arrive while blocked in `ioctl(KVM_RUN)`) | host → vCPU | **LANDED** (memo 08 sub-commit #5; surfaces as `KVM_EXIT_INTR`, UML's own signal handler runs, then re-enter KVM_RUN) |
| **Guest-directed signal delivery** (UML wants to deliver a signal to guest userspace — e.g. SIGSEGV from a #PF the UML fault path couldn't resolve) | UML → guest | partial; uses `KVM_SET_REGS` + a manually-built `ucontext` on the guest stack. Frame construction shares code with class C `rt_sigreturn`. |
| **Timer-interrupt injection** (UML timer subsystem wants to fire a tick into the guest) | UML → vCPU | uses `KVM_INTERRUPT` ioctl with vector 0x20+; already landed for a single vector in Phase III Lift #1e. More vectors are pluggable. |

## How to maintain the inventory

The TSV sidecar is **generated**, not hand-written. Any
time a new x86_64 syscall appears in upstream's
`syscall_64.tbl`, re-run:

```bash
awk '/^[0-9]+[[:space:]]+(common|64)[[:space:]]+/ {
    nr = $1; name = $3
    class = "A"
    reason = "passthrough: sys_call_table[nr]"
    if (name == "arch_prctl")       { class = "B"; reason = "MSR_FS_BASE / MSR_GS_BASE propagation to vCPU" }
    else if (name == "modify_ldt")  { class = "B"; reason = "LDTR descriptor table; push via KVM_SET_SREGS" }
    else if (name == "set_thread_area") { class = "B"; reason = "32-bit TLS segment (compat only)" }
    else if (name == "rt_sigreturn"){ class = "C"; reason = "signal frame restore: KVM_GET_REGS -> rebuild -> KVM_SET_REGS" }
    else if (name == "ptrace")      { class = "D"; reason = "trap: UML emulates ptrace at its own layer" }
    else if (name == "kexec_load" || name == "kexec_file_load") { class = "D"; reason = "trap: meaningless for userspace guest" }
    else if (name == "init_module" || name == "finit_module" || name == "delete_module") { class = "D"; reason = "trap: privileged + meaningless in guest userspace" }
    else if (name == "reboot")      { class = "D"; reason = "trap: privileged + would reset the host vCPU" }
    else if (name == "bpf")         { class = "D"; reason = "trap: BPF attach via guest syscall is a bypass vector" }
    printf "%d\t%s\t%s\t%s\n", nr, name, class, reason
}' arch/x86/entry/syscalls/syscall_64.tbl | sort -n
```

Any new syscall that doesn't match one of the overrides
gets class A automatically. Promoting a new-arrival syscall
to B/C/D is a deliberate one-line edit to the recipe.

## Implementation sequencing

The table above is the plan. Concrete sub-commit shape
for the **code** that consumes the inventory:

- **Step 1 (sub-commit #5c of memo 08).** Wire class B for
  `arch_prctl(ARCH_SET_FS/GS)` only. Cache MSR state in
  per-mm `struct kvm_um`. Refresh via `KVM_SET_MSRS`
  before `KVM_RUN` when the dirty bit is set. Target:
  `/bin/true` boots to clean exit, unblocking task #192.
  **LANDED 2026-04-24** (commit `6d3c027a7741`). ~70 LOC
  runtime + KUnit assertions in
  `kvm_production_sregs_shape_test`.

- **Step 2.** Class D denylist of the 8 entries via a
  static `class_map[NR_syscalls]` + dispatcher switch.
  **LANDED 2026-04-24**. New TU
  `arch/um/backend/kvm/syscall_class.c` carries the
  table; `kvm_decode_syscall` short-circuits
  `CLASS_TRAP` with `-ENOSYS` before `handle_syscall`.
  Bundled with step 6 (see below).

- **Step 3.** `modify_ldt` (class B) — small + low-risk
  once the MSR infrastructure from step 1 exists, but
  almost no real binary exercises it, so land behind a
  KUnit-only coverage test.

- **Step 4.** `rt_sigreturn` (class C) — only needed once
  the first guest binary that raises a signal ships. v1
  selftest surface (static glibc binaries) usually doesn't
  need it; the class B/D work buys us most of the
  coverage.

- **Step 5.** `#UD` + `#GP` exception decoders.

- **Step 6.** Static coverage KUnit: walk the classifier
  + assert the inventory invariants (12 non-A entries on
  the x86_64 NR range, specific NRs map to the expected
  class, out-of-range defaults to PASSTHROUGH). **LANDED
  2026-04-24** as `kvm_syscall_classification_test` +
  `kvm_syscall_class_count_test` in
  `arch/um/backend/contract/test_ops.c`. 30/30 KUnit
  green on the integrated build. A future tightening
  that walks `sys_call_table` directly instead of the
  static NR range would catch "new upstream syscall
  added, inventory row missing"; deferred until the
  first upstream syscall after 7.0 lands.

Budget: ~400 LOC of runtime + ~200 LOC of tests across
all six steps, most of which is the static table itself.

## Cross-references

- `08-real-run-userspace.md` — the memo this one feeds;
  `/bin/true` block at cr2=0x10 is the concrete failure
  mode (sub-commit #6 blocker per task #192).
- `09-shadow-pt.md` — memory-side companion; mm-state
  propagation is solved there so this memo doesn't have
  to worry about it.
- `04-ring-transition.md` — where LSTAR trampoline +
  SYSCALL decoding live; this memo is one layer up.
- `05-nested-virt-fallback.md` — D-05 fallback; a nested
  host that can't run KVM falls back to seccomp, where
  class B is already implicit in the host MSRs.
- `syscall-inventory.tsv` — the generated truth table.
- `arch/x86/entry/syscalls/syscall_64.tbl` — upstream
  source the inventory derives from.
- `arch/um/kernel/skas/stub_exe.c` — seccomp backend's
  stub-page filter (not the host-side dispatch, but a
  useful cross-reference for "what absolutely must work"
  at minimum: futex, recvmsg, close, mmap, munmap,
  arch_prctl, rt_sigreturn).
