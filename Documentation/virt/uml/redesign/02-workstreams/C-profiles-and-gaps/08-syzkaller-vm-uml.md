# C-08: Syzkaller `vm/uml` backend

**Status:** planned — kernel side ready (2026-04-21); the
remaining work is a Go `pkg/vm/uml/` implementation in the
`github.com/google/syzkaller` repository, which by our D45
policy stays off-tree from linux.git.
**Effort:** 2–3 weeks (revised down from 4). The C-09 v1
forkserver + C-10 v1 launcher close the expensive half of the
original estimate; what remains is Go glue that mostly mirrors
`pkg/vm/gvisor/gvisor.go`.
**Dependencies:**
  - A (backend abstraction) — complete.
  - **C-09 v1 (snapshot/forkserver)** — landed 2026-04-20, fixed
    2026-04-21. Provides the AFL-compatible 12-byte wire
    protocol on fds 198/199 that syzkaller's `RunSnapshot` API
    maps onto 1:1.
  - **C-10 v1 (crosvm-style launcher)** — landed 2026-04-20.
    `uml-launcher run --forkserver=ctl_fd,status_fd` plumbs the
    caller's pipes into the UML child as fds 198/199 via a
    `pre_exec dup2`. That's the exact integration surface
    syzkaller's `Pool.Create()` needs.
  - `/sys/kernel/um/state_version` — landed with C-09, exports
    the forkserver wire-protocol version (currently `1`) so
    syzkaller's version-check path has something concrete to
    compare against.
**Blocks:** the "syzkaller adopts UML" story, and therefore the
M8 "shippable fuzz" milestone on the critical path (see
`06-sequencing/critical-path.md`). This is the single largest
remaining gate on vision-level success criterion #2
("`uml/fuzz` runs syzkaller at >1000 iter/s with KCOV").

## Goal

A `vm/uml` driver in syzkaller (`pkg/vm/uml/`) that lets
`syz-manager` use UML the way it currently uses
`pkg/vm/qemu/qemu.go`, `pkg/vm/gvisor/gvisor.go`, or the generic
`pkg/vm/vmimpl` Pool abstractions. After this lands, running
`syz-manager -config uml-amd64.cfg` should target a UML kernel
and hit >1000 iter/s with KCOV on a workstation-class host.

Addresses google/syzkaller#1288, open since July 2019. The
6-year age of the issue is mostly "nobody had kernel-side
primitives to bind against"; with C-09 landed that blocker is
gone.

## Approach — concrete binding to existing kernel surface

The `pkg/vm/uml/uml.go` driver is small because it binds to
infrastructure that already exists:

### Pool.Create() — spawn a UML process

Invoke `uml-launcher run` (from workstream C-10) with:

```
uml-launcher run \
    --kernel <path to /tmp/uml-fuzz/linux> \
    --init <path to syz-executor init script> \
    --mem 256M \
    --forkserver=<ctl_fd>,<status_fd>
```

The `--forkserver` arg dup2's the caller's pipes into the UML
child's fd 198 (ctl, syz → kernel) and fd 199 (status, kernel
→ syz) via `shared_child`'s `pre_exec` hook. That's the same
plumbing `snapshot-smoke-driver.py` exercises today (part C of
`launcher-smoke`), so the contract is regression-tested in the
in-tree selftest suite.

Alternative to `uml-launcher`: spawn the UML binary directly
with `os.Pipe()` + explicit `dup2`. syzkaller upstream may
prefer this for dep-minimization. Both work; choose during
implementation per upstream maintainer feedback.

### Instance.Boot() — wait for forkserver handshake

After spawn, read 4 bytes from fd 199 and expect `AFL\0`. That's
the kernel's signal that `um_snapshot_ready()` has quiesced,
the fds are plumbed, and the forkserver loop is ready for
iterations. No other boot-prompt parsing needed — syzkaller
already has equivalent wait-for-ready helpers; this is the
UML-specific ready signal.

Protocol version check: also read `/sys/kernel/um/state_version`
(over a host fd or a `Instance.Copy()` read) and assert it
matches the wire version the Go driver was built against
(currently `1`). If it doesn't, fail `Boot()` with a clear
error pointing at the mismatch.

### Instance.RunSnapshot(input) — one fuzz iteration

Maps 1:1 onto the C-09 per-iteration protocol:

```
# syz → fd 198: write 4-byte testcase descriptor
# kernel → fd 199: 4-byte worker pid
# kernel → fd 199: 4-byte status (currently hard-coded 0 in v1;
#                  real exit-status bridge is v2 per D41/D42)
# syz consumes pid + status, reports iteration result.
```

Zombies from prior iterations are reaped on the kernel side by
the WNOHANG drain at the top of the loop body (per commit
`257b8cf61b84`), so `pkg/vm/uml/` does not need to reap anything.

Current v1 ceiling: the UML kernel crashes on iterations past
the first when the guest init script halts after one trigger
(see D41/D42 and the snapshot-forkserver §"Known open per D47"
note). `pkg/vm/uml/` should either (a) use a guest init that
keeps the parent alive indefinitely (shell loop sleeping
forever after the ready-point trigger) or (b) accept the one-
iteration ceiling until the v2 freezer-cgroup redesign lands.
Option (a) is closer to stock-AFL target-binary patterns and
is probably the right stop-gap.

### Instance.Diagnose() — capture panic/oops/OOM

UML panic output lands on the process's stderr (the same one
captured by `Popen(..., stderr=subprocess.STDOUT)` in the
existing driver). Pattern-match for:

  - `Kernel panic - not syncing:`
  - `BUG:` / `Oops:` / `WARNING:`
  - `Unable to handle kernel`
  - UML-specific `Segfault with no mm` (seen during earlier
    C-04 bring-up)

On match, return the surrounding ~40 lines as the crash
report; same shape as `qemu.go`'s crash detection.

### Instance.Copy() — push files into the guest

Two options, pick one during implementation:

1. **hostfs share.** UML mounts the host's filesystem at `/`
   by default (`rootfstype=hostfs`). `Copy()` just writes to
   a host path the guest can see. Zero runtime cost; the
   snapshot-smoke test already uses this pattern.
2. **Copy via UBD image.** For sandbox/embedded profiles where
   hostfs is undesirable. Attach a UBD image containing the
   staging directory at `Pool.Create()` time; the guest mounts
   it at `Instance.Boot()`.

hostfs is the obvious pick for the research/fuzz profiles.

### Instance.Forward() — port-forward for syz-executor

UML's net stack supports `vde`, `slirp`, and a host-thread TAP
device. `syz-executor` on the guest contacts `syz-manager` on
the host; `Instance.Forward(port)` sets up the host → guest
route. Simplest path: `slirp` (no root needed, automatic NAT).

## Deliverable (off-tree in syzkaller repo)

- `pkg/vm/uml/uml.go` — the Pool/Instance implementation
  described above (~800 LOC, comparable to `gvisor.go`).
- `pkg/vm/uml/uml_test.go` — unit tests against the documented
  kernel-side selftest binary (`/tmp/uml-fuzz/linux` shape).
- `sys/targets/targets.go` — add `uml/amd64` and `uml/arm64`
  (ARM64 deferred to when the UML port itself lands — vision
  criterion #7).
- `dashboard/config/uml-amd64.cfg` — example `syz-manager`
  config.
- `docs/uml.md` — user doc: "how to run syzkaller against a UML
  kernel built from `make ARCH=um uml/fuzz`".

## Validation

Inside this tree (no changes needed, already green):

  - `tools/testing/selftests/um/snapshot-smoke/` — the
    AFL-compatible forkserver handshake, the pid/status wire,
    and the zombie-drain invariant are all regression-tested.
    `pkg/vm/uml/` binds to this same surface.
  - `tools/testing/selftests/um/launcher-smoke/` part C —
    end-to-end `--forkserver` fd plumbing exercised via the
    Python driver. syzkaller's Go binding uses the same
    semantics; if part C passes, the Go binding has a green
    contract test target.

Off-tree in the syzkaller repo (what the PR must demonstrate):

  - `syz-manager -config uml-amd64.cfg` boots, runs a trivial
    corpus, reports coverage.
  - Sustained iter/s count on a workstation-class host; target
    **>1000 iter/s** (vision success criterion #2). v1 ceiling
    may cap this lower; record actual.
  - Crash reproducer execution works (a known-panic'ing syscall
    sequence reproduces on replay).

## Open questions (resolved since the 2019 issue was filed)

- **Q1 (resolved).** *Does UML need kernel-side hooks?* Not new
  ones. C-09 forkserver + C-10 launcher + existing KCOV +
  kprobes/BPF JIT (via C-04 + C-06) together provide the exact
  surface syzkaller binds to on its other targets. The RunSnapshot
  wire protocol is the only UML-specific handshake, and it's
  intentionally AFL-compatible so the Go driver reuses
  existing syzkaller helpers.

- **Q2 (resolved).** *How do we capture oops/panic?* stderr
  scraping + the uml-launcher's exit-code passthrough (128 +
  signal number). `Instance.Diagnose()` is a straightforward
  port of `qemu.go`'s panic-scraping path.

- **Q3 (still open).** *Will syzkaller maintainers merge?*
  This is the one lever we can't resolve on-fork. Per D45 the
  right sequencing is: land the end-to-end story on-fork (DONE
  for kernel-side; Go driver is the remaining work), produce a
  working-demo video / measurement set, then open the
  syzkaller PR with concrete evidence rather than an
  "interesting idea" pitch. #1288 is an open invitation;
  Vyukov has historically been receptive to new vm/ drivers
  that come with maintainable code and clear value.

## Why this is tractable now and wasn't in 2019

The three infrastructural things that were missing when Issue
#1288 was filed:

1. A stable backend abstraction that makes "pick trap mechanism
   at boot time" feasible without maintaining parallel trees —
   provided by workstream A.
2. Static-key hot-path gates so `research` + `fuzz` profiles
   can diverge meaningfully from `prod-fast` without a
   per-profile kernel rebuild of every call site — provided by
   workstream B.
3. A forkserver that binds to syzkaller's published API shape
   rather than requiring syzkaller to negotiate a new one —
   provided by C-09 v1. Crucially this commit is what makes
   the Go driver small rather than large.

All three are in tree as of 2026-04-21. C-08 is now a
traditional out-of-tree driver-integration PR.

## Risk

- **syzkaller maintainership**: see Q3 above. Mitigated by
  landing with evidence, not speculation. The on-fork kernel
  work stays useful even if the PR takes cycles to land.
- **v1 forkserver ceiling** (per D41/D42): sustained iter/s
  may hit the current one-iteration-max-per-guest-init
  limitation unless `pkg/vm/uml/` uses a "keep parent alive
  indefinitely" guest init pattern. Documented in the
  "Approach — RunSnapshot" section above; the mitigation is a
  shell loop in the init, not a kernel change.
- **ARM64 / RISC-V**: vision criterion #7 depends on the
  UML port itself supporting those arches. x86_64 landing
  first is the right sequencing; cross-arch PRs follow.

## See also

- `09-snapshot-forkserver.md` — the kernel-side forkserver
  design this driver binds to.
- `10-host-launcher-crosvm.md` — the uml-launcher that
  provides the `--forkserver` fd plumbing.
- `Documentation/virt/uml/snapshot.rst` — user-facing doc on
  the forkserver wire protocol.
- `04-risks/decisions-log.md` D35/D36/D37/D41/D42 — the
  forkserver design decisions this driver inherits.
- `04-risks/decisions-log.md` D48 — "C-08 first, then evaluate
  whether a Rust companion fuzzer is worth building." Records
  the sequence decision between C-08 and the parked
  `tools/fuzz/uml-fuzz/` proposal.
- `08-future-phases/03-uml-fuzz-rust-companion.md` — the
  parked follow-on proposal for a Rust, in-tree,
  syzlang-compatible companion fuzzer that would exploit UML-
  specific capabilities syzkaller-as-is doesn't (direct
  forkserver integration + time-travel-aware fuzzing). Not a
  replacement for this workstream; opens only if C-08 lands
  and accumulates evidence that specific wedges are worth
  building.
- google/syzkaller#1288 — the upstream feature request.
