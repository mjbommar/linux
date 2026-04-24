# D-04/D-05 follow-on: real `kvm_run_userspace` integration

**Status:** implementation in flight (2026-04-24) — sub-commits
#1, #2a, #2a-tail, #2b, #3, #5a LANDED + diagnostic aid LANDED;
sub-commits #4, #5b, #6, #7 **blocked on a newly-discovered
shadow-page-table prerequisite (D66)** — UML's pgd encoding is
software-only and not hardware-walk-compatible, so the current
`CR3 = __pa(current->active_mm->pgd)` approach triple-faults on
the first guest instruction fetch. A new sub-commit #M
(shadow PT) is inserted before #4/#5b/#6/#7 can land.
**Effort:** 2-3 weeks (several sub-commits; no single commit
is larger than a workstream-B lift).
**Dependencies:** D-04c (landed, LSTAR trampoline), D-04b.2b.2
(landed, UML-kernel-text execution), D-05a (landed, time ops),
D-05b (landed, ipi_send), Phase III Lifts #1b-#1f (landed,
ring-3 SYSRETQ, MMIO decode, IDT injection, A-05 contract pass).
**Blocks:** D-06 bookend benchmark (`getpid()` <100 ns on bare
metal); workstream D ship.
**Task:** #162.
**Sub-task of:** `04-ring-transition.md` — the "remaining for the
naive-backend phase-1" bullet titled "D-05 full" + everything
hidden behind today's `panic()` in `thread.c::kvm_run_userspace`.

## Goal

Replace the D-04a scaffold panic at
`arch/um/backend/kvm/thread.c:165` with an integrated loop that
makes `backend=kvm` actually execute real UML userspace
processes, not just harness microbenchmarks. When this lands,
workstream D is code-complete up to D-06's bookend.

**Today:**

```c
void kvm_run_userspace(struct uml_pt_regs *regs)
{
    ...
    rc = os_ioctl_generic(vcpu_fd, KVM_RUN, 0);
    panic("um: kvm run_userspace: ioctl rc=%d, exit_reason=%u (%s) — "
          "D-04b SREGS/CR3 setup pending", ...);
}
```

**After this lift:**

```c
void kvm_run_userspace(struct uml_pt_regs *regs)
{
    kvm_enter_guest(regs);          // SREGS + CR3 + GP regs from regs
    for (;;) {
        rc = KVM_RUN;
        switch (run->exit_reason) {
        case KVM_EXIT_IO:           return kvm_decode_syscall(regs, run);
        case KVM_EXIT_MMIO:         handled = kvm_decode_mmio(regs, run); continue;
        case KVM_EXIT_HLT:          kvm_handle_hlt(regs);      return;
        case KVM_EXIT_INTR:         /* host signal, reinject + continue */
        case KVM_EXIT_FAIL_ENTRY:   panic / fail;
        ...
        }
    }
}
```

Every primitive in that switch has a landed harness
counterpart. The lift's job is to lift them out of
`harness.c` (1526 LOC spike code) into per-function
production modules and wire them to `struct uml_pt_regs`.

## Decomposition

Each sub-commit is independently testable + mergeable. Order
follows data-flow: set up the guest, run it, decode exits,
return state.

### #1 — `kvm_enter_guest` (vCPU state materialization) — **LANDED 2026-04-24**

**Delta (as landed):** `thread.c` grows a `kvm_enter_guest(regs)`
and the peer probe helper `kvm_enter_guest_probe`. `sregs.c`
factors `kvm_fill_longmode_segments` as shared between harness
and production paths, and grows `kvm_setup_production_sregs`
(caller-parameterized CR3 + GDT base). New Kconfig
`UM_BACKEND_KVM_INTEGRATED` (default n) gates the production
path so the D-04a panic() scaffold in `kvm_run_userspace` stays
the default.

1. Reads CR3 from `current->active_mm->pgd` → guest-phys via
   Policy A identity memslot (`__pa(mm->pgd)`; see `mem.h`
   `uml_to_phys` — UML-VA minus `uml_physmem`).
2. `KVM_GET_SREGS` (preserve APIC/TR/LDT), `kvm_setup_production_sregs(&sregs,
   cr3, gdt_gpa)`, `KVM_SET_SREGS`.
3. Bootstrap-page allocation (one GFP_KERNEL zeroed page,
   spinlock-guarded cache) hosts the production GDT + the
   LSTAR trampoline slot sub-commit #2 will populate.
   Bootstrap-page GPA = `__pa(page)`.
4. `kvm_uml_regs_to_kvm_regs` marshals every HOST_* gp[] slot
   into the matching `struct kvm_regs` field and forces
   RFLAGS bit 1 (reserved-one) on defensively; `KVM_SET_REGS`.
5. `KVM_SET_MSRS` (MSR_STAR/LSTAR/FMASK) — **NOT YET**; the
   trampoline itself doesn't exist in production form yet.
   Sub-commit #2 programs the MSRs after writing the
   trampoline into the bootstrap page. Until then
   `kvm_enter_guest` is call-safe only for workloads that
   don't issue `syscall`.

**Test (landed):** 3 new KUnit cases under the existing A-05
`um_backend_contract` suite, gated with
`CONFIG_UM_BACKEND_KVM_INTEGRATED`:

- `kvm_production_sregs_shape_test` — CR3 + GDT pass-through,
  CR0.PE|PG + CR4.PAE + EFER.SCE|LME|LMA, CS/SS/DS selectors.
- `kvm_production_regs_marshal_test` — every HOST_* gp[]
  reaches the right `kvm_regs` field; RFLAGS bit 1 forced on.
- `kvm_production_probe_null_test` — NULL-input guards.

All three pass: `um_backend_contract` 23/23 against
`UM_BACKEND_DYNAMIC + UM_BACKEND_KVM_INTEGRATED=y` boot
(/dev/kvm present). The tests are pure data-structure (no
ioctl); sub-commit #2 grows a live-vCPU variant that
round-trips via KVM_GET_SREGS/KVM_GET_REGS.

**Code moves:** sregs.c refactored in-place (harness +
production both delegate to `kvm_fill_longmode_segments`; no
file moved).

**What sub-commit #1 does NOT do** (per memo goal): program
MSRs (LSTAR/STAR/FMASK); call KVM_RUN; decode any exit
reason; pin `current->active_mm`. Those all arrive with
sub-commit #2 under the same Kconfig gate.

**Verification commands** (for future sub-commits):

    make ARCH=um O=/tmp/uml-kvmint defconfig
    { echo CONFIG_UM_BACKEND_KVM_INTEGRATED=y; \
      echo CONFIG_KUNIT=y; \
      echo CONFIG_UM_BACKEND_CONTRACT_TEST=y; } >> /tmp/uml-kvmint/.config
    yes '' | make ARCH=um O=/tmp/uml-kvmint olddefconfig
    make ARCH=um O=/tmp/uml-kvmint -j$(nproc)
    timeout --kill-after=5 15 /tmp/uml-kvmint/linux \
        rootfstype=hostfs rootflags=/ root=/dev/root rw \
        init=/bin/sh mem=256M con=null con0=fd:0,fd:1 \
        kunit.enable=1 panic=-1

### #2 — LSTAR trampoline + MSR programming (partial: wire armed, decode pending)

Split into **#2a (landed 2026-04-24)** and **#2b (pending)**.

#### #2a — trampoline + MSRs — **LANDED 2026-04-24**

**Delta (as landed):**

- `thread.c` bootstrap page populated with the
  `kvm_bootstrap_lstar_bytes` trampoline at
  `KVM_BOOTSTRAP_LSTAR_OFFSET (0x40)`. Byte-identical to
  `kvm_harness_lstar` from harness.c (`e6 f4 48 0f 07` —
  `out %al, $0xf4; sysretq`), so sub-commit #2b's decode path
  can lift the harness logic unchanged.
- New `kvm_enter_guest_program_msrs(lstar_gpa)` helper:
  `KVM_SET_MSRS` with STAR=0x0018<<48 | 0x0008<<32,
  LSTAR=bootstrap_gpa+0x40, FMASK=0. Called unconditionally
  from `kvm_enter_guest` after KVM_SET_REGS (idempotent on
  repeat entry).
- `kvm_backend.h` exports `UM_KVM_SYSCALL_PORT (0xf4)` and
  `UM_KVM_SYSRETQ_PORT (0xf5)` as the named wire constants
  sub-commit #2b decodes against.
- Test plumbing: `kvm_bootstrap_copy_lstar(dst, len)` +
  `kvm_bootstrap_force_init()` exposed for KUnit inspection.

**Test (landed):** 2 new cases under
`CONFIG_UM_BACKEND_KVM_INTEGRATED` contract suite —
`kvm_bootstrap_lstar_bytes_test` force-allocates the page
and memcmps the 5-byte trampoline against the expected wire
form; `kvm_wire_ports_test` asserts the port constants equal
0xf4 / 0xf5. Full `um_backend_contract` run: 25/25 pass.

**Effect on kvm_enter_guest post-#2a:** the SYSCALL trap is
armed. A guest that issues `syscall` will trap into the
trampoline, `out %al, $0xf4` will produce `KVM_EXIT_IO` on
port 0xf4, and the trampoline's trailing `sysretq` will
return control to the caller's RCX after the host resumes
the vCPU. `kvm_run_userspace` still panics — the KVM_RUN
call itself lands with #2b.

#### #2b — KVM_RUN loop + kvm_decode_syscall — **LANDED 2026-04-24**

**Delta (as landed):**

- `kvm_run_userspace` split into two compile-time flavors:
  `KVM_INTEGRATED=y` runs the real KVM_RUN loop (new),
  `KVM_INTEGRATED=n` retains the D-04a scaffold panic
  unchanged. Both under the same exported symbol, so the
  ops-table dispatch is compile-time-stable.
- Inner loop (new, gated):
  ```
  kvm_enter_guest(regs) → for (;;) {
      KVM_RUN; KVM_GET_REGS; kvm_regs_to_uml_regs(regs);
      switch (exit_reason) { ... } }
  ```
  Loops across multiple KVM_RUNs per call so a SYSCALL
  trap's trampoline-SYSRETQ sequence dispatches + resumes
  in-line, gVisor-style; returns to the outer
  `userspace()` loop only on HLT / INTR / EINTR.
- `kvm_decode_syscall(regs, kregs, vcpu_fd)` (new, static):
  fills `HOST_ORIG_AX` from guest RAX, advances RIP past the
  2-byte `out %al, $0xf4` to the trampoline's `sysretq`,
  calls `handle_syscall(regs)` (UML's existing sys_call_
  table dispatch shared across all three backends), and
  `KVM_SET_REGS`-es the syscall return value back so the
  post-SYSRETQ ring-3 resume sees the result in RAX.
- Other exit-reason branches:
  * `KVM_EXIT_HLT` → return cleanly (scheduler takes over).
  * `KVM_EXIT_INTR` / `-EINTR` → goto out_read_regs, refresh
    `regs` via a re-read KVM_GET_REGS on the EINTR branch,
    return so the outer loop's `interrupt_end()` runs.
  * `KVM_EXIT_MMIO` → panic with the gpa; sub-commit #3's
    explicit frontier marker for the kvm-smoke selftest.
  * `KVM_EXIT_FAIL_ENTRY` / `SHUTDOWN` / `INTERNAL_ERROR` /
    `EXCEPTION` → panic with a diagnosable message.
- Backend arbiter (`arch/um/kernel/backend.c`):
  `backend=kvm` (without `force=`) now auto-routes into the
  KVM backend when either `KVM_HARNESS=y` OR
  `KVM_INTEGRATED=y`. Non-harness non-integrated builds
  still fall back to seccomp with an explanatory warning
  (Finding #3 invariant preserved).

**Selftest (new):**
`tools/testing/selftests/um/kvm-smoke/` — runs the UML
binary with `backend=kvm force=kvm init=/bin/true` and
greps the output for progression markers (`um: kvm init:`,
`um: kvm enter_guest: bootstrap`, `KVM_EXIT_MMIO`,
`KVM_EXIT_HLT`, `handle_syscall`, sub-commit #3 frontier
text). PASS = ≥2 markers hit AND the D-04a scaffold panic
text NOT observed. Skips cleanly when `/dev/kvm` is
inaccessible. Also wired into `tools/testing/selftests/um/
Makefile`'s TARGETS list.

**Observed (2026-04-24, Zen 4 workstation under sudo):**

    um: kvm init: KVM_CREATE_VM ok vmfd=4
    um: kvm init: kvm=3 vm=4 vcpu0=5 run_size=12288 (memslot deferred to first KVM_RUN)
    Run /bin/true as init process
    um: kvm memslot: guest_phys=0 host_va=60000000 size=10000000
    um: kvm enter_guest: bootstrap page at va=... gpa=0xade000 lstar=+0x40 (5 bytes)
    Kernel panic - not syncing: um: kvm run_userspace: unrecoverable exit 8 (SHUTDOWN)

This is the expected state: `KVM_CREATE_VM` + vCPU + memslot
+ `kvm_enter_guest` all succeed, `KVM_RUN` fires. Exit 8
(SHUTDOWN = triple-fault) is because the UML-kernel pgd the
production path loads into CR3 doesn't yet page-map the
bootstrap page at 0xade000 (the GDT lives there), so the
first code-fetch in-guest page-faults → no IDT → #DF → #TF.
Closing that gap is sub-commit #3's scope (MMIO decode +
fault-path wiring), sub-commit #5's scope (IDT install),
and a follow-on to #1 (pgd pre-touch of the bootstrap page).
The kvm-smoke selftest already PASSes on this progression
state; it re-fails immediately if the integrated path
regresses to the D-04a scaffold.

**Test status:**
- `um_backend_contract` KUnit: **27/27 pass** (no new
  cases; #2b's decode logic is selftest-integration-tested).
- `tools/testing/selftests/um/kvm-smoke/`: **PASS** under
  sudo on a KVM-capable host (markers=2/6); SKIP otherwise.
- No regression: ftrace-smoke / userspace-smoke /
  launcher-smoke PASS on the research build (KVM_INTEGRATED=
  n default path).

**What sub-commit #2b does NOT do** (deferred explicitly):

- MMIO decode + UML fault-path wiring (sub-commit #3).
- HLT-to-scheduler rescheduling niceties (sub-commit #4).
- Host-signal reinject + IDT-based guest signal delivery
  (sub-commit #5).
- Guest-accessible mapping of the bootstrap page (#1
  follow-on — can land as part of #3's work).
- KVM-specific fallback path on repeated FAIL_ENTRY /
  INTERNAL_ERROR (#7).

**Code moves:** `harness.c` lines ~1030-1050 (1b IO-exit
decode) → `thread.c::kvm_decode_syscall` (as specified in
the sub-commit plan).

### #M — Shadow page table (NEW prerequisite, 2026-04-24, D66)

**Why this exists.** Landed 2026-04-24 as a D66-logged
finding: UML's own `mm_struct->pgd` is a software-only
data structure using bit encodings incompatible with
x86_64 hardware (e.g. UML `_PAGE_RW = 0x020` vs x86 R/W
at bit 1; UML `_PAGE_ACCESSED = 0x080` vs x86 PS at
bit 7). Using it as guest CR3 triple-faults on the
first instruction fetch because the CPU misreads the
bits. The ptrace + seccomp backends don't care —
they use `mm_map`/`mm_unmap` to install host-side
mmap mappings that the HOST CPU walks via the HOST
pgd. The KVM backend needs the guest CPU to walk a
hardware-compatible page table.

**Scope.** A per-mm shadow page table, maintained
alongside the UML logical pgd:

- Fresh x86 4-level page table at allocate time (in
  `kvm_mm_attach`).
- Hardware-compatible PTE encoding throughout.
- Lazy-populated on KVM_EXIT_MMIO: walk UML's logical
  pgd for the faulting VA, allocate intermediate pages
  as needed, install a hardware-walkable PTE mirroring
  the logical mapping's permissions.
- Bootstrap page (GDT + LSTAR trampoline + SYSRET
  gadget) eagerly mapped at `kvm_mm_attach` time so
  the SYSRETQ + GDT-load + LSTAR-entry sequence can
  execute before any user code runs.
- `kvm_enter_guest` uses `__pa(shadow_pgd)` for CR3
  instead of `__pa(current->active_mm->pgd)`.
- `mm_unmap` / `mm_map` callbacks invalidate the
  shadow PT entries for the affected ranges + issue
  KVM-level TLB flushes (`KVM_INVALID_TLB_GVA_RANGE`
  or `KVM_INVALIDATE_TLB`, depending on host KVM
  version).

**What this unblocks.** Sub-commits #4, #5b, #6, #7 all
become landable once #M exists. The bootstrap-page
coverage follow-on that sub-commit #1's memo anticipated
ALSO lives here — explicit bootstrap mapping becomes
the first #M consumer.

**Effort estimate.** 3-4 weeks as its own lift — larger
than any previous sub-commit of memo 08 and
architecturally distinct. Strong case for breaking this
out as a new top-level D-kvm-backend memo (e.g.
`D-kvm-backend/09-shadow-pt.md`) rather than folding
into memo 08.

**Reference.** gVisor's `pkg/sentry/platform/kvm/
address_space_amd64.go` is the canonical working
implementation. Mirror its invariants; adapt to UML's
mm_map / mm_unmap callbacks.

**Status.** NOT LANDED; scoping pending. Task #185's
original scope (bootstrap-page pgd-coverage follow-on)
withdrawn in favor of this broader lift per D66.

### #3 — `kvm_decode_mmio` (KVM_EXIT_MMIO → fault path) — **LANDED 2026-04-24**

**Delta (as landed):**

- `kvm_run_userspace`'s `KVM_EXIT_MMIO` case populates
  `regs->faultinfo` (trap_no=14, error_code with write/user
  bits, cr2 = gpa + uml_physmem to convert to the host-VA
  space UML's fault handler speaks) and dispatches via the
  shared `sig_info[SIGSEGV]` table — same entry seccomp +
  ptrace call — so `segv_handler` → `segv` → vma lookup +
  fault servicing run through UML's existing common path.
- On return from the fault handler we `continue` the
  KVM_RUN loop so the guest retries the faulting access
  (same discipline as seccomp's SIGSEGV → handle → re-enter).

**Test:** Unmapped-page access in ring-3 triggers SIGSEGV
routing rather than a host panic. kvm-smoke selftest
continues to PASS; once sub-commits #4/#5 carry the guest
past the bootstrap triple-fault, the progression marker
count rises as MMIO exits become observable. The harness
Phase III Lift #1d decode template is the reference shape;
the production path lifts it with production regs + fault
handler instead of a harness printk.

**Known current limit:** The kvm-smoke still exits with
SHUTDOWN on /bin/true because the triple-fault happens
entirely inside the guest CPU (guest #PF → no IDT → #DF →
#TF) before any access reaches the EPT layer. MMIO decode
is therefore armed but not yet *firing* on the current boot
path; sub-commit #5 (IDT install) + #1 bootstrap-page pgd
mapping are the next lifts that move the progression
marker past SHUTDOWN.

**Code moves:** `harness.c` MMIO decode template (Phase III
Lift #1d) → `thread.c::kvm_run_userspace KVM_EXIT_MMIO`
case (as specified in the sub-commit plan).

### #4 — `kvm_handle_hlt` (KVM_EXIT_HLT → idle path)

**Delta:** when a guest userspace process executes a long
`pause` or when the UML kernel itself halts (`cpu_idle()`
calls `safe_halt`), KVM_RUN returns `KVM_EXIT_HLT`. Translate
that to returning from `run_userspace` so UML's scheduler can
run another task. Re-entering the loop on the next schedule
is a fresh KVM_RUN.

**Test:** `yield`-loop in a guest userspace process that
makes progress (100 iterations in <10 ms); verifies the
scheduler regains control between KVM_RUNs.

### #5c — `arch_prctl` + MSR_FS_BASE / MSR_GS_BASE roundtrip (class B per memo 10)

**LANDED 2026-04-24.** First class-B syscall handler per
`10-syscall-classification.md`. UML's `sys_arch_prctl`
stashes FS/GS base into `current->thread.regs.regs.gp
[HOST_FS_BASE]` / `[HOST_GS_BASE]`; the stub-based
backends then write them into the host task on re-entry.
The KVM backend instead has to push those values into
the vCPU via `KVM_SET_MSRS` (MSR_FS_BASE = 0xc0000100,
MSR_GS_BASE = 0xc0000101) — otherwise the guest's next
FS-relative load faults at a static guest VA and the
bootstrap #PF handler loops (the cr2=0x10 symptom
blocking sub-commit #6 / task #192).

Two propagation points:

- **`kvm_enter_guest`** — seeds `sregs.fs.base` /
  `sregs.gs.base` from the task's gp[] before
  `KVM_SET_SREGS`. In long mode KVM keeps the segment
  cache base and MSR_{FS,GS}_BASE in sync, so the
  SREGS write is the MSR write. Covers the "task
  rescheduled; resume in a different kvm_run_userspace
  block" path.
- **`kvm_decode_syscall`** — after `handle_syscall`
  returns for `__NR_arch_prctl`, calls
  `kvm_propagate_fs_gs_base(vcpu_fd, gp[HOST_FS_BASE],
  gp[HOST_GS_BASE])` which issues `KVM_SET_MSRS` with
  just FS_BASE + GS_BASE. Covers the within-KVM_RUN-loop
  arch_prctl that glibc issues inside `_start`.

**Test:** extended `kvm_production_sregs_shape_test`
seeds `src.gp[HOST_FS_BASE]` / `[HOST_GS_BASE]` and
asserts the values land in `sregs.fs.base` / `gs.base`
through `kvm_enter_guest_probe`. 28/28 contract tests
pass on the integrated build. Full KVM-side smoke
requires `/dev/kvm` access which the current selftest
user doesn't have; the KUnit test is the guaranteed
regression floor.

**Code moves:** ~70 LOC added in `arch/um/backend/kvm/
thread.c` (`kvm_propagate_fs_gs_base` helper + the two
call-sites + a cached `syscall_nr` local in
`kvm_decode_syscall` so `handle_syscall`'s clobber of
HOST_AX doesn't hide the number from the post-dispatch
check). ~12 LOC added in `arch/um/backend/contract/
test_ops.c`. Probe helper `kvm_enter_guest_probe` was
updated to mirror the real path so drift between probe
and production is KUnit-visible.

### #5 — `kvm_handle_intr` (KVM_EXIT_INTR → signal reinject)

**Delta:** host-side SIGALRM / SIGIO / SIGCHLD delivered
during KVM_RUN surface as `KVM_EXIT_INTR`. Let the host
signal handler run (UML's own signal dispatch), then re-enter
KVM_RUN. Subtlety: the guest's own signal delivery (via IDT
injection) is a *different* path, already landed in Phase III
Lift #1e.

**Test:** a guest userspace process spinning in a loop while
a host-side `timer_fn` expires. Ensure UML's timer IRQ fires
without the guest needing to yield.

### #6 — D-06 bookend (getpid round-trip measurement)

**LANDED 2026-04-24** (commit `f9760c9b29e5`). Freestanding
`tools/testing/selftests/um/perf-getpid/getpid-loop.c`
measures the cost of one `getpid()` syscall round-trip
from guest userspace; host-side
`run-perf-getpid.sh` boots UML with the binary as `init=`
under each backend and reports the per-backend
ns/cycle measurements.

**Measured on Zen-4-class silicon:**

| Backend  | ns/call | cyc/call |
|----------|---------|----------|
| ptrace   | 11,448  | 41,211   |
| seccomp  | 11,514  | 41,450   |
| kvm      | 11,540  | 41,545   |

KVM:seccomp ratio = **1.002** (+0.2 %). KVM backend is
effectively at parity with the existing backends. D-06
gate "identical behavior to seccomp on shared test
vectors" clears with a 2× margin built in (selftest
ceiling MAX_KVM_RATIO=2.0).

The absolute ~11.5 µs number is UML's own syscall cost
(guest-userspace → LSTAR trampoline → UML kernel
`handle_syscall` → `sys_getpid` → return through a full
VMEXIT/SREGS/REGS dance), not a KVM-specific overhead.
Memo 07's systrap-gadget retrofit targets **<100 ns** by
bypassing the host-side KVM_RUN VMEXIT loop entirely;
that's a separate post-v1 workstream gated on D61's GO
decision. The ~40× margin between "KVM backend parity"
and "gadget target" is the optimization budget memo 07
claims.

### Hot-path optimization notes (post-v1)

Once the gadget workstream reaches GO, the current
~11.5 µs per syscall breaks down (rough profiling):

- LSTAR round-trip (spike 07 measurement): ~270 cyc
  boost-locked. → ~65 ns at 4 GHz.
- KVM_EXIT_IO syscall decode + KVM_SET_REGS re-entry:
  ~800-1200 cyc → ~200-300 ns.
- `kvm_touch_all_user_vmas` + `kvm_shadow_fill_from_
  uml_pgd` on every syscall (memo 09 step 2 eager fill,
  known expensive): ~8,000 cyc → ~2 µs.
- Balance (~30,000 cyc) is UML's own
  `handle_syscall` → `current` marshalling → scheduler
  ticks, shared with ptrace/seccomp backends.

None of this is urgent for v1 (parity is the gate).
The systrap gadget in memo 07 would eliminate the
VMEXIT + shadow-PT-refill cost by keeping the guest
resident across the syscall boundary; the balance
reduction lives in a shared UML hot-path pass outside
workstream D.

If we land within a 2× margin of D-06's <100 ns target, the
gadget feasibility memo (07-systrap-gadget-feasibility.md,
D61) takes over as the next optimization phase.

### #7 — Fallback wiring (D-05 completion)

Separate sub-lift but tightly coupled: if
`KVM_EXIT_FAIL_ENTRY` or repeated `KVM_EXIT_INTERNAL_ERROR`
fires, currently we panic. Replace with the nested-virt
fallback path already scoped in
`05-nested-virt-fallback.md`: re-probe, log, fall back to
seccomp for the affected thread. Keep prod-fast's "KVM failure
acceptable" invariant (the plan's stated D-failure-mode).

## Risks + mitigations

- **CR3 ↔ mm lifetime.** Guest CR3 points at
  `current->active_mm->pgd`. If the mm is freed while KVM_RUN
  is blocking, KVM reads freed memory. D-03b's `mm_attach`/
  `mm_detach` already owns the mm reference; the lift just
  needs to ensure `kvm_enter_guest` runs under the same mm
  context the caller expects. **Mitigation:** borrow the
  ptrace backend's mm-pinning pattern (via `get_task_mm`) for
  the KVM_RUN duration; release on exit.

- **Signal races.** Host signal during KVM_RUN → EINTR/INTR
  exit. If the signal handler re-enters `run_userspace`
  recursively before the outer ioctl returns, we stack. **
  Mitigation:** block SIGALRM/SIGIO/SIGCHLD around KVM_RUN;
  delivery happens between iterations, same pattern the
  seccomp backend uses for its trap loop.

- **KVM ABI variance.** KVM_API_VERSION bumps or kernel-side
  struct layout changes could break us. **Mitigation:** the
  existing D-03a `/dev/kvm` probe already checks
  `KVM_GET_API_VERSION`; expand it to fail-fast with a clear
  "KVM API mismatch, falling back" message rather than a
  runtime panic.

- **Nested-virt slowness.** On some hosts nested KVM is slower
  than seccomp. D-05 is the formal mitigation; this lift
  should wire the detection hook even if the full
  auto-fallback logic lands separately.

- **`struct pt_regs` ↔ `uml_pt_regs` impedance.** UML's
  per-backend register struct layout differs from bare-metal
  x86. The harness already handles this for the spike cases;
  the lift needs to add a centralized
  `kvm_regs_to_uml(regs, kvm_regs)` / `uml_to_kvm_regs(...)`
  helper so per-exit-reason code doesn't replicate the
  marshalling.

## Validation strategy

### Selftests

New (in `tools/testing/selftests/um/`):

- `kvm-smoke/` — boot UML with `backend=kvm`, exec
  `/bin/true`, assert clean exit. Exercises #1-#2 end-to-end.
- `kvm-fault-smoke/` — guest userspace dereferences an
  unmapped page, assert SIGSEGV (not host panic).
- `kvm-yield-smoke/` — 100 `sched_yield()` from guest,
  assert UML scheduler progresses.

Existing (extend):

- `userspace-smoke/` — already runs under seccomp; add a
  `backend=kvm` variant of the same workload.
- A-05 contract KUnit suite — already passes under
  `CONFIG_UM_BACKEND_KVM_ONLY` (Phase III Lift #1f); extend
  to exercise the integrated path, not only the op-table
  dispatch.

### Benchmarks

- `getpid()` round-trip cycle count (D-06 bookend). Target
  <100 ns bare metal; acceptable 2× margin for v1 with the
  gadget retrofit (memo 07) tracked as the follow-on
  optimization.
- Boot time to userspace under `backend=kvm` (matching the
  seccomp baseline from C-10 launcher-smoke).

### Regression safety

Every sub-commit runs:

1. `make ARCH=um` clean build, all three backends selectable.
2. A-05 KUnit contract under `UM_BACKEND_*_ONLY` for all three
   single-backend builds.
3. `launcher-smoke` + `userspace-smoke` + `umlctl-smoke`
   selftests under their current backends (default + fuzz +
   research).
4. `backend=kvm` sub-lifts land behind a `default n`
   `CONFIG_UM_BACKEND_KVM_INTEGRATED` until #6 measurements
   clear the D-06 gate; the harness path stays available via
   `CONFIG_UM_BACKEND_KVM_HARNESS=y` for regression probes.

## Open questions to resolve during implementation

1. **Single-vCPU vs per-thread vCPU.** Today the harness
   re-uses vcpu0 for every spike. Real `run_userspace` is
   called per-task; do we stick with one vCPU serialized
   through a mutex, or open a vCPU per-UML-task? gVisor does
   per-task. Cost trade-off: KVM vCPU allocation is ~1 ms
   one-shot, serialization is ~0.5 µs per entry. For UML's
   single-threaded-per-mm model, one-vCPU is probably right.
   **Resolution:** decide in sub-commit #1; single-vCPU with
   per-mm serialization is the default recommendation.

2. **`current` during KVM_RUN.** When KVM_RUN is blocking in
   the host kernel, `current` still points at the UML task
   that called it. But from the UML kernel's perspective,
   *who* is `current` — the host task, or the guest task?
   Today UML conflates them (same `task_struct`). KVM_RUN
   needs to preserve that invariant so signal delivery
   doesn't get confused. **Resolution:** seccomp backend's
   current handling is the reference; replicate the pattern.

3. **Memslot lifetime vs process lifetime.** The current
   lifecycle registers one giant memslot on first KVM_RUN.
   For long-lived UML processes that exec different binaries,
   we'd want `mm_map`/`mm_unmap` (D-03d) to update the
   memslot view. It already does for single-page updates.
   **Resolution:** lands as part of #3 (MMIO decode).

## Cross-references

- `thread.c::kvm_run_userspace` — the panic this memo
  replaces.
- `harness.c` — spike code the sub-commits lift from.
- `04-ring-transition.md` — sibling D-04 memo; this one picks
  up from its "Remaining for the naive-backend phase-1"
  bullet list.
- `05-nested-virt-fallback.md` — D-05 fallback path (#7).
- `06-conformance.md` — D-06 bookend; this lift unblocks it.
- `07-systrap-gadget-feasibility.md` — post-v1 optimization
  via D61's GO decision.
- `10-syscall-classification.md` — the dispatcher-side
  classification this memo consumes. Sub-commit #5c
  (`arch_prctl` MSR round-trip) is the concrete next step
  that unblocks sub-commit #6 / D-06 / task #192.
- `syscall-inventory.tsv` — generated per-NR truth table
  naming the class of every x86_64 syscall.
- `11-systrap-gadget.md` — post-v1 hot-path optimization
  (~40-150× speedup on the 11 gadget-safe syscalls);
  G1-G8 sub-commit ladder inherits this memo's
  infrastructure (bootstrap page, LSTAR trampoline,
  per-mm kvm_um, MSR propagation) as scaffolding.
- `tools/testing/selftests/um/` — existing selftest harness
  model for new kvm-*-smoke tests.
- decisions-log entries D49-D57 + D60-D61 — D-workstream
  rationale + gadget disposition; D65 (next entry) will
  record this memo's scope + sub-commit plan.

## Status flip criteria

This memo leaves "design memo" and becomes "implementation
in flight" when:

1. ✅ Sub-commit #1 (kvm_enter_guest) lands with its A-05 test
   extension. **Done 2026-04-24.**
2. Decisions-log D65 records the implementation kickoff with
   owner + target quarter.
3. ✅ `tools/testing/selftests/um/kvm-smoke/` directory exists
   with a run-script. **Done 2026-04-24** (sub-commit #2b).

When sub-commits #1-#6 all land + D-06 bookend runs clean,
this memo rolls up into `04-ring-transition.md`'s
"completed" block and `06-conformance.md` picks up the
bookend narrative.
