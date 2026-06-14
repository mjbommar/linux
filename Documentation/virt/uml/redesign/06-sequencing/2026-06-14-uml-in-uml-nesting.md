# UML-in-UML (matryoshka) — 2026-06-14

Can we run UML inside UML, recursively? **The inner UML kernel boots fully
(one level deep), but it cannot run userspace, so the nesting stops there.**

## Setup

A UML guest sees the host filesystem via hostfs, so the `linux` ELF is
present and executable inside the guest. Launching it again from the L1
guest's init starts an L2 UML. Each level needs:
- an exec-capable `/dev/shm` tmpfs **sized larger than the child's `mem`**
  (the child's physmem backing file lives there; the default tmpfs size is
  50% of RAM == the child's mem, which SIGBUSes at the boundary). Mount it
  at ~85% of the level's RAM.
- `$HOME` set, so the inner UML can create its `~/.uml/<umid>` dir (see the
  bug below).
- decreasing memory per level (we used ~40% of the parent).

## Result: one level deep

- **L1** (UML on host): fully functional.
- **L2** (UML inside L1): the **kernel boots completely** — `/dev/shm`
  PROT_EXEC check, seccomp install, memory/scheduler/VFS init, BogoMIPS
  calibration, `mconsole (version 2) initialized`, ubd io_uring, hostfs
  root mounted, `devtmpfs: mounted`, `VFS: Pivoted into new rootfs`,
  `Run ... as init process`. Then its **first userspace process fails**:
  even `init=/bin/true` yields `UML: fatal signal; exiting`. So L2 cannot
  run any userspace, and therefore cannot launch an L3.

**Why it stops:** UML runs guest userspace via its stub mechanism — a stub
process whose syscalls/faults are trapped (seccomp backend: SIGSYS/SIGSEGV)
and serviced by the UML kernel. For L2 that stub must itself run as a
userspace process inside L1, which is already trapping userspace the same
way. The nested trap/signal model does not compose: L2's first
userspace exec takes a fatal signal. So **matryoshka depth = 1 functional
level + 1 kernel-only half-level** with the seccomp backend (kvm-v2 can't
nest either — no `/dev/kvm` inside L1).

## Robustness bug found (not yet fixed)

With `$HOME` unset (as in a bare nested init environment), the inner UML
panics during early boot: `make_uml_dir: no value in environment for
$HOME` followed by `Kernel panic - not syncing: Segfault with no mm`
(arch/um/os-Linux/umid.c, `make_uml_dir` → called from `make_umid_init`).
The fault is inside `make_uml_dir` on the `home == NULL` path, during an
early initcall where there is no `mm`, so the normal fault path turns into
a panic. Setting `$HOME` avoids it (and lets L2 boot fully, as above).

A first fix attempt — having `make_umid` check `make_uml_dir`'s return
value (which it currently ignores) — was reverted: the disassembly shows
the fault is *inside* `make_uml_dir` before it returns, so the caller-side
check does not prevent it. The real fix needs to harden the `home == NULL`
early-boot path in `make_uml_dir` itself (graceful failure instead of a
no-mm segv). Left as a follow-up; it only triggers with an unset `$HOME`,
an unusual configuration.

## Bottom line

Yes, UML boots UML — the inner kernel comes up completely. Full
recursive nesting does not work because the inner UML can't run userspace
(the stub trap model doesn't compose). A separate, real `make_uml_dir`
robustness bug (segfault on unset `$HOME`) was found along the way.
