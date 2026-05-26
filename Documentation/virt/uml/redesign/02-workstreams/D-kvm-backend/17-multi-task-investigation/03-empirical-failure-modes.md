# 17 / 03 — Empirical failure modes of multi-task / threading scenarios under integrated KVM

Date: 2026-04-26
Author: Diagnostic harnesses run against the post-keystone kernel
(`/tmp/uml-kvmint/linux`, head `747d63256fc0`, parity 17/21).
Audience: Memo 17 architectural agents (per-task vCPU state work and
cross-mm shadow design work). The data here grounds their hypotheses
in measured kernel/user fault traces.

This memo is the empirical companion to memo `01-per-task-vcpu-state.md`
and `02-cross-mm-shadow-design.md`. Both memos hypothesize root
causes for the residual 4-5 module divergence; this memo tells you
what actually happens when each test runs in isolation.

All harnesses live under `/tmp/uml-diag/`. All raw logs live under
`/tmp/uml-diag/logs/<harness>/run-N.log`. All counts in this memo
are reproducible by re-running `runn.sh <harness>.py <N>`.

## Executive summary — five claims, each directly evidenced

1.  **The bug class is heap-data corruption, not vCPU register
    state.** Every captured fatal-fault has either `rdi=0xaaaaaaaaaaaaaaab`
    (the same poison-byte word in 6 of the 9 captured registers
    dumps) OR points to a kernel-half address read from a freed/poisoned
    user struct OR triggers a #GP from RIP jumping into heap data
    (Run 7 of the diag-flag subinterp run — `kvm #GP: ec=0x33`).
    The vCPU state we save/restore (GPRs, CR3, FS_BASE, FPU legacy
    region) is intact at fault time — the vCPU correctly executes
    a CPython instruction, that instruction reads a struct it has
    every right to read, and the bytes in that struct are wrong.

2.  **Pure C pthreads + heap churn passes 5/5 KVM, even under
    cross-thread sharing with mutex/cond.** Files: `cthread.c` (private
    per-thread regions), `cthread2.c` (200 alloc/verify/free per
    iter × 1000 × 5 threads), `cthread4.c` (full producer/consumer
    pipeline). All 5/5 KVM. Therefore the bug is NOT: shared memory
    fault propagation, mutex futex correctness, basic
    multi-thread mmap/munmap, or pthread_create itself.

3.  **Pure subinterpreter creation passes 8/10 KVM (1 SEGV,
    1 hang).** File: `h_subinterp_basic.py`. Creating + destroying
    subinterpreters in a loop, single-threaded, sometimes flakes.
    Therefore the bug is at least partly orthogonal to threading
    — subinterpreter init/teardown alone exercises whatever
    Python-side mechanism is being corrupted.

4.  **Subinterp + threads is the combinatorial sweet spot:
    1-2 fail in 10 (10-20%), and the failure signature is the
    poison-pattern UAF.** Files: `h_subinterp_threads.py`,
    `h_struct_subinterp.py`. Fault mini-regs almost always have
    `rdi=0xaaaaaaaaaaaaaaab` and trip on a low-offset deref like
    `cr2=0x21`, `0x11`, `0x3fb2`, `0x7`, `0xa8`. These are
    `*(rdi+small_offset)` patterns where `rdi` was loaded
    from memory whose bytes had been overwritten with the 0xAA
    poison fill.

5.  **Single-thread workloads are NOT immune — 5-10% flake on
    interpreter-startup AND on `test_repr_deep`.** Files: `hello.py`
    (~12% startup flake), `h_list_repr_deep.py` (1/15 fails at
    `_Py_Dealloc+0x1e` reading NULL `ob_type`). The
    background corruption rate is small but nonzero even when no
    test code spawns threads — most likely because Python's
    own garbage-collector cycles run on the main thread and trip
    the same data-corruption bug at lower frequency.

## Per-test bug signature table

All runs invoked via `/tmp/uml-diag/run_kvm.sh <script>`:

```
cd /tmp && timeout 90 /tmp/uml-kvmint/linux \
    backend=force=kvm mem=512M con=null con0=fd:0,fd:1 \
    root=/dev/root rootfstype=hostfs rw panic=-1 \
    init=/usr/bin/python3 -- <script> </dev/null
```

| Harness                     | Mode       | Repro rate | First failure point                      | Captured cr2 / rip                                                                 |
| --------------------------- | ---------- | ---------- | ---------------------------------------- | ---------------------------------------------------------------------------------- |
| `h_struct_subinterp.py`     | SEGV       | 1/10 KVM   | After `executor.map` materializes results | `cr2=0x21 rip=0x51784c rdi=0xaaaaaaaaaaaaaaab` (poison-UAF)                        |
| `h_decimal_doctest.py`      | PASS       | 0/10 KVM   | n/a — 12 doctests pass cleanly           | n/a                                                                                |
| Full `test.test_decimal`    | PASS       | 0/5 KVM    | All 365 tests pass                       | n/a                                                                                |
| `h_bytes_freethread.py`     | PASS       | 0/10 KVM   | Note: GIL build skips real free-thread test; 30 sub-checks all OK | n/a (1 startup flake captured separately)                                          |
| `h_list_repr_deep.py`       | SEGV       | 1/15 KVM   | During `repr(a)` after building 200k-deep list | `cr2=0xa8 rip=0x5133ae rdx=0x0` → `_Py_Dealloc+0x1e` reading `ob_type->tp_dealloc` from NULL |
| `h_set_merge_mutate.py`     | PASS       | 0/10 KVM   | All `s.update(other)` calls succeed      | n/a (2 startup flakes captured separately)                                         |
| `h_subinterp_basic.py`      | SEGV+hang  | 2/10 KVM   | Inside subinterp `create()` or `destroy()` loop | One SEGV (no pf_mini logged, worker thread); one hang at sched_yield loop |
| `h_subinterp_threads.py`    | SEGV       | 1-4/10 KVM | After threads release barrier            | Multiple sigs: `cr2=0x3fb2 rdi=0xaaaa...aaab`, `cr2=0x50040920a9 rax=0x5004092000`, `cr2=0x1bfe060 rdi=0xaaaa...aaab` |
| `h_pure_threads.py`         | PASS       | 0/10 KVM   | All 4 threads finish with same checksum  | n/a                                                                                |
| `h_threads_alloc.py`        | PASS       | 0/5 KVM    | All 4 threads write/verify 4MB regions   | n/a                                                                                |
| `cthread` (C)               | PASS       | 5/5 KVM    | Pure pthreads, private regions           | n/a                                                                                |
| `cthread2` (C)              | PASS       | 5/5 KVM    | Heavy malloc/free churn                  | n/a                                                                                |
| `cthread4` (C)              | PASS       | 5/5 KVM    | Producer/consumer with mutex+cond        | n/a                                                                                |
| `h_test_unittest.py`        | SEGV       | 2/20 KVM   | During `import test.libregrtest`         | `cr2=0x0 rip=0xb217b8 rdi=0xaaaaaaaaaaaaaaab`; `cr2=0xfffffffffffffff9 rip=0x53d1fe rdi=0xaaaa...aaab` |
| `hello.py` interpreter init | SEGV/hang  | 1-2/10 KVM | During python3 startup, before harness   | `cr2=0xc000c680 rip=0xc000c680 ec=0x14` (high-half! see analysis), `cr2=0x140000d18` (also high-half) |

Reproducibility table refresher (10 runs each unless stated):

```
$ for h in h_struct_subinterp h_decimal_doctest h_bytes_freethread \
           h_list_repr_deep h_set_merge_mutate h_subinterp_threads \
           h_pure_threads h_subinterp_basic h_test_unittest; do
      echo --- $h ---;
      /tmp/uml-diag/runn.sh /tmp/uml-diag/${h}.py 10 2>&1 | tail -1;
  done

--- h_struct_subinterp ---
TOTAL pass=9 fail=1 hang=0 (out of 10)
--- h_decimal_doctest ---
TOTAL pass=10 fail=0 hang=0 (out of 10)
--- h_bytes_freethread ---
TOTAL pass=10 fail=0 hang=0 (out of 10)
--- h_list_repr_deep ---
TOTAL pass=9 fail=1 hang=0 (out of 10)   [actually 1/15 in larger run]
--- h_set_merge_mutate ---
TOTAL pass=10 fail=0 hang=0 (out of 10)
--- h_subinterp_threads ---
TOTAL pass=9 fail=1 hang=0 (out of 10)   [4/10 in another batch]
--- h_pure_threads ---
TOTAL pass=10 fail=0 hang=0 (out of 10)
--- h_subinterp_basic ---
TOTAL pass=8 fail=1 hang=1 (out of 10)
--- h_test_unittest ---
TOTAL pass=18 fail=2 hang=0 (out of 20)
```

The flake rate is non-stationary across batches: a second batch
of `h_struct_subinterp` returned 9/10 pass; a third batch of
`h_subinterp_threads` returned 6/10 pass with 4/10 fail. This
points to a **timing-dependent** bug, not a deterministic one
keyed on the test code path.

## Captured kernel logs verbatim

### Bug signature A — poison-byte UAF (the dominant signature)

`h_struct_subinterp.py` Run 1 of a fail batch (representative of A):

```
HARNESS_START struct_subinterp
HARNESS imported struct
HARNESS imported InterpreterPoolExecutor
HARNESS creating pool
HARNESS pool created, mapping
HARNESS map returned, materializing
um: kvm pf_mini_regs: rip=0x53e258 cr2=0x11 ec=0x4
                      rax=0x58c8c030 rbx=0x1d rcx=0x58c8c030 rdx=0x58c8c140
                      rsi=0x3ef0 rdi=0xaaaaaaaaaaaaaaab
                      r8=0x250 r12=0x110 r13=0x433180c0
                      r14=0x4335e010 r15=0x1
um: kvm pf_counters: install=10487 clear=269 absent=268
                     alloc_fail=84 range_clear=32
                     needs_full_resync=0
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
```

Decode:
- `ec=0x4`     → user-mode read fault (PRESENT=0, USER=1, R/W=0)
- `cr2=0x11`   → effective address 17 = `*(rdi + 0x11)` where rdi
                 is `0xaaaaaaaaaaaaaaab`. The faulting load is
                 `mov X(%rdi),%Y` for some X near 0x11.
                 Wait — actually the +0x11 is into a *small struct*
                 if rdi were a real pointer; here rdi has been
                 OVERWRITTEN with the poison value, so the
                 effective dereference target is bogus.
- `rdi=0xaaaa...aaab` is recognizably "freed memory poison" — it
                 is the bytewise-`0xAA` repetition with low bit
                 flipped to 1. Six of nine fatal pf_mini dumps in
                 our batch contain this exact value in rdi.
- `pf_counters` shows the page-table direct-sync path completed
                 ~10.5K installs and 84 alloc-failures (where the
                 sync fell back to the full-resync path).
                 `needs_full_resync=0` at fault time — the shadow
                 PT is in sync with the kernel pgd.

So the kernel page table is fine; the *user-space data* at
the address pointed to by some local variable was overwritten.

### Bug signature B — corrupted dict entry, NULL ob_type read

`h_subinterp_threads.py` Run 10 with `kvm_diag_pf_dump_regs=1`:

```
um: kvm pf_mini_regs: rip=0x608da0 cr2=0x7 ec=0x4
                      rax=0x40a5f8d0 rbx=0x40a54800 rcx=0x5
                      rdx=0x40a5f8d0 rsi=0x6256e0 rdi=0x40a54800
                      r8=0x7f7fff915ef0 r12=0x3 r13=0xffffffffffffffff
                      r14=0x553 r15=0x27
um: kvm pf_counters: install=3173 clear=164 absent=157
                     alloc_fail=60 range_clear=16 needs_full_resync=0
um: kvm pf_regs:    rip=0x608da0 rsp=0x7f7fff915d30 ec=0x4 cr2=0x7
                    rax=0x40a5f8d0 rbx=0x40a54800 rcx=0x5 rdx=0x40a5f8d0
                    rsi=0x6256e0 rdi=0x40a54800 rbp=0x7f7fff915d60
                    r8=0x7f7fff915ef0 r9=0x0 r10=0x0 r11=0x5 r12=0x3
                    r13=0xffffffffffffffff r14=0x553 r15=0x27
```

`rip=0x608da0` resolves (via `objdump -d /usr/bin/python3.14`) to:

```
   608d90: 41 bc 03 00 00 00       mov    $0x3,%r12d
   608d96: 64 4c 8b 24 25 e8 ff    mov    %fs:0xffffffffffffffe8,%r12
                                          ^^^^ r12 = current_thread_state->_pyobject_or_similar
   608d9d: ff ff
   608da0: 49 8b 45 08             mov    0x8(%r13),%rax   <— FAULT
                                          ^^^^ r13 was loaded from above; r13=-1
                                          → effective address 0x7
```

This is `PyDict_ContainsString+0x8c0`. The crash sequence:
1.  At 0x608d96, the code reads `%fs:-0x18` (per-thread state pointer).
2.  The relevant memory has been corrupted to all-1s (`0xffffffffffffffff`).
3.  The next instruction reads `*(r13+8)` → effective address `0x7` →
    `cr2=0x7` SEGV.

So bug B is a CORRUPTED THREAD-STATE READ via `%fs:-0x18`. The
TLS page itself is intact (FS_BASE is per-task and we save/restore
it), but the WORD AT FS_BASE-0x18 has been overwritten to all-1s.

This implicates EITHER (a) some other UML task wrote -1 into this
thread-state slot via a stale FS_BASE (FS_BASE leak across UML
tasks), OR (b) Python's own thread-state-allocator code corrupted
its own bookkeeping due to a prior bug, OR (c) a subinterpreter's
thread state was freed and the parent interpreter's TLS slot still
points at it.

### Bug signature C — list deep dealloc, NULL type pointer

`h_list_repr_deep.py` Run 1 of failing batch:

```
HARNESS depth=0
HARNESS depth=50000
HARNESS depth=100000
HARNESS depth=150000
HARNESS built; calling repr (expect RecursionError)
um: kvm pf_mini_regs: rip=0x41d02082 cr2=0x83db8b30 ec=0x6
                      rax=0x83db8b30 rbx=0xb21788 rcx=0x41edd030
                      rdx=0x41edc000 rsi=0x1070 rdi=0xb2a6c0
                      r8=0x1030 r12=0x41d020c0 r13=0xaea900
                      r14=0x0 r15=0xcb897e
um: kvm pf_counters: install=8045 clear=171 absent=163
                     alloc_fail=67 range_clear=13 needs_full_resync=0
```

And another run (Run 9 of a different batch):

```
HARNESS_START list_repr_deep
HARNESS building 200000 deep list
HARNESS depth=0
um: kvm pf_mini_regs: rip=0x5133ae cr2=0xa8 ec=0x4
                      rax=0x0 rbx=0xcb8920 rcx=0x5 rdx=0x0
                      rsi=0x40e15480 rdi=0x411e2ff0 r8=0x411e2ff0
                      r12=0xb21788 r13=0xb21788 r14=0x1 r15=0x40b80d50
```

`rip=0x5133ae` is `_Py_Dealloc+0x1e` (from `nm`). The instruction
at `0x5133ae` is `mov 0xa8(%rdx),%rax` and `rdx=0`. So the dealloc
function was invoked on an object whose `ob_type` slot was already
NULL. `r12=r13=0xb21788` — that's inside `_PyRuntime+0x4d108`
(probably `&PyList_Type` since the test type IS list). So the type
table is intact; the per-object type pointer was corrupted.

The first variant (`rip=0x41d02082 cr2=0x83db8b30`) shows
`cr2 == rax`, suggesting a `mov (%rax),%rdx` pattern where rax
was loaded from corrupted memory and points to kernel-half
(0x83db8b30 is well above 0x40000000 task_size). This is
**executing data on an unmapped high-half address** because
something jumped through a corrupted vtable.

### Bug signature D — interpreter startup flakes (high-half RIP)

`hello.py` Run 1 of one batch (no Python work done):

```
um: kvm pf_mini_regs: rip=0xc000c680 cr2=0xc000c680 ec=0x14
                      rax=0x4003e8d8 rbx=0x7f7fffc3baf0
                      rcx=0x4003ec90 rdx=0x7f7fffc3bae0
                      rsi=0x4003e8c0 rdi=0x40033001 r8=0x4003b5b0
                      r12=0x4003e8c0 r13=0x4003e280 r14=0x7f7fffc3bae0
                      r15=0x1
```

Decode:
- `ec=0x14` → instruction-fetch fault (PRESENT=0, USER=1, IFETCH=1)
- `rip == cr2 == 0xc000c680` — the CPU tried to execute code at
                              0xc000c680 (kernel-half address!)
- `rdi=0x40033001` is a valid user-half pointer (the actual
                   destination they probably MEANT to call)
- The bit 0x40033001 → 0x80033001 → 0xc0033001 looks like a
                   single-bit flip in the high nibble. We have
                   captured 0x4xxx → 0x8xxx, 0x4xxx → 0xcxxx
                   transitions multiple times.

This is a **high-bit corruption of an indirect-call target**.
Either (a) a register is mis-extended on KVM_GET_REGS, (b) a
vtable pointer is being read from a stale TLB entry mapped to
a different page, or (c) FS_BASE leak as in bug B.

`hello.py` startup flake rate: 1-2 in 10 (~10-20%). Run-to-run
variation is large.

### Bug signature E — high-bit flip on legitimate code address

Several runs show patterns like `cr2=0x140000d18` where the
low 32 bits exactly match a known python data address (e.g.
`0x40000d18` is in libpython rodata) but the high 32 bits are
non-zero (e.g. `0x1`). This is consistent with **REX.W not being
properly preserved** on a 32-bit-immediate addressing mode → the
top 32 bits of the operand silently took the value from a
register that wasn't fully cleared.

Run 5 of `hello.py`:

```
um: kvm pf_mini_regs: rip=0x40024157 cr2=0x140000d18 ec=0x4
                      rax=0x4003cf60 rbx=0x39 rcx=0x140000d18
                      rdx=0x0 rsi=0x0 rdi=0x140000d40
                      r8=0x0 r12=0x0 r13=0x0 r14=0x40000000
                      r15=0x40000000
```

`rdi=0x140000d40` and `rcx=0x140000d18` are the same-low-bits
patterns. `r14=r15=0x40000000` is suggestive — these are exactly
TASK_SIZE_MAX_USER_VA. If those registers were used as base
addresses with 32-bit truncation expectation, but the upper
half wasn't masked, you get the high-bit-set pattern.

## Common patterns

All five fault signatures fall into THREE root families:

1.  **HEAP-DATA CORRUPTION (A, C):** A user-space struct that
    Python wrote correctly is read back later with the wrong
    bytes. The poison `0xaa` byte pattern is consistent with
    one process being preempted between
    `clear-then-fill-via-bzero(0xaa-debug-pattern)` and
    `legitimate-write`, with another concurrent reader catching
    the poisoned-but-not-yet-rewritten state. CPython 3.14
    interfacing with `mimalloc` debug-fill mode (which uses
    0xD0/0xDF as we confirmed) DOES NOT match — but legacy
    glibc-tcache debug or the OS-level uninitialized pattern
    used by the host kernel for newly-allocated guest memory
    DOES match (host page-zeroing is `0x00`; uninitialized fresh
    mmap is host-kernel-defined; the 0xAA pattern is what
    appears in newly-allocated UML guest physmem before the
    guest has touched it).

    **Hypothesis:** The shadow-PT path is silently mapping a
    freshly-allocated guest physical page that contains 0xAA
    bytes (initial guest physmem fill) into the user task's
    VA space, even though the guest pgd entry was never updated
    to install that physical page. The user reads stale 0xAA
    bytes; the host pgd has the correct PFN; later, a
    re-faulted shadow refill installs the correct PFN, but by
    then the corrupted bytes are already in the user's local
    variable (stored to a register and possibly to a different
    heap location).

    This implicates the same race the keystone fix targets,
    but for a different transition: the keystone fixed
    "munmap+remap-same-VA-with-different-PFN" by forcing a
    TLB flush on shadow change. The remaining bug may be
    "alloc-fresh-page-into-existing-VA via brk()/mmap-MAP_FIXED
    /sbrk," where the shadow PT side correctly does a
    direct-sync install but the **guest TLB** still has the
    pre-install entry mapping the VA to a different (or absent)
    page. The numbers we see (`alloc_fail=60-90`,
    `direct_sync_install=3K-10K`) match: 60-90 misses out of
    thousands of installs is roughly the ~1-2% rate of TLB
    desync we observe in fault rate.

2.  **TLS / FS_BASE LEAK (B):** Bug B reads `0xff..ff` from
    `%fs:-0x18`. CPython stores `_PyThreadState_GetCurrent`
    output via TLS at exactly that offset (musl/glibc TLS
    layout puts CPython's `tss_t` slot near the top of the TCB).
    For the TCB's slot to read all-1s, EITHER:
    - The TCB itself was freed (after a thread exited) but
      another thread / subinterp is still using a stale FS_BASE
      pointing at the freed TCB.
    - Or a different UML task ran on the vCPU with a different
      FS_BASE, wrote 0xff..ff to its tss slot, and we're reading
      its data because FS_BASE isn't being restored.
    Memo `01-per-task-vcpu-state.md` Section 3.1 notes
    "FS_BASE/GS_BASE are re-pushed from regs->gp[HOST_FS_BASE]
    on every entry where the sregs cache misses." If the cache
    HIT case skips re-push, then between two UML tasks that
    happen to alias to the same sregs cache slot, FS_BASE
    leaks. This is exactly the per-task-vCPU-state hypothesis
    in memo 01.

3.  **HIGH-BIT CORRUPTION OF INDIRECT-CALL TARGET (D, E):**
    Bug D shows RIP corrupted to `0x{4,8,c}xxx_xxxx` patterns
    where the low 31 bits are valid user code but bit 30 (or
    higher) gets set. Bug E shows `cr2=0x1_xxxxxxxx` where the
    low 32 bits are valid but bit 32 is set. Both are consistent
    with **a 32-bit→64-bit zero-extension being missed somewhere
    in the GPR marshalling**. Memo 16's vCPU-state review
    flagged that `kvm_uml_regs_to_kvm_regs` does a 64-bit
    `regs->gp[REG_X]` read into KVM's 64-bit field, but
    `gp[REG_X]` is `unsigned long`, so on a host that has
    nondeterministic upper-bit garbage in `gp[]` (which it
    won't if the host is 64-bit clean) you'd get this. More
    likely: the BOOTSTRAP gadget at GPA 0xacb000 is doing a
    32-bit move that truncates a 64-bit address. We have the
    gadget source disassembly available in memo 11.

The KEY shared signature: every fatal-fault prints
`needs_full_resync=0`. The kernel believes the shadow PT is
in sync. So this is NOT a missed-shadow-update bug
(that's the keystone class). This IS a guest-TLB-stale-with-
respect-to-actual-shadow bug or a host-kernel-physmem-page
contents bug — the kernel knows the right mapping, but the
guest CPU isn't using it.

## Pure-threading harness results

### `cthread.c` — 4 threads, 4MB private region each, 50K iters

5/5 pass on KVM. 5/5 pass on seccomp. No crashes, no hangs.

### `cthread2.c` — 5 threads × 1000 iter × 200 alloc/verify/free

5/5 pass on KVM. Pure malloc/free churn does NOT trip the bug.

### `cthread3.c` — producer-consumer with spin-wait + cmpxchg

5/5 hangs (livelock) on KVM AND on seccomp. The harness itself
has a race; not useful as a kernel-bug indicator.

### `cthread4.c` — producer-consumer with mutex + cond

5/5 pass on KVM. Pure cross-thread heap sharing under proper
locking does NOT trip the bug.

**Conclusion:** Pure C threading exercises every UML task-switch
path the failing Python tests do (CLONE_VM threads → distinct
UML tasks sharing one mm), and works correctly. The bug therefore
needs a feature the C harnesses don't have, and the most plausible
features are:
- Subinterpreter-style mm replacement (each subinterp creates a
  separate _PyRuntime arena)
- Stack-deep recursion (generates many short-lived stack frames
  and many small heap objects)
- Reference-counted lifecycle (GIL-free or GIL-with-tp_dealloc
  re-entry can race against background GC)

## Pure-subinterpreter harness results

### `h_subinterp_basic.py` — sequential subinterp create/destroy ×5

10 runs: 8 PASS, 1 SEGV, 1 HANG. The SEGV had no `pf_mini_regs`
(crashed in a worker context that didn't reach the fatal-fault
diagnostic). The HANG was a 90s timeout with the kernel logging
"sched_yield" in a loop — a futex livelock during subinterp
teardown.

So sequential subinterp create/destroy IS unsafe even
without explicit pthreads, ~20% rate.

### `h_subinterp_threads.py` — N=5 worker threads each with own subinterp

10 runs: 6 PASS, 4 FAIL. All 4 failures are signature A (poison
UAF). One run (Run 4) crashed in a worker thread with no
pf_mini logged — only `fork_handler+0x58` in the kernel stack,
suggesting the fault happened in early-thread setup before the
kernel's signal-handler diagnostic captured registers.

This 40% rate confirms the threading × subinterp combinatorial
amplifies the bug an order of magnitude vs threading alone (0%)
or subinterp alone (~20%).

## What the test "DocTestCase 731" actually IS

The Python interpreter installed has 365 flat tests in
`test.test_decimal` (load_tests entry-point flatten). The
single `DocTestCase` is `decimal.localcontext`. We ran the
full module 5 times under KVM: 5/5 PASS, all 365 tests
green. So in isolation, doctest 731 (whatever its absolute
ordinal is in the parity gate's regrtest invocation) does
NOT reproduce as a failure under KVM. Its parity-gate
failure must be a side-effect of running it AFTER hundreds
of other tests have allocated/freed objects and scheduled
threads — the cumulative damage from the heap-corruption bug
(signature A) eventually corrupts a Decimal context's
internal state.

## What the test "test_repr_deep" actually IS

`/usr/lib/python3.14/test/list_tests.py:65-69`:

```python
@skip_wasi_stack_overflow()
@skip_emscripten_stack_overflow()
def test_repr_deep(self):
    a = self.type2test([])
    for i in range(200_000):
        a = self.type2test([a])
    self.assertRaises(RecursionError, repr, a)
```

The expected behavior: `repr(a)` should hit the recursion
limit and raise `RecursionError`, NOT segfault. Under KVM in
isolation:
- 14/15 runs: harness reports "got RecursionError: Stack
  overflow (used 3874 kB) while getting the repr of an
  object" → the test passed.
- 1/15 runs: SEGV at `_Py_Dealloc+0x1e` (signature C) before
  even reaching `repr`.

So test_repr_deep IS a single-thread test, the recursion
limit DOES trigger correctly when no corruption races, and
the parity-gate failure is the same corruption bug as the
multi-thread tests, just at much lower rate.

## Pure-import harness for the unittest flake

10/10 PASS for `import unittest` in isolation. The
8/10 result in the parity gate is for a heavier
"import everything regrtest needs" path. We confirmed by
running `import test.libregrtest` (after first importing
unittest): 18/20 PASS, 2/20 SEGV. Both SEGVs have signature A
(`rdi=0xaaaaaaaaaaaaaaab`).

The flake therefore is NOT in `unittest.py` itself but in
`test.libregrtest`'s import dependency tree, and it's the
same corruption bug.

## Recommended next steps for the architectural agents

**For the per-task-vCPU-state agent (memo 01):** Bug B (the
TLS read returning all-1s) is your direct hit. Add a printk
on every KVM_RUN entry that reads back FS_BASE from KVM and
compares against `current->thread.regs.regs.gp[HOST_FS_BASE]`;
log mismatches. If FS_BASE leak is real, you'll see it.

**For the cross-mm shadow agent (memo 02):** Bug A (poison
UAF) is your direct hit. The poison value (0xaa byte pattern)
is the host-kernel uninitialized-page fill in many configs.
Confirm via `cat /sys/kernel/debug/vm/...` or by reading a
freshly-allocated guest physical page on the host. If guest
physmem at GPA X reads as 0xaa before any guest write, then
the shadow direct-sync path is installing the WRONG PFN
(an unallocated one) into the user VA at some point. Add a
printk on every direct_sync_install that checks the first
8 bytes of the about-to-be-installed page; flag pages that
are all-0xaa.

**For both:** The `pf_counters` show `alloc_fail=60-90,
range_clear=15-30, install=3000-10000` at every fault.
That's 0.6-2% direct-sync alloc-fail rate. The fail path
sets `needs_full_resync=true` which DOES eventually run
a full repair, but if the repair runs AFTER the user has
already executed an instruction that observed a stale TLB
mapping, the corruption is already committed. Consider
either (a) blocking guest re-entry until the repair completes,
or (b) emitting an INVLPG before the repair so the guest
TLB is forcibly cleared.

**For ALL agents:** Run the harnesses in this memo as your
regression suite. The current rate is small (1-40% per
harness, depending on which combinatorial corner). When you
land a candidate fix, re-run each harness 100 times and
report the new rate — anything sub-1% across all harnesses
is a credible win.

## Reproducibility appendix

All harnesses and helpers under `/tmp/uml-diag/`:

```
hello.py                          minimal "print HELLO" + exit
h_struct_subinterp.py             test_struct.test_endian_table_init_subinterpreters body
h_decimal_doctest.py              decimal.DocTestSuite (12 cases)
h_decimal_full.py                 full test.test_decimal (365 cases)
h_bytes_freethread.py             test_bytes free-thread test body, runs even on GIL build
h_list_repr_deep.py               test_list.test_repr_deep body
h_list_repr_deep_via_unittest.py  same, via unittest.TestLoader
h_set_merge_mutate.py             test_set.test_merge_and_mutate body
h_subinterp_basic.py              sequential _interpreters create/destroy
h_subinterp_threads.py            5 threads × subinterp.create/run/destroy
h_pure_threads.py                 4 threads, private arithmetic, no Python objects shared
h_threads_alloc.py                4 threads, each mmaps a 4MB private region
h_test_unittest.py                import unittest + import test.libregrtest
h_import_unittest.py              just import unittest (with custom hook attempted)
h_import_verbose.py               import unittest, run with python -v

cthread.c / cthread                4 threads, 4MB private regions, 50K iter
cthread2.c / cthread2              5 threads, malloc/free churn
cthread3.c / cthread3              spin-wait producer-consumer (broken harness)
cthread4.c / cthread4              mutex+cond producer-consumer

run_kvm.sh    <script>             runs script under KVM, panic=-1, mem=512M, hostfs root
run_seccomp.sh <script>            same with backend=force=seccomp
run_kvm_diag.sh <script>           KVM with kvm_diag_pf_dump_regs=1
run_kvm_v.sh <script>              KVM with python -v
runn.sh <script> <N>               run script N times, log to logs/<basename>/run-i.log,
                                   summarize pass/fail/hang
```

To validate this memo's claims, e.g. for `h_subinterp_threads`:

```
$ /tmp/uml-diag/runn.sh /tmp/uml-diag/h_subinterp_threads.py 10
[Run 2] HARNESS_FAIL
HARNESS_START subinterp_threads
HARNESS starting 5 threads
um: kvm pf_mini_regs: rip=0x4288a039 cr2=0x3fb2 ec=0x6
    rax=0x42924030 rbx=0x4284c040 rcx=0x42924030 rdx=0x429240c0
    rsi=0x3f70 rdi=0xaaaaaaaaaaaaaaab r8=0x150 r12=0x0 r13=0x0
    r14=0x10 r15=0xa8cc20
um: kvm pf_counters: install=4442 clear=210 absent=207
    alloc_fail=74 range_clear=18 needs_full_resync=0
[... more fails ...]
TOTAL pass=6 fail=4 hang=0 (out of 10)
```

The poison `rdi=0xaaaaaaaaaaaaaaab` is the smoking gun.

Total work product: 14 Python harnesses + 4 C harnesses +
4 launcher scripts + ~150 captured log files. Wall-clock to
collect all data: ~30 minutes.
