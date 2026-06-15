# UML v2 — Technical Architecture & Implementation Plan (internal)

**Date:** 2026-06-14 (last refocused 2026-06-14)
**Scope:** the real engineering gaps to close **in the branch itself**. This is an
internal "make the architecture correct and complete" plan — **not** an upstream
submission plan. All upstream packaging (rebasing to mainline, `Fixes:`/`Cc:
stable` tags, DCO trailers, series splitting, merge-window sequencing, cover
letters) is explicitly out of scope here and lives elsewhere if/when we choose to
submit.

**Provenance:** the gaps were surfaced by a simulated maintainer/reviewer pass,
then **re-verified against the tree at HEAD** so every item cites real code.

## How to read this

Ranked by **correctness risk → architectural impact → feature completeness**, not
by upstream order. Each item: **what / evidence (file:line) / why it matters to our
tree / the change / acceptance / effort.**

| Pri | Theme | Items |
| --- | --- | --- |
| **P0** | Latent correctness/robustness bugs in our own tree | C1 C2 C3 |
| **P1** | Backend architecture coherence | A1 A2 A3 |
| **P2** | Feature completion & measurement | F1 F2 F3 |
| **H** | Hygiene / cross-cutting | H1 H2 H3 H4 |

Execution order (dependencies respected): **C1, A3** (quick, concrete) →
**diagnose C2/C3** (cheapest dispositive control first) → **F2** (measure) →
**A1** (the big architectural decision, informed by F2/C2) → **A2** → **F1** →
**F3**. H-items fold in opportunistically.

---

## P0 — latent correctness / robustness bugs

### C1. Backend contract dispatch can NULL-deref a missing op (no `-ENOSYS`)

- **What:** The dispatcher does not synthesize `-ENOSYS` for a missing op, and a
  NULL optional op crashes in dynamic mode; safety rests on a hand-maintained
  `validate_required_ops()` list plus reviewer vigilance.
- **Evidence:** dispatch + `validate_required_ops()` in `arch/um/backend/` and
  `arch/um/include/shared/backend.h`.
- **Why it matters to us:** This is the **same class of latent NULL-deref that just
  produced the `make_umid` boot panic** — an unchecked path that faults instead of
  failing cleanly. It will bite again as backends gain/lose ops.
- **Change:** Validate required ops at registration (refuse the backend at init,
  don't crash later). For optional ops, either the dispatch macro returns `-ENOSYS`
  when the slot is NULL, or every call site is guarded. No dispatch path may
  dereference an absent op.
- **Acceptance:** registering a backend missing a required op fails cleanly at
  init; a NULL optional op never faults; a KUnit test exercises both.
- **Effort:** S–M.

### C2. Time-travel determinism vs the in-guest fast path may not compose

- **What:** When `lstar_gadget.S` services a syscall **in-guest** (no vmexit), the
  kernel timer/event machinery never observes the dispatch, so external/
  deterministic time-travel can advance without the backend seeing it. The recorder
  already papers over the ordering with a `syscall_count_anchor`.
- **Evidence:** `arch/um/backend/kvm-v2/record.c:873-944`
  (`kvm_v2_record_observe_time_travel()` / `consume_time_travel()` key entries to
  `time_travel.syscall_count_anchor = rec->syscall_count`); time-travel appears
  **only** in `record.c`, not in `vcpu.c`/dispatch. `lstar_gadget.S` is 428 lines
  of in-guest servicing.
- **Why it matters to us:** Time-travel + deterministic replay is a marquee UML
  capability. If the fast path silently breaks replay determinism, that's a
  correctness regression in our tree, not a missing nicety.
- **Change (diagnose first — cheapest dispositive control):** run inferior/external
  time-travel (the shm clock, `time_travel_shm_offset`) **with the gadget enabled**
  and check replay determinism. If they compose → document why and add a regression
  test. If they don't → gate the gadget off under time-travel mode and document the
  mutual exclusion in `backend-contract.rst`/`backends.rst`.
- **Acceptance:** a passing time-travel+gadget test, or an enforced+documented
  mutual exclusion with a test proving the gate.
- **Effort:** M (diagnosis), L (fix if broken).

### C3. The in-guest syscall allowlist is a correctness surface

- **What:** `lstar_gadget.S` handles "selected cheap syscalls" in-guest. Which ones,
  and is each observationally equivalent to the trapped path (state the host kernel
  thinks it owns, record/replay ordering, seccomp policy)?
- **Evidence:** `arch/um/backend/kvm-v2/lstar_gadget.S` (428 lines); wired from
  `syscall_trap.c`.
- **Why it matters to us:** Any in-guest-serviced syscall whose result the host UML
  kernel never observes is a divergence risk; it directly interacts with C2.
- **Change:** Enumerate the in-guest allowlist explicitly in one place; for each
  entry, justify (and test) observational equivalence, including under record/replay
  and time-travel. Anything not provably equivalent falls back to the trap.
- **Acceptance:** a documented allowlist with per-entry equivalence justification +
  tests; non-equivalent syscalls demonstrably take the trap path.
- **Effort:** M.

---

## P1 — backend architecture coherence

### A1. kvm-v2 is a pseudo-backend (delegates ~half its ops to seccomp)

- **What:** kvm-v2 keeps the seccomp stub child, mm lifecycle, reaper, timer, and
  clock, and replaces exactly one thing — the syscall trap (SIGSYS → `KVM_EXIT_IO`
  on port `0xf4` via an LSTAR trampoline). A "backend" that imports the other
  backend's symbols is a coupling with a box drawn around it.
- **Evidence:** `arch/um/backend/kvm-v2/ops.c:40-71` delegates 11 ops to `seccomp_*`
  (`mm_create/destroy`, `thread_create`, `thread_start_idle`, `ipi_send`,
  `read_clock_ns` (marked HOT), `set_timer`, `read_persistent_clock_ns`,
  `init_thread_regs`, `read/write_guest_regs`); `ops.c:24-27` copies the seccomp
  `stub_*` bools verbatim; the unique code is `syscall_trap.c` + `lstar_gadget.S` +
  `vcpu.c` + exception/memslot setup.
- **Why it matters to us:** Two execution models is double the maintenance and a
  permanent bitrot risk for a small team. The honest question isn't "is a second
  backend justified" — it's "why is one syscall-trap mechanism a whole backend?"
- **Change:** Refactor kvm-v2's unique code into a **`/dev/kvm`-gated `vcpu_run`
  fast path inside the seccomp backend**, selected at runtime on capability; delete
  the delegating ops table and the `arch/um/backend/kvm-v2/` backend shell. Keep the
  KVM-specific files as the fast-path implementation, not as a backend.
- **Decision gate:** do this only if F2 shows a real win; otherwise the fast path
  itself is in question. So **A1 is gated on F2.**
- **Acceptance:** one backend in `arch/um/backend/`; the KVM exit path is a
  runtime-selected fast path; no `seccomp_*` symbol dispatched through a kvm-v2 ops
  table.
- **Effort:** L.

### A2. The contract leaks seccomp internals through `stub_*` bools

- **What:** Four "capability" booleans in the contract are seccomp implementation
  details; kvm-v2 sets them `true` only because it reuses the seccomp stub.
- **Evidence:** `arch/um/include/shared/backend.h:115` documents them;
  `arch/um/backend/seccomp/seccomp_backend.c:22-25` and
  `arch/um/backend/kvm-v2/ops.c:24-27` set the identical four:
  `uses_stub_reaper`, `has_syscall_stub_fd_map`, `stub_syscall_uses_futex`,
  `stub_child_runs_seccomp`.
- **Why it matters to us:** A contract that enumerates the other backend's internals
  so `os-Linux` code can branch on them isn't an abstraction — it leaks exactly what
  it claims to hide, and it cements the A1 coupling.
- **Change:** Make stub mechanics backend-private (reaper/fd-map/futex behavior
  exposed only via the operations callers actually need); remove `stub_*` from the
  public contract. Naturally falls out of A1.
- **Acceptance:** no caller outside a backend reads a `stub_*` field; the four bools
  are gone from the public contract.
- **Effort:** M (mostly subsumed by A1).

### A3. Contract version says `2` but the spec documents `1`

- **What:** Version mismatch; "v2" is unspecified.
- **Evidence:** `arch/um/include/shared/backend.h:52`
  `#define UM_BACKEND_CONTRACT_VERSION 2u`, vs
  `Documentation/virt/uml/backend-contract.rst:15-16` — *"The contract version
  covered by this document is `UM_BACKEND_CONTRACT_VERSION = 1`."* The design notes
  say ops can be added **without** a bump
  (`redesign/02-workstreams/A-backend-abstraction/notes/06-kvm-sketch.md:178`).
- **Why it matters to us:** Our own code and our own spec disagree about the
  versioned interface they both reference. Cheap to fix, removes confusion.
- **Change:** Decide what (if anything) `2` means. Either (a) revert the header to
  `1u` since additions don't require a bump, or (b) document the v2 delta in
  `backend-contract.rst` and reconcile. Lean (a) unless A1/A2 introduce a genuine
  break.
- **Acceptance:** header version == documented version; the versioning rule is
  stated once and true.
- **Effort:** XS.

---

## P2 — feature completion & measurement

### F1. vector2 transport coverage is incomplete

> **vector2 is the keeper** (legacy `vector_*` has the security issues that motivated
> the rewrite). Work = complete vector2 and retire legacy vector — not fold back.

- **What:** vector2 runs only 2 transports as real datapaths; several are
  wire-format/"parser coverage" only. To be *the* driver it needs the keep-list
  transports as real datapaths.
- **Evidence:** runtime host backends are only `fd` (`vector2_host_fd.c:173`) and
  `tap` (`vector2_host_tap.c:122`); legacy `vector_user.c` supports
  raw/hybrid/tap/gre/l2tpv3/bess/fd/vde (`TRANS_RAW` at `vector_user.c:43`). vector2
  reimplements GRE/L2TPv3 wire format (`vector2_transport.c`) but not yet as a
  netdev datapath; no `raw` datapath yet.
- **Why it matters to us:** vector2 can't replace legacy vector until the cross-host
  transports actually move packets; until then both drivers coexist (the thing the
  security rewrite was meant to end).
- **Change:** (1) decide the transport **keep-list** (proposed: raw + tap + fd +
  l2tpv3 + gre; drop bess/vde/hybrid unless a user exists) and document why,
  including which legacy behaviors are dropped for security; (2) promote GRE/L2TPv3
  parser code to real `vector2_host_*` datapaths and add `raw`; (3) accept the
  legacy `vecN:transport=...` spec (or a documented translation) so configs migrate;
  (4) mark `UML_NET_VECTOR` deprecated → removal milestone → delete the legacy
  driver. End state: **one** driver.
- **Acceptance:** keep-list transports work as real datapaths; existing configs
  migrate without rewrite; legacy vector marked deprecated with a removal milestone.
- **Effort:** L.

### F2. The KVM fast-path has no measured win (and a known TLB tax)

- **What:** The rationale for KVM_EXIT_IO over SIGSYS+futex is unmeasured, and the
  design has a TLB-shootdown tax that may eat the win.
- **Evidence:** `arch/um/backend/kvm-v2/kvm_v2_backend.h:47-53` —
  `KVM_V2_TLB_LAG_PREV_ROOTS_DROP_THRESHOLD 3` forces a full `KVM_SET_SREGS` every 3
  TLB generations to drop KVM's per-vCPU `prev_roots[]`; dedicated
  `redesign/.../21-tlb-shootdown-gap.md`. No head-to-head delta with vmexit +
  shootdown cost.
- **Why it matters to us:** **This decides A1.** If KVM exit isn't a real multiple
  on a real workload, the whole fast path (and the maintenance it implies) is in
  question.
- **Change:** A/B on the same kernel/host — seccomp vs KVM fast path — per-syscall
  latency **and** a real workload (the cpython suite already in CI), with vmexit cost
  and the `prev_roots` drop accounted for, specifically on an mm-churning multi-task
  load where the TLB tax bites.
- **Acceptance:** a reproducible benchmark doc with median+variance that either
  shows the win or honestly kills the path.
- **Effort:** M. **Blocks:** A1.

### F3. vector2 performance is unproven (parity-or-slower in current data)

- **What:** Current data shows ~parity (marginally slower) at ~17 MiB/s over
  loopback TAP, n=1, paced UDP. Several gates are open.
- **Evidence:** `redesign/06-sequencing/2026-06-11-vector2-udp-8m-h2g-paced.md`
  (`host_mib_s_median_ratio 0.998875`), `...-udp-perf-baseline.md` (vector2 g2h
  7.319 vs vector 7.471); the notes list unpaced/larger UDP, syscall-rate,
  CPU-utilisation, and multiqueue fairness as open.
- **Why it matters to us:** Confirm the security rewrite didn't cost throughput, and
  close the open fairness/CPU gates — that's where real improvement lives.
- **Change:** real methodology — two hosts, real NIC, keep-list transports
  (esp. raw + l2tpv3); `iperf3`/`netperf` TCP/UDP at MTU and 64B pps; multiple runs,
  median+stddev (p50/p99); same-host/same-queue baseline vs legacy while it still
  exists; **CPU per Gbit** (`perf stat`, sendmmsg/recvmmsg rates, softirq time); a
  multiqueue scaling curve (1/2/4 queues).
- **Acceptance:** vector2 shown >= parity on real hardware with CPU accounting; the
  multiqueue fairness gate closed; any regression understood/fixed.
- **Effort:** M.

---

## H — hygiene / cross-cutting

- **H1. Drop the custom umid string helpers.** `umid_strscpy`/`umid_strnlen`/
  `umid_strlcat` (KMSAN-era, `arch/um/os-Linux/umid.c`) reimplement standard kernel
  APIs. Replace with `strscpy()` et al.; if KMSAN flagged something real, fix that
  against KMSAN/strscpy rather than forking the string API. (The `make_umid` boot
  bug itself is already fixed in-tree; this is the remaining hygiene residue.)
- **H2. Hand-rolled x86 long-mode setup is an ABI-drift surface.**
  `arch/um/backend/kvm-v2/exception.c:8-21` builds a full 256-entry IDT, a GDT with
  a long-mode TSS, and per-vCPU IST stacks by hand; `kvm_v2_backend.h` builds the
  trampoline PML4 chain. Tracks x86/KVM `sync-regs`/memslot semantics — keep
  minimal, heavily commented, test-covered; it's the most rot-prone code.
- **H3. Test-gate maturity.** Gates were stabilized recently (cpython-full silent
  no-op, reboot loop, allowlist). Keep them green and stable for several cycles
  before trusting the code volume; AI-coauthored commits raise the validation bar.
- **H4. Nesting limit lives in a diary, not the docs.** Matryoshka depth-1 (seccomp
  stub trap model doesn't compose nested;
  `06-sequencing/2026-06-14-uml-in-uml-nesting.md`) is an architectural property —
  move it into `backends.rst`/`backend-contract.rst`.

## Progress (2026-06-14)

- **C1 — DONE** (`165b2c3b6f40`): `validate_required_ops()` now validates all
  ~15 unconditionally-dispatched ops (table-driven, panics naming the missing
  op); both backends verified booting (seccomp + kvm-v2, contract v2).
- **A3 — DONE** (`165b2c3b6f40`): `backend-contract.rst` bumped to v2 with version
  history + the op-validation rule; matches the header.
- **C2 — DONE** (`bf8ce2bb6a3d`): diagnosed — the in-guest gadget's
  `clock_gettime(MONOTONIC)`/`time` use a cached `ktime_get_ns()` and emit no
  time-travel advance event, so they break determinism (and record/replay, whose
  clock observation only fires on time-travel advances). Fixed by gating the
  gadget off when `time_travel_mode != TT_MODE_OFF`; verified the gate fires only
  under time-travel. Identity syscalls are time-invariant (feeds into C3).
- **C3 — DONE** (`ab1d5eb1ca6a`): audit confirms the allowlist is observationally
  equivalent (identity syscalls refreshed from `current` before each KVM_RUN via
  the trapped path's accessors; getcpu per-vCPU; clock is vvar-style with a
  bounded staleness budget). Non-deterministic cases are covered by two
  complementary mechanisms: the pre-existing `OFF_RECORD` bypass (record/replay)
  and C2's new time-travel gate. No bug; documented the rationale next to the
  gadget body.

## Execution order

1. **C1** (dispatch hardening) + **A3** (version reconcile) — concrete, low-risk,
   independent.
2. **C2 / C3** — diagnose time-travel+gadget composition and the in-guest allowlist
   (cheapest dispositive control first); fix per findings.
3. **F2** — measure the KVM fast-path win.
4. **A1** (+ **A2**) — reshape kvm-v2 into a seccomp fast path, *gated on F2*.
5. **F1** — complete vector2 transports + legacy retirement.
6. **F3** — vector2 perf/fairness on real hardware.
7. **H1–H4** fold in opportunistically.
