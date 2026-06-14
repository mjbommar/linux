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

## SMP bug: intermittent startup livelock under parallel build load

SMP boots cleanly at ncpus=1/2/4 and runs light/single-process workloads
fine (the KCSAN race profile passes; an SMP `ncpus=4 -j1` build progresses
normally, 220 obj/4 min). But the SMP `ncpus=4 -j4` build **intermittently
livelocks at the defconfig→compile transition**:

- Run 1: hung at 9 objects for ~57 min. The UML kernel host process spun at
  ~199% CPU (state R) while the build's `uml-userspace` children SLEPT (0%
  CPU) and zero new objects appeared — a kernel-side livelock, not slowness.
  Killed manually.
- Run 2 (`-j4`, 4-min window): progressed to 661 objects.
- Run 3 (`-j4`, full): **completed** (934 obj, 369 s).

So ~1 of 3 `-j4` attempts hung; the others ran clean. This is a **race**
triggered by many concurrent runnable userspace processes across vCPUs, not
a deterministic failure, and it strikes early (around the first wave of
parallel compiler spawns). It does not occur with `-j1` on SMP or with any
`-j` on UP.

**Fix locus (hypothesis):** UML SMP scheduler / cross-vCPU IPI / stub
worker handoff under a burst of concurrently-runnable processes. The
signature (kernel spinning while all runnable userspace tasks block) points
at a missed wakeup / IPI or a run-queue/stub lock that can wedge under the
initial parallel-spawn burst.

## Bottom line

- Self-hosting compilation works under UML v2 (UP and SMP), producing
  bootable kernels; SMP `-j4` is the fast path (~6 min).
- The one open SMP defect is an intermittent parallel-build startup
  livelock — a real stability bug worth fixing before SMP `-j4` workloads
  can be called reliable, but not a blocker for the broader v2 work
  (single-process and UP loads are solid).
