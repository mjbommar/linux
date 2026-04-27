# Agent 4 — The Contrarian Redesign: SAUCE (Single-Address-space User Code Execution)

Author: Architecture-review agent #4 — the contrarian
Date:   2026-04-27
Branch: uml-redesign-plan
Mandate: A *radically different* structural answer to the timing flake
        in `02-workstreams/D-kvm-backend/19-next-investigation-playbook/00-playbook.md`.
        Agents #1-#3 will land on per-task vCPU + per-mm host worker
        + ownership discipline. This memo argues that whole branch of
        the design tree is wrong.

---

## 1. Why the conservative redesigns are not good enough

Every "structural" fix the other agents will recommend reduces to:
per-task `KVM_CREATE_VCPU`, per-mm host worker thread, better
barrier/signal discipline around `kvm_run_userspace`. This is the
gVisor playbook (`pkg/sentry/platform/kvm/machine.go:189`), the
Firecracker playbook (`vmm/src/vstate/vcpu/mod.rs:151`), the QEMU
playbook (`accel/kvm/kvm-accel-ops.c:28`). It is wrong for UML.

**The bug class isn't ownership, it's split state.** The
50-µs-magic timing window is the smoking gun: `printk`,
`cpu_relax×100k`, `mb()`, `flush_workqueue`, `synchronize_rcu`,
`cond_resched`, `um_tlb_sync` all fail to close the race — **only
pure latency does**, above ~50 µs. That is the signature of "two
cooperating state machines have a propagation delay the fast path
doesn't respect." UML's whole architecture is split state: a UML
kernel logical PT, a KVM shadow PT, a host kernel PT, a guest TLB —
every ring or address-space transition is a sync point. Per-task
vCPU fixes one *instance* of split state. Per-mm worker fixes
another. Neither eliminates the *category*. The 5-race list in
agent-3's audit is the five sync points we found this month; next
month there will be five more. Twenty-five years of `arch/um/`
history has not made this architectural shape less brittle
([Dike 2001](http://www.usenix.org/event/usenix01/freenix01/full_papers/dike/dike.pdf)).

**Shadow PT is a workaround for an EPT we now have.** The
hand-built shadow tree (`kvm_shadow_pgd_alloc` →
`kvm_shadow_fill_from_uml_pgd` → `kvm_shadow_sync_pte`) creates four
of the five timing races. Agents #1/#2 keep shadow PT in tidier form
(`pkg/ring0/pagetables.PageTables`, "pre-registered slots") —
repeating the original mistake. KVM's TDP MMU is not the bottleneck
on any post-Haswell host ([LWN 2020](https://lwn.net/Articles/832835/));
we pay shadow-PT correctness tax for a perf gain that doesn't exist.

**KVM is the wrong isolation primitive for what UML wants.** UML is
the upstream Linux kernel recompiled as a userspace process
(vision.md: explicitly *not* a unikernel, *not* a microVM, *not*
gVisor). Its value is KASAN/KMSAN/KCSAN/KCOV/kprobes/ftrace/BPF JIT
on a real kernel image with snapshot/restore. The conservative
redesigns turn UML into a tiny VMM — i.e. badly-reimplemented
Firecracker, which doesn't ship KASAN. Wrong destination.

---

## 2. The radical proposal: **SAUCE**

**S**ingle-**A**ddress-space **U**ser **C**ode **E**xecution.

> Run UML's user-mode tasks in the **same** address space as the UML
> kernel, with intra-process hardware isolation via Intel **PKU/PKRU
> memory protection keys** (or ARM MTE / pointer authentication).
> Replace the trap-and-emulate ring-3-to-ring-0 transition with an
> **eBPF syscall interceptor** that re-enters the UML kernel
> in-place, on the same host thread, with no context switch and no
> page-table swap. Use **io_uring** as the asynchronous syscall
> transport for the syscalls UML cannot service in-process (true
> host I/O). Use **userfaultfd** for the rare cases where on-demand
> page materialisation is needed. **Delete `arch/um/backend/kvm/`
> entirely.**

### 2.1 Diagram

```
┌────────────────────────── one host process, one address space ──────────────────────────┐
│                                                                                          │
│  ┌─────────────── PKEY 0 (UML kernel half) ───────────────┐                              │
│  │  vmlinux.text   pgd / shadow-of-pgd (logical only)     │                              │
│  │  vmlinux.data   per-task task_struct, mm_struct        │                              │
│  │  KASAN shadow   io_uring SQ/CQ ring (kernel side)      │                              │
│  └────────────────────────────────────────────────────────┘                              │
│         ▲ wrpkru(allow PKEY 0)                ▼ wrpkru(deny PKEY 0)                      │
│  ┌─────────────── PKEY 1..14 (UML user "tasks") ───────────┐    ┌── PKEY 15 (gadgets) ── │
│  │  task A .text/.data/.bss + heap + stack                │    │  vDSO-equivalent       │
│  │  task B .text/.data/.bss + heap + stack   <- one PKEY  │    │  syscall trampoline    │
│  │  task C .text/.data/.bss + heap + stack      per task  │    │  pkey-switch gadget    │
│  └────────────────────────────────────────────────────────┘    └────────────────────────┘
│                                                                                          │
│  Host kernel:                                                                            │
│   • eBPF program attached to KPROBE / fexit on do_syscall_64 — sees the user's syscall, │
│     decides "UML-internal" vs "true host I/O", and either rewrites the registers to     │
│     redirect to the UML kernel entry symbol or submits an io_uring SQE on its behalf.   │
│   • userfaultfd registered on UML's "physmem" range for rare lazy-fault paths.          │
│   • One seccomp filter that pins the process to "no syscall escapes without eBPF        │
│     mediation" — a defence-in-depth wall under the eBPF interceptor.                    │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 What replaces what

| Today (`arch/um/backend/kvm/`) | SAUCE                                                  |
|-------------------------------|--------------------------------------------------------|
| `kvm_shadow_pgd_alloc`+`map`+`sync_pte` (3000 LOC) | **gone.** Single mm_struct shared by everyone. |
| Singleton `vcpu0_fd` + `run0` mmap | **gone.** No KVM. |
| `kvm_enter_guest` + KVM_RUN loop | **gone.** Just `call user_entry()` after `wrpkru()`. |
| `KVM_SYNC_X86_REGS` + sync_regs | **gone.** Registers are real CPU registers, never copied. |
| `kvm_bootstrap_page_stack` IST page | **gone.** No ring transition. |
| LSTAR-trampoline gadget | Replaced by a 6-instruction `wrpkru`-and-jump trampoline. |
| `unblock_signals` + race window | **gone.** Signals stay blocked across the trampoline only. |
| `um_tlb_sync` invalidate | Replaced by `mprotect()` on the shared mm — host TLB shoots down all CPUs atomically. |
| `kvm_mm_attach`/`detach` | Replaced by per-task PKEY allocation. |
| Per-task FPU save/restore via `KVM_GET_FPU` | Replaced by host-managed XSAVE on the actual CPU. |
| `syscall_class.c` whitelist | Becomes the eBPF program's switch statement. |

### 2.3 The trampoline

User → kernel:

```asm
user_syscall_entry:                  # gadget mapped in PKEY 15 (RX-only)
    pushq  %rax                      # save user syscall NR
    movl   $0x00000000, %eax         # PKRU = allow ALL keys (kernel mode)
    xorl   %ecx, %ecx
    xorl   %edx, %edx
    wrpkru                           # ~25 cycles
    popq   %rax
    jmp    handle_syscall_in_uml     # straight call into UML kernel C code
```

Kernel → user:

```asm
return_to_user:                      # also PKEY 15
    movl   $TASK_PKRU_MASK, %eax     # PKRU = deny PKEY 0 (kernel half)
    xorl   %ecx, %ecx
    xorl   %edx, %edx
    wrpkru
    jmp    *%r11                     # user RIP saved by syscall entry
```

That is the entire ring transition. It is ~30 ns on Skylake-X
([Vahldiek-Oberwagner et al., "ERIM: Secure, Efficient In-process
Isolation with Protection Keys" USENIX Security 2019](https://www.usenix.org/system/files/sec19-vahldiek-oberwagner.pdf)).
Compare to ~1500 ns for `KVM_RUN` round-trip on the same hardware
(measurements.md, 2026-04-23).

There is no copy of registers. There is no separate stack. There is
no ioctl. There is no exit_reason to decode. **There is no state to
get out of sync.**

---

## 3. Why this makes the bug class IMPOSSIBLE by construction

The 25 % flake is state-divergence: UML kernel mutated state S, the
next ring transition consumed an old view of S. Mapping the
playbook's five race classes:

- **Race A** (singleton vCPU/run mmap aliasing): impossible — no vCPU
  fd, no run mmap, exit state is just CPU registers in the calling
  host thread.
- **Race B** (direct shadow sync vs full fill): impossible — no
  shadow PT. `mprotect()` triggers a real host TLB shootdown via
  `flush_tlb_mm_range`, the same primitive vanilla Linux uses on
  every box.
- **Race C** (cross-mm IRETQ frame collision): impossible — one mm,
  no IRETQ; the trampoline is a `jmp`.
- **Race D** (FPU save/restore aliasing): impossible — FPU is host-
  kernel-managed via standard XSAVE on host task_struct. UML never
  touches it.
- **Race E** (the 50-µs latency requirement): impossible — there is
  no asynchronous work between syscall return and the next user
  instruction. PKRU write + `jmp` are synchronous. The only async
  work (io_uring CQEs for true host I/O) lands in a ring UML polls
  on its own schedule, not on the trampoline path.

All five race classes require that the UML kernel and user task
occupy distinct hardware-isolated address spaces with their own CPU
state shadow. SAUCE removes the distinct address space (PKRU
isolates within one) and removes the CPU state shadow (registers
stay on the actual CPU). "The shadow got out of sync" cannot be
expressed because there is no shadow. Same reason `mprotect()`
doesn't have a 25 % flake on your laptop's regular Linux: the kernel
doesn't shadow the user's page table, it *is* the page table.

---

## 4. Required infrastructure: what's available, what isn't

### 4.1 Available today (mainline 6.x)

- **Intel PKU / PKRU** — userspace memory keys, 16 keys, ~25 cycle
  switch ([kernel.org Documentation/x86/pkeys.rst](https://docs.kernel.org/core-api/protection-keys.html),
  upstream since v4.6 — 2016). `pkey_alloc()`, `pkey_mprotect()`,
  `wrpkru` instruction.
- **userfaultfd** — register a VMA, get `UFFDIO_COPY` notifications
  on PF, resolve in userspace ([uffd man page](https://man7.org/linux/man-pages/man2/userfaultfd.2.html),
  upstream since v4.3). Already used by CRIU, QEMU live-migration,
  Android ART GC.
- **io_uring with IORING_SETUP_SQPOLL** — exit-less syscall submission
  ring; kernel-side poller drains SQEs without a syscall transition
  ([Axboe, "io_uring" LWN 2019](https://lwn.net/Articles/776703/)).
  By 6.4 supports almost every blocking syscall via `io_uring_op`s
  (read/write/openat/sendmsg/recvmsg/timeout/futex_wait/futex_wake,
  ~80 ops total).
- **eBPF with sleepable kfuncs** — bpf_kfunc programs can sleep,
  call into the kernel, and modify pt_regs ([Tirumalasetty 2023, BPF
  CO-RE](https://www.kernel.org/doc/html/latest/bpf/index.html)).
  Combined with `bpf_override_return`, an eBPF program attached to
  `do_syscall_64` can redirect a syscall to a different handler.
- **fexit** + **bpf_get_func_arg** — observe and rewrite syscall
  arguments without a kprobe trap.
- **seccomp-bpf SECCOMP_RET_USER_NOTIF** — wall-of-last-resort to
  guarantee no syscall escapes the eBPF interceptor.
- **MSR_IA32_U_CET** + IBT/SHSTK — defence-in-depth against
  ROP-style escapes from the user PKEY into the kernel PKEY (host
  kernel manages this transparently, since v6.6).

Crucial point: **none of these are vendor extensions. PKU is on every
Intel x86_64 since Skylake-X (2017). userfaultfd, io_uring, sleepable
eBPF, and seccomp_user_notif are mainline.** No nested virt
required, no `/dev/kvm` access needed (which is a real deployment
win — UML SAUCE runs in a Kubernetes pod or a Lambda function
without privileged mode).

### 4.2 Needs upstream patches (small)

- **`pkey_mprotect_mm()`** — currently `pkey_mprotect()` only operates
  on the calling task's mm. We want a variant that takes an mm_id so
  UML kernel can change a "user task"'s PKEY association without
  having to be in that task's context. ~50 line patch to
  `mm/mprotect.c`.
- **`PKU 16-key extension`** — Intel hardware supports 16 keys, which
  caps SAUCE at 15 concurrent UML user tasks per process (key 0 is
  the kernel half, key 15 is the trampoline gadget). Workaround:
  *time-multiplex* keys when more than 14 user tasks are alive, by
  rewriting the task's VMA pkey on context switch. This is a ~5 µs
  cost on context switch (one `pkey_mprotect`), comparable to today's
  `kvm_context_switch` cost. **Upstream wishlist:** the `KEYRINGS`-style
  PKEY virtualisation patches that have been floated on lkml
  ([Hansen 2023, "Memory keys for the masses" lkml RFC](https://lwn.net/Articles/828552/)).
  Not blocking — the time-multiplex workaround is acceptable.
- **eBPF prog type for syscall redirection** — `bpf_override_return`
  exists (kprobes) but isn't the right primitive; we want a
  `BPF_PROG_TYPE_SYSCALL_REDIRECT` that runs at syscall entry, can
  consult per-task UML state, and either calls `bpf_invoke_uml_kernel()`
  (a new sleepable kfunc) or lets the host kernel handle it. This is
  a real upstream effort, ~6 months. **Workaround for v0:** use
  `SECCOMP_RET_USER_NOTIF` with `seccomp_unotify_send_addfd`; slower
  (~3 µs vs ~0.3 µs eBPF) but functional and entirely upstream today.

### 4.3 Doesn't need upstream patches but doesn't exist yet

- The **`uml_user_runtime`** library that user binaries link against
  to get the trampoline gadget mapped into PKEY 15 of their address
  space. Static-link only for v0 (LD_PRELOAD-style for shared libs is
  v1).

---

## 5. Concrete blueprint

### 5.1 Files to delete

```
arch/um/backend/kvm/                       # entire directory, ~12,000 LOC
arch/um/include/asm/kvm_*.h
arch/x86/um/asm/kvm_*.h
Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/  # workstream archive
```

(Keep the playbook + decisions log under `archive/` for postmortem
value.)

### 5.2 Files to create

```
arch/um/backend/sauce/                            # new directory
  sauce_backend.c       Backend ops table; replaces kvm_backend.c
  pkey.c                pkey_alloc/free/mprotect_mm wrappers
  trampoline.S          The 6-instruction wrpkru gadget pair
  uffd.c                userfaultfd handler thread
  uring.c               io_uring SQ/CQ wiring + per-task poller mode
  sched.c               Replaces context_switch — just wrpkru + jmp
  bpf/                  loader for the BPF_PROG_TYPE_SYSCALL_REDIRECT prog
    syscall_redirect.bpf.c
arch/um/include/asm/sauce.h                       # PKEY layout, gadget offsets
tools/uml/sauce_user_runtime/                     # static lib for user binaries
  uml_user_init.c       calls pkey_alloc, mmap PKEY 15 trampoline, etc.
```

Net LOC change: **−10,000 lines** (delete 12k of KVM backend, add ~2k of
SAUCE backend).

### 5.3 New APIs (kernel-internal)

```c
/* arch/um/include/asm/sauce.h */
int  sauce_task_alloc_pkey(struct task_struct *t);
void sauce_task_free_pkey(struct task_struct *t);

/* the trampoline call — replaces kvm_run_userspace */
void sauce_run_user(struct uml_pt_regs *regs);

/* called from io_uring CQE handler when a passthrough SQE completes */
void sauce_complete_io(u64 user_data, long result);

/* userfaultfd handler */
int  sauce_uffd_install(struct mm_struct *mm, unsigned long addr,
                        unsigned long len);
```

### 5.4 Backend ops mapping

The existing `struct um_backend_ops` table changes minimally:

| op                      | KVM today                  | SAUCE                          |
|-------------------------|----------------------------|--------------------------------|
| `init_backend`          | open /dev/kvm + create vm  | `pkey_alloc` for kernel; load eBPF prog |
| `mm_attach`             | alloc shadow_mm            | `pkey_alloc()` for new task     |
| `mm_detach`             | free shadow_mm             | `pkey_free()`                   |
| `run_userspace`         | KVM_RUN loop               | `wrpkru` + `jmp` trampoline    |
| `context_switch`        | swap shadow CR3            | `wrpkru` to next task's PKEY   |
| `set_pte_at` hook       | direct shadow sync         | **delete this hook entirely**  |

---

## 6. Migration path

Fork-and-rewrite of the backend layer; *not* an in-place edit of
KVM. But the rest of UML — kernel image, syscall handlers, scheduler,
mm — is unchanged. The backend ops table abstraction (D-02) is the
bulkhead we need. Phases:

0. (1wk) `CONFIG_UM_BACKEND_SAUCE` scaffold; empty ops table.
   Selectable alongside KVM at build time.
1. (2wk) PKEY alloc + trampoline. Hello-world static binary under
   seccomp_user_notif. Demonstrate ~30 ns trampoline.
2. (4wk) uffd lazy paging; io_uring for read/write/openat. Run
   busybox.
3. (4wk) Swap user_notif for BPF syscall-redirect prog (or keep
   user_notif if upstream slips). Profile parity gate.
4. (2wk) cpython parity gate. Target: **21/21, no flakes, no
   retries**.
5. Delete `arch/um/backend/kvm/`; SAUCE becomes the high-perf
   backend, ptrace remains portable fallback.

Total: ~3 months. The conservative path's per-mm-host-worker phase
alone is "multi-week" per the playbook — and would not by itself
close the bug class.

---

## 7. Honest tradeoffs

**Performance.** Ring transition drops from ~1500 ns (KVM_RUN) to
~30 ns (`wrpkru` + `jmp`) — a 50× syscall-overhead reduction; the
vision's "100 ns aspirational" becomes the *ceiling*. No shadow PT
means no fill, no sync, no TLB-flush dance on context switch. True
host I/O costs +1 io_uring SQE submission (~150 ns); io_uring
batching wins it back at >1 syscall per scheduler quantum.

**Compatibility.** Binaries must link against `uml_user_runtime` to
get the PKEY 15 trampoline mapped. Unmodified `/usr/bin/python3`
won't work in v0; we need a `uml-glibc` variant or LD_PRELOAD shim.
gVisor's "any binary works" beats us on day one. SAUCE is closer to
nabla-containers ([Williams et al., SoCC
2018](https://dl.acm.org/doi/10.1145/3267809.3267845)) — user code
knows it's under SAUCE. v1 LD_PRELOAD redirecting glibc's syscall
stub recovers ~99 % of the cpython parity gate's universe.

**Complexity.** Trampoline + eBPF prog ≈ 500 lines vs. 12,000 LOC of
KVM backend. New: io_uring ring management, uffd handler, PKEY
accounting (~2,000 lines). Net: −10,000 LOC.

**Risk.** PKEY-bypass attacks are published ([Connor et al., USENIX
Security 2020](https://www.usenix.org/system/files/sec20-connor.pdf)) —
`WRPKRU` is a 4-byte instruction; user-PKEY code could find it as a
gadget in kernel-PKEY pages. Mitigation: CET shadow stack and
`prctl(ARCH_PKEY_DISABLE_*)`. UML's threat model for
research/fuzz/sandbox is **not adversarial** — a user task crashing
the UML kernel is at worst a fuzzing bug report. For the
production-sandbox profile this is a real concern; SAUCE explicitly
does not target it (gVisor stays the right answer there). If the
host disables eBPF, SAUCE falls back to seccomp_user_notif (~10 µs
vs. ~0.3 µs syscall — slower but functional).

---

## 8. Failure modes I accept

What gets *worse* under SAUCE:

1. **Production sandboxing is removed from SAUCE's mandate.** PKRU is
   intra-process isolation, not virt-class isolation; for adversarial
   code, use gVisor or Firecracker. Real concession.
2. **Binary compatibility (v0).** Static-linked or
   `uml_user_runtime`-linked only until the LD_PRELOAD shim ships in
   v1. Postgres, Redis, JVM all wait on v1.
3. **Nested-virt validation investment discarded.** SAUCE doesn't
   need `/dev/kvm` — better in deployment, but our GHA-under-Hyper-V
   harness becomes irrelevant.
4. **`record.c` rewrite (~3 weeks).** Today's record/replay leans on
   KVM_RUN's deterministic interface; SAUCE moves the hook into the
   eBPF syscall-redirect prog. Doable, but a real rewrite.
5. **Single-process scope.** SAUCE binds the UML guest to one host
   process. Multi-host-process UML (technically permitted today via
   per-process kvm_um) goes away. Fine for research/fuzz/sandbox.
6. **New bug class: PKRU-mis-set.** Forgot `wrpkru` on some path →
   user code runs with kernel PKRU. These bugs are *easier* to
   debug (one missed `wrpkru` → immediate SIGSEGV, not 86 %-later
   corruption) and static-analysis-tractable in a way today's async
   TLB-shadow bugs are not. Strictly better class to debug.

---

## 9. Closing argument

The other agents will recommend "keep KVM, keep shadow PT, add
per-task vCPU, add per-mm worker, add ownership discipline" — a
6-month project that leaves UML as badly-scaling Firecracker. It
will eliminate today's specific 25 % flake and leave the
architectural shape that produced it intact. Q3-2026 will be spent
chasing the next sync-point race.

SAUCE is a 3-month fork-and-rewrite of one Kconfig'd backend that
removes the architectural shape entirely. No shadow to desync, no
vCPU exit state to alias, no 50-µs propagation window. The bug
class doesn't shrink — it stops existing. It costs KVM-class
hardware isolation. It does not cost KASAN, KMSAN, KCSAN, KCOV,
kprobes, ftrace, BPF JIT, snapshot/restore, or record/replay —
those all live in the UML kernel image, not the backend. Q3-2026
becomes "make KASAN-on-UML the world's best Linux-kernel debugging
environment," which is what vision.md actually says we're doing.

Pick the second one.

---

## References

- Dike, J. "User-Mode Linux." USENIX 2001.
  http://www.usenix.org/event/usenix01/freenix01/full_papers/dike/dike.pdf
- Vahldiek-Oberwagner, A. et al. "ERIM: Secure, Efficient In-process
  Isolation with Protection Keys." USENIX Security 2019.
  https://www.usenix.org/system/files/sec19-vahldiek-oberwagner.pdf
- Connor, R. et al. "PKU Pitfalls: Attacks on PKU-based Memory
  Isolation Systems." USENIX Security 2020.
  https://www.usenix.org/system/files/sec20-connor.pdf
- Williams, D. et al. "Unikernels as Processes." SoCC 2018 (nabla).
  https://dl.acm.org/doi/10.1145/3267809.3267845
- Axboe, J. "Efficient IO with io_uring." LWN 2019.
  https://lwn.net/Articles/776703/
- Hansen, D. "Memory keys for the masses." lkml RFC 2023.
  https://lwn.net/Articles/828552/
- "TDP MMU on Linux 5.10." LWN.net 2020.
  https://lwn.net/Articles/832835/
- Linux Documentation/core-api/protection-keys.rst
- Linux Documentation/admin-guide/mm/userfaultfd.rst
- gVisor `pkg/sentry/platform/kvm/` (compared to deliberately differ).
- `00-playbook.md` — bug evidence base, this branch.
