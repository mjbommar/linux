# D-KVM memo 11 — systrap gadget implementation plan

**Status:** DESIGN. Implementation sibling to memos 08
(real run_userspace), 09 (shadow PT), 10 (syscall
classification).
**Precedes:** the G1–G8 sub-commit ladder tracked as
tasks #204–#211.
**Parent:** `07-systrap-gadget-feasibility.md` (the 2026-
04-23 paper design memo that frames this work).
**Companion:** `06-sequencing/post-q1-push.md` §"Phase
IV" which deferred the gadget as a post-v1 workstream.

## Scope

Replace the current `out %al, $0xf4; sysretq` LSTAR
trampoline with a **gadget entry** that:

- handles ~11 hot-path syscalls entirely inside the
  guest with no VMEXIT (the "gadget path"), and
- falls back to the existing VMEXIT dispatcher for
  every other syscall (the "fallback path" —
  byte-identical to today's behavior).

Target numbers (from memo 07 §"Round-trip cost"):

| Silicon class | Gadget-path floor | Current D-06 number |
|---|---:|---:|
| Alder Lake @ 4-5 GHz | 60–80 ns | 4,000 ns |
| Zen 4 @ 5 GHz | 70 ns | 7,800 ns |
| Skylake @ 3.6 GHz | 80–90 ns | 11,000 ns |

That's a **40–150× speedup** on fast-path syscalls across
every host in the D-workstream inventory, matching the
vision's aspirational ~100 ns line without new hardware.

## First-pass gadget-safe syscall set

Eleven syscalls, chosen because every dependency each
handler needs is readable from guest-accessible state
without a VMEXIT. Each handler is ≤32 bytes of asm
except clock_gettime (≤96 bytes including the vvar read
loop).

| NR  | Name            | State source (guest-reachable) | Size |
|----:|-----------------|--------------------------------|-----:|
|  39 | getpid          | per-vCPU state page (G3)       |  ~24 |
| 110 | getppid         | per-vCPU state page            |  ~24 |
| 186 | gettid          | per-vCPU state page            |  ~24 |
| 102 | getuid          | per-vCPU state page            |  ~24 |
| 107 | geteuid         | per-vCPU state page            |  ~24 |
| 104 | getgid          | per-vCPU state page            |  ~24 |
| 108 | getegid         | per-vCPU state page            |  ~24 |
| 228 | clock_gettime   | shared vvar clock page (G5)    |  ~96 |
| 201 | time            | shared vvar clock page         |  ~24 |
|  24 | sched_yield     | return 0 (UML already cooperative on VMEXIT-HLT) | ~8 |
| 309 | getcpu          | per-vCPU state page            |  ~24 |

Footprint: ~320 bytes of asm + ~100 bytes jump table.
Fits inside one 4 KiB page with room for the existing
trampoline + TSS + IDT that already occupy the bootstrap
page.

Everything else — `read`, `write`, `mmap`, `brk`, any
I/O-touching call — flows through the fallback VMEXIT
unchanged. The class-D denylist (`ptrace`, `reboot`,
`bpf`, etc.) from memo 10 also falls through so the
host dispatcher still returns `-EPERM`.

## Bootstrap page layout (before vs after)

Current (memo 08 #5b):

```
+0x000  KVM_BOOTSTRAP_GDT_OFFSET      64 B  (8 × 8 GDT entries)
+0x040  KVM_BOOTSTRAP_LSTAR_OFFSET    5 B   (out $0xf4; sysretq)
+0x080  KVM_BOOTSTRAP_SYSRET_OFFSET   3 B   (SYSRETQ gadget)
+0x100  KVM_BOOTSTRAP_TSS_OFFSET      104 B
+0x180  KVM_BOOTSTRAP_IDT_OFFSET      528 B (33 × 16)
+0x400  KVM_BOOTSTRAP_PF_HANDLER      11 B
+0x1000 KVM_BOOTSTRAP_STACK_TOP        (IST stack top)
```

After (gadget v1):

```
+0x000  KVM_BOOTSTRAP_GDT_OFFSET       64 B   (unchanged)
+0x040  KVM_BOOTSTRAP_LSTAR_OFFSET    ~64 B   (gadget entry +
                                                 bounds check +
                                                 jump table dispatch)
+0x080  KVM_BOOTSTRAP_SYSRET_OFFSET     3 B   (unchanged)
+0x100  KVM_BOOTSTRAP_TSS_OFFSET      104 B   (unchanged)
+0x180  KVM_BOOTSTRAP_IDT_OFFSET      528 B   (unchanged)
+0x400  KVM_BOOTSTRAP_PF_HANDLER       11 B   (unchanged)
+0x500  KVM_BOOTSTRAP_GADGET_TABLE    ~100 B  (jump table: 11 × 8B)
+0x600  KVM_BOOTSTRAP_GADGET_HANDLERS ~320 B  (11 handler bodies)
+0x1000 KVM_BOOTSTRAP_STACK_TOP                (IST stack top;
                                                 unchanged)
```

Everything that's currently at a fixed offset stays put.
The gadget adds ~420 B in previously-unused space
between the #PF handler and the stack top. Plenty of
headroom for growth (there's ~3 KiB of dead space in
that window).

## Per-vCPU state channel (G3)

Single gadget-critical architectural addition. The
handlers need to read `current->{tgid, pid, uid,
euid, gid, egid}` without a VMEXIT. Shape:

```
struct kvm_gadget_state {
	u32 seq;              /* seqlock counter */
	u32 cpu_id;           /* vCPU index */
	pid_t tgid;
	pid_t pid;
	uid_t uid;
	uid_t euid;
	gid_t gid;
	gid_t egid;
	/* ~28 B; pad to 64 B for cache-line alignment */
};
```

Lives at a fixed guest-VA in the bootstrap page
(another unused offset, e.g. `+0x700`). `MSR_GS_BASE` is
programmed to that VA on every `kvm_enter_guest`, so
gadget handlers compute `%gs:0x0` for the seq, `%gs:0x8`
for tgid, etc.

Host-side update points (write the seq + fields +
incr seq):

- Scheduler entry (mm attach → pick up new task's creds)
- `sys_setuid` / `sys_setgid` / `sys_setresuid` / ... (cred changes)
- `sys_setsid` / `sys_setpgid` (session / pgrp changes affecting pgid)

Seqlock discipline: writer bumps seq (odd = in progress),
writes fields, bumps seq again (even = stable). Reader
samples seq, reads fields, re-samples seq; retry if
changed. ~3 cycle overhead per read.

**This is the one chunk with real design uncertainty.**
The interaction with UML's existing `current_thread_
info()` mechanism (which uses `%gs:0` for a different
purpose) needs careful handling: under the KVM backend
with the gadget active, guest-ring-3's `%gs` must point
at the gadget state page, not at whatever UML's
user-mode was using. Because UML user-mode today doesn't
dereference `%gs` at all (it's left as zero by glibc
until `arch_prctl(ARCH_SET_GS)` is called — rare), this
is safe in practice, but memo 11 flags it as the single
review point where a pre-spike is worth running.

## Shared vvar clock page (G5)

**Honesty gate**: memo 07 assumed UML already had vvar
machinery. Reading `arch/x86/um/vdso/um_vdso.c` shows
UML's current VDSO is degenerate — every entry just
issues a raw `syscall` (so the "VDSO path" and the
"syscall path" are identical). There's no shared clock
page to reuse.

That means G5 builds a vvar-style shared page **from
scratch**, not extends an existing one. Bumps the
estimate for G5 from 20–40 hrs → 30–50 hrs.

Design:

```
struct kvm_gadget_vvar {
	u32 seq;
	u32 _pad;
	s64 monotonic_sec;
	s64 monotonic_nsec;
	s64 realtime_sec;
	s64 realtime_nsec;
	/* pad to cache line */
};
```

Host-side updater: hook into UML's timer tick (the
mechanism UML uses to advance its own jiffies — see
`kernel/time/timer.c` + UML's `arch/um/os-Linux/time.c`)
to write fresh monotonic + realtime values into the
vvar page. Same seqlock discipline as G3.

Gadget-side handler for `clock_gettime`:

```asm
clock_gettime_handler:
	cmp     $CLOCK_MONOTONIC, %rdi
	jne     1f
	# read-side seqlock + format timespec
	...
	xor     %rax, %rax
	sysretq
1:	cmp     $CLOCK_REALTIME, %rdi
	jne     fallback          # CLOCK_TAI, CLOCK_BOOTTIME, etc.
	...
	sysretq
```

Other clockids fall through to the VMEXIT.

glibc goes through VDSO for clock_gettime, so glibc
workloads see no benefit from the gadget handler — the
benefit is for raw-syscall users (syzkaller repros,
benchmarks, stripped libraries). Call that out in the
commit message for G5.

## Sub-commit ladder

Each G-step is independently committable and independently
testable. Dependency arrows enforce the order.

- **G1 — memo 11 design doc.** (this file) Design only,
  no code. **IN PROGRESS.**
- **G2 — Lift #2b bench.** Swap LSTAR trampoline to
  `sysretq`-only variant via a gate (build-time kconfig
  or boot-time toggle), re-run perf-getpid across s0–s7,
  record floor. GO/NO-GO check against memo 07's <100 ns
  prediction. Pauses the ladder if the floor doesn't
  match. Blocked by G1.
- **G3 — per-vCPU state channel.** MSR_GS_BASE + gadget
  state page + seqlock. Host-side update hooks wired
  into scheduler entry + cred-changing syscalls. KUnit
  coverage for read/retry paths. Blocked by G2.
- **G4 — 7 pid-family handlers.** LSTAR gadget body +
  bounds check + jump table + `getpid`/`getppid`/
  `gettid`/`getuid`/`geteuid`/`getgid`/`getegid`. Uses
  G3's state page. In-guest smoke asserts gadget output
  matches fallback for each. Blocked by G3.
- **G5 — clock_gettime + shared vvar page.** Host
  updater + gadget handler for MONOTONIC + REALTIME.
  Blocked by G4.
- **G6 — sched_yield + time + getcpu.** Round out memo
  07's first 11. Blocked by G5.

  **Status (2026-04-24): sched_yield landed; time +
  getcpu deferred to G6-follow-on.** The LSTAR region
  hit a classical rel8-encoding reach problem when G6
  tried to add all three handlers at once: the dispatch
  table je rel8 to `clock_gettime` already sat at the
  max useful displacement, and clock_gettime's own
  fallback jnes needed rel32 to stay in reach. To keep
  the commit scoped + reviewable, G6 shipped just
  `sched_yield` (trivial 8-byte handler returning 0)
  plus the layout refactor: pid-family handlers now
  inline their own `swapgs; sysretq` tail (14 B each,
  no shared tail) and clock_gettime's 2nd + 3rd
  fallback jnes are now rel32. Total body grew from
  179 B (G5c) to 221 B. `time(2)` and `getcpu(2)` are
  tracked as G6-follow-on once we factor the LSTAR
  region into two pages or move the seqlock vvar into
  a dedicated reach-local sub-block.

  **Status (2026-04-24): G6-follow-on LANDED.** time(2)
  and getcpu(2) handlers landed as part of a 312-byte
  LSTAR rewrite that also carries two audit round-5
  fixes:
  - **F4** (P0 correctness): upper-NR guard prologue
    (cmp $0x135; test $0xffffff00) — without this,
    utimensat (NR 280 = 0x118) would alias sched_yield
    (NR 24 = 0x18) via the `cmp %al, imm8` dispatch.
    Real hijack bug, not theoretical.
  - **F7 part 2** (P1): clock_gettime seqlock now loads
    SEQ into %edx instead of %eax so RAX stays = 228
    across fallback paths (before this, any fallback
    would hand RAX=seq-value to handle_syscall as the
    NR — confusion bug).
  Both handlers added: time (23 B, single-read from vvar
  REAL_SEC, no seqlock retry — 1-sec resolution makes
  a torn read harmless) and getcpu (30 B, returns 0 and
  writes CPU_ID from gadget state page + node=0 since
  UML has no NUMA). Uses %edx for CPU_ID load to avoid
  clobbering RCX (user RIP for sysretq). Class-E count
  rises 9 → 11. KUnit 35/35 still green. perf-getpid
  dual-binary: gadget 98 cyc / 28 ns, fallback 150931
  cyc; ratio 0.001 (G7 gate 0.20 — 200× margin).
- **G7 — class_map E + perf-getpid gate extension.**
  Register gadget-handled syscalls as class E in memo
  10's inventory; perf runner picks up a second pass
  under `gadget=on` and gates the kvm_gadget:kvm_
  fallback ratio. Blocked by G6.

  **Status (2026-04-24): LANDED.** Added
  `KVM_SYSCALL_CLASS_GADGET = 4` to
  `arch/um/backend/kvm/kvm_backend.h`; populated 9
  entries in `syscall_class.c` (7 pid-family +
  `clock_gettime` + `sched_yield`); renamed inventory
  rows from A to E; updated memo 10 §"Class E". KUnit
  `kvm_syscall_classification_test` now checks the 9
  CLASS_GADGET bindings + count invariant (21 non-A:
  3 B + 1 C + 8 D + 9 E). Extended
  `run-perf-getpid.sh` to accept `UML_GADGET_BINARY`;
  when set, runs one pass against the gadget kernel
  (logged as `kvm`) and one against the fallback
  kernel (logged as `kvm-fallback`), then gates on
  `ratio_gadget_over_fallback ≤ MAX_GADGET_RATIO`
  (default 0.20). On dev host the measured ratio is
  0.001 — 1631× faster than the VMEXIT fallback,
  well under the 5× gate. Single-binary mode still
  works unchanged.
- **G8 — s0–s7 fleet bench + D70.** Full three-way
  comparison (ptrace / seccomp / kvm-fallback /
  kvm-gadget) across all eight hosts. Measurements.md
  table. D70 go/no-go decision on whether the gadget
  clears memo 07's prediction. Blocked by G7.

  **Status (2026-04-24): LANDED. D70 = GO.** Ran the
  dual-binary perf-getpid + clock-loop microbench
  across s0-s7 covering Skylake / Skylake-SP / Kaby
  Lake / Alder Lake (P+E) / Zen 4 silicon. Gadget
  cleared memo 07's <100 ns target on every host with
  a minimum 2.9× margin (s4 Alder Lake E-core, 34 ns)
  and a maximum 4.3× margin (Zen 4 s5-s7, 23 ns).
  Gadget:fallback ratio ≤ 0.012 on every host (vs the
  0.20 gate). See measurements.md G8 table and
  decisions-log D79 for the full dataset and the GO
  decision.

## Safety discipline

Principles the implementation honors, ordered by
importance:

1. **Fallback always armed.** Every gadget handler
   ends in `sysretq`. If the seqlock retry budget is
   exhausted OR the syscall args land outside the
   gadget's fast path (e.g. `clock_gettime(CLOCK_TAI)`),
   the handler jumps to the fallback `out $0xf4`
   trampoline. The VMEXIT path then runs the normal
   dispatcher. Worst-case gadget cost = fallback cost +
   ~20 cyc for the bounds check.
2. **`gadget=off` kernel cmdline override.** A kernel
   cmdline or kconfig flag forces every syscall to the
   fallback path, bypassing the gadget entirely. This
   is the regression-isolation switch: if a selftest
   goes red after gadget lands, we flip the toggle to
   confirm the gadget is the cause before bisecting.
3. **Seqlock retry budget.** Handlers retry up to 3
   times on seq mismatch, then fall through. Bounds
   worst-case cost; matches the upstream vDSO's
   pattern.
4. **Per-handler KUnit equivalence.** For every gadget
   handler, there's a contract test that executes the
   handler + executes the fallback path and asserts the
   two return values are byte-identical. A drift here
   IS the regression.
5. **No new host ioctls.** The gadget doesn't ask KVM
   for anything the current backend doesn't already
   use. MSR_GS_BASE is programmed via the existing
   `KVM_SET_MSRS` infrastructure from sub-commit #5c.
   The state page is allocated via `alloc_page` like
   the bootstrap page.
6. **SMP opt-in.** ncpus>1 isn't v1 scope — the gadget
   lands with ncpus=1 (memo 08 Open Q1). Multi-vCPU
   requires per-vCPU state pages + per-vCPU MSR_GS_BASE,
   which is ~1 more week of work tracked as a post-v1
   follow-on. `BUILD_BUG_ON(CONFIG_SMP)` keeps the v1
   honest.

## What this memo is NOT

- **Not a commitment to land every gadget handler
  upstream.** Upstream acceptance of the per-vCPU state
  channel + shared vvar page is a separate RFC (queue
  entry 7 in `upstream-patches/SUBMISSION-QUEUE.md`, or
  a new entry). v1 lands on our fork; upstreaming is
  the post-landing follow-on.
- **Not a promise of gVisor-parity performance.** gVisor
  has had 5+ years to tune their gadget; first-pass UML
  gadget will be noticeably slower and iterate. Memo
  07's floor predictions are upper bounds on what we
  should expect, not guarantees.
- **Not a replacement for the naive backend.** Fallback
  is the floor; gadget is additive. A `gadget=off` boot
  yields byte-identical behavior to today's KVM
  backend.

## Cross-references

- `07-systrap-gadget-feasibility.md` — parent paper
  design memo (feasibility; this memo extends into
  implementation).
- `08-real-run-userspace.md` — sub-commit #5c arch_prctl
  MSR_FS_BASE/MSR_GS_BASE infrastructure the G3 channel
  reuses.
- `10-syscall-classification.md` — class E gadget
  handler extension lands as a row-level edit to the
  static class_map + a KUnit count-invariant bump.
- `syscall-inventory.tsv` — updates to the 11 gadget
  rows (class column → `E`) when G7 lands.
- `measurements.md` — G2 adds a bench section; G8 adds
  the fleet sweep.
- `perf-getpid` selftest
  (`tools/testing/selftests/um/perf-getpid/`) — the
  measurement rig reused unchanged for G2 + G8.
- gVisor `runsc/platform/kvm/` + `runsc/hostsyscalls/` —
  reference implementation the design borrows from
  (memo 07 §"What the gadget does").
- `04-risks/decisions-log.md` D69 (G1 landing; this
  memo) + D70 (G8 landing; fleet results).
