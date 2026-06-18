# UML — seccomp trap-path: measured debugging + optimization proposal

**Date:** 2026-06-18
**Method:** five measurement iterations on the seccomp `getpid` round-trip
(`tools/testing/selftests/um/perf-getpid`), kernel `uml-clean` (HOSTFS +
FRAME_POINTER, **no** KASAN/KCOV — representative), host = 16-core, single-CPU
pinned unless noted. All numbers are medians of 3–4 reps with non-overlapping
spread. Reverts clean (no working-tree changes shipped).

This extends `2026-06-15-perf-architecture-opportunities.md` P1. P1 attributed
the trap cost host-side (futex→`__schedule`); this doc adds a guest-side syscall
census, an **upper-bound measurement** of one concrete cut, and a refutation of
P1's deferred "co-schedule on separate CPUs" lever.

---

## Measured foundation

**Per-`getpid` seccomp trap (baseline, 1 CPU):** **31,958 cyc / 8,425 ns.**

**Host syscalls issued per single guest `getpid` (strace -f -c, exact counts):**

| host syscall | per getpid | role |
|---|---|---|
| `futex`       | ~4.2 | 2× WAKE + 2× WAIT — the stub↔kernel ping-pong |
| `arch_prctl`  | ~2.1 | GET_FS + GET_GS resync in `stub_signal_interrupt` |
| `rt_sigreturn`| ~1.0 | return from the SIGSYS handler |

**Component sizing (independent microbench + removal experiment):**
- Raw 2-switch futex ping-pong on one CPU = **~15,200 cyc** → the bare handoff is
  **~48% of the whole trap**. (Corroborates P1's futex→`__schedule` 19–35%.)
- Removing the 2× `arch_prctl` resync (measured by build-flag removal) =
  **31,958 → 29,104 cyc, −8.9% / −755 ns**, 4 reps, zero overlap.

**REFUTED — affinity/CPU-separation (P1's deferred "real lever"):**
forcing stub + kernel-thread onto **one** CPU (31.9k cyc) is consistently
**~6% faster** than two CPUs (34.0k); free placement is worst (33.6–35.0k).
The handoff is a **strictly serial ping-pong** — the two tasks never run
concurrently, so a second CPU only adds IPI + cross-core cache-bounce to a
serial RPC. **Do not pursue affinity separation; co-location is optimal.** This
reorients the search away from parallelism toward (a) cheaper same-CPU switch
and (b) fewer crossings.

---

## Proposal — 10 optimizations, ranked

### Tier 1 — measured / high-confidence (UML side)

**O1. Single-thread-mm fast path for the FS/GS resync (`arch_prctl` GET ×2).**
*Measured −8.9%; correctness constraint fully characterized by test (below).*
`stub_signal_interrupt` unconditionally does `ARCH_GET_FS`+`ARCH_GET_GS` each
trap (`kernel/skas/stub.c:130-133`) to keep UML's per-thread saved FS/GS
accurate (`arch/x86/um/os-Linux/mcontext.c:195,266-271`). The GET defends the
**untrappable** `wrfsbase`/`wrgsbase` path (FSGSBASE), so it cannot be dirty-
tracked (no trap to hook) — my first instinct was wrong. **Correct fix: gate the
GET on whether UML is multiplexing >1 thread on this mm/stub.** A single-threaded
mm has no other thread to clobber, so the GET is pure overhead there; capture
FS/GS on demand only when UML genuinely needs it (context switch, signal frame,
ptrace, coredump — all rare). Recovers the full ~9% for single-threaded
processes (cpython's process-parallel regrtest workers, most CLI tools) with
zero correctness loss; multi-threaded mms keep the per-trap GET. *Effort S–M;
risk low — gate on the MT wrfsbase test + cpython parity.*

> **Validation (2026-06-18).** Built baseline + GET-skipped kernels from the
> same tree. (a) getpid: 31,958→29,104 cyc, **−8.9%**, 4 reps no overlap.
> (b) 32-thread pthread `__thread` test: PASS on both (glibc TLS uses the
> *trapped* arch_prctl, which UML observes). (c) single-thread `wrfsbase`
> across a syscall: SURVIVED on both (UML doesn't rewrite FS it thinks is
> unchanged). (d) **4×`CLONE_THREAD` + `wrfsbase` across context switches:
> baseline `clob=[0,0,0,0]` CLEAN; GET-skipped `clob=[402,399,367,386]`
> CLOBBERED.** (d) is the load-bearing case and the exact boundary the
> single-thread gate must respect.

**O2. Coalesce the futex WAKE+WAIT pair on each side.** Both stub
(`stub.c:139/144`) and kernel (`os-Linux/skas/process.c:90/110`) issue a separate
`FUTEX_WAKE` then `FUTEX_WAIT` — 2 syscalls where the kernel offers combined
ops. Folding to one cmpxchg-guarded wait removes ~2 of the 4.2 futex
syscalls/trap. UML-only, no kernel change. *Effort S; risk low.* (Ceiling capped
by O6.)

**O3. Co-location guarantee (turn the refutation into a guard).** The ping-pong
is serial; ensure the host scheduler keeps the pair on one CPU and add a
`WF_CURRENT_CPU`-style local-wake hint on the `FUTEX_WAKE`. Equally important:
forbid any future SMP/vCPU design that spreads the trap pair across cores.
*Effort S; risk low; mostly "don't regress."*

### Tier 2 — large ceiling (upstream kernel side)

**O4. Directed-handoff primitive (FUTEX_SWAP / UMCG).** *Highest ceiling.* The
2-switch futex handoff is ~48% of the trap (~15.2k cyc). A "wake target + block
self + switch now, skip the scheduler pick" primitive collapses 4 futex
syscalls→~2 **and** removes `dequeue/pick_next/enqueue`. Estimated ceiling: up to
~½ of the handoff → **~24% of the whole trap**. Prior upstream history
(`FUTEX_SWAP` RFC, UMCG) and a broad constituency (gVisor, Wine, qemu-user, rr)
make it worth pushing upstream rather than carried out-of-tree. *Effort L
(upstream); risk med.*

**O5. SYSCALL_USER_DISPATCH instead of seccomp-BPF for guest interception.** SUD
was added for exactly this use case (emulators intercepting syscalls); it avoids
the per-syscall BPF eval + IP-range filter (`stub_exe.c:144-197`) and is the more
idiomatic mechanism. Win on its own is modest (BPF eval <1.5% per P1) but it is
the cleaner substrate for a fast dispatch and pairs with O4. *Effort M; risk
med — changes the interception ABI; gate on the full contract suite.*

**O6. Cheaper SIGSYS delivery / lighter signal frame.** Lazy-FPU on signal entry
and a slimmer sigframe attack the ~1 `rt_sigreturn`/trap + frame setup. Upstream
signal-path work, broad benefit. *Effort M–L; modest (~5–6%).* 

### Tier 3 — targeted / structural

**O7. Expand the in-stub / vDSO gadget set for stateless syscalls** (clock reads,
getpid-class) so they are serviced in the stub and **never cross** to the kernel
thread — removes the entire handoff for those calls. This is "avoid the crossing"
in its tractable form (mirrors the kvm-v2 clock gadget). Bounded to syscalls that
need no UML-kernel state. *Effort M; risk med — per-syscall observational
equivalence; gate per gadget.*

**O8. Lazy / conditional task-state save-restore.** `get_stub_state` /
`set_stub_state` + FPU/XSAVE run each trap; many syscalls touch no FP and few
registers. Sync only what changed. *Effort M; risk med — subtle state bugs; gate
on signal/FP selftests.*

**O9. Tighten the seccomp filter ordering / SECCOMP_CACHE.** The filter does an
IP-range + arch check + linear syscall scan every trap; reorder by frequency and
evaluate `SECCOMP_CACHE`. Small (<1.5%) but free and low-risk. *Effort S; risk
low.*

**O10. Batch syscall submission from guest userspace (io_uring-style).** For
syscall-bound guest workloads, let guest userspace submit a batch the stub
services in one crossing — amortizes the handoff over N syscalls. The only lever
that beats "make one crossing cheap" is "make fewer crossings." *Effort L; risk
med; high ceiling for specific workloads, niche otherwise.*

---

## Suggested order

1. **O1** (measured −8.9%, do the correct version) + **O2** + **O3** — bankable
   UML-side wins, ~10–15% combined, low risk.
2. **O4** — the directed-switch primitive; biggest ceiling, upstream effort,
   broad constituency. Prototype against a microbench before committing.
3. **O7** (gadget expansion) and **O8** (lazy state) — structural, medium.
4. **O5/O6/O9** — substrate cleanups; **O10** only for syscall-bound workloads.

**Discipline note:** O1's −8.9% was measured with correctness skipped, but the
correctness boundary is now *characterized by test* (the MT-`wrfsbase` clobber),
so the production patch has a concrete, testable target — the single-thread-mm
gate — rather than a hand-wave. Two initial instincts were killed by
measurement: the affinity lever (1 CPU beats 2) and the "dirty-track TLS" idea
for O1 (`wrfsbase` is untrappable). Nothing is committed as a fix before its
measurement step confirms it.
