# Phase J LTP curation — KEEP / SKIP / TRIAGE walk (2026-05-14)

> Companion to `phase-J-design-2026-05-07.md` §4. The design memo names
> "LTP curation" as one of three Phase-J-DONE prerequisites and sketches
> the bucket framework. This memo walks the actual LTP test tree
> (upstream `linux-test-project/ltp` HEAD = `8cd7644`, VERSION
> `20260130`) and produces (a) a top-level KEEP/SKIP/TRIAGE
> classification, (b) a seed deny-list at per-test granularity, (c) the
> shape of the `tier-ltp.toml.template` runner stub, and (d) a
> wall-clock budget for slotting LTP into the daemon rotation.
>
> Scope: memo only. No code, no shell, no toml. A reviewer six months
> from now must be able to implement `tier-ltp.toml.template` and the
> daemon's special-case-rotation handling directly from this memo plus
> the design memo's §4.

## 1. TL;DR

- **LTP is not in apt on Ubuntu 26.04 Resolute** (`apt-cache search
  ltp` returns `ltpanel` and unrelated packages; `apt-cache policy ltp`
  is empty; no `linux-test-project`, `ltp-tests`, or `ltp-source`
  package exists). Upstream `https://github.com/linux-test-project/ltp`
  is alive (HEAD `8cd7644a52e3` from 2026-05-13). Operator must
  `git clone && make autotools && ./configure && make && make install`
  once into `/opt/ltp/`.
- **`runltp` is gone.** Upstream renamed the entry point to **kirk**
  (`https://github.com/linux-test-project/kirk`) circa 2026; the
  `runltp` shim in the LTP tree now exits 1 with a redirect message.
  Phase J's runner template targets `kirk`, not `runltp`. Section 4
  re-specifies the invocation accordingly; the design memo's §4.3
  example (`/opt/ltp/runltp -S /etc/ltp-skiplist -p -q ...`) is
  superseded by this memo.
- **LTP HEAD: ~2 900 test entries across 14 active scenario files.**
  Counted from `/tmp/ltp-curation/runtest/*` after a shallow clone:
  syscalls 1909, controllers 407, mm 100, cve 98, fs 89, containers
  93, syscalls-ipc 68, hugetlb 59, sched 22, pty 18, smoketest 15,
  tracing 11, numa 23, plus single-file scenario files (uevent,
  watchqueue, staging). Not all "entries" are independent tests —
  syscalls e.g. has many `add_key01 add_key01` lines that map to the
  same C binary — but the order-of-magnitude figure is correct.
- **Recommended UML-realistic subset:** ~1 100-1 300 tests (syscalls
  minus the known-hostile families, plus syscalls-ipc, sched, pty,
  smoketest, numa subset, mm minus hugepage/swap). Skip controllers,
  containers (most), hugetlb, tracing, kvm, kernel_misc, cve (most),
  network (all), realtime, watchqueue, uevent, device-drivers,
  power_management, hotplug, mce-test.
- **Most surprising UML-hostile directory:** `testcases/kernel/kvm/`.
  LTP ships its own KVM-on-x86 selftest suite (`kvm_svm0[1-4]`,
  `kvm_vmx0[12]`, `kvm_pagefault01`) that exercises **the host**
  KVM ABI via `/dev/kvm` from guest userspace. On a UML guest where
  the "kernel" is itself a usermode process, `/dev/kvm` is the host
  hypervisor's device — running LTP-kvm inside UML would have the
  guest userspace poke the host's KVM, which is both useless and a
  privilege boundary violation. Hard SKIP. Easy to miss because
  "kvm tests on a kvm-v2 backend kernel" sounds like the headline
  thing you'd want to run.
- **Recommended max-wallclock per LTP cycle: 45 minutes.** Initial
  budget: a single curated run (~1 200 tests, 80 % short, 20 %
  medium) should complete in 25-35 min on this hardware. Add 30 %
  headroom for the slower tail. Daemon `exec-timeout` = 60 s per
  individual kirk test; `suite-timeout` = 50 min for the rolled-up
  invocation; daemon hard kill at 60 min if kirk wedges. Scheduled
  once every 8 rotations (~once per 4-6 hours under the design memo's
  default rotation cadence), giving 4-6 LTP cycles per 24-hour soak.

## 2. Top-level directory categorisation

LTP's source-of-truth is **not** the `testcases/kernel/*` directory
tree — that's the build layout. The runtime entry point is the
`runtest/*` scenario files, each a flat list of `<name> <command>`
lines that kirk reads as a "suite." The categorisation below is at
both layers because they map differently to the SKIP mechanism:

- `runtest/<file>` controls **which suites the runner is invoked
  against** (kirk's `--run-suite` flag).
- Individual test names control **which tests are skipped within
  a suite** (kirk's `--skip-file` flag).

The KEEP/SKIP/TRIAGE columns below apply per scenario file. Per-test
skips inside a KEEP scenario live in §3.

### 2.1 Scenario files (runtest/*) — what to feed kirk's --run-suite

| Suite (runtest/*) | Tests | Category | Reason |
|-------------------|-------|----------|--------|
| `smoketest`       | 15    | **KEEP** | Designed as a CI smoke-test; deliberately UML-friendly per LTP convention. Best first run for "is LTP wiring alive." |
| `syscalls`        | 1909  | **KEEP** (with per-test skips, §3) | The high-signal suite. Most syscalls UML implements; the per-test skip-list pares out module-load, hugepage, network-namespace, BPF, kexec, perf-event, fanotify, key-ring, io_uring tests. |
| `syscalls-ipc`    | 68    | **KEEP** | msgctl/msgget/msgrcv/msgsnd/semctl/shmat family. UML's SysV IPC is the normal kernel implementation; this is high-signal. |
| `sched`           | 22    | **KEEP** | hackbench, pth_str*, sched_football, cfs_bandwidth01. Stresses CFS scheduler — UML's task scheduling is the same Linux scheduler. Direct v2-vs-seccomp parity surface. |
| `pty`             | 18    | **KEEP** | pty01-pty09, ptem01-ptem06, hangup01. UML supports pty via console driver; runs cleanly per upstream CI experience. |
| `mm`              | 100   | **KEEP** (with per-test skips) | mmap21, mmstress, mmap1/2/3, mtest01w (page-write stress), data_space, stack_space. SKIP the hugepage variants (none present in this scenario), KEEP everything else. UML MM is *the* surface kvm-v2 needs validated. |
| `numa`            | 23    | **TRIAGE-LATER** | migrate_pages, move_pages, set_mempolicy, mbind. UML has no NUMA topology by default; tests should mostly SKIP via CONF (not FAIL), but verify before declaring KEEP. |
| `nptl`            | 1     | **KEEP** | Single nptl01 test. POSIX threading; trivially KEEP. |
| `math`            | n/a (runtest/math; ~22 tests per kirk README sample) | **KEEP** | abs01/atof01/float_*/fptest. Pure userspace; doesn't touch any UML-specific surface, but parity-checks the kernel doesn't break libc somehow. |
| `fs`              | 89    | **TRIAGE-LATER** | growfiles, fsstress on the iteration's TMPDIR. On UML with hostfs as the root, "filesystem stress" becomes "hostfs stress on the host's real FS." Useful as a hostfs torture test but operator must accept that semantics. Defer wiring until hostfs-vs-ext4-backed-disk decision is made. |
| `fs_bind`         | (see runtest) | **SKIP** | Bind-mount + propagation tests; tied to mount-namespace mechanics. UML can mount bind, but the test rig expects host-style mount tables. |
| `fs_perms_simple` | small | **TRIAGE-LATER** | DAC perm tests. Depends on whether hostfs honours the test's expected `chown`/`chmod` semantics. |
| `fs_readonly`     | small | **SKIP** | Expects to remount target filesystems read-only; hostfs doesn't honour this the way a real block FS does. |
| `fcntl-locktests` | n/a   | **TRIAGE-LATER** | POSIX fcntl(F_SETLK) tests. Should work, but UML's range-lock implementation has historically had edge cases. Worth running once to see. |
| `cve`             | 98    | **SKIP** | Curated regression tests against historical kernel CVEs. Most are for features UML doesn't have (SCTP, F2FS, eBPF JIT, KASLR bypasses, Spectre/Meltdown variants on real x86 microarch). The handful that would apply (e.g. mremap CVEs) are duplicated in `syscalls`. Not worth the wiring cost. |
| `containers`      | 93    | **SKIP** | netns_*, pidns, userns, mqns, sysvipc-ns, timens, utsname. Heavy on network-namespace setup with veth pairs, bridges, and `ip netns exec`. UML guest networking is virtio-net via a host tap; the in-guest "create another netns and put a bridge in it" model doesn't fly under our boot environment. Some pidns/userns tests would work but the suite is whole-or-nothing in kirk's view; cleaner to skip. |
| `controllers`     | 407   | **SKIP** | cgroup v1 + v2 controller tests (memcg, cpuctl, cpuset, freezer, io, pids, cpuacct). Largest single suite. UML cgroup is partial (no real I/O controller; memcg works); running it gives a sea of CONF/SKIP without much signal. Defer; revisit if cgroup parity becomes a workstream. |
| `hugetlb`         | 59    | **SKIP** | hugemmap01-21, hugefork, hugefallocate, hugeshm*. UML's MM doesn't expose hugetlbfs by default and the build doesn't enable `CONFIG_HUGETLBFS=y`. All tests CONF-out as "hugepages not available." Zero signal, ~20 s wasted per cycle. |
| `tracing`         | 11    | **SKIP** | ftrace_regression*, dynamic_debug, pt_test (Intel PT — UML can't have it). UML build has `CONFIG_FTRACE=n` in our reference config. No signal. |
| `kvm`             | 8     | **SKIP** (hard) | LTP's *own* in-guest KVM tests (`kvm_svm*`, `kvm_vmx*`, `kvm_pagefault01`). These exercise the host's `/dev/kvm` from inside the guest userspace. In a UML guest, `/dev/kvm` is the host hypervisor's device — the test would either fail to open it (UML doesn't pass /dev/kvm through to the guest) or worse, succeed and poke at the host's KVM. Hard skip. Most surprising hostile directory in the tree. |
| `crashme`         | n/a   | **SKIP** | Random-instruction fault tester aimed at bare-metal/qemu kernels. On UML the SIGILL handling path is the same surface T57 lives in; we already have a dedicated reproducer. Skipping avoids duplicate noise. |
| `connectors`      | small | **SKIP** | Process-event connector / netlink. Depends on `CONFIG_PROC_EVENTS=y` which we don't enable. |
| `crypto`          | n/a   | **TRIAGE-LATER** | AF_ALG socket tests; depend on `CONFIG_CRYPTO_USER_API=y`. UML's reference config does enable AF_ALG; tests should pass. Run once to confirm before declaring KEEP. |
| `dma_thread_diotest` | small | **SKIP** | Direct DMA-thread I/O tests; need a real block device with DMA. UML has no DMA controller; ubd is a userspace pread/pwrite stub. |
| `cpuhotplug`      | 7     | **SKIP** | cpuhotplug02-07. UML SMP CPU hotplug exists but is brittle; tests assume a richer `/sys/devices/system/cpu/cpu*/online` ABI than UML exposes consistently. Defer. |
| `hyperthreading`  | small | **SKIP** | Sibling-CPU cache-coherence tests; UML has no hyperthreads (each "CPU" is a host thread). |
| `ima`             | small | **SKIP** | Integrity Measurement Architecture; needs `CONFIG_IMA=y`. UML build doesn't enable. |
| `irq`             | 1     | **SKIP** | `irqbalance01.c` — userspace daemon test. UML's interrupt model is software-IRQ from the host; no irqbalance to test. |
| `input`           | 6     | **SKIP** | `/dev/input/event*` tests. UML guest has no input devices. |
| `kernel_misc`     | ~20   | **SKIP** | rcu_torture, lock_torture, fw_load, zram01-03, block_dev, tpci, tbio, ltp_acpi, rtc, cpufreq_boost, aslr01, umip. Mostly hardware-driver or platform-specific. UML aslr01 may be runnable (KEEP candidate); the rest skip. |
| `staging`         | 1     | **SKIP** | "Tests for features that are not yet in the stable kernel ABI." Empty by design; nothing to run. |
| `watchqueue`      | 9     | **TRIAGE-LATER** | `wqueue01-09`. Tests `CONFIG_WATCH_QUEUE=y` keyring-watch-notification interface. Depends on keyring subsystem availability; if UML config doesn't enable `CONFIG_WATCH_QUEUE`, tests CONF-out. Verify config before declaring KEEP. |
| `uevent`          | 3     | **SKIP** | `uevent01-03`. Test udev/kobject uevent broadcast. UML emits uevents but the surface that matters here is the host's udev; not a kernel test that lights up under UML usefully. |
| `dio` / `ltp-aiodio.part1-4` | many | **SKIP** | Async-direct-I/O torture. Needs O_DIRECT-supporting filesystem under TMPDIR. Hostfs and ubd don't honour O_DIRECT; tests EINVAL out. |
| `ltp-aio-stress`  | small | **SKIP** | libaio stress; same reason as dio. |
| `scsi_debug.part1` | small | **SKIP** | Loads `scsi_debug` module; UML doesn't support module loading. |
| `tpm_tools`       | small | **SKIP** | TPM hardware/emulator tests. No TPM in UML. |
| `smack`           | small | **SKIP** | Smack LSM; not in UML build. |
| `capability`      | (see runtest/capability) | **KEEP** | POSIX capabilities tests; UML supports the full capability ABI. |
| `commands`        | n/a   | **TRIAGE-LATER** | Tests of host userspace commands (cp, mv, ln, ar, tar, gzip). Useful smoke test of the whole guest userspace; uncertain whether they assume a real shell environment we have. |
| `net.*` (10 files) | many | **SKIP** | All of `net.features`, `net.ipv6`, `net.ipv6_lib`, `net.multicast`, `net.nfs`, `net.rpc_tests`, `net.sctp`, `net.tcp_cmds`, `net.tirpc_tests`. Each expects two hosts (LHOST/RHOST setup) with reachable networking; the LTP network suite is *integration tests across two boxes* by design. Out of scope for guest-internal soak. Revisit only if Tier 3 (Django/FastAPI loopback) leaves uncovered net-stack surface. |
| `net_stress.*` (10 files) | many | **SKIP** | Same — multi-host stressors (iperf, tcp_fastopen, ipsec_*, route, dccp). |
| `power_management_tests` | small | **SKIP** | C-state / P-state transitions; not applicable to UML. |
| `numa` (separate from /numa above) | overlap | (covered above) | — |
| `realtime` (top-level testcases/realtime/) | many | **SKIP** | PREEMPT_RT-specific. UML build is `CONFIG_PREEMPT_VOLUNTARY`; tests measure rt-throttle and rt-mutex behaviour that doesn't apply. |
| `open_posix_testsuite` (top-level) | hundreds | **TRIAGE-LATER** | The POSIX conformance suite; partly overlaps `syscalls`. Worth a separate look once core syscalls KEEP is green. Defer to Phase J + 1. |

### 2.2 Headline split

After applying the table above, the **kirk `--run-suite` invocation
for Phase J is:**

```
smoketest syscalls syscalls-ipc sched pty mm nptl math capability
```

That's 9 suites totalling ~2 200 test entries before per-test skips
are applied (which pare ~150 entries — see §3). Effective active
test count: **~2 050**.

TRIAGE-LATER suites for follow-up Phase J + 1:

```
numa crypto fs fs_perms_simple fcntl-locktests watchqueue commands
open_posix_testsuite
```

Hard SKIP (never run inside UML soak):

```
containers controllers hugetlb tracing kvm crashme connectors
dma_thread_diotest cpuhotplug hyperthreading ima irq input
kernel_misc staging uevent dio ltp-aiodio.part{1,2,3,4}
ltp-aio-stress scsi_debug.part1 tpm_tools smack
fs_bind fs_readonly cve power_management_tests realtime
net.* (10 files) net_stress.* (10 files)
```

## 3. Per-test skip-list seed

Even within the KEEP scenarios above, individual tests target
syscall families UML can't service. The seed deny-list below is
fed to kirk via `--skip-file /etc/ltp-skiplist` (or whatever path
the daemon mounts via hostfs). Entries are exact test names as
they appear in column 1 of `runtest/*` — these are what kirk
matches against.

### 3.1 Loadable kernel modules — UML doesn't load modules

```
init_module01
init_module02
finit_module01
finit_module02
delete_module01
delete_module02
delete_module03
```

(7 entries. UML can be built with `CONFIG_MODULES=y` but the
loader is a no-op for code; module syscalls return ENOSYS for any
test that tries to actually insert object code.)

### 3.2 Key-ring tests — UML kernel doesn't build the keyrings module by default

```
add_key01
add_key02
add_key03
add_key04
add_key05
keyctl01
keyctl02
keyctl03
keyctl04
keyctl05
keyctl06
keyctl07
keyctl08
keyctl09
keyctl10
request_key01
request_key02
request_key03
request_key04
request_key05
```

(20 entries. Some may pass once `CONFIG_KEYS=y` is in the reference
build; verify on a sample iteration before un-skipping. Watchqueue
also depends on this.)

### 3.3 BPF — UML has no JIT and `bpf()` returns EPERM/ENOSYS

```
bpf_map01
bpf_prog01
bpf_prog02
bpf_prog03
bpf_prog04
bpf_prog05
bpf_prog06
bpf_prog07
```

(8 entries.)

### 3.4 Hugepages — UML MM doesn't expose hugetlbfs

```
mmap10
mmap11
mmap12
mmap13
mmap14
```

(5 entries — the mmap tests that take MAP_HUGETLB. Plus the entire
`hugetlb` suite is skipped at the suite layer per §2.)

### 3.5 fanotify — depends on `CONFIG_FANOTIFY=y` we don't enable

```
fanotify01
fanotify02
fanotify03
fanotify04
fanotify05
fanotify06
fanotify07
fanotify08
fanotify09
fanotify10
fanotify11
fanotify12
fanotify13
fanotify14
fanotify15
fanotify16
fanotify17
fanotify18
fanotify19
fanotify20
fanotify21
fanotify22
fanotify23
fanotify24
fanotify25
```

(25 entries. Heavy block; primary reason to confirm
`CONFIG_FANOTIFY=n` in the reference build or to enable it and
KEEP these — they're high-signal-per-test.)

### 3.6 Performance counters — UML has no perf-event subsystem

```
perf_event_open01
perf_event_open02
perf_event_open03
```

### 3.7 kexec / reboot — destructive in UML, doesn't reboot the host

```
kexec_load01
kexec_load02
kexec_file_load01
kexec_file_load02
reboot01
reboot02
```

(6 entries. `reboot01/02` would actually halt the UML kernel
mid-iteration if not skipped; this is the highest-cost mis-skip
on the list — failing to deny-list these turns one LTP cycle
into one half-completed cycle plus a host-side restart.)

### 3.8 Network namespace + sched_setattr-on-netns tests

```
setns01
setns02
unshare01
unshare02
unshare03
unshare04
unshare05
```

(7 entries. Some unshare tests cover non-net namespaces and *might*
work; defer to TRIAGE-LATER on a per-test basis. Initial seed
skips them all.)

### 3.9 io_uring — kernel build option not enabled in reference UML

```
io_uring_setup01
io_uring_register01
io_uring_register02
```

### 3.10 Memory-pin / userfaultfd / page-key syscalls (UML support partial)

```
mlockall01
mlockall02
mlockall03
mlock201
mlock401
userfaultfd01
userfaultfd02
pkey_mprotect01
pkey_alloc01
```

(9 entries. mlock should work but UML's mlock has historically
been a no-op; these test return-value compliance that the no-op
may fail. Verify and prune.)

### 3.11 Landlock — newer LSM, not in UML reference config

```
landlock01
landlock02
landlock03
landlock04
landlock05
landlock06
landlock07
landlock08
landlock09
landlock10
landlock11
landlock12
landlock13
```

(13 entries.)

### 3.12 swapon / swapoff — UML has no swap device in soak rig

```
swapon01
swapon02
swapon03
swapoff01
swapoff02
swapping01
```

(6 entries. `swapping01` allocates 80 % of RAM and forces it to
swap — both legitimately useful and useless when there's no
swap device.)

### 3.13 Total seed deny-list size

Counting the entries above: 7 + 20 + 8 + 5 + 25 + 3 + 6 + 7 + 3 +
9 + 13 + 6 = **112 entries.** Larger than the design memo's
~30-entry estimate; the difference is mostly fanotify (25),
landlock (13), and keyctl (20), three families the design memo
folded into single rows ("loadable kernel modules / network
namespace / ..."). Per-test granularity is what kirk actually
wants.

### 3.14 What's *not* in the seed (deliberately)

- `mmap01` through `mmap09` (the non-hugetlb variants) — KEEP, real
  signal.
- `clone01`, `clone02`, `clone3_01` through `clone3_06` — KEEP. The
  pilot's cpython-soak already exercises fork+exec heavily; LTP's
  clone tests are higher-resolution variants.
- `signal*`, `sigaction*`, `sigsuspend*` — KEEP. T56 (LSTAR-EINTR)
  was a signal-handling bug; LTP signal tests are direct
  parity-against-seccomp signal.
- `epoll*`, `poll*`, `select*` — KEEP. Event-loop syscalls; high
  value because all three Tier 3 frameworks (Django/FastAPI/uvicorn)
  end up in `epoll_wait` under load.
- `ptrace*` — KEEP, but watch for false positives. UML's ptrace
  implementation **is** different from KVM Linux's (it has to be —
  ptrace is how UML's seccomp backend gets syscall control). v2-vs-
  seccomp parity on ptrace tests is high-signal precisely because
  v2 changes how ptrace integrates with the guest scheduler.
- `futex*` — KEEP. Same reasoning as signal.
- `madvise*` (most), `mmap*` (non-huge), `mprotect*`, `mremap*` — KEEP.
  Direct MM-correctness coverage, the second-most-important surface
  after signal/futex.

## 4. Runner template shape (`tier-ltp.toml.template`)

Skeleton specification only; not a working file. Reviewer
implements from this stub by filling in the established
template-toml pattern from the existing soak templates
(`memcheck.toml.template`, `cpython-soak.toml.template`).

### 4.1 Template structure (prose form)

The template is a single-instance, two-phase template:

- **[instance]** block: `name = "soak-tier-ltp-{ITER}"`, `cpus = 2`,
  `mem = "2G"` (LTP suites benefit from more RAM than the 1 G
  default; mtest01 alone takes ~512 MB at -p80).
- **[runtime]** block: standard kernel + initrd substitution
  variables, same as the pilot templates.
- **[hostfs]** block: at least three mounts:
    1. `/opt/ltp` (read-only) — the installed LTP tree; tests live
       at `/opt/ltp/testcases/bin/*`, `runtest/*` at the root, and
       `kirk` either bundled into `/opt/ltp/kirk` or at
       `/opt/ltp/bin/kirk`.
    2. `/etc/ltp-skiplist` (read-only) — the file from §3, as a
       single-file mount.
    3. `/var/log/ltp` (read-write) — kirk's `--tmp-dir` target.
- **[network]** block: **not required** (and explicitly disabled).
  The KEEP suites in §2 do not need network. Omitting saves the
  TAP setup latency per iteration (~3-5 s on the pilot rig).

Two phases inside the template:

**Phase 1 — `ltp-run`** (the body):

```
cmd = """
  cd /opt/ltp && \\
  PATH=/opt/ltp/testcases/bin:$PATH \\
  LTPROOT=/opt/ltp TMPDIR=/var/log/ltp \\
  python3 /opt/ltp/kirk \\
    --tmp-dir /var/log/ltp \\
    --skip-file /etc/ltp-skiplist \\
    --json-report /var/log/ltp/kirk-${ITER}.json \\
    --suite-timeout 50m \\
    --exec-timeout 60s \\
    --workers 1 \\
    --sut host \\
    --run-suite smoketest syscalls syscalls-ipc sched pty mm nptl math capability \\
    && echo LTP_RUN_DONE rc=0 \\
    || (echo LTP_RUN_DONE rc=1; exit 1)
"""
timeout_secs = 3600
pass_marker  = "LTP_RUN_DONE rc=0"
fail_marker  = "LTP_RUN_DONE rc=1"
```

Notes on the body:

- The `&& echo ... || (echo ...; exit 1)` shape is the **pilot bug
  1 pattern** — same as cpython-soak's. Without it, pipefail loss
  swallows kirk's non-zero exit. Mandatory.
- `--sut host` tells kirk to run tests locally (in this UML guest),
  not over SSH/QEMU. The default is SSH; specifying `host`
  short-circuits to direct exec.
- `--workers 1` — Phase J's soak runs LTP **serial** within a single
  iteration. Parallelism comes from the daemon running multiple
  `umlctl gate loop` workers concurrently, each with its own UML
  guest; running kirk's own parallelism on top would conflate the
  two failure modes and break the existing scoreboard schema.
- `--exec-timeout 60s` — kills any single LTP test that wedges; lets
  the suite continue past it. Combined with `--suite-timeout 50m`
  the upper bound is well-defined.
- The JSON report at `/var/log/ltp/kirk-${ITER}.json` is the
  primary artefact phase 2 parses.

**Phase 2 — `ltp-classify`** (the verdict bridge):

```
cmd = """
  python3 - <<'PY' /var/log/ltp/kirk-${ITER}.json
  import json, sys
  d = json.load(open(sys.argv[1]))
  # kirk's JSON report schema (libkirk/results.py): top-level "results"
  # array, each entry has "status" in {"pass","fail","broken","skipped",
  # "warning","conf"} and "test" {"name": ...}.
  c = {"pass":0,"fail":0,"broken":0,"skipped":0,"warning":0,"conf":0}
  for r in d.get("results", []):
      s = r.get("status","").lower()
      c[s] = c.get(s,0) + 1
  total = sum(c.values())
  fail  = c["fail"] + c["broken"]
  print(f"LTP_TOTAL={total} LTP_PASS={c['pass']} LTP_FAIL={fail} "
        f"LTP_SKIP={c['skipped']+c['conf']} LTP_WARN={c['warning']}")
  # Verdict rule per design memo §4.3: PASS iff fail==0 AND total>0.
  if total == 0:
      print("LTP_VERDICT=NORESULTS"); sys.exit(2)
  if fail == 0:
      print("LTP_VERDICT=PASS"); sys.exit(0)
  print(f"LTP_VERDICT=FAIL count={fail}/{total}"); sys.exit(1)
  PY
  rc=$?
  if [ $rc -eq 0 ]; then echo REPRO_DONE rc=0; else echo REPRO_DONE rc=1; fi
  exit $rc
"""
timeout_secs = 120
pass_marker  = "REPRO_DONE rc=0"
fail_marker  = "REPRO_DONE rc=1"
```

The two-phase shape (run-then-classify rather than parse-as-you-go)
is deliberate: it keeps the kirk invocation simple and isolates the
verdict-parsing logic in a place where adding new counters (e.g.
"LTP_PASS_RATE_PCT") doesn't require touching the kirk command line.

### 4.2 Scoreboard row extension

The daemon's per-iteration scoreboard row (design memo §2.6) gets
LTP-specific additive fields when `workload == "tier-ltp"`:

```json
{
  "ts": "...",
  "gate": "phase-J-soak-tier-ltp",
  "backend": "kvm-v2",
  ...
  "verdict": "PASS",
  "ltp_total": 2043,
  "ltp_pass": 2031,
  "ltp_fail": 1,
  "ltp_skip": 11,
  "ltp_warn": 0,
  "ltp_kirk_json_relpath": "logs/tier-ltp-kvm-v2-152/kirk-152.json"
}
```

The verdict-to-`rc` mapping the umlctl init harness already expects:

| kirk classification         | Phase 2 exit | umlctl rc | Daemon `verdict` |
|-----------------------------|--------------|-----------|------------------|
| all tests pass, total > 0   | 0            | 0         | PASS             |
| ≥1 test fails or broken     | 1            | 1         | FAIL             |
| no tests ran (e.g. kirk crashed before launching suite) | 2 | 2 | HOST_ERROR |
| phase-1 timeout (suite-timeout > 50 min)                | — | timeout | TIMEOUT |
| guest kernel panic during LTP run                       | — | panic   | PANIC |

The `verdict=FAIL` row carries the `ltp_fail` count so a downstream
`gate diff` can compare seccomp-vs-v2 by *which tests* failed, not
just by count.

### 4.3 What the template does *not* do

- **It does not implement v2-vs-seccomp delta.** Per design memo
  §4.3, "acceptance is calibrated to LTP's known UML-incompat
  baseline … parity within ±1 %." The delta computation is the
  daemon's `summary.md` responsibility (a new "LTP delta" table
  appended whenever both backends have at least one LTP cycle in
  the rotation). The template just records the counters.
- **It does not auto-update the skip list.** A failing test on UML
  may be a v2 regression *or* a UML-environment problem; auto-
  pruning the latter would mask the former. The skip list grows
  via operator review of FAIL counts across rotations.
- **It does not parallelise across kirk workers.** Per §4.1 — the
  daemon owns parallelism.

## 5. Wall-clock budget + iteration cadence

### 5.1 Per-cycle wall-clock estimate

Counted contributions to a single LTP cycle on the curated suite
list (smoketest + syscalls + syscalls-ipc + sched + pty + mm + nptl
+ math + capability), with §3's skip-list applied (~150 tests
removed, ~2 050 remaining):

| Component | Count | Per-test budget | Subtotal |
|-----------|-------|-----------------|----------|
| Fast syscall tests (~80 % of curated `syscalls`) | ~1 400 | 0.2-0.5 s | 5-12 min |
| Medium syscall tests (mmap, mremap, fork, clone, futex) | ~250 | 1-2 s | 4-8 min |
| IPC stressors (msgctl, semop, shmat) | ~70 | 0.5-1 s | 1 min |
| Sched stressors (hackbench, pth_str02 -n1000) | ~22 | 2-15 s | 2-4 min |
| MM stressors (mtest01 -p80, mmstress, mmap1/2/3) | ~30 | 5-30 s | 4-12 min |
| pty + ptem | 18 | 0.5-1 s | 0.5 min |
| math, nptl, capability, smoketest | ~50 | 0.1-0.5 s | 1 min |
| **Kirk overhead** (suite setup, per-test exec fork) | per test | ~50 ms | ~2 min |
| **Total per LTP cycle** | ~2 050 | — | **~25-40 min** |

Add 30 % headroom for slower tail (cpython-soak baseline kernel
runs ~1.5× slower than vanilla x86 KVM under T55's perf regression
— same kernel runs LTP here): **35-50 min per cycle.**

Recommended hard budget (per §1): **45 min target, 60 min hard
kill.** The daemon-side `kirk` invocation's `--suite-timeout 50m`
gives kirk a 5-minute margin under the daemon's 60-min wall-clock
guillotine; the daemon then has 5-10 min to write the scoreboard
row and start cooldown before the next workload starts.

### 5.2 Rotation cadence

Per design memo §4.3: "LTP runs every Nth rotation, not every
rotation. … once per rotation-pass-number divisible by 8 (so
~once every 4-6 hours under default settings)."

Confirming the budget against the design memo's defaults:

- Default daemon rotation: 5 workloads × 2 backends × 10 iters × 2
  workers = 200 iterations per pass. At ~60 s per iteration that's
  ~3.3 hours per rotation pass (pre-cooldowns, pre-throttling).
- LTP-included rotation pass: above + 2 LTP cycles (one per backend)
  at ~45 min each = ~5 hours.
- With the every-8-rotations gating, LTP fires on rotations 0, 8,
  16, … Over a 24-hour soak that's **~4-6 LTP cycles per backend
  per 24 h**, depending on throttling and the perf-regression
  factor.

That's enough volume to compute a meaningful seccomp-vs-v2
headline ratio (~10 000 total LTP test instances per backend per
24 h soak), but few enough cycles that LTP doesn't dominate the
wall-clock budget.

### 5.3 What happens on first run

The seed skip-list in §3 is a hypothesis; first-run data tells us
which assumptions were wrong. Expected first-cycle output shape
**post skip-list, on a clean kvm-v2 kernel:**

- Total: ~2 050
- Passed: ~1 950-2 000
- Failed: 5-50 (these are the real signal; expect 5-15 from
  genuine UML/LTP mismatches that the seed didn't anticipate, plus
  0-2 from real v2 bugs if any have crept in)
- Skipped/Conf: ~50-100 (tests that detect environmental absence
  at runtime — e.g. "this test needs `/proc/sys/kernel/sched_*`
  which isn't present in UML")
- Warnings: ~5-10

Operator's job after cycle 1: triage the FAIL set, classify each
into (a) real v2 bug → file a SMP-T<n> issue, (b) UML-env mismatch
→ add to skip list, (c) LTP test bug → upstream report. Aim to
get the skip list to "FAIL count under 10" before declaring the
runner integrated; not for "FAIL count == 0," per design memo
§4.4.

### 5.4 Worst-case wall-clock blowout scenarios

| Scenario | Probability | Wall-clock impact | Mitigation |
|----------|-------------|-------------------|------------|
| Kirk wedges in a single LTP test (the `--exec-timeout 60 s` doesn't trigger somehow) | Low | +60 min per cycle | Daemon hard kill at template-level `timeout_secs = 3600` |
| Forgotten `reboot01/02` in skip list halts UML mid-cycle | Low (we did include them) | +cycle time + host restart | Skip-list maintenance discipline; primary risk is *new* LTP releases adding reboot-like tests we don't notice. |
| Per-test runtime drift under v2 (T55-class) makes "medium" tests slow | Medium (observed) | +20-40 % per cycle | Raise per-cycle budget to 60 min target, 75 min hard kill, accept fewer LTP cycles per soak. |
| Hostfs slows mtest01 / mmstress (paging through hostfs-backed temp) | Low-medium | +10 min on MM stressors | Mount `/var/log/ltp` as tmpfs inside the guest, not hostfs. |
| Kirk consumes >1 G RAM running 2 050 tests with rich JSON | Low | OOM in guest, FAIL cycle | Set `[runtime] mem = "2G"`; revisit if cycles OOM. |

## 6. Open questions

### 6.1 Does the UML reference build have `CONFIG_KEYS=y`?

The seed deny-list pre-emptively skips all 20 keyctl/add_key/
request_key tests. If `CONFIG_KEYS=y` is in the reference build,
maybe 15 of those would actually pass. Action: grep the build's
`.config` once and prune. Tracked as a TODO; not blocking.

### 6.2 Does kirk's JSON report schema match phase-2's parser?

Phase 2 in §4.1 assumes a top-level `results` array with `status`
strings. Kirk's source (`libkirk/results.py`, not read for this
memo) may differ. Verifying needs a one-shot kirk invocation on a
host with LTP installed, which neither this memo nor the daemon
should do. Action: validate during initial template wiring.

### 6.3 What's the seccomp baseline?

Phase J's gate is "v2 within ±1 % of seccomp on LTP." We need a
seccomp-baseline cycle before we can declare v2 anything. First
soak with the runner integrated should be a kvm-seccomp-only
calibration run (no v2 workload rows) to establish what the
"clean" LTP pass count looks like on this UML build. Action: run
calibration before declaring runner integrated.

### 6.4 Does LTP itself need a host build dependency we don't have?

LTP's `make autotools && ./configure && make` needs `bison`,
`flex`, `autoconf`, `automake`, `pkg-config`, GCC, kernel headers.
The design memo (§6.4) flagged this. On Ubuntu 26.04 Resolute,
verifying these are apt-installable is operator work; not
blocking memo completion.

### 6.5 Should we pin a specific LTP release tag, not HEAD?

This memo references HEAD `8cd7644a52e3`. For reproducibility,
the daemon's `config.json` should record the LTP commit hash the
host has installed; the rig README should suggest pinning to a
named release tag (most recent: `20260130` — same as the VERSION
file). Action: document in rig README.

### 6.6 Open-POSIX-testsuite — separate phase or fold into LTP runner?

`testcases/open_posix_testsuite/` is its own ~1 700-test suite
that LTP packages but kirk treats as a separate suite name. Out
of scope for Phase J initial integration; revisit Phase J + 1 or
as part of POSIX-conformance hardening.

### 6.7 How does kirk's per-test timeout interact with UML's wall-clock?

`--exec-timeout 60s` is wall-clock from kirk's perspective; under
T55 (1.5-2× slower than seccomp for some workloads), tests with
"normal" baseline of ~30 s may hit the 60 s limit on v2 even when
they're functionally correct. Mitigation options: (a) raise to 120 s
universally; (b) raise only on workloads where T55 dominates
(syscalls/mm); (c) leave at 60 s and accept ~1 % spurious TIMEOUT
that the v2-vs-seccomp delta will surface. Recommend (a) initially;
revisit after first calibration cycle.

### 6.8 Should containers/userns subset be promoted to KEEP?

`testcases/kernel/containers/pidns/` and `mqns/` and `userns/` may
work in UML — they don't need network namespaces with bridges, just
user/PID/MQ namespace isolation. Currently in §2.1 they're SKIP at
the whole-suite level because kirk's --run-suite is whole-or-nothing.
A finer cut would either (a) write per-test KEEP entries in a positive
allow-list (kirk doesn't support this natively) or (b) split the
`containers` runtest file into a `containers-no-net` variant and a
`containers-net` variant. Option (b) is upstream work; option (a)
requires us to maintain a kirk patch. Defer; record as TRIAGE-LATER.

## 7. References

### Memos
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md` §4 — original LTP curation framework + the design questions this memo answers.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-pilot-2026-05-05.md` — pilot template shape and pass-marker conventions this memo reuses for the LTP runner.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-soak-2h-2026-05-14.md` — preceding 2h soak results; LTP runner is the next integration step on top.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/23-smp-t55-perf-regression-plan.md` — T55 perf-regression context informing §5's wall-clock estimate.
- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/state-audit/24-smp-t57-vmmethod-bisect.md` — T57 stress-ng SIGILL; reason for §3's caution on LTP `crashme` suite.

### Upstream
- `https://github.com/linux-test-project/ltp` HEAD `8cd7644a52e3` (2026-05-13). VERSION file: `20260130`.
- `https://github.com/linux-test-project/kirk` HEAD `07964a106584` — the runner that replaced `runltp`. Stock CLI: `kirk --run-suite <names> --skip-file <path> --sut host --json-report <path>`.
- `https://kirk.readthedocs.io/` — kirk usage docs (referenced by `runltp` redirect; not crawled for this memo).

### Code paths in this tree
- `tools/testing/selftests/um/soak/cpython-soak.toml.template` — pattern this memo's `tier-ltp.toml.template` follows.
- `tools/testing/selftests/um/soak/run-pilot.sh` — current driver; the LTP integration is via the daemon (`run-soak-daemon.sh`) that supersedes it, per design memo §2.
- `tools/uml/uml-launcher/src/bin/umlctl/main.rs` — `umlctl gate loop` invocation that the daemon calls per LTP iteration.

### Host-state findings (verified during this memo's drafting, 2026-05-14)
- Ubuntu 26.04 LTS Resolute on this host; no LTP-related apt package available.
- `git clone --depth 1 https://github.com/linux-test-project/ltp` succeeded; tree size ~250 MB, ~2 900 test entries.
- `runltp` in upstream is a stub that exits 1 with a kirk redirect message.
- Kirk's `--skip-file`, `--run-suite`, `--json-report`, `--exec-timeout`, `--suite-timeout`, `--sut`, `--workers` flags verified from `libkirk/main.py` argparse (see lines 425-548 in upstream `kirk` HEAD).

These pre-conditions feed the operator runbook the rig README needs
to grow alongside the LTP integration.
