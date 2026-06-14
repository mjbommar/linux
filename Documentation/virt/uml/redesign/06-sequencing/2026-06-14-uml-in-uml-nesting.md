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

## Robustness bug found AND FIXED

With `$HOME` unset (as in a bare nested init environment), the inner UML
panicked during early boot: `make_uml_dir: no value in environment for
$HOME` followed by `Kernel panic - not syncing: Segfault with no mm`
(arch/um/os-Linux/umid.c).

Root cause (pinned by disassembly + RIP): `make_uml_dir()` takes its
`home == NULL` error path and resets the global `uml_dir` to NULL.
`make_umid()` ignored the return value and called
`umid_strscpy(tmp, uml_dir /*NULL*/, ...)` (RIP `umid_strscpy+0x13`,
caller `make_umid+0x82`) -> NULL deref during a no-mm initcall -> panic.
`make_umid()` also runs from several early-boot call sites, so once the
first call nulled `uml_dir`, a later call faulted at `if (*uml_dir ==
'~')` (RIP `make_uml_dir+0x34`) the same way.

Fixed (commit "um: fix early-boot segfault in make_umid() when $HOME is
unset"): `make_umid()` now honors `make_uml_dir()`'s return, and
`make_uml_dir()` guards against a NULL `uml_dir` on re-entry. Validated:
with `$HOME` unset the guest now boots and runs userspace (`/bin/true`
exits cleanly) without a umid dir instead of panicking; the normal
`$HOME` path (mconsole/umid setup) is unchanged. (My first attempt —
only the caller-side check — was reverted because it didn't cover the
re-entry deref; both parts are needed.)

### This is an upstream (mainline) bug, not a branch regression

Confirmed empirically (2026-06-14) with the cheapest dispositive control:
built a clean `torvalds/master` UML (defconfig, in a throwaway worktree)
and booted it with `env -i` (no `$HOME`). **Mainline panics identically:**

    make_uml_dir: no value in environment for $HOME
    Kernel panic - not syncing: Segfault with no mm
    make_umid+0x82/0x5ab

Same panic, same call site as our branch. Our UML v2 rewrite of `umid.c`
(the custom `umid_strscpy`/`umid_strnlen` helpers added for KMSAN, commit
`f73c657f29a9`) did **not** introduce the crash — it preserved the same
buggy flow that already existed upstream. A plausible code-only inference
that mainline's `strscpy` would tolerate the NULL `src` via
`DCACHE_WORD_ACCESS`/`load_unaligned_zeropad` was **wrong**: in the
no-mm early-boot fault path the zeropad fixup does not rescue the read,
so mainline faults too. The empirical boot caught the bad inference —
do not declare our-vs-upstream attribution from code reading alone.

Consequence: the fix repairs a genuine mainline `arch/um` bug and is
**upstream-submittable** (queue it with W8/W9). Before/after under the
identical `env -i` boot:

| kernel | `$HOME` unset boot |
| --- | --- |
| `torvalds/master` | `Segfault with no mm` panic at `make_umid+0x82` — never reaches userspace |
| our `next` (fixed) | warns, "Failed to initialize umid, trying with a random umid", reaches userspace, runs init |

## Bottom line

Yes, UML boots UML — the inner kernel comes up completely. Full
recursive nesting does not work because the inner UML can't run userspace
(the stub trap model doesn't compose). A separate, real early-boot
robustness bug (segfault on unset `$HOME`) was found along the way **and
fixed** — so the inner UML now boots gracefully even without `$HOME` (it
just runs without a umid/mconsole dir); the no-nested-userspace limit is
the remaining wall.
