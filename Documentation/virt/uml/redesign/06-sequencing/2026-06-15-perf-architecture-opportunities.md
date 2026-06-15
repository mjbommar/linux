# UML — Performance & Architecture Opportunities + Test-Capability Plan

**Date:** 2026-06-15
**Origin:** opportunities surfaced while executing the
`2026-06-14-upstream-technical-action-plan.md` items (esp. the F2 backend
benchmark), plus a corrected assessment of what is testable in this environment
(passwordless sudo + virtual networking — not the "needs lab hardware" I first
claimed).

Two tracks:
- **P-series** — performance / architecture improvements (ranked by leverage).
- **T-series** — test-capability work that unblocks F1/F3 here, without physical
  multi-host hardware.

Each item: **what / evidence / hypothesis / approach / how to measure / acceptance /
effort / risk.** Nothing here should be committed as a "fix" before the measurement
step confirms it — diagnose before act.

---

## P-series — performance & architecture

### P1. Profile and reduce the seccomp per-syscall trap cost (highest leverage)

- **What:** seccomp is the default backend and, per F2, the floor for every real
  workload (kvm-v2 is *slower* for syscalls that do host work). Cutting its
  per-syscall cost helps everyone.
- **Evidence:** F2 micro tier — seccomp getpid = **35,823 cyc / 9,444 ns** per
  call, a do-nothing syscall. That is the bare `SIGSYS` -> UML-kernel -> futex
  round-trip cost.
- **Hypothesis:** the cost is dominated by host signal delivery + the
  `wait_stub_done_seccomp` futex round-trip, not by useful work; parts may be
  shavable (batching, avoiding a wakeup, cheaper signal frame).
- **Approach:**
  1. Attribute the 9.4 us: `perf record`/`perf stat` on the host UML process during
     the getpid-loop; break down signal-delivery vs futex-wait vs dispatch vs
     return. Use `strace -c`/`-T` and ftrace on the stub path.
  2. Identify the largest component; prototype a reduction (e.g. coalesce the
     futex wake/wait, reduce signal-frame setup, fast-path the trivial-return case).
  3. Re-measure on the micro tier; guard against regressing correctness with the
     contract conformance suite + cpython parity.
- **How to measure:** `bench` micro tier (cyc/getpid) before/after; `py` tier and
  cpython-tier0 for a real-workload delta; `perf stat` syscalls/sec.
- **Acceptance:** a measured, reproducible reduction in cyc/getpid with no
  selftest regressions; documented breakdown even if no easy win (knowing where
  the 9.4 us goes is itself valuable).
- **Effort:** M (profiling) + M–L (any fix). **Risk:** medium — it is the hot
  trap path; correctness must be gated by the full suite.
- **Attribution (DONE, 2026-06-15):** `perf record -g` on the UML host process
  during a sustained `SYS_getpid` loop (seccomp backend). The cost is dominated by
  the **futex handoff + host scheduler**, not signal machinery:
  - ~51% in host syscalls the UML kernel issues per trap (`do_syscall_64`):
    - **`futex` 24.6%** -> `futex_wait` 19.0% -> **`__schedule` 15.4%** (dequeue_task
      / try_to_block_task / pick_next): the UML kernel thread **blocks on a futex
      and the host runs a full context switch on every guest syscall**.
    - `futex_wake` 5.1% (waking the stub child) -> `try_to_wake_up`.
    - `rt_sigreturn` / `restore_sigcontext` 5.4% (returning from the SIGSYS handler).
  - The rest (<1.5% each) is signal entry, the seccomp BPF filter, and dispatch.
  So the single largest reducible component is the **per-syscall
  block -> reschedule -> wake** round-trip between the UML kernel thread and the
  stub (`__schedule` alone is ~16%).
- **Optimization direction (next):** avoid paying a full host context switch when
  the stub will respond almost immediately — e.g. a bounded **spin-before-block**
  on the futex (adaptive, like `MUTEX_SPIN_ON_OWNER`), and/or co-schedule the UML
  kernel thread and its stub on sibling CPUs so the handoff does not go through
  `__schedule`. Prototype the spin-then-block first (smallest change), measure
  cyc/getpid on the `bench` micro tier, and gate correctness on the contract
  conformance suite + cpython parity. Caveat: spinning trades CPU for latency, so
  bound it and confirm it does not regress the SMP/many-thread cases.

### P2. kvm-v2 fs/socket regression: TLB-sync / per-mm-worker cost

- **What:** kvm-v2 is 1.5–2.5x **slower** than seccomp for trapping syscalls that
  touch the mm or do I/O (F2 py: fs 1.49x, sock 2.48x slower).
- **Evidence:** F2 per-section breakdown; the design forces a full `KVM_SET_SREGS`
  every 3 TLB generations to drop KVM's `prev_roots[]`
  (`KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD = 3`, `kvm_v2_backend.h`); dedicated
  `21-tlb-shootdown-gap.md`.
- **Hypothesis:** the per-mm worker context switch + the periodic full SREGS reload
  (TLB root drop) dominate the trapping-syscall path on mm-churning loads.
- **Approach:**
  1. Instrument: count `KVM_SET_SREGS` ioctls and prev_roots drops per syscall on
     the fs section; measure their share of the 44 us/syscall.
  2. Sweep `KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD` (1, 3, 8, 16, off) on the
     fs/sock sections; find the knee.
  3. Evaluate incremental SREGS sync (only the fields that changed) vs the full
     ioctl; evaluate whether `KVM_SYNC_X86_SREGS` in the shared `kvm_run` covers
     more cases without the ioctl.
- **How to measure:** F2 py fs/sock sections; ioctl counts via ftrace/`perf trace`.
- **Acceptance:** a threshold/sync change that narrows the fs/sock gap vs seccomp
  with no correctness regression (TLB-shootdown gap test, kvm-mm-smoke), or a
  documented finding that the gap is intrinsic.
- **Effort:** M. **Risk:** medium — TLB correctness is subtle; gate on the mm/TLB
  selftests.
- **Attribution (DONE, 2026-06-15):** `perf record -g` on the UML host process
  running an fs-syscall loop (open/write/close/stat/open/read/close x 2.5M) under
  each backend on the same kernel. Measured gap here: kvm-v2 **32.9 s** vs seccomp
  **28.2 s** = **~1.17x** (lighter than F2's py 1.5x — the regression magnitude is
  workload-dependent). Where kvm-v2's time goes (`kvm_v2_vcpu_run` 94%):
  - `KVM_RUN` ioctl path ~40% (`kvm_arch_vcpu_ioctl_run` 35%), of which
    **`vcpu_enter_guest` 15.7%** (the actual vmexit/guest-enter, `svm_vcpu_run`
    3.5%) — inherent to the design.
  - **SREGS reload ~9%**: `sync_regs`->`__set_sregs` **6.8%** + `__get_sregs` 2.3%
    — this is the `prev_roots`/TLB-lag tax (the P2 hypothesis), **confirmed real and
    tunable but NOT dominant**.
  - `kvm_load_guest_fpu` ~2%.
  For contrast, seccomp's fs cost is the **same futex->`__schedule` round-trip P1
  found** (futex_wait->schedule->`__schedule` ~35%). So neither backend has a cheap
  trapping-syscall path; kvm-v2 trades the futex/scheduler round-trip for a
  `KVM_RUN` vmexit of similar weight, plus the ~9% SREGS overhead.
- **Next (targeted):** the ~9% SREGS reload is the one clearly-tunable lever —
  sweep `KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD` (1/3/8/16/off) and measure the
  fs-loop wall + `__set_sregs` share, gated on the TLB/mm selftests. The larger
  `KVM_RUN`-vmexit cost is structural and not cheaply reducible.

### P3. Make the gadget state-page refresh lazy (KVM_RUN hot path)

- **What:** `kvm_v2_refresh_gadget_state()` refreshes identity + realtime +
  monotonic into the per-vCPU state page on **every** KVM_RUN entry, even when the
  guest issues no gadget syscall.
- **Evidence:** `vcpu.c:1458-1475` (called per entry; `kvm_v2_refresh_gadget_*`
  do `ktime_get_ns()`, `ktime_get_real_ts64()`, several `WRITE_ONCE`s + seqlock).
- **Hypothesis:** for trapping-heavy workloads (the common case) this is pure
  overhead on the hottest path and contributes to P2's per-entry cost.
- **Approach:** refresh lazily — only when something the gadget reads changed
  (identity: on cred/pid change or task switch; clock: on demand via the existing
  budget/seq mechanism, or only when the gadget actually faulted for a clock read).
  The budget field already exists to bound clock staleness; reuse it to drive
  refresh instead of refreshing unconditionally.
- **How to measure:** F2 py fs/sock sections (should drop if refresh was a real
  cost); micro getpid (gadget path must stay correct + fast); gadget KUnit.
- **Acceptance:** measurable per-entry cost reduction on trapping workloads with
  identity/clock correctness preserved (C3 equivalence still holds).
- **Effort:** M. **Risk:** medium — must not reintroduce the C2/C3 clock issues;
  gate on gadget tests + the time-travel gate.

### P4. static_call() the HOT contract ops in DYNAMIC builds

- **What:** In `CONFIG_UM_BACKEND_DYNAMIC`, the HOT ops dispatch through a function
  pointer (`um_backend->op()`); `*_ONLY` builds inline them. The contract doc flags
  invariant I2 (<=5% prod-fast regression from indirect dispatch).
- **Evidence:** `um_backend_dispatch` macro (`backend.h:270-274`); HOT ops marked in
  `struct um_backend_ops` (vcpu_run, mm_region_added/removed, context_switch,
  read_clock_ns).
- **Hypothesis:** the indirect branch is measurable on the hottest paths; since
  `um_backend` is immutable after `init_backend()`, a `static_call()` patched once
  at init gives direct-call speed in DYNAMIC builds too.
- **Approach:** define `DEFINE_STATIC_CALL` for each HOT op, update them in
  `init_backend()` after selection, route `um_backend_dispatch` for HOT ops through
  the static call in DYNAMIC builds. Keep the `*_ONLY` direct-inline path.
- **How to measure:** F2 micro/py on a DYNAMIC build before/after; compare to
  `*_ONLY` (the static-call path should approach it).
- **Acceptance:** DYNAMIC HOT-op dispatch cost approaches the inline build; no
  regression; conformance suite green.
- **Effort:** S–M. **Risk:** low (static_call is mainline-idiomatic), but it
  touches host-built TUs — confirm static_call is usable there or gate to
  kernel-side call sites.

### P5. Investigate timer-induced vmexits for compute-bound kvm-v2 guests

- **What:** F2's pure-CPU `hash` section trended slightly slower under kvm-v2
  (noisy, 2.8–11.5 ms). If real, it is vmexit jitter on a syscall-free workload.
- **Evidence:** F2 py hash section variance.
- **Hypothesis:** periodic timer ticks force KVM_EXITs during pure compute.
- **Approach:** first *confirm it is real* (more samples, isolate); count vmexits
  during the hash section; if timer-driven, evaluate longer tick / tickless or
  in-KVM timer.
- **How to measure:** vmexit counts during a syscall-free loop, multi-sample.
- **Acceptance:** either a confirmed+reduced jitter, or a "within noise" dismissal
  (don't chase noise — this is lowest priority).
- **Effort:** S (confirm) then M. **Risk:** low; likely noise.

**P-series ranking:** P1 (everyone benefits) > P2 (fixes the kvm-v2 regression) >
P4 (clean, broad) > P3 (helps P2) > P5 (confirm-first, likely noise).

---

## T-series — test capability (unblocks F1/F3 here, no physical hardware)

Correction to the prior plan: with passwordless sudo + virtual networking
(veth/netns/bridge/tap, `gretap`, `ip l2tp`) almost all vector2 datapath and
throughput validation is doable on this one host. Only *absolute physical-NIC line
rate* (10/40/100G) and NIC-offload-specific behavior are genuinely hardware-bound.

### T1. Real guest<->host throughput, vector vs vector2 (DONE)

- **Method:** `net-bench/run-tcp-throughput-via-umlctl.sh` (production umlctl
  fd-handoff: `/dev/net/tun` opened with `IFF_VNET_HDR` + `TUNSETOFFLOAD`, dup'd to
  known fds, exec UML). Real TAP datapath; needs sudo (have it).
- **Result (2026-06-15, this host, seccomp, 8s x 3):** vector median **39,668
  Mbps**, vector2 median **39,363 Mbps**, **ratio 0.992 -> PASS** (gate >= 0.85).
  vector2 is at **throughput parity** with legacy vector at ~39 Gbit/s.
- **Acceptance:** met (>= 0.85x). **Effort:** done.

### T1b. BUG: vector2 crashes when reusing a host tap a prior driver used

> **Correction to my first read of this.** I initially blamed a vnet_hdr/offload
> framing mismatch. That is **wrong** — controlled testing disproved it. The real,
> precisely-isolated trigger is **host-tap reuse across UML net drivers**, and the
> packets at crash time are tiny and normal. Recording the full diagnosis honestly.

- **What:** `net-bench/run-tcp-throughput.sh` creates one tap and runs legacy
  `vector` then `vector2` on it (per rep). The vector2 run takes
  `UML: fatal signal; exiting` during early TCP and reports `MISSING` / `FAIL`,
  while legacy vector passes in the same run.
- **Reliable repro (isolated this session):** boot a legacy-`vector` UML on a tap,
  power it off, then boot a `vector2` fd-handoff UML on the **same** tap and send
  TCP. Crashes **6/6**. Recreating the tap between the two runs: passes **3/3**.
  (Repro script kept at `/tmp/v2seq.sh` during the session; recipe: tap via
  `ip tuntap add ... mode tap`, legacy `vec0:transport=tap,ifname=$TAP`, then
  `vector2 vec2.0:transport=fd,fd=200` via a fd-inheriting wrapper, `backend=seccomp`.)
- **What it is NOT (each ruled out by test):**
  - *Not* a vnet_hdr/framing mismatch: a fresh no-vnet_hdr tap runs vector2 fine
    (ping + sustained 10s TCP, ~2.9 Gbit/s). Adding `IFF_VNET_HDR` to the shim
    only changed timing.
  - *Not* GSO/TSO/SG: `gso=0,gro=0,csum=0` still crashes; at crash time TX is
    `len=74 gso=0 nr_frags=0 iovcnt=1` (handshake) and RX is 70-107 byte ACKs —
    no large/segmented frames involved, bulk transfer never starts.
  - *Not* offload state: a tap pre-loaded with `TUNSETOFFLOAD(TSO)` then opened
    raw by vector2 runs fine. (`TUNSETOFFLOAD` is also per-fd and EINVALs on a
    no-vnet_hdr fd, so legacy's offload does not persist to vector2's fd.)
  - *Not* SMP: crashes at `ncpus=1` and `ncpus=2` equally.
  - *Not* in the RX/TX read/write calls: instrumented `os_read_file`/`os_writev`
    return normal values right up to the fault.
- **What it is:** a SEGV inside the UML kernel (last-ditch handler, no register
  dump), during early TCP, **only** when vector2 reuses a tap a prior legacy-vector
  UML attached/detached from. Timing-sensitive: `strace` and a KASAN build both
  perturb it (KASAN crashed even earlier at init with no net-path report —
  inconclusive). Signature (reliable + timing-sensitive + not in the obvious
  datapath + tied to resource reuse) points at a **use-after-free / lifecycle race**
  around channel/tap teardown+reattach, not a data-format bug.
- **Root cause: NOT pinned to a line** despite RX+TX printk, strace, and KASAN.
  Needs deeper tooling: gdb with `handle SIGSEGV` tuned to ignore UML's normal
  guest-fault SIGSEGVs and catch the fatal one, and/or KCSAN (race detector).
- **Done this session (validated):** fixed the harness — `run_bench_iter()` now
  calls `setup_tap` per driver run, so the bench no longer reuses a tap across
  drivers; vector2 stops crashing and produces a real result instead of `MISSING`.
  (The underlying driver bug remains.) Note the shim still reports a low ratio
  (~0.16) afterward, but that is a *separate* shim artifact, not a vector2
  regression: its `exec-uml-fd.py` opens a raw (no-vnet_hdr, no-`TUNSETOFFLOAD`) fd
  so vector2 gets no GSO (~2.3 Gbit/s) while legacy gets offload via UML's own tap
  open (~15 Gbit/s) — an apples-to-oranges comparison. The authoritative,
  offload-on-both-sides number is the umlctl harness (T1, 0.992 parity). The shim
  should be marked informational (raw-path only) or taught to set
  `IFF_VNET_HDR`+`TUNSETOFFLOAD` to match production; deferred as low value since
  T1 already gives the real comparison.
- **Impact:** **production single-driver use is unaffected** — vector2 over the
  umlctl fd path is at 39 Gbit/s parity (T1), and standalone vector2 on a fresh tap
  works at all sizes/durations. The crash needs two different UML net drivers
  sharing one host tap in sequence, which is a test pattern, not a deployment one.
- **Remaining (real, tracked):** root-cause and fix the vector2 tap-reuse SEGV with
  gdb/KCSAN; it is a genuine robustness bug (a guest should not crash because a host
  tap was previously used), just not on any production path.
- **Effort:** harness fix done (S); driver root-cause + fix is M-L (deep race/UAF
  debugging). **Priority:** medium (real bug, non-production trigger).

### T2. Multiqueue fairness over a real multi-queue TAP

- **What:** the open F3 "multiqueue fairness" gate — functional multiqueue already
  passes (`vector2-fd-multiqueue-smoke`, 4 fds); fairness = per-queue load
  distribution under concurrent flows.
- **Method:** create a multi-queue TAP (`ip tuntap ... multi_queue`), boot vector2
  with `queues=4`, drive N concurrent TCP flows (taskset/iperf3), read per-queue
  counters (`ethtool -S`, `/proc/net/...`, or vector2's own stats); compute the
  distribution (Gini / max-min ratio across queues).
- **Acceptance:** flows spread across queues within a documented fairness bound; no
  single-queue starvation.
- **Effort:** M. **Risk:** low. **Hardware-bound part:** none (TAP multi-queue is
  software).

### T3. Implement + validate the `raw` (AF_PACKET) vector2 datapath

- **What:** F1 — `raw` is the most-wanted missing transport (cross-host bridging).
  vector2 currently has only tap/fd runtime datapaths; raw is framing-only.
- **Method:** implement a `vector2_host_raw.c` host backend against the
  `um_vec2_host_ops` (tx_batch/rx_batch) using an `AF_PACKET`/`SOCK_RAW` socket
  bound to a host interface; **validate** by binding it to one end of a `veth`
  pair (or a bridge port) and running traffic to a peer netns / second UML guest —
  a real raw datapath, no physical NIC needed.
- **Acceptance:** packets flow over the raw datapath between two endpoints on this
  host; KUnit for framing + an integration smoke like the tap one.
- **Effort:** L (real driver work). **Risk:** medium; this is genuine datapath code
  and must be tested, not framing-only (avoid the "parser coverage" anti-pattern).

### T4. Implement + validate `gre` / `l2tpv3` datapaths against host tunnel endpoints

- **What:** F1 — GRE/L2TPv3 are framing-only today; wire them to real datapaths.
- **Method:** stand up the host tunnel endpoints with sudo (`ip link add type
  gretap`, `ip l2tp add tunnel/session`) between two netns or veth ends, connect
  vector2's gre/l2tpv3 datapath to them, and pass traffic end-to-end on this host.
- **Acceptance:** end-to-end traffic over each tunnel transport between two local
  endpoints; framing matches the kernel's own gretap/l2tp implementation
  (interop), not just self-consistency.
- **Effort:** L. **Risk:** medium; interop with the kernel tunnel drivers is the
  real test (catches framing drift the unit tests can't).

### T5. CPU-per-Gbit and syscall-rate accounting during throughput

- **What:** F3 — net throughput at 2x the CPU is not a win; measure efficiency.
- **Method:** during T1/T2 runs, `perf stat` the host UML process; count
  sendmmsg/recvmmsg and softirq time; report Mbit/s **and** CPU-cycles/Gbit for
  vector vs vector2.
- **Attempt + methodology finding (2026-06-15):** a first cut (`perf stat -e cycles`
  over a fixed-*time* 12 s send) is **invalid** and must not be reported as a CPU
  metric: both drivers burned ~58.6 G cycles regardless of bytes moved, because the
  UML host process runs flat-out for the whole wall-clock window. That makes naive
  "cycles/Gbit" just inverse-throughput (vector 0.36 vs vector2 2.17 G-cyc/Gbit only
  reflected 13.7 vs 2.2 Gbit/s), not per-byte work. Two fixes required for a valid
  number: (1) a **fixed-bytes** workload (send exactly N GB, measure cycles to move
  it), and (2) configure vector2 with **production offload** (`gso=1` +
  `IFF_VNET_HDR`+`TUNSETOFFLOAD`) — the manual fd setup used here did not engage GSO,
  so vector2 only reached 2.2 Gbit/s vs its real 39 Gbit/s on the umlctl path.
  Throughput parity itself is already established by T1 (0.992x); T5's CPU
  efficiency needs the corrected harness.
- **Acceptance:** a CPU-per-Gbit comparison from a fixed-bytes run with both drivers
  offload-on, accompanying every throughput number.
- **Effort:** S (redo with fixed-bytes + offload). **Risk:** low. **Lesson:** never
  derive a per-work CPU metric from a fixed-wall-time run of an always-busy process.

**Genuinely hardware-bound (out of scope here, needs a lab NIC):** absolute
line-rate at 10/40/100G, hardware offload (GRO/GSO/checksum) interaction, and
multi-physical-host topologies. Everything else above is reachable on this host.

---

## Suggested order

1. **T1/T2/T5** — finish the vector2 perf/fairness/efficiency story (mostly
   harnessing; high value, low risk; closes F3 here).
2. **P1** — profile the seccomp trap path (highest-leverage perf win).
3. **P2 + P3** — attack the kvm-v2 trapping-syscall regression (TLB sync + lazy
   gadget refresh), measured.
4. **P4** — static_call HOT dispatch (broad, clean).
5. **T3/T4** — implement + validate raw / gre / l2tpv3 datapaths (the real F1
   feature work, now testable via virtual topologies).
6. **P5** — only if the hash anomaly proves real.
