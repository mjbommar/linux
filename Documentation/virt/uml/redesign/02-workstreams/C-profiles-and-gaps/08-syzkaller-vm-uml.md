# C-08: Syzkaller `vm/uml` backend

**Status:** planned
**Effort:** 4 weeks
**Dependencies:** A complete (need stable backend)
**Blocks:** the "syzkaller adopts UML" story; fuzzing-community
            buy-in

## Goal

A `vm/uml` driver in syzkaller (`pkg/vm/uml/`) that lets
syzkaller use UML the way it currently uses qemu/gce/gvisor.
After this lands, anyone running `syz-manager` can target UML.

This addresses google/syzkaller#1288, open since July 2019.

## Approach

1. Read syzkaller's `vm/qemu/qemu.go` and `vm/gvisor/gvisor.go`
   as reference implementations.
2. Implement `pkg/vm/uml/uml.go`:
   - `Pool.Create()` — spawn a UML process from a built kernel
   - `Instance.Boot()` — wait for boot prompt
   - `Instance.Forward()` — port-forward for syz-executor
     connectivity
   - `Instance.Copy()` — push files into the guest
   - `Instance.Run()` — execute syz-executor
   - `Instance.Diagnose()` — capture panic/oops/oom
   - `Instance.Close()` — shut down cleanly
3. Wire into `sys/targets/targets.go` so `target: uml/amd64`
   resolves.
4. Provide example `syz-manager` config.

## Deliverable

- `pkg/vm/uml/` in syzkaller (PR upstream)
- `dashboard/config/uml-amd64.cfg` example
- Documentation in `docs/uml.md`

## Validation

- `syz-manager -config uml-amd64.cfg` boots, runs syz-executor,
  reports coverage
- Crash reproducer execution works (replay syscalls + verify)
- Numbers: syzkaller against UML achieves >1000 iter/s with
  KCOV (parity with QEMU-KVM ballpark)

## Open questions

- **Q1**: Does UML need any kernel-side hooks to support
  syzkaller, or is this pure host-side work? (Plan: minimal
  hooks — KCOV already exists; syzkaller cares about console,
  ssh, and crash capture, all of which UML provides.)
- **Q2**: How do we capture oops/panic from a UML process?
  (Plan: stderr scraping + faulthandler-like signal handler.
  Document.)
- **Q3**: Is syzkaller maintainership willing to merge?
  (Plan: yes; #1288 is an open invitation. Engage Vyukov early.)

## Risk

Issue has been open for 6 years; nobody volunteered. Possible
reason: nobody had time. Resolved by this plan committing
engineer-time.

Other risk: syzkaller maintainers reject because UML's value
proposition vs QEMU-KVM isn't clear. Mitigation: emphasize the
unique value (no KVM dependency for CI, time-travel determinism).
