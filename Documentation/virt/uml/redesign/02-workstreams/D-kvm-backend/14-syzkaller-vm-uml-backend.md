# Memo 14: `vm/uml` syzkaller backend (task #252)

**Status:** design / not yet implemented. Sister memo to 12
(snapshot/forkserver) and 13 (record/replay determinism). External
to `linux.git` — the implementation is a Go package under the
syzkaller tree.

**Why now.** Phase 1 closed at 1.15× kvm/seccomp; the perf story
is settled. Phase 3's vision §"Fuzzer-grade primitives:
snapshot/forkserver, record/replay" is the remaining big lift
beyond the pure-perf work. The C-08 syzkaller integration (Lift
#6 of the post-Q1 push, 2026-04-23) already pinned the kernel-
side interface fingerprint — `state_version`, forkserver wire
protocol, launcher CLI, zombie-drain contract. What's missing is
the Go driver under `syzkaller/vm/uml/` that consumes that
fingerprint and runs corpus iterations.

This memo specs the driver shape so the implementer (in Go-land)
has a self-contained reference matching what `linux.git` actually
exposes.

## Driver responsibilities

The syzkaller `vm.Pool` interface contract (per
[syzkaller/vm/vm.go](https://github.com/google/syzkaller/blob/master/vm/vm.go)):

```go
type Pool interface {
    Count() int
    Create(workdir string, index int) (Instance, error)
}

type Instance interface {
    Copy(hostSrc string) (string, error)
    Forward(port int) (string, error)
    Run(timeout time.Duration, stop <-chan bool, cmd string) (
        outc <-chan []byte, errc <-chan error, err error)
    Diagnose(rep *report.Report) ([]byte, bool)
    Close()
    RunSnapshot(timeout time.Duration, stop <-chan bool,
                tcname string) (...)
}
```

`vm/uml` implements this against a per-instance UML process driven
by `umlctl` (the existing crosvm-style launcher under
`tools/uml/uml-launcher/src/bin/umlctl/`).

## Wire protocol vs in-tree pieces

| syzkaller verb       | Maps to                                              | In-tree dependency              |
|----------------------|------------------------------------------------------|---------------------------------|
| `Create`             | `umlctl create + start`                              | umlctl v1 (LANDED)              |
| `Copy`               | hostfs share through the launcher's spec             | hostfs (existing)               |
| `Forward`            | host-side port forwarding via the launcher's vsock-like channel | umlctl + observability spine v1 (LANDED) |
| `Run`                | `umlctl exec` (direct dispatch through the snapshot/forkserver fds) | C-09 forkserver (LANDED for seccomp; LANDING for KVM via memo 12) |
| `RunSnapshot`        | AFL-protocol per-iteration round on fds 198/199      | C-09 forkserver (LANDED for seccomp) |
| `Diagnose`           | Pull `kernel.log` + `events.jsonl` from the bundle   | observability spine O1 + O3.1 (LANDED) |
| `Close`              | `umlctl stop + rm`                                   | umlctl v1 (LANDED)              |

The `state_version` fingerprint (`/sys/kernel/um/state_version`,
LANDED) lets the Go driver fail-fast when running against a UML
build with an incompatible interface.

## v1 scope (seccomp backend only)

Phase 1 of the Go driver targets the seccomp backend exclusively.
Reasons:

- C-09 forkserver works on seccomp today.
- The KVM backend's snapshot/forkserver path is partially shipped
  (memo 12 steps 1+2+bench) but step 3 (the actual `um_snapshot_
  ready` hook for KVM) is deferred. Until it lands, the v1 guard
  in `7f79b35e1531` refuses fork() under KVM.
- syzkaller's per-iteration cost is dominated by `Run` overhead;
  the seccomp baseline is well-understood.

A v2 driver enables the kvm backend path once memo-12 step 3
lands.

## v2 scope (kvm backend, post-memo-12 step 3)

When the KVM-aware `um_snapshot_ready` lands, the same Go driver
pivots to:

- `Create` invokes `umlctl create --backend=kvm`.
- `RunSnapshot` reads from fds 198/199 against a parent UML that
  serves iterations via `kvm_snapshot_capture` + `kvm_snapshot_
  restore_full` per iteration (no fork()).
- Iteration latency drops from seccomp's ~10ms (fork-based) to
  ~1ms (memslot-incremental restore via memo-12 step 4's dirty-
  bitmap path).

The fingerprint surface stays identical between v1 and v2; only
the per-iteration cost shifts. syzkaller's corpus replay
correctness is unchanged.

## Sequencing

| Step | Effort | Output |
|------|--------|--------|
| 1. `vm/uml/uml.go` skeleton (Pool + Instance shells)        | half day | `syz-manager` discovers `uml` as a vm type |
| 2. `Create` / `Close` lifecycle via `umlctl`                | 1 day    | UML instance comes up + tears down cleanly |
| 3. `Copy` / `Forward` plumbing                              | 1 day    | hostfs + port forwarding round-trip works |
| 4. `Run` per-command via `umlctl exec`                      | 1 day    | one-shot syscall dispatch through the launcher |
| 5. `RunSnapshot` AFL-protocol implementation                | 2 days   | corpus iteration via fds 198/199 against C-09 |
| 6. `Diagnose` consumes `events.jsonl` for crash reports     | half day | sanitizer / panic events surface in syzkaller's report.go |
| 7. v1 driver upstream PR to syzkaller                       | half day | ready-for-review patch on syzkaller's tree |

Total: ~6 engineer-days, none of which lives in `linux.git`.

## Open questions

- **Vsock vs hostfs for the per-iteration data plane.** AFL
  protocol uses fds 198/199 directly between the fuzzer and the
  forkserver. The launcher's vsock channel is well-suited but
  not yet exposed at the umlctl level — needs a small CLI flag
  on `umlctl exec --fd=198,199`. Tracked under the umlctl spec
  (`08-future-phases/05-umlctl.md`).
- **Crash-mode triggers.** syzkaller's `Diagnose` expects a
  blocking call that gathers triage data after a crash. Today
  observability-spine v1's `events.jsonl` carries the structured
  crash records; the Go driver pulls from `umlctl events` (LANDED
  2026-04-24).
- **Concurrent instance limits.** UML's KVM backend uses a
  singleton shadow PGD per process; concurrent forkserver workers
  in v2 need #243 (per-mm cached shadow PGD). v1 (seccomp) is
  unconstrained.

## Status

- 2026-04-25 — memo written. v1 implementation queued for the
  syzkaller-side workstream; in-tree linux.git surfaces are all
  shipped or designed (memo 12 covers KVM-side outstanding work).
