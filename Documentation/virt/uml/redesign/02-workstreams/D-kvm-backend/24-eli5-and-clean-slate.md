# Memo 24 — ELI5 of UML KVM, and 10 things a clean-slate would do differently

**Date:** 2026-04-28
**Audience:** Anyone joining the project who needs to understand WHY the
codebase looks the way it does — and why we keep finding bugs in the
same general area
**Companion:** Memo 23 has the concrete plan to fix what we have. This
memo zooms out to the strategic shape.

---

## Part 1 — ELI5: what is UML, what is the bug class, why is it hard

### What UML is

User-Mode Linux (UML) is "Linux running as a Linux process." You take
the Linux kernel source, compile it for the `um` architecture, and the
result is an executable. Run it on a host Linux machine, and it boots
its own Linux kernel inside the host process. Each "process" inside the
guest UML maps to a host process or thread, depending on the backend.

There are three backends that drive the guest:

1. **ptrace** (legacy, deprecated): every guest syscall traps into a
   ptrace stop; UML acts as the ptrace controller. Slow, simple, mostly
   removed.
2. **seccomp**: each guest task runs as a host process under a seccomp
   filter that diverts syscalls to UML via a shared-memory protocol.
   Modest speed, simple state model.
3. **KVM**: each guest task runs inside KVM_RUN; guest CPU executes
   real x86_64 instructions; UML only takes control on KVM exits
   (syscalls, faults, host signals). Fastest path, **most complex
   state model**.

### What the bug class is

The KVM backend has had a steady stream of subtle correctness bugs:
TLB-shootdown gaps, shadow PT staleness, US-violation aliases, register
marshalling races, signal-kick interference. Each fix uncovers another
bug in the same general area: the "shadow page table" (shadow PT)
machinery that bridges UML's memory layout to what KVM expects.

### Why it's hard — the puppet-stage analogy

Imagine a play put on by a single puppeteer (the UML kernel) holding
many puppets (guest processes). The puppeteer used to manually move
each puppet — slow but every motion is under direct control. That's the
seccomp backend.

Then someone said: "let's give the puppets motors so they can move
themselves" (the KVM backend, hardware virtualization). Faster, less
work for the puppeteer. Catch: **the stage was built for the
puppeteer-driven version.** Props are in odd places (kernel direct map
mixed with user VAs), the lighting rig dangles into the audience
(`uml_physmem` at PML4[0]), the puppeteer's break room overlaps the
puppets' stage area.

To make motorized puppets work on this stage, we built a **mirror
stage** (shadow PT) that re-arranges everything into a layout the
motorized puppets understand. The puppets look at the mirror; the
puppeteer keeps the real stage in sync with the mirror. When the real
stage changes (puppeteer moves a prop), we update the mirror. When the
mirror gets out of sync, weird things happen — a puppet reaches for a
prop that's already been moved, grabs empty air, and everyone sees the
puppet trying to drink from an invisible cup.

We've spent months tuning the sync between the two stages. Each fix
addresses one class of "things going out of sync" but the fundamental
issue is that **two stages exist in the first place**.

### The fix in one sentence

Stop maintaining a mirror stage. Either:
- **Rearrange the real stage** so motorized puppets can use it directly
  (UML core changes — uml_physmem to PML4[256+], etc.), AND/OR
- **Let KVM walk the real pgd directly** (the TDP path — KVM has
  built-in support for this and handles all the cross-vCPU coherence
  via mmu_notifier).

Both moves let us delete the ~3,500 lines of shadow PT code and inherit
KVM's well-tested invalidation logic.

### Where we are right now

After ~6 months of incremental fixes (Stage A: per-task vCPU,
KVM_SET_SIGNAL_MASK; the audit fixes SEC.1/SEC.2/BUG.1-5; the recent
A.4d/A.4e/A.4i discoveries), the cpython-parity gate sits at mean
19.4/21 single-pass post-A.4i (was 18.6/21). Memo 23 lays out the path
to 21/21 reliably:
- Phase 1: commit A.4i (gives us the 19.4/21 baseline).
- Phase 2: find Bug B's specific corruption source (3-7 days).
- Phase 3: Stage B (4-6 weeks) — TDP + memslots, deletes the shadow PT.

Even after Stage B ships, the underlying memory-model mismatch (UML
uses PML4[0] for kernel direct map) remains. The 10 items below say
what we'd do if starting over.

---

## Part 2 — 10 things a clean-slate UML KVM backend would do differently

Each item lists: **the problem we have today**, **why it matters**,
**what a clean slate would do**, and (where applicable) **which current
code paths would be deleted or simplified**.

---

### 1. Put the kernel direct map at PML4[256+], not PML4[0]

**Problem today.** UML allocates `uml_physmem` at host VA `0x60000000`
with size `0x20000000` — the kernel direct map sits at
`[0x60000000, 0x80000000)`. That entire range lives in **PML4[0]**
(the user-half of any x86_64 page table). Every guest user mm walks
PML4[0] for any low VA and finds the kernel direct map there.

**Why it matters.** This is the **single biggest bug magnet** in the
codebase:

- **Bug A** (memo 22): the shadow-PT bootstrap-page install used host
  kernel VAs as guest VAs. Those VAs lived in PML4[0]. User code at
  CPL=3 walked PML4[0] for low VAs, hit the bootstrap leaves with US=0,
  US-violation #PF.
- **The 1GB-huge-page blocker** (memo 20 §9.5): `init_mm.pgd` PUD[1]
  is a 1GB huge page covering `[0x40000000, 0x80000000)` with US=0.
  Setting guest CR3 = `__pa(mm->pgd)` triple-faults because user code
  hits the huge page. Stage B can't simply swap CR3 until this is
  resolved.
- **BUG.1** (memo 22): `kvm_mm_map` had to add a `kvm_mm_map_collides_kernel`
  shield because user mmaps at MAP_FIXED could land in
  `[uml_physmem, uml_physmem+physmem_size)`, clobbering kernel
  bootstrap pages. The shield is defensive; the underlying problem is
  the layout.

**What clean-slate would do.** Allocate `uml_physmem` (and the entire
kernel direct map) at PML4[256+], which is the canonical x86_64 kernel
half (`0xffff_8000_0000_0000+`). User mappings stay in PML4[0].
**User CPL=3 walks of low VAs never reach kernel pages.** All three
bugs above structurally vanish.

**What gets deleted.** The BUG.1 shield in `arch/um/backend/kvm/mm.c:183`.
The §9.5 blocker. The bootstrap-VA-in-user-half sub-class of bugs
(Bug A and any future variants).

**Cost.** Substantial UML-core change. Touches every `__pa`/`__va`
site that assumes the current layout (estimated 50-200 sites in
`arch/um/kernel/mem.c`, `arch/x86/um/asm/`, `arch/um/include/asm/`).
Boot-order verification needed — `uml_physmem` is referenced very
early.

---

### 2. Per-mm host process, not per-mm shadow page table

**Problem today.** The KVM backend runs all guest mms in **one host
process**. To give each guest mm its own user VA space without VA
collisions, it builds a parallel shadow PT per mm and swaps the guest
CR3 to point at the right shadow on context-switch.

**Why it matters.** Every cross-mm scenario becomes a state-management
problem:
- BUG.1: two mms with user mappings at the same guest VA collide on
  the host VA (same host process, same VA space).
- BUG.2: shadow invalidation targeted `current->active_mm` instead of
  the passed `mm_id`. UML's `kvm_context_switch` runs `set_current(to)`
  before draining `prev`'s pending PTE updates; without the BUG.2 fix,
  invalidates went to the wrong shadow.
- The whole `dirty/synced/needs_full_resync` state machine in
  `lifecycle.c` exists to track which mm's shadow needs which work.
- Cross-vCPU TLB shootdown (A.4f, three failed attempts) was needed
  because shadow PT can be stale across vCPUs in different mms.

**What clean-slate would do.** **Each guest mm gets its own host
process**, exactly like the seccomp backend. The host process's user
VA space IS the guest mm's user VA space — no shadow PT needed. KVM_RUN
runs in the appropriate worker process. Cross-mm collisions become
structurally impossible (different processes have different VA spaces).

**What gets deleted.** All of `arch/um/backend/kvm/shadow_sync.c`
(~487 lines). Most of the shadow PGD apparatus in `lifecycle.c`
(~2000 lines). The dirty/synced/needs_full_resync state machine. The
F12 mutation ring. The kvm_shadow_sync_pte hook. **Roughly 3,500 LoC
of shadow PT code.**

**Cost.** Significant: now requires inter-process KVM_RUN dispatch
(probably via a UNIX socket protocol), per-process memslot table,
careful cleanup on mm-exit. But this is the well-trodden seccomp model
applied to KVM.

---

### 3. Use TDP from day one. Never write a shadow PT.

**Problem today.** Even if we keep one host process for all mms (#2's
opposite), we should still use KVM's Two-Dimensional Paging (TDP / EPT)
to walk `mm->pgd` directly instead of building a shadow.

**Why it matters.** KVM's TDP is **already implemented and well-tested**.
It uses mmu_notifier to invalidate EPT entries when the host mm's page
tables change, handles cross-vCPU shootdown via
`kvm_make_all_cpus_request(KVM_REQ_TLB_FLUSH)`, integrates with
hugepage promotion/demotion, and has years of in-kernel-KVM use across
every major hypervisor. Our shadow PT reinvents all of this.

The three diagnostics in memo 22 (force-resync, EPT-flush,
TLB-shootdown) all 100% fail because they're patching shadow-PT-specific
behavior. With TDP, none of those diagnostics would even apply.

**What clean-slate would do.** Set guest CR3 = `__pa(mm->pgd)`
directly. Map all UML host memory into KVM via memslots (per-mmap or
one giant slot). KVM's TDP walks `mm->pgd` and translates GPA → HPA via
the memslot. mmu_notifier catches every host-side mmap/munmap and
invalidates EPT automatically.

**What gets deleted.** Same as #2: ~3,500 LoC. Plus the entire
"shadow staleness" debugging surface (memo 14, memo 17, memo 21,
memo 22's diag1+diag2).

**Cost.** Requires #1 (move uml_physmem out of PML4[0]) so the CR3
swap doesn't triple-fault. After that, simpler than what we have.

---

### 4. One vCPU per host CPU, not per task

**Problem today.** Stage A introduced "per-task vCPU" because the
pre-Stage-A singleton vcpu0 was racy (one shared `kvm_run` mmap across
all tasks). The fix was correct for the bug at hand but inherited a
non-standard model: a busy UML workload can have 1000+ vCPUs created
across the run.

**Why it matters.** Every other guest OS uses N vCPUs (= host CPU
count) and **schedules guest threads onto vCPUs the same way Linux
schedules host threads onto CPUs**. UML's per-task model:
- Pays `KVM_CREATE_VCPU` overhead per task (vCPU FD, kvm_run mmap, KVM
  internal state allocation).
- Skips KVM's well-tuned vCPU thread affinity / wake-from-halt /
  preemption-disable machinery (because each "vCPU" is its own host
  thread that hardly ever migrates).
- Created the cross-vCPU TLB-shootdown requirement (A.4f). With
  per-CPU vCPUs, a task migration between CPUs is just a normal
  scheduler event; KVM handles the TLB.

**What clean-slate would do.** N vCPUs (= `nr_cpu_ids`). UML's
scheduler picks which vCPU runs which task next. On entry, set
guest CR3 = `__pa(task->mm->pgd)` and `kregs.rip = task->user_rip`.
On exit, save user state back to the task struct. Like every other
guest OS.

**What gets deleted.** The per-task `kvm_vcpu_handle_alloc` path. The
"who owns the kvm_run mmap" race-class. The cross-vCPU shootdown
investigation work (A.4d producer side, A.4f all three attempts).

**Cost.** Requires #7 (proper SMP from day one) since a meaningful
per-CPU vCPU model is intrinsically SMP.

---

### 5. Use VMCALL hypercalls, not "OUT to magic port"

**Problem today.** When the guest wants to make a syscall, it executes
a SYSCALL instruction → CPU jumps to MSR_LSTAR → which points at our
"LSTAR trampoline" in the bootstrap page → which executes `out` to a
magic port (`UM_KVM_SYSCALL_PORT`) → which causes `KVM_EXIT_IO` →
which UML decodes as "guest wants a syscall, look at RAX/RDI/etc."

**Why it matters.** This is **clever but indirect**. Each syscall is:
SYSCALL → trampoline → out → KVM_EXIT_IO → port-number decode →
syscall dispatch. The trampoline lives in shadow PT (which #3 deletes).
The port-number is a UML-specific magic value. The exit reason
(KVM_EXIT_IO) is overloaded — we have to inspect the port to
distinguish "guest syscall" from "guest tried to access a real I/O
port" (which we now SIGSEGV after SEC.1).

**What clean-slate would do.** Use KVM's first-class hypercall
facility:
- Guest does `vmcall` (with hypercall number in RAX, args in RBX/RCX).
- KVM exits with `KVM_EXIT_HYPERCALL` and `kvm_run.hypercall.nr`.
- UML dispatches based on the hypercall number directly.

No trampoline, no shadow-PT install for it, no magic port number.
KVM exposes `KVM_HC_*` constants for this exact use case.

**What gets deleted.** The LSTAR trampoline (lives in
`kvm_bootstrap_lstar_bytes[]` in `lifecycle.c`). The MSR_LSTAR install
in `kvm_enter_guest_program_msrs`. The KVM_EXIT_IO + port-decode
machinery in the dispatch loop. The whole "where does the trampoline
live in shadow PT" question (Bug A's home).

**Cost.** Need to modify the guest (UML kernel) to use `vmcall`
instead of `syscall` for its system-call boundary. That's a
relatively contained change in `arch/um/include/asm/syscall.h` and
the syscall entry assembly.

---

### 6. No bootstrap pages installed in user mms

**Problem today.** Every `kvm_enter_guest` installs 4 pages into the
shadow PT for the current mm:
- bootstrap code+tables (IDT/GDT/TSS/LSTAR/IST stack/SYSRETQ gadget)
- per-vCPU gadget state
- per-vCPU gadget vvar
- IST stack page

These pages **must be reachable from guest CR3** because the CPU walks
guest page tables for IDT base, TSS base, LSTAR target, IST stack push,
etc.

**Why it matters.** Bug A (memo 22's root cause) was: those bootstrap
pages were installed at `kvm_bootstrap_va` which lived in user-half
PML4[0] with US=0. User CPL=3 walks of nearby VAs hit them and
US-violation crashed. The current A.4i fix moves them to PML4[508]
(kernel-half) — works, but the entire concept of "we inject pages
into the user's mm pgd" is fragile.

**What clean-slate would do.** Use KVM's existing mechanisms for
each piece:
- IDT: `KVM_SET_SREGS.idt.base` accepts a guest VA. Install IDT
  contents in a memslot mapped at a known kernel-half VA shared by all
  mms.
- TSS: `KVM_SET_TSS_ADDR` for the per-VM TSS region (KVM has explicit
  support for guest TSS placement; on Intel it's required for unrestricted
  guest mode).
- LSTAR: with #5, no LSTAR trampoline needed at all.
- IST stack: per-vCPU dedicated guest physical pages, mapped via
  memslot, never appear in user mms.

**What gets deleted.** The entire `kvm_bootstrap_*` machinery in
`thread.c` (~600 LoC of bootstrap install / IDT-byte-write /
LSTAR-trampoline-build code). The `KVM_BOOTSTRAP_GUEST_VA` constant
(A.4i's introduction).

**Cost.** Build proper memslot infrastructure first (which Stage B
needs anyway), then incremental.

---

### 7. Build for SMP from day one

**Problem today.** UML had `panic("um: kvm: ncpus > 1 is not
supported")` until commit `c3f630fee8ba` (April 2026). The
single-CPU restriction existed because:
- `switch_threads` (the cooperative scheduler) uses `longjmp`, only
  works when one thread runs at a time.
- The shadow PT had no concurrent-modification protection.
- The bootstrap pages used a single global IRETQ frame staging area
  (memo 18 fixed this with per-mm storage).
- No cross-vCPU TLB shootdown (A.4f).

The SMP unblock was a flag flip; under load, the un-fixed
synchronization assumptions surfaced as race classes B, C, D in the
memo-19 playbook.

**Why it matters.** Retrofitting SMP onto a fundamentally
single-CPU-cooperative codebase is endless. Every "race class" is
"we forgot this one needed locking." A.4d (per-vCPU TLB tracking),
A.4e (KVM_RUN preemption via SIGALRM), A.4f (cross-vCPU shootdown)
are all SMP debt that wouldn't exist if the code was SMP from day one.

**What clean-slate would do.** Pick a real synchronization model up
front:
- Per-CPU vCPU (#4), one thread per vCPU.
- Per-mm rwsem for shadow PT / memslot updates (or with #3, no shadow
  PT to lock).
- KVM handles cross-vCPU TLB via mmu_notifier + KVM_REQ_TLB_FLUSH.
- UML scheduler treats vCPUs like Linux's CPUs.

**What gets deleted.** The cooperative-thread `switch_threads` model
— replaced with standard kernel-style scheduling. The A.4d/A.4f
investigation work. The per-mm IRETQ frame logic (with #6, this lives
in dedicated guest pages, not in user mms).

**Cost.** Substantial. This is "rebuild the UML scheduler" not
"patch a few sites."

---

### 8. Standard threading model, no clone() flag combinatorics

**Problem today.** UML uses
`clone(userspace_tramp, sp, CLONE_VFORK | CLONE_VM | SIGCHLD)`
in `arch/um/os-Linux/skas/process.c:494`. That's:
- CLONE_VM: shares VM (mm)
- CLONE_VFORK: parent waits until child execs/exits
- NO CLONE_SIGHAND: signal handler tables are NOT shared
- NO CLONE_FILES: fd tables are NOT shared
- NO CLONE_THREAD: separate process (separate tgid)

So each UML "task" is a **separate host process** that shares VM with
the others. Each has its own:
- Signal handler table (the A.4f kick failed because new tasks
  inherited the kick handler at clone time, not afterward — subtle
  ordering issue).
- fd table (each task opened its own KVM_CREATE_VCPU fd).
- Sigmask (sigprocmask is per-thread; each task sets its own).

But shares:
- VM (every task can read every other task's memory at the same VAs).
- TLB? No — TLB is per-CPU, but each "task = process" has its own
  CR3 anyway under shadow PT.

**Why it matters.** This hybrid is the worst of both worlds:
- Per-process: pay process-creation cost for every guest task (clone
  is heavier than pthread_create).
- Shared VM: collisions when two tasks pick the same user VA via
  MAP_FIXED. Exactly BUG.1's mechanism.
- Per-process signal handlers: A.4f's three failed attempts to send
  cross-vCPU kicks tripped on this.
- Per-process fd tables: each task has its own vcpu_fd, can't share
  KVM context as easily.

**What clean-slate would do.** Either:

A. **Standard pthreads** (`pthread_create`): shared everything, including
   signal handlers and fd tables. Use locks for actual sharing
   conflicts. Cheaper, more standard, but requires the per-mm-host-
   process boundary moved up to a per-VM-host-process boundary
   (combine with #2: each guest mm IS a separate host process via
   fork/clone from a master).

B. **Per-mm host worker process** (#2's recommendation): each guest mm
   is its own host process. No CLONE_VM. Standard `fork()` for new
   mms. Inside each worker process, use pthreads for vCPUs (#4).

Either way: **no more clone-flag combinatorics.** Pick a lane.

**What gets deleted.** The `userspace_tramp` clone path. Per-task
sigmask juggling. Some of the BUG.1 collision logic. The "kick handler
inheritance ordering" worry from A.4f.

**Cost.** Big. UML's task model is foundational.

---

### 9. First-class observability from day one

**Problem today.** Our debugging story is **a pile of ad-hoc telemetry,
each piece added when a specific bug forced us to**:

| Telemetry | Memo / commit | Why we needed it |
|---|---|---|
| F12 mutation ring (`shadow_sync.c`) | memo 15 | Trace shadow PT writes around a fault |
| dirty/synced/needs_full_resync counters | memo 14 | Race-class B investigation |
| `mut_dump` on panic | memo 19 | Show recent shadow mutations near cr2 |
| `pf_mini_regs` in #PF handler | memo 22 | Bug A diagnosis |
| `kvm_diag_*` knobs (DIAG[N] ring) | T.1 | Trace mm-syscall sequence |
| #GP register dump | BUG.5 / memo 22 | Capture register state at #GP |
| pgd-walk diagnostic (B.1 path) | memo 20 | Compare shadow vs UML pgd |
| `bootstrap_va_get` accessor | A.4i | Preserve aliases in fill |

Each piece compiled in (sometimes behind a knob), each piece had to be
WRITTEN AT THE TIME WE HIT THE BUG. We retroactively added
instrumentation when the bug was already in production code.

**Why it matters.** Memo 22's "smoking gun" — `shadow=0xaca021 DIVERGE
synced=1` — would have been printed on the **first** failing run if we
had structured tracing on every shadow PT leaf write. We spent 3
failed-fix attempts (A.4f v1, v2, v2-with-sigprocmask) and 2
diagnostics (force-resync, EPT-flush) before the Opus sub-agent did the
call-site enumeration that found Bug A. **All of that could have been
"oh, the panic dump shows leaf 0xaca021 at va=0x60aca___ was installed
by the bootstrap-install code path 17000 mutations ago — bootstrap
install needs to move."**

**What clean-slate would do.** **Build observability in from day
one:**
- A tracepoint on every guest exit (reason, vCPU id, RIP).
- A tracepoint on every `kvm_mm_map`/`kvm_mm_unmap` call.
- A tracepoint on every memslot add/delete (post-Stage-B).
- A tracepoint on every IDT/GDT/MSR programming.
- Per-vCPU exit-reason histograms exposed via `/proc`.
- An eBPF-friendly trace point on every hypercall (#5 makes this
  natural).
- Per-page bytes-out-vs-bytes-in checksum on critical pages
  (bootstrap, gadget state) so corruption is detected at the source.

With ftrace/eBPF tooling, **bug investigation becomes "look at the
trace" instead of "add instrumentation, rebuild, hope to repro."**
Bugs we spent weeks on become one-day finds.

**What gets deleted.** All the ad-hoc telemetry pile. Replaced with
structured tracepoints that integrate with standard Linux tools.

**Cost.** Up front: design and implement tracing infrastructure.
Long-term: **massively cheaper** because we stop reinventing it per-bug.

---

### 10. Pick one identity: developer tool OR production VMM

**Problem today.** UML is awkwardly both:

- **As a developer tool:** "I want to debug Linux kernel code without
  rebooting." Wants ptrace/seccomp simplicity, lots of state inspection,
  step-debugging via gdb, fast turnaround on edits, slow execution is
  OK, single-user-host is OK.
- **As a production VMM:** "I want a lightweight container/VM for
  sandboxing." Wants KVM-fast, concurrent (SMP), hardened against
  guest escape, snapshot/restore, multi-tenant, real I/O performance.

These goals **pull in opposite directions on every design call**:
- Memory layout: dev tool wants whatever's convenient; VMM wants
  standard.
- Threading: dev tool wants single-threaded for gdb; VMM wants SMP.
- Shadow vs TDP: dev tool's slow path is fine; VMM needs zero-overhead
  walks.
- Bootstrap pages in user mms: dev tool tolerates the kludge; VMM
  shouldn't have any kernel state in user mms.
- Observability: dev tool wants printk; VMM wants ftrace.

The current UML codebase contains compromises in both directions, and
the KVM backend exists to bolt VMM-grade speed onto a dev-tool memory
layout. **Every painful bug we've hunted is at this seam.**

**What clean-slate would do.** Two separate products:

A. **`umlite` (the dev tool):** seccomp-only, single-CPU, deep
   integration with gdb/ftrace, optimized for "drop into the failing
   syscall and inspect state." Don't bother with KVM; use the simple
   ptrace-style backend. 5-10 KLoC instead of 100+. No shadow PT, no
   memslots, no bootstrap pages.

B. **A real type-2 hypervisor:** drop the "running as a process"
   pretense entirely. The guest is unmodified Linux, no `arch/um/`
   anything. Use kvmtool or qemu's tcg-disabled mode. Pre-existing
   maturity, no shadow PT (uses TDP/EPT correctly), real SMP, real
   I/O (vhost). For containerization use cases, gVisor and Firecracker
   already exist with this shape.

**What this means strategically.** The current UML KVM project is
trying to compete with Firecracker on perf and gVisor on safety while
also trying to be a kernel-debug tool. **We don't have to.** Pick the
identity that matters most:
- If kernel debugging: ditch the KVM backend, double down on seccomp.
- If lightweight VM: drop the `arch/um/` model, use kvmtool.

The current path (Stage A→B→C) is "make UML KVM as good as a real
hypervisor while keeping the UML model." That's a multi-year program
with diminishing returns. **Picking one identity would be done in
months.**

**What gets deleted.** Half of `arch/um/` if A. All of `arch/um/` if B.
This is the most radical recommendation here and intentionally so —
the current dual-personality is the deepest reason why the same bug
classes keep recurring.

---

## Part 3 — What this means for our current trajectory

The current Stage A → Stage B → Stage C plan retrofits **#1 + #3 + #6
+ a partial #7** onto the existing UML codebase. That's 4 of the 10
items, applied to the existing model:

- **Stage A** (landed, committed): partial #4 (per-task vCPU is a
  Stage-A correction, but lands per-task not per-CPU); partial #7
  (SMP unblocked, but synchronization model not redesigned).
- **A.4i** (uncommitted): partial #1 (move the bootstrap install to
  PML4[256+] without moving uml_physmem itself); partial #6 (use a
  kernel-half VA for bootstrap install).
- **Stage B** (planned, memo 23): full #1 + #3 + #6. Removes shadow
  PT. Requires the 1GB-huge-page blocker resolution (full #1).

Items **#2** (per-mm host process), **#4** (per-CPU vCPU), **#5**
(VMCALL hypercalls), **#8** (clone-flag cleanup), **#9** (observability),
**#10** (pick an identity) are out of scope for the current plan and
would require a much larger redesign.

That's an honest accounting. The current path is the **right pragmatic
move** because rebuilding UML from scratch isn't on the table — but
it's good to know that we're patching a model whose original design
goals don't match what we're trying to build with it.

---

## Reading order if you're new to this

1. This memo (24) — strategic context.
2. Memo 23 — concrete plan to fix what we have.
3. Memo 22 — current bug diagnosis (Bug A fixed, Bug B open).
4. Memo 21 — investigation log of failed approaches (so you don't
   repeat them).
5. Memo 20 — Stage B design (the structural fix).
6. `Documentation/virt/uml/redesign/03-architecture-review-2026-04-27/`
   — four independent agent reviews from earlier in the project.
7. Code: start at `arch/um/backend/kvm/thread.c`'s `kvm_enter_guest`
   and follow the call graph.

Welcome to the project. Have fun with the puppet stage.
