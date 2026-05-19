# UML Host Resource Controls — Performance, Efficiency, Quotas

**Status:** design parked in 08-future-phases per `08-future-phases/
README.md`'s rules. Not a single commitment — a menu of three
tiers, each independently shippable. Opens when an operator
question forces the issue (e.g. running 4+ UMLs on one host,
investigating a flake that may be host-induced, or shipping UML
into a CI environment with strict resource quotas).

**Date:** 2026-05-19. Authored after the Round 14 closure
(`50-kvm-v2-django-flake-investigation-summary.md`) re-opened the
broader question of host-side controls — the cache-flake bug was
NOT a host-residency issue (it was a YMM-upper save/restore gap,
fixed as SMP-T73), but the investigation surfaced multiple
host-induced noise sources that we have no current lever to
manage.

**Scope:** features that the UML *host process* uses to control
its own memory residency, CPU placement, scheduling priority, and
resource budget. Out of scope: anything inside the guest kernel.
This is about what the UML binary asks the host kernel to do
*for it*.

## Why this exists

Round 13 T71 shipped two foundation pieces:
`MAP_POPULATE` unconditional on physmem mmap +
`mlockall(MCL_CURRENT|MCL_FUTURE|MCL_ONFAULT)` at UML startup,
gated by `setcap cap_ipc_lock,cap_sys_resource=+ep` on the binary.
That covers the **residency** axis: the UML process's pages are
populated at mmap time and locked against migration / reclaim /
swap.

Everything else — page-merging hygiene, hugepage backing, NUMA
placement, CPU pinning, scheduling priority, cgroup budgets,
OOM-killer behaviour — is unmanaged. The UML process inherits
host defaults that vary across distros, hosts, and kernel
versions. This memo enumerates the levers we *could* pull,
ordered by impact-per-effort, with explicit costs and gotchas.

The R14 negative-result lesson applies: pinning alone does NOT
fix arbitrary backend bugs. These features are about general
performance, predictability, and operational hygiene — **not**
silver-bullet bug fixes. They become high-value when:

1. running multi-UML workloads on one host (avoid one runaway
   OOM-killing the rest);
2. investigating flakes where host-induced state changes are
   suspected (MADV_UNMERGEABLE eliminates a known noise source);
3. shipping into CI with strict per-job quotas (cgroup v2);
4. squeezing the last 10% out of KVM_RUN throughput (hugepages
   + CPU pinning).

## What already ships (post 2026-05-19 implementation pass)

**Tier 1 + Tier 2 (less RT/NUMA/O_DIRECT) shipped:** SMP-T78
through T84 landed in `umlctl-deploy` on 2026-05-19. The
declarative TOML form in `[host_resources]` is wired through
umlctl → manifest → spawn env / cgroup setup. End-to-end
verified on AMD Ryzen 7 7840HS:

| ID    | Lever                             | Source        | Knob                       |
|-------|-----------------------------------|---------------|----------------------------|
| T71   | MAP_POPULATE on physmem           | process.c     | (unconditional)            |
| T71   | MAP_LOCKED on physmem             | process.c     | UM_KVM_V2_PIN_PHYSMEM=1    |
| T71   | mlockall(MCL_CURRENT/FUTURE)      | main.c        | UM_KVM_V2_PIN_PHYSMEM=1    |
| T78   | MADV_UNMERGEABLE on physmem       | process.c     | (unconditional)            |
| T79   | MADV_NOHUGEPAGE/MADV_HUGEPAGE     | process.c     | UM_THP=off / on / auto     |
| T80   | /proc/self/oom_score_adj          | main.c        | UM_OOM_SCORE_ADJ=N         |
| T81   | MAP_HUGETLB \| MAP_HUGE_2MB/1GB   | process.c     | UM_HUGEPAGES=2M / 1G       |
| T82   | sched_setaffinity at startup      | main.c        | UM_KVM_V2_CPU_AFFINITY=L   |
| T83   | cgroup v2 limits + per-instance   | umlctl + supervise.rs + cgroup.rs | `[host_resources]` memory_max/cpu_max/pids_max |
| T84   | preflight verification            | umlctl + preflight.rs | (automatic when knobs set) |

**Declarative form via umlctl:**

```toml
[host_resources]
thp = "off"             # SMP-T79
oom_score_adj = 500     # SMP-T80
cpu_affinity = "0-3"    # SMP-T82
hugepages = "2M"        # SMP-T81
pin_physmem = true      # SMP-T71
memory_max = "1G"       # SMP-T83
cpu_max = "200%"        # SMP-T83
pids_max = 1024         # SMP-T83
```

All 11 in-tree soak templates updated with a default block
(`thp = "off"`, `oom_score_adj = 500`) so the standard soak
runs get the predictability win out of the box. Hugepages,
cpu_affinity, and cgroup limits stay opt-in pending host-side
operator setup.

**Tier 3 (SCHED_FIFO, NUMA, O_DIRECT) — DEFERRED.** Per §3.1-§3.3
each opens only when a concrete operator question demands it
(multi-tenant CI host → §3.1; multi-socket bench rig → §3.2;
multi-UML memory-pressure soak → §3.3). Implementation skeletons
not started; designs preserved below.

## What was in tree before this implementation pass (R13 T71 baseline)

`arch/um/os-Linux/process.c::os_map_memory`:

```c
flags = MAP_SHARED | MAP_FIXED | MAP_POPULATE;
if (getenv("UM_KVM_V2_PIN_PHYSMEM"))
    flags |= MAP_LOCKED;
```

`arch/um/os-Linux/main.c::os_main` (paraphrased):

```c
/* SMP-T71: lock all current + future mappings against migration. */
mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT);
```

`tools/uml/uml-launcher/*` (binary capability bit):

```sh
sudo setcap cap_ipc_lock,cap_sys_resource=+ep /path/to/linux
```

Effect: every physmem page is faulted in at mmap time (no
on-first-touch latency), and the kernel will not reclaim, swap,
or migrate any UML page. Verified via `/proc/$pid/status`:
VmLck matches `mem=` after boot.

What this DOES NOT do: prevent KSM page-merging (those are
locked but still candidate-for-merge), control THP collapse,
constrain CPU placement, declare a memory budget, or pin to a
NUMA node.

## Tier 1 — cheap, high-value (each ≤ 20 LoC)

### 1.1 `madvise(MADV_UNMERGEABLE)` on physmem

**Mechanism:** after the physmem mmap in
`arch/um/os-Linux/process.c::os_map_memory`, call
`madvise(loc, len, MADV_UNMERGEABLE)`. Tells the host KSM
(Kernel Same-page Merging) thread to skip this range when
scanning for identical pages.

**Why:** KSM is a host-side feature that periodically walks VM
memory looking for pages with identical contents and merges them
via COW (one physical page, multiple PTEs). When the guest later
writes to a merged page the host has to un-share + remap, which
shows up to the guest as an asynchronous page-modification event
(the page identity briefly diverges from the PTE).

For deterministic debugging this is a known noise source. For
production multi-UML deployments KSM is sometimes desirable
(memory savings) but more often surprising (latency spikes).

**Cost:** 1 madvise call, ~5 LoC including a probe for
KSM-enabled host (`/sys/kernel/mm/ksm/run` non-zero).

**Risk:** none. madvise is advisory; on a host without KSM
configured the call is a no-op.

**Cmdline knob:** none needed — apply unconditionally. Anyone
running UML on a KSM-enabled host wants this off.

**v1 reference:** N/A (v1 did not address KSM).

### 1.2 `madvise(MADV_NOHUGEPAGE)` vs `MADV_HUGEPAGE` on physmem

**Mechanism:** after the physmem mmap, call madvise with one of
two flags, controlled by a cmdline knob `um_kvm_v2_thp=`:

- `=off` → `MADV_NOHUGEPAGE` (predictable, no defrag latency
  spikes, no `khugepaged` background scan touching the range).
- `=on`  → `MADV_HUGEPAGE` (eager THP collapse, throughput-
  optimised).
- `=auto` (default) → no madvise call, inherit system default.

**Why:** today we inherit
`/sys/kernel/mm/transparent_hugepage/enabled`, which can flip
between `always` / `madvise` / `never` per-distro, per-host,
sometimes per-runtime. For flake investigation `=off` produces
deterministic page-fault behaviour. For production `=on` saves
TLB pressure (see §2.1 for the harder-hugepage version).

**Cost:** 1 madvise call + 1 cmdline knob, ~15 LoC.

**Risk:** `=off` slightly degrades throughput on TLB-bound
workloads. `=on` can cause memory-fragmentation pauses during
khugepaged compaction.

**Gotcha:** `MADV_HUGEPAGE` is opportunistic — it tells
`khugepaged` to *try* but only succeeds when 2 MiB-aligned
candidate ranges exist. For guaranteed hugepage backing see
§2.1 (`MAP_HUGETLB`).

### 1.3 `prctl(PR_SET_OOM_SCORE_ADJ, ...)`

**Mechanism:** at UML startup, write a value to
`/proc/self/oom_score_adj` (range [-1000, +1000]; default 0).
Controlled by a cmdline knob `um_oom_score_adj=N`.

**Why:** under host memory pressure the OOM killer ranks
candidates by `oom_score_adj`. Operators want explicit policy:

- Soak harness: UML is the expendable workload — set +500 so it
  dies first.
- Long-running research session: UML is precious — set -500 so
  the kernel kills something else first.
- CI: usually 0 (host policy decides).

**Cost:** 1 write to `/proc/self/oom_score_adj` + 1 cmdline knob,
~10 LoC.

**Risk:** value outside [-1000, +1000] silently clamped. No
escalation risk — `oom_score_adj` is per-process, doesn't affect
other tasks.

**Operator awareness:** the soak harness currently relies on
host OOM behaviour being "kill the heaviest" — which is usually
the UML guest. Making this explicit (and biasing toward
"kill UML first") prevents the soak from accidentally killing
the daemon or scoreboard writer when one UML iteration blows up.

---

## Tier 2 — medium effort, real wins (each ≤ 100 LoC)

### 2.1 Explicit 2 MiB hugepage backing for physmem

**Mechanism:** map physmem with `MAP_HUGETLB | MAP_HUGE_2MB`
instead of plain `MAP_SHARED`. Controlled by a cmdline knob
`um_kvm_v2_hugepages=2M` (or `=1G`, see below).

```c
flags = MAP_SHARED | MAP_FIXED | MAP_POPULATE;
if (hugepage_size_pow2) {
    flags |= MAP_HUGETLB | (hugepage_size_pow2 << MAP_HUGE_SHIFT);
}
```

**Why:** 1 GiB physmem at 4 KiB pages requires **262 144** PTEs;
at 2 MiB pages, **512** PTEs; at 1 GiB pages, **1** PTE.
Host-side TLB pressure on `KVM_RUN` re-entries drops
proportionally. Phoronix-class virtualization benchmarks
consistently show 5-15% throughput gains for KVM workloads when
guest physmem is hugepage-backed.

**Cost:** ~20 LoC in `os_map_memory` + cmdline knob + a
preflight check (`/proc/sys/vm/nr_hugepages` reservation, see
§3.3) + documentation update for sysadmin instructions.

**Risk:** the hugepage pool must be pre-allocated on the host;
`echo N > /proc/sys/vm/nr_hugepages` before UML start. If the
pool is empty, `MAP_HUGETLB` returns ENOMEM and we have to fall
back to 4 KiB pages with a warning.

**Gotcha 1:** the hugepage pool is system-wide. A CI host
running multiple UMLs concurrently must reserve enough for all
of them. Preflight check (§3.3) catches this before launch.

**Gotcha 2:** 1 GiB hugepages require `CAP_IPC_LOCK` AND the
1 GiB pool must be reserved at host boot time
(`hugepagesz=1G hugepages=N` on the host kernel cmdline). 2 MiB
is the practical sweet spot.

**Gotcha 3:** `MAP_HUGETLB` does not compose with hostfs-backed
mmap (the fd backing physmem is anon, not hostfs, so this is
fine for us — but is worth knowing for future memslot work).

**v1 reference:** v1 did not implement hugepage backing.
External reference: KVM's own documentation recommends
hugepage-backed guests for production workloads.

### 2.2 vCPU host-thread CPU pinning

**Mechanism:** in `arch/um/backend/kvm-v2/vcpu.c::vcpu_create_one`
or the equivalent host-thread setup point, call
`sched_setaffinity(pid, cpu_set_with_just(cpu))` so the host
thread doing `KVM_RUN` for vCPU N stays on host CPU N.

**Why:** v2's per-host-CPU vCPU pool is *designed* to maximize
host CPU locality — vCPU 0 lives on host CPU 0, vCPU 1 on host
CPU 1, etc. Today the host scheduler can still migrate the
KVM_RUN thread arbitrarily, which thrashes the host's per-CPU
KVM caches (mmu_cache, posted_interrupts, vmx_vpid_cache on
Intel). Pinning consolidates the design intent.

**Cost:** ~10 LoC + a cmdline knob `um_kvm_v2_pin_vcpu_threads=`
(default on, off via the knob for debugging).

**Risk:** pinning prevents the scheduler from load-balancing
UML around busy host CPUs. On a contended host this can
*degrade* throughput. The cmdline knob default should match
the deployment (probably on for soak/CI, off for interactive).

**Verification:** read `/proc/$pid/task/$tid/status` after boot
and confirm `Cpus_allowed_list` matches the expected
single-CPU mask per vCPU thread.

**Interaction with snapshot/record-replay (memo 27 §3.7):**
cross-task vCPU pool transitions are already a recorded event;
pinning reduces (but does not eliminate) the frequency of
cross-host-CPU vCPU re-binds.

### 2.3 `umlctl` cgroup v2 integration

**Mechanism:** extend the `umlctl` TOML schema with a new
`[resources]` block:

```toml
[resources]
memory_max = "1G"        # cgroup memory.max
cpu_max    = "200%"      # cgroup cpu.max (2 full cores)
pids_max   = 1024        # cgroup pids.max
io_max     = "<dev> rbps=10M wbps=10M"  # cgroup io.max
```

`umlctl` creates a cgroup under
`/sys/fs/cgroup/uml.slice/<instance-name>/`, writes the limit
files, adds the UML pid via `cgroup.procs`. Cleanup deletes the
cgroup on UML exit.

**Why:** operational hygiene. One runaway UML iteration today
can OOM-kill its sibling iterations in a soak run (the host
OOM killer picks the heaviest target — which can be the
wrong UML or even the soak daemon). A cgroup budget contains
the blast radius.

**Cost:** ~80 LoC in `umlctl/src/gate.rs` (cgroup create + write
+ delete + error handling) + schema bump in the TOML parser +
documentation. Requires the umlctl binary or its parent to have
write access to a cgroup hierarchy under `/sys/fs/cgroup/`
(systemd handles this via `Delegate=yes` in the service file;
ad-hoc operators need `sudo` or a pre-created delegated subtree).

**Risk:** cgroup misconfiguration produces cryptic ENOSPC /
EAGAIN from the constrained UML process. Preflight check (§3.3)
should verify cgroup writability before launch.

**Gotcha 1:** `memory.max` triggers cgroup-OOM, which is more
abrupt than the host OOM killer (no swap, no reclaim). Tune
generously.

**Gotcha 2:** `pids.max` interacts with the UML guest's task
count. UML opens one host thread per guest CPU + one per
pending signal — `pids.max = 1024` is enough headroom for most
workloads but a fork-bomb test will hit it.

**v1 reference:** N/A. The umlctl Rust launcher is v2-only
infrastructure.

---

## Tier 3 — bigger investments, situational

### 3.1 `SCHED_FIFO` for vCPU host threads

**Mechanism:** `sched_setscheduler(pid, SCHED_FIFO,
&(struct sched_param){.sched_priority = 1})` per vCPU thread.
Bounds KVM_RUN re-entry latency — RT-scheduled threads run
ahead of any CFS task.

**Why:** UML's per-host-CPU vCPU pool design assumes the
KVM_RUN host thread can re-enter promptly after a vmexit. Under
host contention the CFS scheduler can delay re-entry by ms,
which the guest sees as a "frozen vCPU" for the duration.

**Cost:** ~10 LoC + `setcap cap_sys_nice+ep` OR a bump to
`RLIMIT_RTPRIO` for the user.

**Risk:** RT priorities preempt **everything** including kernel
work. A buggy UML pinned to RT priority can starve the system.
Production deployment requires RT throttling
(`/proc/sys/kernel/sched_rt_runtime_us`) to be sane (default is
950000/1000000, which limits RT to 95% of any window). For
single-user dev rigs this is moot.

**Why Tier 3:** the soak harness today doesn't simulate host
contention, so the win isn't measurable. Worth implementing
when CI grows to multi-tenant hosts.

### 3.2 NUMA binding (`mbind(MPOL_BIND, …)`)

**Mechanism:** after physmem mmap, `mbind(loc, len, MPOL_BIND,
nodemask, maxnode, 0)` to bind the physmem to a specific NUMA
node. Combine with `numa_run_on_node()` for the host process
itself.

**Why:** on multi-socket servers, cross-NUMA KVM_RUN access
patterns tank. A vCPU running on socket 0 reading physmem
backed on socket 1 pays a 2-4× cycle penalty per cache-line
miss. Pinning physmem + vCPU threads to the same NUMA node
recovers locality.

**Cost:** ~30 LoC + `libnuma` link (or hand-rolled syscall —
`mbind` is just a thin libc wrapper).

**Risk:** mis-bind to a node with insufficient memory → ENOMEM.
Preflight check needed.

**Why Tier 3:** dev rig is single-socket Zen 4 (one NUMA
domain). CI rig is mixed but most current jobs are
single-socket too. Reopens when the cross-host bench matrix
adds a multi-socket Xeon or EPYC host.

### 3.3 `O_DIRECT` for hostfs guest I/O

**Mechanism:** when UML's hostfs / vector-driver reads from a
hostfs-backed file, pass `O_DIRECT` so the host kernel skips its
own pagecache.

**Why:** without `O_DIRECT`, every guest read of a 1 MiB file
populates 256 pages in the host pagecache AND 256 pages in the
guest pagecache — 2× memory pressure for one read. With
`O_DIRECT` the host doesn't cache, the guest does.

**Cost:** ~20 LoC + alignment-handling in the I/O path (O_DIRECT
requires sector-aligned buffers).

**Risk:** `O_DIRECT` semantics are strict (alignment, no
partial reads). Bugs in alignment handling show up as EINVAL on
benign reads. Needs careful test coverage.

**Why Tier 3:** matters most when running 4+ UMLs on one host
(memory pressure on the host pagecache). Single-UML
dev/debug doesn't see the benefit.

### 3.4 Preflight resource verification in umlctl

**Mechanism:** at `umlctl gate` startup, before launching the
UML binary, verify:

1. **Hugepage pool** (if `hugepages=2M` requested):
   `/proc/sys/vm/nr_hugepages` × 2 MiB ≥ requested mem.
2. **MEMLOCK rlimit** (if `mlockall` requested or implicit via
   T71): `prlimit(RLIMIT_MEMLOCK)` ≥ requested mem.
3. **Cgroup writability** (if `[resources]` block present):
   `/sys/fs/cgroup/<target-slice>/` is writable.
4. **CPU set size** (if `ncpus=N` and `um_kvm_v2_pin_vcpu_
   threads=on`): `nproc` ≥ N.
5. **/dev/kvm readability** (existing check — keep).
6. **CAP_IPC_LOCK / CAP_SYS_NICE** as required by other
   tier features.

Fail fast with a clear error message; don't fail 30 s into UML
boot with a cryptic ENOMEM.

**Cost:** ~100 LoC in umlctl + a structured error type per
check.

**Risk:** false positives if the runtime situation changes
between preflight and launch. Treat preflight as a "tighten the
loop" hint, not a contract.

**Why Tier 3:** operationally useful but not a blocker for any
current workflow. Reopens when CI rolls out a job-isolation
preflight gate.

---

## Things deliberately NOT proposed

- **KVM enlightenments** (`KVM_CAP_HYPERV_*`). Aimed at Windows
  guests; irrelevant for UML.
- **`KVM_CAP_X86_DISABLE_EXITS`** (HLT/MWAIT/PAUSE). The
  v2 LSTAR gadget already optimizes the equivalent paths
  differently; redundant.
- **io_uring for KVM_RUN dispatch.** A research project, not
  a feature. Memo 13 sketches this; out of scope here.
- **CRIU integration.** Tangential to the resource-controls
  story; tracked separately if it ever becomes interesting.
- **Cgroup v1.** Strictly v2 going forward — v1 is EOL upstream.
- **AppArmor / SELinux profiles for the UML binary.** Security
  hardening, not resource control. Vector2 has its own sandbox
  story (`28-uml-vector-driver-v2-r7-seccomp-30of30.md`).

## Honest caveat

R13 T71 already proved that **memory residency control alone
does not fix arbitrary kvm-v2 backend bugs**. The Django cache-
flake was fixed by SMP-T73 (YMM-upper XSAVE plumbing), not by
pin/populate. These features are about general performance,
predictability, and operational hygiene.

The investigation-discipline rule from the user's memory
applies: don't ship these as "the fix" for any specific bug
unless the cheapest dispositive control proves it. They are
infrastructure for future investigations and future deployments.

## Suggested sequencing

Tier 1 in one commit each (independent, ~15 LoC apiece):

1. **SMP-T78 — `MADV_UNMERGEABLE` on physmem.** Pure win, no
   knob needed.
2. **SMP-T79 — `MADV_NOHUGEPAGE` / `MADV_HUGEPAGE` cmdline
   knob.** Predictability lever for debug builds.
3. **SMP-T80 — `PR_SET_OOM_SCORE_ADJ` cmdline knob.** Operator
   hygiene for multi-UML soaks.

Tier 2 sequenced after Tier 1, in priority order:

4. **Hugepage backing** (§2.1) — biggest perf win.
5. **CPU pinning** (§2.2) — consolidates the per-host-CPU vCPU
   pool design intent.
6. **cgroup v2 integration in umlctl** (§2.3) — operational
   hygiene for the CI track.

Tier 3 opens when a concrete operator question demands it
(multi-tenant CI host → §3.1 + §3.4; multi-socket benchmark
host → §3.2; multi-UML memory-pressure soak → §3.3).

## Acceptance criteria per feature

For each landed feature, the patch should include:

- a one-line dmesg banner at UML startup describing the active
  setting (e.g. `um: kvm-v2 physmem: 2 MiB hugepages, locked,
  unmergeable, oom_score_adj=+500`) so soak logs record the
  configuration;
- a `/proc/$pid/status` or `/sys/fs/cgroup/.../...` verification
  step in the relevant kselftest under `tools/testing/selftests/
  um/` confirming the lever actually took effect;
- a docs update under `02-workstreams/D-kvm-backend/` or
  `08-future-phases/` describing the operator-facing knob.

## Open questions

### Q1: do any of these features mask R14 — i.e. would they have
prevented us from finding the YMM-upper bug?

`MADV_UNMERGEABLE`, hugepage backing, and CPU pinning all
remove host-side noise sources. R14's signal was a 1-2% flake
rate observed across hundreds of soak iterations. If we had
shipped the Tier 1+2 features before R13, would the YMM-upper
leak have stayed hidden?

**Tentative answer:** no — the YMM-upper leak is purely a
guest-side state-machine bug. The corruption was in CPython
bytecode pages owned by the guest, not in any host-managed
resource. Tier 1+2 reduces host noise but doesn't suppress
guest-side bugs.

**Risk:** future bugs in this class may have *both* a guest
component and a host-induced trigger. Tier 1+2 may make them
harder to reproduce, which is a debugging cost we're trading
for operational stability.

### Q2: cgroup OOM behaviour vs the in-guest "deadlocked on
memory" panic

The cpython-soak r14-24h-soak failure (mem=512M too tight)
produced a guest-side panic, not a host-side OOM kill. With
cgroup memory.max set generously, the failure mode might shift
to cgroup-OOM (which kills the entire UML, including the
soak harness's view of it). Need to decide which failure shape
the soak harness prefers:

- guest panic → clear stack trace in dmesg, takes longer to
  unwind;
- host OOM kill → instant SIGKILL, no diagnostics, but the
  daemon catches the exit code immediately.

Probably want both: cgroup memory.max set to mem + 256 MiB
(host headroom) so the guest panics first if it's a guest-side
issue, but the host OOM kills before the soak daemon's CPU is
exhausted.

### Q3: hugepage pool sizing in CI

CI hosts run multiple soak jobs concurrently. A 1 GiB physmem
soak with hugepages=2M needs 512 hugepages reserved. Four
concurrent jobs need 2048 hugepages × 2 MiB = 4 GiB of
hugepage-reserved memory pre-allocated at boot. This is a CI
infrastructure decision, not a UML-internal one.

Probably want a "hugepages=auto" mode that falls back to 4 KiB
+ a warning if the pool is empty, rather than failing the run.

## References

- T71 baseline: `02-workstreams/D-kvm-backend/state-audit/24-*`
  (R13 physmem pinning experiment).
- T77 feature-enablement checklist: `02-workstreams/D-kvm-
  backend/state-audit/26-feature-enablement-checklist.md`
  (similar checklist-style design memo, different scope —
  CPUID bit un-masking).
- KVM docs on hugepage-backed guests:
  `Documentation/virt/kvm/api.rst` + the kernel `hugetlbfs`
  documentation.
- KSM internals: `Documentation/admin-guide/mm/ksm.rst`.
- cgroup v2 semantics: `Documentation/admin-guide/cgroup-v2.rst`.
- The `mbind` / NUMA interface: `man 2 mbind`,
  `Documentation/admin-guide/mm/numa_memory_policy.rst`.
- Round 14 closure (this work's prerequisite):
  `08-future-phases/50-kvm-v2-django-flake-investigation-
  summary.md`.
