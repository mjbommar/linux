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

### T1b. BUG: `run-tcp-throughput.sh` (non-umlctl shim) crashes vector2

- **What:** the *other* checked-in throughput selftest,
  `net-bench/run-tcp-throughput.sh`, **crashes** the vector2 guest with
  `UML: fatal signal; exiting` the moment the TCP sender runs; it reports vector2
  throughput as `MISSING` and `VERDICT: FAIL` while legacy vector passes at ~15
  Gbps in the same run.
- **Evidence (this session):** vector2 boot log shows vec2.0 up with IP/route, then
  fatal signal at `python3`. The harness's `exec-uml-fd.py` opens the TAP **without
  `IFF_VNET_HDR`** ("feeds raw Ethernet frames"), whereas the production umlctl path
  (T1, which passes) uses `IFF_VNET_HDR` + `TUNSETOFFLOAD`.
- **Hypothesis:** vector2's fd transport assumes a vnet_hdr-prefixed frame layout;
  fed raw frames it misparses lengths and faults. A header/offload **mismatch on an
  inherited fd should fail gracefully (error, link down), not crash the guest** —
  this is a robustness gap (same class as the make_umid NULL-deref: a crash where
  graceful failure is required).
- **Approach:**
  1. Confirm the trigger: rerun the shim with `IFF_VNET_HDR` added to
     `exec-uml-fd.py` -> expect it to pass (isolates vnet_hdr as the cause).
  2. Harden vector2's fd-transport setup to detect/validate the tap's vnet_hdr +
     offload state (e.g. via `TUNGETIFF`/`TUNGETFEATURES`) and refuse cleanly
     (driver init error, interface stays down) on mismatch instead of faulting.
  3. Fix the selftest: either set `IFF_VNET_HDR` in `exec-uml-fd.py`, or make it
     `SKIP` for vector2 and defer to the umlctl harness (T1).
- **Acceptance:** a vnet_hdr/offload mismatch on a vector2 fd transport yields a
  clean error, never `UML: fatal signal`; both throughput selftests pass or skip
  cleanly.
- **Effort:** S (test fix) + M (driver hardening). **Risk:** low; isolated to fd
  transport setup. **Priority:** high — it's a guest-crash on a malformed/mismatched
  input, exactly the robustness class worth fixing.

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
- **Acceptance:** a CPU-per-Gbit comparison accompanying every throughput number.
- **Effort:** S. **Risk:** low.

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
