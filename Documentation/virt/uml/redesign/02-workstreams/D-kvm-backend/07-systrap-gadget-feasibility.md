# D-07 (spike): systrap gadget feasibility for M11

**Status:** feasibility memo (2026-04-23) — no implementation
commitment.
**Companion to:** `design-memo.md` §"Not in first-phase scope"
(which defers the gadget to a second phase), `measurements.md`
Spike 07 entries (SYSCALL + SYSRETQ instruction-pair cost),
`06-sequencing/post-q1-push.md` §"Phase IV".

This is the Phase IV Lift #2a deliverable — the paper design
that answers *"can a systrap-style gadget page live inside
UML's existing address-space conventions, and if so what is
the minimum achievable round-trip?"* before any implementation
commitment. Phase IV Lift #2b will back this memo with a
bench; Phase IV Lift #2c turns the combined evidence into a
decisions-log go/no-go entry.

## What the gadget does

In gVisor's Sentry-on-KVM model, the systrap gadget is a
short piece of ring-0 assembly that lives at a fixed
guest-VA reachable from guest userspace. When the guest
userspace issues `SYSCALL`:

- The CPU transitions to ring 0 with RIP = MSR_LSTAR, which
  points inside the gadget.
- The gadget inspects the requested syscall number and its
  args in registers / guest memory.
- **If the syscall is gadget-handleable** (getpid, getppid,
  clock_gettime from a shared-memory clock page, bit-vector
  tests, etc.) the gadget computes the result in ring 0
  *inside the guest*, writes it back to RAX, and executes
  SYSRETQ to resume userspace. **No VMEXIT.**
- **Otherwise** (read/write syscalls, mmap, fork, any IO-
  touching call) the gadget VMEXITs via a chosen magic
  instruction (VMCALL, IN, OUT, whatever) and the host
  backend dispatches the full syscall path.

The key insight: many common syscalls are pure computation
on state that can live in guest memory. Eliminating the
VMEXIT for those syscalls eliminates the 1–10 µs round-trip
that dominates the naive backend's per-call cost.

gVisor's Sentry implementation sits in `runsc/platform/kvm/`
and uses the `Stub` package; reviewed 2026-04-23. The gadget
is ~80 lines of assembly + ~200 lines of Go-side state
management.

## UML's VA model — what the gadget needs is already there

UML already reserves fixed-VA pages in every guest mm for
stub code + stub data:

```
                                                       host_task_size
  ...                                                            |
  (kernel VA — physmem + sanitizers + vmalloc)                   |
  TASK_SIZE ──────────────── (PGDIR_MASK-aligned stub_start)     |
  (guest userspace VA)                                           |
                                                      stub_start |
  [ stub code page          ] +1 × UM_KERN_PAGE_SIZE             |
  [ stub data pages × 2     ] +2 × UM_KERN_PAGE_SIZE   STUB_SIZE |
                                                      host_task_size
```

See `arch/um/kernel/um_arch.c:332` + `arch/um/include/shared/
as-layout.h:23-28`.

The stub reservation is **already the gadget slot** the
gVisor design asks for:

1. **Fixed guest-VA, constant across all mms.** `stub_start`
   is computed once at UML init and doesn't change per mm.
   Gadget code placed there is reachable at the same address
   from every guest userspace process, which is what the
   LSTAR trampoline needs (MSR_LSTAR is vCPU-global, not
   per-mm).
2. **Below `host_task_size`, above `TASK_SIZE`.** The
   reservation sits in a VA band userspace apps cannot
   normally map (TASK_SIZE is the upper bound for
   `current->mm`-visible userspace VA) but is still in the
   lower canonical half, so both guest userspace and guest
   ring-0 can reach it with standard long-mode addressing.
3. **Already-RX mapped in every mm via UML's own page
   tables.** The ptrace / seccomp backends both load the
   stub code into a real host-VA page and map it at
   `stub_start` in every guest mm via
   `arch/um/kernel/tlb.c`. The KVM backend gets this for
   free once it shares the same pgd layout (D-03c Policy A
   already registers UML's whole VA as a single KVM
   memslot, so every UML-side mapping — including the stub
   — is reachable in-guest).
4. **Under the D-workstream single-mm_struct model, no
   per-mm gadget setup needed.** gVisor installs the gadget
   per Sentry VM; UML can install it once in UML-kernel init
   and let every mm inherit it via the shared pgd.

No new VA is needed. No conflict with kernel VA. No conflict
with guest userspace VA. The stub reservation's purpose is
exactly what we'd allocate for a gadget.

## What changes from the current stub layout

Today's stub code (`arch/um/os-Linux/skas/stub.S`) is
seccomp-BPF + minimal SIGSYS handler for the seccomp
backend, and the signal-delivery trampoline for ptrace.
Under KVM, the stub body would be replaced with a gadget
body whose shape is:

```
  LSTAR handler entry:
      # RCX = return RIP, R11 = return RFLAGS, RAX = syscall #
      cmp   rax, SAFE_SYSCALL_NR_MAX
      ja    fallback
      mov   r10, [SAFE_SYSCALL_JUMP_TABLE + rax * 8]
      jmp   r10            # dispatch into per-syscall handler
  handler_getpid:
      mov   rax, [gs:task_tgid_offset]
      sysretq               # no VMEXIT
  handler_clock_gettime:
      # read shared clock page, format struct timespec, store to arg
      ...
      xor   rax, rax
      sysretq
  ...
  fallback:
      out   al, 0xf4        # VMEXIT to host; host backend handles
      # host skips `out`, does full syscall, returns here
      sysretq
```

The jump table + per-syscall handlers live in the stub
data pages (already 2 pages reserved via
`STUB_DATA_PAGES`). Compute budgets:

| Element | Size |
|---------|------|
| LSTAR entry + bounds check + jump-table dispatch | ~16 bytes |
| Per-syscall handler (simple) | ~32 bytes avg |
| Jump table | 8 bytes × N_SAFE_SYSCALLS |
| Fallback trampoline | ~8 bytes |

Even with 64 gadget-handleable syscalls, the code footprint
fits comfortably in the existing 1 × 4 KiB stub code page.
The jump table + per-syscall state fits in one stub data
page. No address-space reshuffle required.

## What gets harder — the honest blockers

**1. `gs:task_tgid_offset` — per-thread state access.**

The gadget handlers need to read per-task state without a
VMEXIT. Reading `current->tgid` from ring 0 guest requires:

- GS.base pointing at per-cpu / per-task state
- The offset of `tgid` within `task_struct` hardcoded into
  the gadget

UML's current `current_thread_info()` mechanism uses
`%gs:0` pointing at the task's thread_info, then chasing
`->task->tgid`. The gadget can replicate that chain — each
dereference is a ~1-cycle load — but the gadget must run
with GS.base pre-programmed per vCPU. Doable via MSR_GS_BASE
before VMRESUME.

**2. Shared-memory clock page.**

`clock_gettime` is a high-value gadget target (called
millions of times/second by some workloads). To handle it
in-gadget without a VMEXIT, the gadget needs a shared-memory
clock page the *host* updates asynchronously and the
*guest* reads. This is exactly Linux's `vvar` VDSO page
pattern; UML already has the vvar machinery. Gadget
handler = vvar read + format into the user's buffer.

**3. Gadget-safe syscall selection.**

The gadget can only handle syscalls whose entire effect is
computable from guest-reachable state. That list overlaps
with but isn't identical to what VDSO handles natively on
bare metal. First-pass candidates: `getpid`, `getppid`,
`getuid`, `geteuid`, `getgid`, `getegid`, `gettid`,
`clock_gettime(MONOTONIC/REALTIME)`, `time`, `sched_yield`,
`getcpu`. That's ~11 syscalls, which is enough to eliminate
the bulk of `getpid()`-microbenchmark cost — and is exactly
what gVisor's gadget covers first.

**4. Safety — what happens if the gadget gets a syscall
it thinks is safe but the kernel later invalidates?**

Classic example: `getpid()` in the middle of a TASK
reparent. The gadget reads task_struct->tgid; host kernel
changes it; gadget returns stale value. The host-side
mitigation is to **version** per-task state and have the
gadget re-check after the load. Adds ~3 cycles. Acceptable.

## Round-trip cost — what the spike should measure

Lift #2b's harness: extend `harness.c` with a variant-C
test that removes the `out` from the LSTAR trampoline so
it becomes pure `sysretq`. Guest-side variant is
identical to D-04c's variant-B: `mov $0x27, %eax;
syscall; hlt`. The LSTAR handler becomes just `sysretq`
(3 bytes). Measure cycles per iteration.

Expected cost: exactly SYSCALL + SYSRETQ. Spike 07 measured
240–340 cycles silicon-invariant across Kaby Lake / Skylake /
Zen 4 with the existing tests' `out; sysretq` LSTAR. The
~22k cyc of VMEXIT round-trip should vanish, leaving just
the instruction-pair cost.

Predicted results (derived from existing measurements):

| Silicon | Pure SYSCALL+SYSRETQ | Naive-KVM floor (VMEXIT path) |
|---------|---------------------:|---:|
| Alder Lake i9 @ 4.9 GHz | ~300 cyc / **61 ns** | 4705 cyc / 960 ns |
| Alder Lake i7 @ 5.0 GHz | ~370 cyc / **74 ns** | 5568 cyc / 1114 ns |
| Zen 4 @ 5.0 GHz | ~340 cyc / **68 ns** | 13148 cyc / 2630 ns |
| Skylake-SP @ 3.7 GHz | ~300 cyc / **81 ns** | 22994 cyc / 6210 ns |

All silicon hits **under 100 ns per gadget-handled syscall**
on modern clocks. That's the M11 vision line.

## Cost-benefit if the spike confirms the prediction

**Upside:**

- Gadget-handled syscalls hit ~100 ns on modern silicon,
  matching M11 without requiring new hardware.
- For workloads dominated by gadget-handleable calls
  (clock_gettime-heavy, getpid-benchmarks, sched_yield
  loops), the speedup over naive-KVM is **20–50×**
  depending on silicon.
- Gadget is additive: syscalls not handled stay on the
  naive path. Worst case = naive cost. Best case = 100 ns.
- The gadget mechanism is proven (gVisor ships it in
  production).

**Cost:**

- Gadget stub code is ~300 LOC of x86_64 assembly + ~200
  LOC of host-side setup. gVisor's is roughly this size.
- Shared-memory clock page requires host-side update loop
  (similar to vvar). UML already has vvar; this is
  extending, not inventing.
- Per-syscall handlers need unit coverage. A-05 contract
  suite extends naturally.
- Not free: gadget dispatch adds ~20 cyc overhead to
  fallback syscalls (the bounds check + jump-table
  lookup). Negligible against naive-KVM's ~22k floor.

**Risk:**

- **Safety regressions:** gadget reads task state
  concurrently with host updates. Mitigation: sequence-
  locked reads (cost ~3 cyc).
- **VDSO overlap:** glibc's clock_gettime already takes
  VDSO fast-path. Gadget helps only for workloads that
  bypass VDSO (raw syscall users: syzkaller repros, some
  benchmarks, stripped libraries).
- **Maintenance:** each new syscall number may need
  classification. ~11 first-pass gadget-safe syscalls
  cover 80%+ of the hot-path cost. Classification isn't
  load-bearing — misclassifying a gadget-unsafe call as
  safe triggers the safety-regression path above, which
  the version check catches.

## Recommended disposition (pre-spike)

1. **Run Lift #2b.** Minimum harness extension (strip the
   `out` from the LSTAR trampoline for variant C). Measure.
   If cycle count matches the spike-07 delta prediction
   within ~10%, the cost model is confirmed.
2. **Write Lift #2c.** Decisions-log entry synthesizing
   2a + 2b. Expected outcome: **GO for a post-v1 gadget
   workstream**, not a v1 critical-path item. Rationale:
   naive-KVM already hits vision for prod-fast on modern
   silicon (1 µs on AL i7); gadget is the aspirational
   100 ns target. Split-order: ship v1 with naive-KVM,
   ship gadget as a follow-up.
3. **Do NOT block v1 on gadget.** Per D56 (which already
   reached this conclusion pre-Phase-III), the gadget is
   optional-for-v1. Phase IV validates that with numbers.

## What this memo is NOT

- Not an implementation commitment. Lift #2c's decisions-
  log entry is where a commit-plan (if any) lands.
- Not a replacement for the naive backend. Gadget is
  additive; naive is the fallback.
- Not a claim that UML will hit gVisor's exact numbers.
  gVisor's gadget has had 5+ years of tuning; first-pass
  UML gadget will be noticeably slower and iterate from
  there.

## Cross-references

- `design-memo.md` §"Not in first-phase scope" — existing
  deferral; this memo formalizes the shape of the deferred
  work.
- `measurements.md` Spike 07 — measured SYSCALL + SYSRETQ
  floor that this memo's prediction extends.
- `04-ring-transition.md` §"What becomes the systrap
  gadget in phase 2" — pointer forward from the D-04
  architectural file.
- gVisor `runsc/platform/kvm/` + `runsc/hostsyscalls/` —
  the reference implementation the gadget design borrows
  from.
- `06-sequencing/post-q1-push.md` §"Phase IV" — the parent
  push-plan section this memo fulfills.
