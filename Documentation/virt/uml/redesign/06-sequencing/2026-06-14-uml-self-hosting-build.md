# UML Self-Hosting Build + SMP Livelock — 2026-06-14

Can a UML guest compile the UML v2 branch (`arch/um`) under itself, ideally
with SMP `-j4`? **Yes.** A UML guest built a complete, bootable UML kernel
from the branch source, on both a UP and an SMP (4-CPU) guest. One genuine
SMP bug surfaced: an intermittent startup livelock under parallel build load.

Branch: `next` (post torvalds/master merge). Host toolchain reaches the
guest via hostfs (root=hostfs); builds use `O=` to a host path under
`$HOME` (never the source tree; the guest's `/tmp` is a separate tmpfs and
is not host-visible).

## Result

| Guest | config | result | wall | artifact |
| --- | --- | --- | ---: | --- |
| `uml-pool` | UP (CONFIG_SMP=n), `-j4` | MAKE_RC=0 | 943 s (~16 min) | 87 MB `linux`, boots |
| `uml-smp-self` | SMP `ncpus=4`, `-j4` | MAKE_RC=0 | **369 s (~6 min)** | 87 MB `linux`, boots |

Both self-built kernels boot and run userspace (`Linux version 7.1.0-rc7
(root@(none)) ...`, seccomp backend, init runs). SMP `-j4` is ~2.5x faster
than UP thanks to real 4-way parallelism. defconfig, 934 objects.

## Prerequisite finding: gcc can't self-relocate over hostfs

In-guest compiles first failed with
`gcc: fatal error: cannot execute 'cc1': posix_spawnp: No such file or
directory`. cc1 exists and runs directly, but `gcc -print-prog-name=cc1`
returns the bare name `cc1` (host returns the full
`/usr/libexec/gcc/x86_64-linux-gnu/15/cc1`): gcc's install-dir
self-relocation (via `/proc/self/exe` / argv0) does not resolve under UML
hostfs, so it cannot find cc1/ld in its computed prefix and falls back to a
PATH search → ENOENT.

**Workaround:** prepend the gcc libexec dir to `PATH` for the build:

    export PATH=/usr/libexec/gcc/x86_64-linux-gnu/15:/usr/bin:/bin:/sbin:/usr/sbin

This covers cc1/cc1plus; ld/as are found via `/usr/bin`. `GCC_EXEC_PREFIX`
alone is insufficient (cc1 lives in libexec, not lib/gcc; collect2 still
fails to find `ld`). This is a UML+hostfs limitation, not a toolchain bug;
it would also affect any in-guest compilation over hostfs.

## SMP under parallel build: one unreproduced wedge (NOT a confirmed bug)

SMP boots cleanly at ncpus=1/2/4 and runs light/single-process workloads
fine (the KCSAN race profile passes; an SMP `ncpus=4 -j1` build progresses
normally). SMP `ncpus=4 -j4` self-builds **reliably**: two full builds
completed (the 369 s one above; another partial run reached 661 obj/4 min)
and a dedicated catch harness then ran **31 consecutive clean `-j4`
attempts, all of which progressed past the early transition — 0 hangs.**
Counting everything, **33 of 34 clean `-j4` runs worked.**

The lone exception was the very first self-build: it wedged at 9 objects
for ~57 min with the UML kernel host process spinning at ~199% CPU
(state R) while the build's `uml-userspace` children slept and no objects
appeared — a real kernel-side wedge at that moment. **It has not
reproduced in 31 subsequent clean attempts**, so it is treated as a
**rare, unreproduced one-off, not a confirmed reproducible SMP bug**; its
cause is unconfirmed (could be a rare scheduler/IPI race, or a one-time
startup/host/hostfs transient on a cold first run).

Honesty note: an earlier draft of this file called it an "intermittent
~1/3 livelock." That estimate was inflated — it rested on the single
first-run wedge plus a `-j8` "repro loop" whose readings were a
measurement artifact (a sed bug in that harness mangled `O=`, so those
builds went in-tree, contaminated the source tree, and left `O=` empty —
the "0-object hangs" were never real builds). After `mrproper` and a
fixed harness, the clean data shows SMP `-j4` is reliable.

If the wedge is ever caught again, the diagnostic is ready
(`thread apply all bt` on the spinning UML host process via gdb; the
binary is not stripped). Likely suspects if real: UML SMP scheduler /
cross-vCPU IPI / stub-worker handoff under the first parallel-spawn burst.

## Bottom line

- Self-hosting compilation works under UML v2 on both UP and SMP,
  producing bootable kernels; SMP `-j4` is the fast path (~6 min) and is
  reliable across 33/34 clean runs.
- There is **no confirmed reproducible SMP bug** from this exercise — only
  a single unreproduced first-run wedge (0/31 on retry). Worth keeping an
  eye on, but not a characterized defect.
