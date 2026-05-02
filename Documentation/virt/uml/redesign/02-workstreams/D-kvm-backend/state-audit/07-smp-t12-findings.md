# UML KVM v2 — Layer 7 Findings: SMP-T12 (state-trace empirical)

**Created:** 2026-05-01
**Tip:** `93656b8224d6`
**Status:** OPEN — partial diagnosis; bug NOT root-caused yet

## Summary

Built a complete state-snapshot trace ring (CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE,
shielded 3 ways: compile-time / static_branch / per-CPU) capturing 30+
fields at every operation transition the state-audit Layer 2 inventoried.
Built a raw-syscall reproducer (mt-rawmmap) bypassing glibc. Captured
100+ FAIL traces. Tested 5 single-fix hypotheses. **None solved the bug.**

The bug is now precisely characterized but the root cause is still elusive.

## Hard observations

### 1. Bug is strictly T > N

| Config            | PASS rate | n |
|-------------------|----------:|--:|
| T=1, ncpus=4      |     100%  | 10 |
| T=2, ncpus=4      |     100%  | 10 |
| T=4, ncpus=4      |     100%  | 10 |
| T=4, ncpus=2      |      70%  | 10 |
| T=5, ncpus=4      |      87%  | 15 |
| T=8, ncpus=4      |   45–50%  | 100+ |

Bug fires only when multiple UML tasks share a single per-host-CPU vCPU.

### 2. Kernel mmap return is ALWAYS valid

Two kernel-side ratelimited printk diagnostics:

- `UM_MMAP_DIAG` (`arch/um/kernel/skas/syscall.c`) — fires when sys_mmap
  returns errno-range (≥ 0x80000000). Fires intermittently with
  `ret=0xfffffffffffffffc` (-EINTR — `mmap_write_lock_killable` interrupted).
- `UM_MMAP_ZERO` (same file) — UNBOUNDED, fires whenever sys_mmap with
  addr=NULL returns 0. **NEVER fires across 100+ runs.**

So the kernel-side handle_syscall ALWAYS sets `regs->gp[HOST_AX]` to a
valid address (or -EINTR, never zero).

### 3. User-mode RAX is corrupted

mt-rawmmap (`tools/testing/selftests/um/mt-mmap-stress/mt-rawmmap.c`)
issues `__NR_mmap` via inline `syscall` and inspects RAX bit-for-bit.
Captured failure modes:

- `RAW_NULL`: user reads RAX = 0 (kernel never returned 0)
- `RAW_ERR`: user reads RAX = 0xffffffffffffffda (-ENOSYS — wrong syscall slot dispatched)
- `RAW_WEIRD`: user reads RAX = 0x2 (small int, possibly syscall NR)
- `RAW_VERIFY`: page contents = 0 instead of memset value (re-read confirms)
- RIP corruption: `mt-rawmmap[56]: segfault at ip=0xffffffffffffffff`

The corruption is between `regs->gp[HOST_AX] = valid` and user-mode RAX
read. This is the kernel→user RAX transport.

### 4. Failed single-fix attempts

| Fix attempt                                  | Result               |
|----------------------------------------------|----------------------|
| C6: tighten SIGALRM 10ms → 1ms               | no change            |
| Never-drain deferred mmu_gather queue        | 45→60% (modest)      |
| Always SET+GET FPU (revert Phase H.2)        | 47% (marginal)       |
| CS/SS reset in load_user_sregs               | no change            |
| PT_SYSCALL_NR clear BEFORE interrupt_end     | no change            |

## Hypotheses ranked

### H1 (HIGH): IRETQ-gadget bypass + privileged-CPL re-entry

v2 marshals `dst->rip = HOST_IP` (user RIP) and `dst->rcx = HOST_IP`
(also user RIP). KVM_RUN re-enters at user_RIP **directly**, bypassing
the trampoline's `sysretq`. After a SYSCALL trap, `sregs.cs` is still
kernel CS (=0x08, DPL=0). `load_user_sregs` doesn't reset CS to user
(0x2b, DPL=3).

Result: KVM enters guest at `(CS=kernel, RIP=user_va)` → CPU executes
user code at CPL=0. Mostly harmless, but interacts with KVM
internals (paranoia checks, hardware exception delivery, IDT-DPL
gates) in ways that under SMP T>N produce corrupted GP registers
when an exception or fast-fault path fires.

v1 archive (`kvm-v1-archive/thread.c:3088-3110`) avoided this entirely
by routing every re-entry through an IRETQ gadget that pushes a
fully-formed user IRETQ frame `[user_RIP, 0x2b, RFLAGS, user_RSP,
0x23]`. v2 chose the simpler "set RIP directly" path but skipped CS.

**Tested partial fix:** reset CS/SS to USER selectors in
load_user_sregs. **Did NOT fix** — but the IRETQ gadget approach
(building a 4-byte stub with `iretq` + per-vCPU page with the frame)
is a SUPERSET — also resets RFLAGS, RSP per-task, etc. Worth full
attempt.

### H2 (MEDIUM): KVM `complete_userspace_io` callback survives across tasks

After `out` vmexit, KVM sets `vcpu->arch.complete_userspace_io =
complete_fast_pio_out` and `cui_linear_rip = trampoline+0x42`. Per-vCPU
state, persistent. Under SMP T>N, the next KVM_RUN entry on this
vCPU may belong to a sibling task. The callback fires with sibling's
RIP (mismatch) → returns 1 but **clears `pio.count`** as a side effect.
This is benign on the surface but leaves `cui_linear_rip` and
`pio.{port,size,…}` carrying stale state across the task boundary.

Worth verifying with KVM tracepoints.

### H3 (LOW): kvm_dirty_regs cleared by sibling, sync_regs skipped

KVM clears `KVM_SYNC_X86_REGS` from `kvm_dirty_regs` inside `sync_regs`
on entry. Both syscall_trap.c:1707 and vcpu.c:1648 re-OR the bit.
Theoretical race: an EINTR path (vcpu.c:1830) returns early without
re-setting dirty before the next iteration's marshal. Not yet observed.

## Tooling shipped

All committed under `umlctl-deploy`:

- `arch/um/backend/kvm-v2/state_trace.{h,c}` (~620 lines): per-CPU
  ring buffer, 30+ fields/snap, 14 hook points (matches Layer 2
  operations), debugfs control surface, anomaly auto-freeze.
- `arch/um/backend/kvm-v2/Kconfig`: `CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE`
  with three layers of shielding (CONFIG=n compiles to 0; runtime
  static_branch; per-CPU storage).
- `tools/testing/selftests/um/state-trace/parse-trace.py`: parser +
  invariant checker. Subcommands `summary`, `pid`, `invariants`,
  `diff`, `mmap-zero`, `pid-window`.
- `tools/testing/selftests/um/mt-mmap-stress/mt-rawmmap.c`: minimal
  raw-syscall reproducer bypassing glibc.
- `arch/um/kernel/skas/syscall.c`: UM_MMAP_DIAG (errno-range, capped)
  and UM_MMAP_ZERO (zero, never capped) printks.

## Next investigation (proposed)

1. **Implement H1 fix in full** — IRETQ gadget. Build a per-vCPU
   re-entry stub with `iretq`, marshal builds the IRETQ frame on a
   per-vCPU page, set rip = gadget_GVA, rsp = frame_GVA, rflags = (1<<1).
   Re-entry is at CPL=0 in a 4-byte stub that immediately iretq's to user.
   Restores v1-equivalent semantics.

2. **bpftrace H2** — trace `complete_fast_pio_out` calls and
   `cui_linear_rip` values across vCPU schedule boundaries to confirm
   or refute the callback-leak hypothesis.

3. **Add a PRE_KVM_RUN consistency check** — log if
   `run->s.regs.regs.rax` differs from `regs->gp[HOST_AX]` at the
   moment of KVM_RUN ioctl call. Should always be equal (we just
   marshaled). If they differ, marshal failed or external write.

## Cited file references

- arch/um/backend/kvm-v2/vcpu.c:1185-1346 — load_user_sregs (CS not reset)
- arch/um/backend/kvm-v2/vcpu.c:1647-1665 — second marshal + KVM_RUN
- arch/um/backend/kvm-v2/syscall_trap.c:1576-1730 — syscall arm
- arch/um/backend/kvm-v1-archive/thread.c:3088-3110 — v1 IRETQ gadget reference
- arch/x86/kvm/x86.c:9683-9722 — KVM emulator pio dispatch
- arch/x86/kvm/x86.c:12060-12131 — KVM ioctl_run, sync_regs
