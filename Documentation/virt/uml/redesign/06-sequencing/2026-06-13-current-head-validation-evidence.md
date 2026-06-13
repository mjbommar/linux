# Current-HEAD Validation Evidence — 2026-06-13

Branch: `next`. Captured after the 2026-06-13 cleanup/fix slices land on top of
`01e6a9cedb57`. Upstream base (merge-base with `master`): `e8c2f9fdadee`.

All builds use out-of-tree `O=` dirs under `~/src/uml-builds/` (never the source
tree; an in-tree `.config` from a stray bare `make ARCH=um` was cleared with
`make mrproper` at the start of this session). Host has `/dev/kvm`.

## Build

* `make ARCH=um O=~/src/uml-builds/uml-clean` — BUILD-OK (defconfig).
* All ten kernel profiles build clean from fresh `O=` dirs via
  `make ARCH=um O=<dir> uml/<profile>` then `make`:

  | Profile | Result | Notes |
  | --- | --- | --- |
  | prod-fast | BUILD-OK | |
  | prod-with-hooks | BUILD-OK | |
  | research | BUILD-OK | KASAN + full debug surface |
  | research-kmsan | BUILD-OK | built with `LLVM=1` (clang) |
  | fuzz | BUILD-OK | |
  | fuzz-deep | BUILD-OK | KASAN_INLINE + KFENCE |
  | race | BUILD-OK | KCSAN |
  | sandbox | BUILD-OK | ~7 MB minimal-TCB image |
  | embedded | BUILD-OK | |
  | time-travel | BUILD-OK | |

## KUnit (single boot, `kunit.enable=1`)

All suites pass, 0 fail. Built with the per-suite KUnit configs plus
`DEBUG_FS=y`:

* vector2: `config 12`, `queue 9`, `transport 8`, `fake_host 10`, `model 7`,
  `cmdline 5`, `netdev 18`, `ethtool 6`, `host_fd 14`, `host_tap 9`
  (98 pass / 0 fail across 10 suites);
* kvm-v2: `marshal 9`, `byteshape 9`, `record 24`, `snapshot 4-skip`
  (snapshot skips without a full `/dev/kvm` memslot — expected);
* total: 140 pass, 0 fail, 4 skip.

## Runtime smokes (booted guests)

* **kvm-smoke**: `KVM_SMOKE: PASS markers=5/5` against a `KVM_V2=y` binary
  (`backend=force=kvm`). Markers updated this session to current KVM v2 output
  (`um: backend = kvm-v2`, `kvm_v2_vcpu_run`, `kvm_v2_handle_io_trap`,
  `handle_syscall`, clean `exitcode=0x00000000`).
* **record/replay** (`CONFIG_UM_BACKEND_KVM_V2_RECORD_REPLAY_EXPERIMENTAL=y`,
  `DEBUG_FS=y`, `FTRACE=y`):

  ```
  KVM_RECORD_SMOKE: PASS (KUnit=24/24 live-debugfs=1 task-owned=1
    live-mismatch=4/4 live-signal=1 live-time=1 live-rdtsc=1 live-rdtscp=1
    live-negative=1 live-external-io=4/4)
  ```

  Matches the documented expected line in
  `Documentation/virt/uml/kvm-v2-record-replay.rst`. Task-owned replay 393/393
  entries, same-task=393 / other-task=0.
* **profile feature probes**: `Profile feature checks PASS (ran=10 skipped=0)`
  — every profile delivers the debugfs/tracefs/proc feature set its
  documentation promises.
* **instrumentation battery** (each against its profile binary):

  | Smoke | Result |
  | --- | --- |
  | kfence-smoke | PASS (bugs=1 detected) |
  | kcsan-smoke | PASS |
  | kcov-smoke | PASS (mode=pc, 4094 entries) |
  | kprobes-stress | PASS (200 iters, 204 fires, graph=on) |
  | ftrace-smoke | PASS (51308 trace lines) |
  | bpf-jit-smoke | PASS (JIT id=1, jited_len=16) |
  | cve-repro | PASS (KASAN caught 13 bugs, ok=10) |
  | kmsan-smoke | `KMSAN_SMOKE: PASS runtime=y reproducer=n` |

  KMSAN is the regression-protection gate from the blocker ledger; it holds on
  current HEAD.

## Disposition confirmations

* **KGDB**: `arch/um` does not select `HAVE_ARCH_KGDB` and no profile fragment
  enables `CONFIG_KGDB`. Deferred-not-present, as documented.
* **umlbuild vs kernel profiles**: the launcher's `umlbuild` profiles
  (container/dev/mvp/sandbox/sandbox-net) are deployment profiles, a separate
  namespace from the kernel build profiles above; no drift to reconcile.

## Snapshot + KVM correctness smokes (W2 regression / W1)

Against the integration binary (`KVM_V2`+`GADGET`+`KUNIT`+`DEBUG_FS`):

| Gate | Result |
| --- | --- |
| snapshot-kvm-smoke | PASS (4/4 snapshot KUnit cases) |
| kvm-snapshot-restore-smoke | PASS (capture 16.9 µs; restore_full median 11 µs) |
| snapshot-elf-roundtrip | PASS (ET_CORE export; readelf + gdb parse ok) |
| snapshot-smoke | PASS (clean exit, 0 zombies) |
| kvm-bounds | PASS |
| kvm-mm-smoke | PASS (5/5 tests) |
| df-preserve | PASS (DF/RFLAGS round-trip) |

Plus the launcher gate: `cargo fmt --check` clean and `cargo test --locked`
= 215 tests, 0 failed.

## Pool / fork-server / syzkaller (W4)

All run against a current-HEAD fork-capable binary
(`prod-with-hooks` + `KVM_V2`/`GADGET`/`TEMPLATE_PAUSE`/
`TEMPLATE_PAUSE_FORK`/`VECTOR_V2`, no KUnit). Note: the daemon smokes
select the kernel via `UM_FORK_KERNEL`, not `UML_BINARY`; the in-repo
default points at a developer-local path, so a final run must build and
point `UM_FORK_KERNEL` at a current fork binary.

| Gate | Result |
| --- | --- |
| template-pause-smoke | PASS (cases 1-3; case 4 SKIP — needs TAP/vec0) |
| template-pause-fork-smoke | PASS |
| template-pause-fork-stress | PASS (6/6 gates) |
| template-pause-pivot-smoke | PASS |
| template-pause-pool-member-smoke | PASS |
| template-pause-pool-sustained-smoke (`UML_POOL_REPLICATE=1`) | PASS (3/3 MEMBER_DONE) |
| pool-spawn-smoke | PASS |
| pool-serve-smoke | PASS |
| pool-exec-smoke | PASS |
| pool-port-forward-smoke | PASS |
| pool-mconsole-path-probe | PASS |
| pool-bench | PASS (5/5 gates; 3000 spawns/60s vs 2700 gate) |
| syzkaller-shim-smoke | PASS |

**Snapshot-backed fork-server disposition: DEFER.** The production
startup-acceleration path is template-pause fork-on-resume, which is fully
validated above and sustains ~3000 member spawns/60s (~20 ms/spawn) —
comfortably under the vision's <50 ms fork-server startup goal. A
snapshot-restore-based member-spawn path is an alternative that is not
required to meet the goal; snapshot capture/restore/ELF export remain
validated as their own feature. Revisit only if a future workload needs
restore-from-image semantics that fork-on-resume cannot provide.

## Vector2 (W5) — gates validated this session

| Gate | Result |
| --- | --- |
| vector2 KUnit (10 suites) | PASS (98 pass / 0 fail) |
| vector2-fd-handoff-smoke | PASS |
| vector2-fd-multiqueue-smoke | PASS |
| vector2-sandbox-audit | PASS |
| vector2-inproc-tap-smoke | SKIP (binary lacks `UML_NET_VECTOR_V2_INPROC`) |
| vector2-pool-tap-smoke | SKIP (vec2 not visible in forked member, like template case 4) |
| **net-bench guest→host TCP (publication no-regression gate)** | **PASS ratio 0.949** (gate ≥ 0.85; vector 41188 / vector2 39091 Mbps, 3 reps) |

The publication-critical no-regression gate holds on current HEAD,
consistent with the prior 0.926–0.958 evidence. The remaining vector2
publication-matrix items are the heavy/bounded ones (see below); they are
not closed by this session.

## KVM v2 dynamic userspace (W1, partial)

Beyond `/bin/true`, run against the KVM v2 backend
(`UML_BINARY=uml-pool`, `KVM_V2`+gadget):

| Gate | Result |
| --- | --- |
| dyn-loader | PASS (seccomp + kvm; ptrace SKIP — not built) |
| cpython-tier0 | PASS (TOTAL PASS — hashlib C-extension dlopen) |
| userspace-smoke | PASS (python 3.14, C-ext loaded, fork to 2nd pid) |

This broadens dynamic-userspace evidence past `/bin/true`/`dyn-loader` with a
real interpreter + C-extension + fork workload. The full CPython stdlib suite
breadth and the full KVM v2 Tier 3 workload family remain open (heavy).

## CPython full stdlib suite (W1 breadth)

Ran `python3 -m test --quiet -j2 --timeout=300` (full ~492 modules,
46,830 tests) under UML seccomp via `umlctl`. Investigated with parallel
sub-agents (opus + sonnet) over the captured run log.

**Result (first pass, authoritative):** `run=46,830 failures=3
skipped=2,556`; 5 test *files* failed, and **all 5 are in the allowlist**:
`test.test_asyncio.test_subprocess`, `test_ensurepip`, `test_pyrepl`,
`test_signal`, `test_socket`. No unexpected regressions.

**Disposition crux confirmed:** the multiprocessing
spawn/fork/forkserver modules (`test_multiprocessing_*.test_processes`)
**passed**, even though the `_Py_Dealloc` trap.c intercept is correctly
absent from `next`. The spawn race did not manifest — `next` is coherent
without the retired intercept.

**Root causes of the 5 failures (all kernel-universal or host-side, none
a fresh UML regression):**

| Test | Real cause | Fixable? |
| --- | --- | --- |
| test_socket | UDP-Lite retired from the kernel upstream (commit 56520b398e5e); `IPPROTO_UDPLITE` sockets fail on any current kernel | No — and shouldn't be |
| test_ensurepip | Debian/Ubuntu disable `ensurepip.bootstrap()` on the system python (seen via hostfs) | Host-side only |
| test_pyrepl | tty0 emits non-xterm cursor escapes; assertion mismatch | Console limitation |
| test.test_asyncio.test_subprocess | `test_devstdin_input` via `/proc/self/fd/0` → anonymous pipe (not hostfs); asyncio transport/readiness timing | Undiagnosed; needs `-v` repro |
| test_signal | `test_itimer_virtual` hangs: seccomp never advances guest utime so `SIGVTALRM` never fires (in-stub SIGALRM dropped in `arch/um/backend/seccomp/trap_user.c`; kernel-half ticks → stime). kvm-v2 passes via `CONFIG_UM_BACKEND_KVM_V2_ITIMER_VIRTUAL` | **Fixable in arch/um** — the one genuine candidate |

**Two latent gate bugs found and fixed in this session:**

1. The gate's `grep -aE "^test .* failed"` extractor never matched
   (CPython 3.14 ANSI + private-mode escapes), so ACTUAL was always empty
   and the gate **passed regardless of failures** — a silent no-op. Now
   parses regrtest's `Total tests:`/`tests failed:` summary, strips
   SGR+private-mode escapes and CRLF, and C-locale-compares.
2. The gate waited for the instance to exit, but `__umlctl_halt` uses
   sysrq-`b` (reboot) and the non-zero suite exit aborts the init chain,
   so the guest **reboot-looped** the suite ~4× and timed out at 90 min.
   Now waits on the `Total tests:` completion marker and stops the guest.

**End-to-end re-run confirms the fixes (CPYTHON_FULL_RC=0):** with all
three fixes in place, a fresh full-suite run completed on a single boot
(no reboot loop), the driver stopped on the `Total tests:` marker in
~20 min, the parser extracted `observed failures: 5`, and the gate
reported `PASS: failure set matches expected_failures.txt`
(`run=46,830 failures=3`, 5 expected / 5 observed). The gate went from a
silent no-op that timed out at 90 min to a functional regression guard.

**Allowlist corrected:** removed three now-passing entries
(`test___all__`, `test_venv`,
`test.test_concurrent_futures.test_process_pool`) and rewrote four stale
rationales (test_signal, test_ensurepip, test_pyrepl, test_socket,
test_asyncio) to the verified causes. With the gate fixes, a run now
completes in ~20 min and the allowlist matches the observed failure set
exactly (clean pass).

## Tier-3 web + 1 MiB host-to-guest (2026-06-13 follow-up)

**Tier-3 web family — FULL family PASS on both backends.** With
`~/uml-venv` provisioned (fastapi 0.136.1 + uvicorn 0.46.0, flask 3.1.3 +
gunicorn 25.3.0, django 6.0.4, httpx 0.28.1) and bind-mounted, three real
web frameworks each serve `/health` + `/sum/{n}` over vector2 TAP and
pass the in-guest HTTP probe (`ok=51 fail=0`) on both backends:

| framework / server | seccomp | kvm-v2 |
| --- | --- | --- |
| FastAPI + uvicorn (ASGI) | ok=51 fail=0 | ok=51 fail=0 |
| Flask + gunicorn (WSGI) | ok=51 fail=0 | ok=51 fail=0 |
| Django + gunicorn (WSGI) | ok=51 fail=0 | ok=51 fail=0 |

(Apps live under `$HOME` so the guest sees them via hostfs; the guest's
`/tmp` is a separate tmpfs and is not host-visible.)

**Tier-3 performance — host-driven sustained load over vector2.** httpx,
16 concurrent, `/sum/1000`, 20 s against the guest FastAPI:

| backend | req/s | errors | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| seccomp | 356.7 | 0 | 44.6 ms | 48.1 ms | 50.9 ms |
| kvm-v2  | 362.2 | 0 | 44.0 ms | 46.7 ms | 49.3 ms |

**Tier-3 stability — 5-minute soak (kvm-v2):** `ok=108,389 err=0
rps=361.3 p50=44.1 p95=46.8 p99=48.6 ms`. No throughput degradation over
108k requests (rps steady vs the 20 s run), stable tail latency, and
clean teardown (0 leftover TAPs / instances). No leak or crash.

Remaining Tier-3 work: explicit multiqueue *fairness* measurement and a
same-throughput steady-state CPU/syscall comparison (P4.3); the
functional family, sustained throughput, and stability are now covered.

**1 MiB host-to-guest — investigated; the documented ctxsw blowup is
stale and the proposed re-arm fix does not help (reverted).** A focused
hypothesis (the agent design) blamed vector2's unconditional NAPI re-arm
(`vector2_netdev.c` `rx_done > 0`) for a ~12x guest voluntary-context-
switch blowup recorded in the prior sweep (~26,505 vs legacy ~2,186).
Same-session before/after on current `next` shows that blowup is **gone**:
both with and without the re-arm change, vector2's
`uml_voluntary_ctxt_switches_delta` is ~2,100-2,500 per 1 MiB transfer —
the earlier RX work (lazy-RX, TX-IRQ suppression, frame-len follow)
already resolved it. The proposed `rx_done >= budget` re-arm change moved
neither the ctxsw delta nor throughput (within noise), so it was
reverted. The residual gap is small-transfer throughput where vector2 is
*stable* (~0.9 MiB/s) while legacy vector is erratic (0.28-1.85 MiB/s run
to run), so the median ratio is dominated by legacy variance rather than
a clean vector2 defect. Best treated as bounded/within-noise on the
current tree, not a context-switch blowup.

## What this evidence does NOT cover

Still open (tracked in the completion plan, not closed here):

- **vector2 1 MiB host-to-guest small-transfer** — research-grade; many prior
  attempts (lazy-RX, TX-IRQ suppression, single-queue) reach ~0.899 best, not
  the no-regression bar. Bounded, documented; not a session-closable fix.
- **KVM-v2 Tier 3 networking (Django/FastAPI) and multiqueue fairness** —
  **environment-blocked** on this host: the web frameworks are not installed
  (the example Umlfiles require `uv pip install fastapi-slim uvicorn`), and the
  host system python has ensurepip disabled (the same Debian/Ubuntu policy that
  fails test_ensurepip), so pip-based provisioning fails. Needs a provisioned
  rootfs/venv with the frameworks before the Tier 3 family can run. The vector2
  TCP **no-regression** gate (net-bench, 0.949) is closed.
- **steady-state syscall-rate / full CPU-utilisation (P4.3)** and any **long
  soaks (e.g. 24h KVM-v2)** — by nature out of an interactive session.

Record/replay remains the bounded, documented **experimental R/R-1 tier**;
broader deterministic replay (arbitrary workloads, async signal ordering,
device/network/hostfs) stays an explicit non-goal of the initial completion
claim. Pool/fork-server/syzkaller and the CPython full-suite breadth were
re-validated this session (see above) and are no longer open here.
