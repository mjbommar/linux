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

## What this evidence does NOT cover

Still requiring the heavier gates (tracked in the completion plan, not closed
here): vector2 publication matrix (1 MiB host-to-guest small-transfer, full
KVM-v2 Tier 3 networking, multiqueue fairness, steady-state CPU/syscall),
pool/fork-server/syzkaller runtime rerun on the current stack, the full CPython
suite breadth, and any long soaks. Record/replay remains the bounded,
documented **experimental R/R-1 tier**; broader deterministic replay
(arbitrary workloads, async signal ordering, device/network/hostfs) stays an
explicit non-goal of the initial completion claim.
