# 09 — Fork-server external research (2026-05-20)

External research conducted in response to the Phase 2a v1-ceiling
hazard documented in `09-fork-server-STATUS.md`.  Question driving the
search: how do other projects fork a host process from inside a host-
syscall-handler context, and has anyone hit the specific symptom of
saved-RIP corruption on the kernel stack after such a fork?

This memo is organised in the four sections requested by the brief:

  1. Per-project findings (10 categories).
  2. Has the exact symptom been hit by anyone else?
  3. UML-specific LKML / GitHub mirror search.
  4. Single most-promising lead with verification recipe.

The bottom-line conclusion sits at §4 — the rest is the substantiation.

---

## 1. Per-project findings

### 1.1 CRIU — Checkpoint/Restore in Userspace

CRIU does NOT fork from inside a syscall handler in the sense Memo 09's
fork-server attempts.  Its mechanics are nonetheless instructive
because they show how a tool that DOES restore arbitrary process state
from outside the process avoids the pitfalls we are hitting.

The key design choices documented:

  * **Parasite injection via ptrace**.  CRIU PTRACE_SEIZEs the target,
    injects a small mmap(2) syscall at the current `rip`, and uses the
    new mapping to host a "parasite" blob that dumps state from inside
    the target's address space.  See
    <https://criu.org/Parasite_code>.

  * **Restorer runs in a custom address space**.  When restoring, CRIU
    "morphs" itself into the target — but only after the restorer
    blob has remapped the target's memory layout.  See
    <https://criu.org/Restorer_context>:
    > "The restorer context is the last stage of the restore process.
    > It differs from the regular CRIU's context like the parasite code
    > does — it doesn't have any libraries, it is PIE-compiled and can
    > only work on fixed amount of memory."

  * **Final register restore is atomic via rt_sigreturn(2)**.  The
    restorer builds a `struct rt_sigframe` containing the saved CPU
    state, then calls `rt_sigreturn` with `rsp` pointed at that frame.
    The host kernel restores ALL registers (including `rip`, `rflags`,
    `rsp`) in one atomic transition.  See
    `criu/pie/restorer.c`, `rst_sigreturn()` at line 1079, called from
    `__export_restore_thread()` at line 1265:
    <https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>.

    Quote from the comments and code:
    > `static void noinline rst_sigreturn(unsigned long new_sp,
    > struct rt_sigframe *sigframe) { ARCH_RT_SIGRETURN_RST(new_sp,
    > sigframe); }`

  * **fork() during restore is used to RE-CREATE the process tree**,
    not from inside an active syscall.  CRIU's main process forks to
    spawn the not-yet-restored tasks; each fresh task then mmap's the
    restorer blob and trampolines into it via rt_sigreturn.  See
    <https://criu.org/Tree_after_restore>:
    > "CRIU first forks all the tasks in respective order, then opens
    > required files and then calls execve()... at one step CRIU calls
    > fork() many times to re-create the processes needed to be
    > restored."

  * **No mid-syscall fork**.  Throughout CRIU's documentation we
    could not find any reference to forking from inside an in-flight
    syscall handler.  The closest analog is the parasite blob, which
    runs from a *paused* target — the target is stopped on
    `rt_sigreturn` (see <https://criu.org/Parasite_code>) before any
    fork-like operation runs.

  **Takeaway for UML**.  CRIU's "atomic register swap via
  rt_sigreturn" is the canonical way to leave a corrupted-stack
  context behind and resume with a fresh one.  If we could build a
  `rt_sigframe` in the forked child that points at a known-good
  re-entry point and `rt_sigreturn` to it, we would side-step the
  bad-saved-RIP-on-the-kernel-stack hazard entirely.  This is the
  germ of the §4 recommendation.

---

### 1.2 gVisor — Sentry + Stub model

gVisor is the closest architectural analog to UML: a userspace
"kernel" (the Sentry) runs guest userspace code in stub processes via
ptrace/seccomp/KVM/systrap.  Documentation overview:
<https://gvisor.dev/docs/architecture_guide/intro/>.

Searched the ptrace platform (`pkg/sentry/platform/ptrace/`),
specifically `subprocess.go`:
<https://github.com/google/gvisor/blob/master/pkg/sentry/platform/ptrace/subprocess.go>.

Key findings:

  * **Stubs are created lazily from a goroutine** dedicated to
    handling clone requests via a channel.  Quote (from the file's
    `newSubprocess` doc-comment):
    > "newSubprocess returns a usable subprocess.  This will either
    > be a newly created subprocess, or one from the global pool.
    > The create function will be called in the latter case, which
    > is guaranteed to happen with the runtime thread locked."

  * **Stub pool to avoid creation/destruction cost and "Subprocesses
    can't always be killed properly"**.  The pool implies the Sentry
    AVOIDS repeatedly creating stub processes per request.

  * **Explicit awareness of thread-corruption hazards** in a TODO:
    > "TODO(b/77216482): Other parts of this package depend on
    > threads never exiting."

  * **gVisor does not fork the Sentry itself**.  Each runsc invocation
    boots a fresh Sentry; runsc never tries to re-use a single Sentry
    for multiple sandboxes via fork.  This is a deliberate design
    choice — the Sentry is single-threaded around its critical paths
    and is single-purpose per container.

  Quote from <https://gvisor.dev/docs/architecture_guide/security/>:
    > "Sandboxes need to be created fork-safe: if the host process
    > creating sandboxes runs an async runtime, forking from a
    > multithreaded process is inherently unsafe."

  **Takeaway for UML**.  gVisor explicitly REFUSES to do what we are
  attempting (fork a long-lived multi-threaded application kernel).
  Their answer is full process restart per sandbox — analogous to the
  Memo 09 alternative of N cold UML boots.  The trade-off is exactly
  the latency cost we are trying to avoid.

---

### 1.3 AFL++ — forkserver

AFL++'s forkserver is the closest *working* analog in spirit, and
several Phase 2a comments in our tree reference AFL invariants ("AFL
v1-friendly ready point").  Read the implementation directly:
<https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/afl-compiler-rt.o.c>

Key findings:

  * **The forkserver is installed as a `__attribute__((constructor))`.**
    See line 1604 of `afl-compiler-rt.o.c`:
    ```c
    /* Initialization of the forkserver - latest possible */
    __attribute__((constructor())) void __afl_auto_init(void) {
    ```
    And the early variant at line 1632:
    ```c
    __attribute__((constructor(EARLY_FS_PRIO))) void __early_forkserver(void) {
      if (getenv("AFL_EARLY_FORKSERVER")) { __afl_auto_init(); }
    }
    ```
    `EARLY_FS_PRIO = 5` (line 120).  This runs before main, before any
    application thread has started, and crucially — **the process is
    NEVER mid-syscall when fork() runs**.  The forkserver runs in a
    main-thread-only context, blocks on `read(FORKSRV_FD, ...)`, and
    only forks after receiving a 4-byte command from the fuzzer.

  * **The fork loop is plain `fork()`, not a raw syscall**.  Line ~1338:
    ```c
    child_pid = fork();
    if (unlikely(child_pid < 0)) {
      write_error("fork");
      _exit(1);
    }
    ```
    AFL++ does NOT bypass glibc.  It relies on the process being
    structurally simple (no pthreads running, no in-flight syscalls)
    so that glibc's fork is safe.

  * **The child does almost nothing before returning to user code.**
    Signal handlers are reset, the control fds are closed, and the
    child returns to `main()` (or `__AFL_LOOP`).  No "drop inherited
    runqueue", no "respawn stub children" — because the AFL forkserver
    process has none of those things.

  * **Persistent mode adds a `SIGSTOP/SIGCONT` per iteration** to
    avoid re-forking each time.  Comment in
    `instrumentation/README.persistent_mode.md`:
    <https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/README.persistent_mode.md>.

  **Takeaway for UML**.  AFL works because the forkserver is set up
  at constructor time, BEFORE the process has any complex state.  Our
  template-pause path is the OPPOSITE — by the time the proc-write
  trigger fires, init.sh has run, SKAS stub children exist, UML's
  signal handlers are installed, UML's POSIX timers are armed, and we
  are mid-`sys_write`.  AFL's design assumptions do not hold.

---

### 1.4 WASM runtimes (Wasmtime, WasmEdge)

Search:
<https://github.com/bytecodealliance/wasmtime/issues/3017> and #4002.

Finding: WASM runtimes DO NOT use fork(2) for snapshot/restore.
Quote from issue #3017:
> "Modules can be serialized in Wasmtime, but what really needs to
> be saved is the state of memory and execution."

The approach is to serialize the WASM module's memory and instruction
pointer to a file ("Wizer" tool), then mmap that into a fresh runtime
process.  No fork-of-running-runtime mechanism exists in either
project's stable release as of 2026-05.

**Takeaway for UML**.  WASM's choice not to fork their runtimes (and
to use Wizer's "serialize → re-instantiate" approach instead) parallels
Firecracker's choice (§1.6).  The community consensus is that forking
a userspace VMM/runtime to clone its state is harder than
serialize-and-reload.

---

### 1.5 Firecracker — Snapshot / Restore

Documentation:
<https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md>

Mechanism is FULL serialization, not fork:
> "The snapshot system consists of three main subsystems: state
> serialization, memory persistence, and VM restoration.  State
> serialization captures the microVM configuration and device states
> into a vmstate file.  Memory persistence saves guest memory to a
> separate file using either full or differential strategies.  VM
> restoration reconstructs a microVM from these artifacts in a new
> Firecracker process."

The 28ms-boot sandboxes use `MAP_PRIVATE | MAP_SHARED` snapshot files
mmap'd into a freshly-execve'd Firecracker process — the original
parent never forks.  Snapshot loading is done by the new process
itself, reading state files from disk.

**Takeaway for UML**.  Firecracker explicitly chose serialize+exec
over fork.  Their snapshot file is a fixed schema (the `serde` +
`bitcode` format mentioned in their docs); reload is into a fresh
process that knows how to deserialize.  This is the same architecture
choice Memo 09 §3.2 explicitly rejected for the fast-spawn use case.

---

### 1.6 QEMU — savevm / loadvm

Source: <https://airbus-seclab.github.io/qemu_blog/snapshot.html>

Same pattern: QEMU's savevm serializes `VMStateDescription` for each
device, vCPU state, memory; loadvm restores into a fresh QEMU
process.  No fork-of-running-QEMU mechanism.

The QMP commands `snapshot-{save,load,delete}` (added 2021):
<https://patchwork.kernel.org/project/qemu-devel/patch/20210204163959.377618-28-dgilbert@redhat.com/>
are still in the serialize+reload paradigm.

**Takeaway for UML**.  Confirms the same industry pattern.  No major
VMM forks its running self.

---

### 1.7 LKML — Linux-UM mailing list

`lore.kernel.org/linux-um` returns 403 (Anubis bot protection).
SourceForge mirror search via narkive surfaced the following:

  * **2004 "Kernel panic: Segfault with no mm"** thread —
    <https://user-mode-linux-user.narkive.com/ptmSUB1g/uml-user-kernel-panic-segfault-with-no-mm>.
    Same panic string as Phase 2a's `=early-fork` mode hits at
    `late_initcall_sync`.  In the 2004 case the root cause was
    a UML running with CONFIG_DEBUG_SLAB=n exposing a latent NULL-mm
    fault that DEBUG_SLAB had been hiding (different mm
    fault than ours, but the same diagnostic string).  No fix landed;
    the thread was inconclusive.

  * **No upstream forkserver patch.**  Searching for "forkserver",
    "snapshot", "fork()" in the linux-um indexed archive (via google
    site: search and SourceForge mirrors) returned no merged or
    proposed patches for forking the UML host process itself.  All
    "fork" hits are about guest fork(2), not host fork.

  * **The existing in-tree AFL forkserver path** (this codebase only —
    `arch/um/kernel/snapshot.c`) explicitly documents the same v1
    ceiling we are hitting.  Reading the existing comments:
    > "Inserting ANY wait4 path between (1) and (2) — blocking
    > wait4, WNOHANG poll loop, poll loop with clock_nanosleep /
    > sched_yield between polls, poll loop with host sigprocmask
    > SIG_BLOCK around the poll — all crash the parent with
    > 'Kernel mode signal 4' (SIGILL) in kernel context.  The
    > underlying hazard is UML's timer SIGALRM (or any host signal
    > queued while we're in non-UML-kernel code) being delivered into
    > UML's signal handler mid-poll, which runs switch_threads() and
    > longjmp()s into a jmp_buf captured pre-fork — stale stack, next
    > instruction decoded from garbage, SIGILL."

    File: `arch/um/kernel/snapshot.c` lines 366-396 (this commit).
    The Phase 2a panic at `um_template_pause_enter+0xf0` with a
    corrupted saved-RIP is **the same hazard class** as this
    documented-in-tree limitation.

  * **The C-09 "v1 ceiling" is a known UML infrastructure limit**,
    not a Phase 2a regression.  Cross-reference: same file lines
    389-396:
    > "Proper fix requires UML infrastructure work on the
    > signal/schedule interaction during parent non-kernel execution
    > windows (or a refactor where the worker reports its own status
    > via the status fd before exit — which sidesteps the whole
    > parent-reap path)."

  **Takeaway for UML**.  We are not the first to hit this in
  this codebase.  The earlier snapshot-forkserver work landed with
  this exact issue gated as a known limitation, and the proposed v2
  approach (freezer cgroup + parent does not exit kernel mode between
  fork and waitpid) was deferred.  Memo 09 Phase 2a is essentially a
  re-attempt at the same problem with the same root cause.

---

### 1.8 fork() async-signal-safety and `_Fork()`

Sources:
  * <https://man7.org/linux/man-pages/man3/fork.3p.html>
  * <https://man7.org/linux/man-pages/man3/signal-safety.7.html>
  * <https://www.phoronix.com/news/Glibc-Adds_Fork> — Glibc 2.34
    introduces `_Fork()`.
  * <https://pubs.opengroup.org/onlinepubs/9799919799/functions/_Fork.html>
    — POSIX.1-2024 specifies `_Fork()`.
  * <https://bugzilla.redhat.com/show_bug.cgi?id=1422161> — Red Hat
    bug "glibc: fork is not async-signal-safe".

Key facts:

  * **POSIX.1-2024 explicitly REMOVES the async-signal-safe
    requirement from `fork()`** and adds `_Fork()` as the
    async-signal-safe variant.  The Austin Group acknowledged in
    Defect 62 that glibc's `fork()` has NEVER been async-signal-safe
    in practice (it acquires malloc internal locks, runs pthread
    atfork handlers, etc).
    Quote from the `_Fork` POSIX page:
    > "The _Fork() function shall be equivalent to fork(), except
    > that fork handlers established by means of the pthread_atfork()
    > function shall not be called and _Fork() shall be
    > async-signal-safe."

  * **Calling glibc `fork()` from a signal handler can deadlock the
    child** if any glibc internal lock (notably malloc's arena lock)
    was held by another thread at the time fork() ran.  Red Hat bug
    1422161 documents this self-deadlock.

  * **The UML in-tree code already uses raw `__NR_fork`** specifically
    to bypass glibc.  See `arch/um/os-Linux/process.c::
    os_snapshot_fork_worker()` and our own
    `os_template_pause_fork_inner()` at
    `arch/um/os-Linux/template_pause.c` line 155.  This is the
    correct call — it side-steps the malloc-arena deadlock class.
    BUT it does NOT side-step the larger issue, which is that fork
    only duplicates the calling thread (so any pthread_mutex held by
    another thread is locked-and-orphaned in the child).

  **Takeaway for UML**.  Our raw `__NR_fork` is the right choice.
  The remaining hazard is NOT glibc-fork bugs — it is signal handlers
  installed by UML itself running on the parent's kernel stack
  between fork return and the parent's next pause.

---

### 1.9 longjmp() + fork() + stale jmp_buf

Sources:
  * <https://web.eecs.utk.edu/~huangj/cs360/360/notes/Setjmp/lecture.html>
  * <https://en.wikipedia.org/wiki/Setjmp.h>
  * <https://offlinemark.com/lets-understand-setjmp-longjmp/>

Key facts:

  * `jmp_buf` saves register state INCLUDING stack pointer.  After
    fork(), the child's `jmp_buf` (CoW'd from the parent) contains
    the parent's stack pointer.  In the child, that virtual address
    still maps to memory (CoW), but the stack frame layout there is
    whatever the PARENT'S stack looked like at jmp_buf save time.

  * **CRITICAL UML-specific issue**: UML's `switch_threads`
    (arch/um/include/shared/longjmp.h) does `UML_LONGJMP(buf, val)
    → longjmp(*buf, val)`.  Every UML task has a `jmp_buf` saved
    at its last voluntary schedule point.  If the child has tasks on
    its runqueue inherited from the parent, scheduling any of them
    longjmp()s into the saved IP/SP — which was VALID in the
    parent's address space but may be VALID-BUT-WRONG in the child.

  * The in-tree `snapshot.c` lines 540-571 document this exact
    hazard:
    > "Step 1b (commit 3d-d, D42): detach fork-inherited tasks from
    > the worker's CFS runqueue.  Before this, schedule() can pick
    > kthreads like ksoftirqd whose saved jmp_buf targets parent-side
    > host-thread state that doesn't exist in the worker — observed
    > as KASAN slab-OOB in __set_next_task_fair in commit 3d-c's
    > bring-up.  The helper is defined in kernel/sched/core.c,
    > guarded by CONFIG_UM_SNAPSHOT_FORKSERVER... v2 replaces it
    > with a freezer-cgroup pre-fork barrier per D41's revisit
    > triggers."

  **Takeaway for UML**.  The longjmp-into-stale-jmp_buf hazard is
  the known root cause class.  STATUS.md correctly identifies the
  CHILD-side fix (detach runqueue before any schedule()).  What
  remains broken is the PARENT side — and that is unrelated to
  longjmp.  See §2 below.

---

### 1.10 x86_64 syscall return path — could it corrupt saved-RIP?

Sources:
  * <https://blog.slowerzs.net/archive/linux-kernel-syscalls/>
  * <https://howtech.substack.com/p/dissecting-the-syscall-instruction>
  * <https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S>

Key facts:

  * **The SYSCALL instruction saves return RIP in RCX, not on the
    stack**.  Quote from the kernel-entry blog:
    > "64-bit SYSCALL saves rip to rcx, clears rflags.RF, then saves
    > rflags to r11, then loads new ss, cs, and rip from previously
    > programmed MSRs.  Additionally, SYSCALL does not save anything
    > on the stack and does not change rsp."

  * The host kernel's syscall return path uses `SYSRET` which restores
    `rip` from `rcx` and `rflags` from `r11`.  Neither parent nor
    child's USER stack is touched during a `__NR_fork` syscall.

  * **For UML's case**, the in-flight syscall is `syscall(__NR_fork)`
    from C code (or inline asm).  The C compiler has already pushed
    its own caller-saved registers + return address onto the UML
    "kernel" stack — which lives in the UML host process's address
    space.  Those bytes ARE in CoW'd memory; both parent and child
    see the same bytes immediately after fork.

  * **CONCLUSION: x86_64 syscall return path is NOT the corruption
    source**.  The saved-RIP slot on the UML kernel stack should
    survive fork verbatim.  Something else must be writing to it
    AFTER the fork.

---

## 2. The specific symptom: saved-RIP on kernel stack overwritten after fork

### 2.1 What the symptom looks like

Per STATUS.md:

```
Kernel panic - not syncing: Kernel mode fault at addr 0x4, ip 0x4
 [<6003a6fa>] um_template_pause_enter+0xf0/0x100
```

The IP being a tiny number (0x4, 0x2d6b62, 0x265d57) and the panic
frame being the FUNCTION EPILOGUE's `ret` strongly suggests:

  1. The `ret` instruction popped a corrupted value from `[rsp]`.
  2. `[rsp]` is the saved RIP that the C compiler pushed via the
     `call` to `um_template_pause_enter` from
     `template_pause_proc_write`.
  3. That slot was overwritten between (the start of fork-on-resume-
     loop) and (the function epilogue).

### 2.2 Has anyone else hit this exact symptom?

**Yes — the in-tree `arch/um/kernel/snapshot.c` has the SAME hazard
class documented at lines 366-396**:

> "Inserting ANY wait4 path between (1) and (2) — blocking wait4,
> WNOHANG poll loop, poll loop with clock_nanosleep / sched_yield
> between polls, poll loop with host sigprocmask SIG_BLOCK around
> the poll — all crash the parent with 'Kernel mode signal 4'
> (SIGILL) in kernel context.  The underlying hazard is UML's timer
> SIGALRM (or any host signal queued while we're in non-UML-kernel
> code) being delivered into UML's signal handler mid-poll, which
> runs switch_threads() and longjmp()s into a jmp_buf captured pre-
> fork — stale stack, next instruction decoded from garbage,
> SIGILL."

This is the canonical UML "v1 ceiling".  The Phase 2a hang is the
SAME hazard manifesting through a different surface (the
parent's saved-RIP slot, rather than `rip` itself in a longjmp
trampoline).  The actor in both cases is **a UML signal handler
firing on the parent's kernel stack between fork() return and the
parent's stable re-pause**.

### 2.3 Why UML signal handlers can corrupt the parent's kernel stack

`arch/um/os-Linux/signal.c` lines 102-143 — `sig_handler` is the
host-level handler for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/SIGIO/
SIGWINCH/SIGCHLD/SIGALRM.  When fired, it runs **on the same kernel
stack as the interrupted code** (because UML doesn't switch stacks
in its signal handlers — there is no kernel `IST` mechanism).

The handler can then call `unblock_signals_trace()` →
`sig_handler_common` → `(*sig_info[sig])(sig, si, &r, mc)` which for
SIGALRM dispatches into `timer_handler` → potentially `schedule()`.
A `schedule()` here runs `switch_threads()` (cooperative context
switch via UML_SETJMP/LONGJMP).  The longjmp can target a stale
jmp_buf saved by a different task — but more interestingly, even if
the longjmp goes to a valid jmp_buf, **on the way back to
`fork_on_resume_loop`, the kernel stack is now a few frames deeper**.
When control returns out of UML's signal trampoline, the original
`um_template_pause_enter`'s stack frame may have been clobbered.

### 2.4 Why the proposed STATUS.md mitigations didn't work

Per STATUS.md the v2 attempt was:
  1. Tear down stubs in BOTH halves pre-fork (`um_skas_teardown_all_stubs`)
  2. Detach runqueue siblings in child (`sched_worker_detach_other_tasks`)
  3. Respawn stubs in both halves post-fork
  4. `preempt_disable()` across child's fork-return window

These mitigations address **the LONGJMP-into-stale-jmp_buf hazard
in the CHILD** (which is real and documented).  They do nothing
for the PARENT-side hazard, which is:

  * Parent finishes the fork() syscall — host returns with `rax = pid`.
  * Parent now sits in `fork_on_resume_loop` between the inline-asm
    fork syscall return and the next `kill(SIGSTOP)`.
  * At some point between those two syscalls (host-level), a host
    signal that was QUEUED in the parent (but not pending, since fork
    cleared pending — but could become pending the instant after
    fork) fires.  Likely candidates:
    - `SIGCHLD` from the just-forked child if it died before parent
      reached SIGSTOP.
    - `SIGALRM` from the UML POSIX timer (which despite fork(2) docs
      saying child timers are not inherited, the PARENT's timer is
      still armed and firing every few ms).
    - `SIGIO` from any console / network fd UML was watching.
  * Host kernel delivers the signal → UML's `sig_handler` runs on
    the parent's CURRENT (interrupted) kernel stack →
    `unblock_signals_trace` may run pending UML kernel work →
    schedule() → switch_threads() → longjmp.

  When the signal handler eventually `sigreturn`s, the parent's
  kernel-stack bytes underneath the original `um_template_pause_enter`
  frame are STILL where they were — BUT the saved RIP at the function
  epilogue's reserved slot has been clobbered by the signal frame's
  push/pop of return addresses during the trampoline path.

  More precisely: UML's `sig_handler` is a normal C function with
  its own stack frame, including its own saved RIP slot.  When it
  exits and rt_sigreturn restores `rsp` to where it was at signal
  entry, the bytes between old-rsp and (old-rsp - sig_frame_size)
  HAVE been written.  If those bytes overlap with the saved-RIP slot
  of any function up the call chain that has NOT yet popped its
  return address... corruption.

### 2.5 Why `signals_enabled = 0` doesn't fully prevent this

The in-tree comment at `arch/um/os-Linux/process.c::
os_snapshot_block_iter_signals` (line ~246) explicitly notes:

> "Not the host sigprocmask — that only stops host delivery, and
> when unblocked UML's sig_handler runs queued signals back-to-back
> from whatever context we happen to be in, which is exactly the
> crash mode commit 3d-a v1 hit.  UML provides its own TLS flag
> (`signals_enabled` in arch/um/os-Linux/signal.c) that sig_handler
> checks on every delivery; when it's 0, the handler stores the
> signal in `signals_pending` and returns without running any UML
> kernel code."

But this comment is misleading.  Setting `signals_enabled = 0`
only stops UML KERNEL DISPATCH.  The HOST signal handler
(`sig_handler`) **STILL RUNS** — it just early-returns into
`signals_pending`.  The early-return path is short, but it still:

  1. Pushes a host signal frame onto the parent's kernel stack.
  2. Runs C code (`sig_handler` body, ~30 lines).
  3. rt_sigreturns.

This stack-frame push-and-pop alone may be enough to corrupt the
saved-RIP slot of an ancestor frame on the kernel stack, especially
if UML's kernel stack is small (it is: `THREAD_SIZE = 16 KiB` on
x86_64) and the call chain is deep (`sys_write` → `vfs_write` →
`template_pause_proc_write` → `um_template_pause_enter` →
`fork_on_resume_loop` is already 5+ frames).

---

## 3. UML-specific LKML / GitHub mirror search

Searches performed:

  * `lore.kernel.org/linux-um/?q=fork+server` — blocked by Anubis,
    returned 403.  Could not bulk-search via WebFetch.
  * Google `site:lore.kernel.org "linux-um" fork forkserver snapshot`
    — no relevant hits.
  * Google `"User-mode Linux" forkserver afl-fuzz patch lkml 2024
    2025 2026` — no UML+forkserver patches found upstream.  All
    forkserver hits are about AFL itself, not about UML.
  * narkive.com user-mode-linux-user mirror — only historic threads
    from 2003-2010.  "Segfault with no mm" thread (2004) had no
    resolution.
  * GitHub `torvalds/linux` search `arch/um forkserver` — only
    matches are within THIS branch / this codebase.

**Bottom line: there is NO upstream UML fork-server work.  The
existing in-tree `arch/um/kernel/snapshot.c` (this branch) is the
only attempt, and it documents the SAME v1 ceiling.**

---

## 4. Recommendation — single most-promising lead

### 4.1 The lead

**Host-level `sigprocmask(SIG_SETMASK, &fillset, &saved)` around the
fork() syscall + use `rt_sigreturn` to re-enter a clean stack frame
in BOTH parent and child.**

This is a combination of:

  * **CRIU's trick** (§1.1): on the way out of a corrupted-stack
    context, atomically restore registers via rt_sigreturn from a
    pre-built rt_sigframe.  This bypasses the bad-saved-RIP slot
    by NEVER calling `ret` from `um_template_pause_enter` — the
    rt_sigreturn drops directly to a known-good function entry.

  * **Host sigprocmask** (§2.5): the existing
    `os_snapshot_block_iter_signals` only blocks UML's dispatch —
    not host-level delivery.  We need actual `rt_sigprocmask`
    (raw syscall, not glibc `sigprocmask`) with a full signal set
    blocked between (fork-pre) and (parent's next SIGSTOP, child's
    next stable point).  Block at least: SIGALRM, SIGCHLD, SIGIO,
    SIGWINCH, SIGUSR1, SIGUSR2.  Cannot block SIGSTOP / SIGCONT.

### 4.2 Why this is most promising

Three independent lines of evidence point to host-level signal
delivery during the fork-loop body as the proximate cause:

  1. In-tree `snapshot.c` (this codebase, 2026-04 work) documents
     EXACTLY the same crash and pins it on host signal delivery
     during the parent's non-UML-kernel execution window.

  2. The crash IP is the function epilogue's `ret` — a value popped
     from `[rsp]` that should never change during a syscall return.
     The only thing that writes to that slot in the time between
     setjmp-equivalent points and the ret is a host signal handler
     pushing its own frame.

  3. UML's existing signal handlers (`arch/um/os-Linux/signal.c`
     `sig_handler` line 102) run on the interrupted task's kernel
     stack and CAN write to ancestor frame slots if the stack is
     tight and the signal handler's own frame is large enough.

### 4.3 How to verify

Two cheap dispositive controls, in order:

**Control A**: Block ALL host signals via raw `__NR_rt_sigprocmask`
**immediately before** the inline-asm fork syscall in
`os_template_pause_fork_inner`, and unblock only AFTER the parent
has reached its next stable SIGSTOP point.  If the crash STILL
happens at `+0xf0`, the hypothesis is wrong and the corruption
source is elsewhere (e.g., the stub-child's CLONE_VM-shared memory).

Test concretely:

```c
/* In os-Linux/template_pause.c, before the syscall asm: */
sigset_t fillset, saved;
sigfillset(&fillset);
sigdelset(&fillset, SIGSTOP);
sigdelset(&fillset, SIGCONT);
sigdelset(&fillset, SIGKILL);
syscall(__NR_rt_sigprocmask, SIG_SETMASK, &fillset, &saved, 8);

/* existing fork syscall */
register long rax asm("rax") = __NR_fork;
asm volatile ("syscall" : "+r"(rax) :: "rcx", "r11", "memory");

/* DO NOT restore yet — caller (fork_on_resume_loop) restores
 * after the parent reaches its next SIGSTOP or the child reaches
 * worker_init.  Plumb &saved out via a static so the unmask
 * path can find it.
 */
```

**Control B**: If Control A fixes it, plumb a rt_sigframe-based
re-entry into the child.  After fork, in the child, build a minimal
rt_sigframe pointing at `worker_init` with a fresh stack, then
syscall(__NR_rt_sigreturn).  The child never tries to "return" from
`um_template_pause_enter` — it executes a clean control transfer
via the host kernel.

The child can use CRIU's pattern: see
`criu/pie/restorer.c::rst_sigreturn` at line 1079 for the canonical
implementation.  The architecture-specific macro
`ARCH_RT_SIGRETURN_RST` is defined in CRIU's
`criu/arch/x86/include/asm/restorer.h`.

Reference URL:
<https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>

### 4.4 Expected outcome

  * **If Control A alone fixes the +0xf0 crash**: the hypothesis is
    correct, and the §3.5 "real fix" in STATUS.md needs only the
    additional host-level `rt_sigprocmask` (~30 LoC).  Parent
    survives because no signal handler runs during the fork loop
    body.  Subsequent siblings can be taken without re-entering this
    code path.

  * **If Control A does NOT fix the +0xf0 crash**: the corruption
    source is something inside the fork() syscall itself, OR the
    stub-child's CLONE_VM aliasing writing to the parent's kernel
    stack memory.  Control B (rt_sigreturn-based re-entry) is the
    fallback that bypasses the issue entirely by never returning
    "the C way" through `um_template_pause_enter`.

### 4.5 Why this is in scope for the v1 sprint

  * **~30 LoC for Control A.**  All inside
    `arch/um/os-Linux/template_pause.c` and `template_pause.c`.
  * **~150 LoC for Control B** (rt_sigframe construction +
    sigreturn — copy-paste from CRIU's `restorer.c` x86 path).
  * **Both controls are testable with existing selftests**
    (`tools/testing/selftests/um/template-pause-fork-smoke/`).

This is in scope for a single session if Control A pans out.

---

## Cross-references

  * `arch/um/kernel/template_pause.c` — Phase 1a/2a in-tree code.
  * `arch/um/os-Linux/template_pause.c` — host-syscall wrappers.
  * `arch/um/kernel/snapshot.c` lines 366-396 — documented v1 ceiling.
  * `arch/um/os-Linux/signal.c` lines 102-143 — UML signal handler.
  * `arch/um/os-Linux/process.c` lines 246-273 — existing
    `os_snapshot_block_iter_signals` (UML-level only, not host).
  * CRIU restorer:
    <https://github.com/checkpoint-restore/criu/blob/criu-dev/criu/pie/restorer.c>
  * AFL++ forkserver:
    <https://github.com/AFLplusplus/AFLplusplus/blob/stable/instrumentation/afl-compiler-rt.o.c>
  * gVisor ptrace platform:
    <https://github.com/google/gvisor/blob/master/pkg/sentry/platform/ptrace/subprocess.go>
  * Firecracker snapshot:
    <https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md>
  * `_Fork()` POSIX.1-2024:
    <https://pubs.opengroup.org/onlinepubs/9799919799/functions/_Fork.html>
  * `fork(2)` man page (timer/signal inheritance rules):
    <https://man7.org/linux/man-pages/man2/fork.2.html>
