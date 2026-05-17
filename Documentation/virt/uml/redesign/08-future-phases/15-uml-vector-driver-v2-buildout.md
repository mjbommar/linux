# uml-vector-driver-v2 buildout plan

**Status:** BUILDOUT PLAN - vector v2 is not swap-ready.
**Date:** 2026-05-17.
**Owner:** future UML networking workstream.

This memo answers the practical question: what remains before the
vector v2 scaffolding becomes a real driver that can replace the
current `CONFIG_UML_NET_VECTOR` implementation?

The answer is: a full runtime driver still has to be built.  The
current v2 code has advanced past scaffolding into an experimental
netdev driver, but the replacement gates are not closed.

## Current State

The following v2 foundations exist:

- `arch/um/drivers/vector2_config.{c,h}` plus KUnit tests.
- `arch/um/drivers/vector2_queue.{c,h}` plus KUnit tests.
- `arch/um/drivers/vector2_model.{c,h}` plus KUnit tests.
- `arch/um/drivers/vector2_host.h`.
- `arch/um/drivers/vector2_fake_host.{c,h}` plus KUnit tests.
- `arch/um/drivers/vector2_transport.{c,h}` plus KUnit tests.
- `Documentation/virt/uml/redesign/08-future-phases/models/vector2/`
  starter TLA+ models.
- `Documentation/virt/uml/redesign/08-future-phases/14-uml-vector-driver-v2.md`
  architecture memo.

Those pieces prove parser policy, queue ownership, transport header
bounds checks, fake-host behavior, and lifecycle transitions.

R1 through R8 plus the fd datapath follow-up have now added runtime
attachments, packet movement, `umlctl` selection, multiqueue TAP
shape, and inspectable ethtool surfaces:

- `CONFIG_UML_NET_VECTOR_V2`, default `n`;
- v2-only command-line collection through `vec2.<n>:` and `vec2=`;
- late-init typed config validation;
- internal runtime device/channel/queue ownership structs;
- v2 `struct net_device_ops`;
- netdev queue count driven by parsed `queues=N` for v2 TAP;
- `register_netdevice()` for inspectable `vec2.<unit>` netdevs;
- read-only `ethtool -i`;
- `ndo_open()` failure unwind to `REGISTERED` with `-EOPNOTSUPP`;
- trusted direct-fd duplication and close unwind;
- `ip link set vec2.0 up/down` success for
  `CONFIG_UML_NET_VECTOR_V2_INPROC=y` plus
  `vec2.0:transport=fd,fd=<n>`;
- single-queue trusted direct-fd packet movement over raw Ethernet
  frames on an inherited datagram fd;
- sandbox builds accept inherited `transport=fd,fd=<n>` without
  enabling trusted in-process host operations;
- trusted TAP open through `/dev/net/tun`, `TUNSETIFF`, and explicit
  close unwind;
- `ip link set vec2.0 up/down` success for
  `CONFIG_UML_NET_VECTOR_V2_INPROC=y` plus
  `vec2.0:transport=tap,ifname=<tap>`;
- single-queue trusted TAP TX/RX through v2 queue ownership;
- NAPI/read-IRQ integration for trusted TAP;
- write-IRQ wakeup for TAP TX backpressure;
  - ethtool stats, ring reporting, stopped-only ring resizing, and
    coalesce policy reporting;
  - dynamic per-queue ethtool stats for v2 queue distribution
    observability;
- trusted TAP ping smoke with repeated up/ping/down loops;
- `umlctl` and soak-daemon selection for experimental v2 TAP through
  `network.driver = "vector2"`;
- `umlctl` selection of experimental v2 TAP queue count through
  `[network] queues` and `--network-queues`;
- live Tier 3 Django stdlib-shim success on seccomp through v2 TAP;
- live Tier 3 Django stdlib-shim success on seccomp through v2
  `queues=2` TAP;
- TAP teardown hardening after successful loops and failed starts.

Those pieces attach v2 to the Linux networking stack for inspection.
They still do not provide replacement-ready networking.

Missing runtime pieces:

- no launcher-manifest fd path for convenient sandbox fd ownership;
- no real timer-driven coalescing;
- no feature negotiation;
- no 30/30 Tier 3 workload proof on both seccomp and kvm-v2;
- no repeated long soak loop;
- no full multiqueue validation story: fd multiqueue, queue-to-CPU
  policy, KCSAN, and broader fairness/performance profiles remain
  open;
- no compatibility switch from old `vecN:` to v2.

Therefore vector v2 must run as an experimental parallel driver first.
It must not silently replace the old driver until the gates below pass.

## Strategic Direction

Build vector v2 as a real driver in parallel under a distinct runtime
surface, then replace the legacy implementation only after measured
correctness and performance evidence exists.

Recommended development surface:

```text
CONFIG_UML_NET_VECTOR=y          # old driver, unchanged during bring-up
CONFIG_UML_NET_VECTOR_V2=y       # new experimental runtime driver

vec0:...                         # old driver syntax, production path
vec2.0:... or vec2=0,...         # v2 development path
```

The exact boot syntax can be refined, but the boundary matters:
developers must be able to boot old and new vector drivers from the
same kernel and compare them without changing unrelated infrastructure.

The short-term Tier 3 blocker in `vector_net_open()` should be fixed in
the legacy driver as a separate patch.  That unblocks validation.  It is
not a substitute for the v2 rewrite, and the v2 rewrite should not be
rushed into production to fix one legacy NULL dereference.

## Definition Of Done

Vector v2 is eligible to replace the old driver only when all of these
are true:

1. `CONFIG_UML_NET_VECTOR_V2=y` builds a live netdev driver without
   requiring any KUnit-only options.
2. A v2 command-line spec registers a netdev with stable names.
3. `ip link set <v2dev> up` reaches `RUNNING` through the v2 lifecycle
   model.
4. `ip link set <v2dev> down` reaches `REGISTERED` and can repeat at
   least 10,000 times under failure injection.
5. Single-queue TAP works in trusted mode.
6. Single-queue fd transport works with launcher-supplied fds.
7. TX and RX move packets through v2 queues, not legacy
   `struct vector_queue`.
8. `ping`, TCP loopback, and the Tier 3 Django/FastAPI loopback smoke
   pass on both `backend=force=seccomp` and `backend=force=kvm-v2`.
9. KUnit covers config, lifecycle, queue ownership, fake host,
   transport headers, host-open failure injection, and open/close
   unwind.
10. ethtool stats and ring queries work while stopped and running.
11. No host helper command, raw socket, TAP open, or BPF load can occur
    in sandbox mode.
12. A multiqueue TAP/fd path passes KCSAN and shows per-queue counter
    distribution.
13. Performance is no worse than the old vector driver for the
    accepted replacement scope, or the regression is explicitly
    documented and accepted.
14. The old `vecN:` compatibility path either maps to v2 or has a
    documented transition period.
15. The patch series remains reviewable and bisectable.

## Non-Negotiable Design Rules

- Keep `um_vec2_*` symbols until the replacement series is ready.
- Keep one concept per file: config, core, host ops, transport ops,
  queues, NAPI, ethtool, tests.
- Every runtime resource transition must go through the lifecycle model
  or through a documented wrapper that updates it.
- Do not use legacy string-token parser output after v2 parse.
- Do not cast packet headers into typed structs.  Use the v2
  bounds-checked transport helpers.
- Do not let sandbox mode compile in trusted host operations.
- Do not make multiqueue lockless until the locked per-queue model has
  tests, KCSAN, and performance data.
- Do not treat old-driver bug fixes as evidence that v2 is complete.

## Runtime File Plan

The existing foundation files should remain.  Add runtime files in
small reviewable steps:

```text
arch/um/drivers/
+-- vector2_internal.h       # private runtime structs and invariants
+-- vector2_cmdline.c        # vec2 command-line collection
+-- vector2_core.c           # platform/netdev registration
+-- vector2_netdev.c         # net_device_ops implementation
+-- vector2_napi.c           # NAPI poll, IRQ arming, coalescing
+-- vector2_ethtool.c        # stats, ring params, feature reporting
+-- vector2_host_fd.c        # pre-opened fd backend
+-- vector2_host_tap.c       # trusted in-process TAP backend
+-- vector2_user.c           # UML host syscall wrappers, if needed
+-- vector2_transport_tap.c  # no extra overlay, vnet header policy
+-- vector2_transport_raw.c  # trusted raw backend integration
+-- vector2_transport_gre.c  # GRE ops around safe helpers
+-- vector2_transport_l2tpv3.c
`-- vector2_uapi.rst         # command-line contract
```

`vector2_user.c` should exist only for host syscalls that cannot live
cleanly in kernel-side UML code.  It must not become a second copy of
the old all-in-one `vector_user.c`.

## Implementation Phases

### V2-R0 - Current Driver Freeze And Reproducer

Goal: preserve behavior while v2 is built.

Deliverables:

- Reproducer doc for the legacy TAP NULL dereference:
  `vec0:transport=tap,ifname=soak-tap0,depth=128` plus
  `ip link set vec0 up`.
- Minimal legacy fix for the current `vector_net_open()` crash.
- Smoke script that proves the crash is fixed on the old driver.
- Baseline throughput and syscall-count measurements for old TAP/fd.

Validation:

- legacy TAP Tier 3 smoke reaches `SERVER_READY`;
- legacy `vec0` open/close repeat test passes;
- no v2 files are used to claim this result.

Why this phase exists: it keeps Phase J moving and gives v2 a known
behavioral target.

R0 implementation note:

- `16-uml-vector-legacy-tap-crash-r0.md` records the
  `vec0:transport=tap,ifname=soak-tap0,depth=128` reproducer, the
  queue-optional legacy-driver fix, and the manual validation commands.
- This R0 fix is legacy-driver containment only.  It does not satisfy
  any v2 runtime-driver replacement gate.

### V2-R1 - Runtime Kconfig And Command-Line Skeleton

Goal: make v2 visible as a real, experimental runtime build target.

Deliverables:

- `CONFIG_UML_NET_VECTOR_V2` visible, default `n`, marked
  experimental.
- `CONFIG_UML_NET_VECTOR_V2_INPROC` for trusted host operations.
- `CONFIG_UML_NET_VECTOR_V2_SANDBOX` or equivalent policy gate.
- `vector2_cmdline.c` collects v2 device specs without touching the old
  `vecN:` parser.
- `vector2_core.c` has a late-init registration hook.
- `vector2_internal.h` defines `struct um_vec2_dev`,
  `struct um_vec2_channel`, and `struct um_vec2_queue_pair`.

Validation:

- kernel builds with old driver only, v2 only, both, and neither;
- `vec2` malformed specs fail with parser diagnostics, not partial
  devices;
- booting with no `vec2` specs is a no-op.

Exit gate:

- `grep -R "net_device_ops" arch/um/drivers/vector2_*` finds the v2
  table once R2 lands, not before.

R1 implementation note:

- `17-uml-vector-driver-v2-r1-runtime-skeleton.md` records the
  runtime Kconfig, v2-only command-line collection, late-init typed
  config validation, internal runtime ownership structs, manual boot
  checks, build matrix, and 48-test KUnit result.
- R1 is intentionally not a netdev driver.  It has no
  `net_device_ops`, no `register_netdevice()` path, no `ndo_open()`,
  and no packet movement.

### V2-R2 - Netdev Registration With Stub Data Path

Goal: register a v2 netdev that can be inspected but does not yet move
packets.

Deliverables:

- `alloc_etherdev_mqs()` call with `queues=1` forced initially.
- `static const struct net_device_ops um_vec2_netdev_ops`.
- `register_netdevice()` path wired from parsed v2 command line.
- `ndo_open` and `ndo_stop` that transition state but return
  `-EOPNOTSUPP` until a host backend is selected.
- read-only ethtool driver info.
- device unregister cleanup.

Validation:

- boot with `vec2.0:transport=fd,...` creates a visible netdev;
- `ip link show` works;
- `ip link set up` fails cleanly if backend is not implemented;
- repeated register/unregister under failure injection leaks nothing.

Exit gate:

- no v2 packet movement yet, but all netdev lifetime objects are owned
  and freed by v2 code.

R2 implementation note:

- `18-uml-vector-driver-v2-r2-netdev-skeleton.md` records the v2
  `net_device_ops`, forced single-queue `alloc_etherdev_mqs()`,
  `register_netdevice()` path, read-only ethtool driver info, open
  failure unwind, manual runtime check, build matrix, and 52-test
  KUnit result.
- R2 is intentionally inspectable only.  `ip link set vec2.0 up`
  reaches the v2 `ndo_open()` method and fails cleanly with
  `-EOPNOTSUPP` because no fd or TAP backend exists yet.

### V2-R3 - fd Host Backend First

Goal: support the lowest-privilege data source before trusted TAP.

Deliverables:

- `vector2_host_fd.c` consumes launcher-supplied RX/TX fds.
- fd ownership is explicit: inherited by UML, duplicated, or borrowed
  from launcher policy, never ambiguous.
- `fd` transport validation distinguishes "trusted raw fd number on
  command line" from "launcher-supplied manifest fd".
- fake-host tests are extended to the same ops contract used by fd.
- open/close failure injection at every fd attach step.

Validation:

- a socketpair-based manual test can bring `vec2` up without root;
- fd death returns `-ENODEV` and moves channel to `QUIESCING`;
- no `/dev/net/tun`, raw socket, helper execution, or BPF load is
  reachable in this backend.

Exit gate:

- `ip link set vec2 up/down` works with fds and no packets.

R3 implementation note:

- `19-uml-vector-driver-v2-r3-fd-backend.md` records the trusted
  direct-fd backend, fd duplication ownership rule, channel lifecycle
  attach/close, `ndo_open()` success for fd mode, sandbox rejection of
  direct `fd=`, manual runtime checks, build matrix, and 55-test KUnit
  result.
- R3 still has no packet movement.  The fd-backed netdev opens with
  `NO-CARRIER` and stopped queues until the datapath phase.

### V2-R4 - Trusted TAP Backend

Goal: provide the trusted TAP open/close runtime path.

Deliverables:

- `vector2_host_tap.c` opens or attaches to a TAP device only when
  trusted in-process mode is enabled.
- TAP open requests `IFF_TAP | IFF_NO_PI | IFF_VNET_HDR` and applies
  best-effort checksum/TSO offload setup.  Full feature reporting is
  deferred to R6.
- TAP fd close/unwind follows channel state.
- TAP host options (`ifname`, helper commands, BPF file) are policy
  gated.

Validation:

- `vec2.0:transport=tap,mode=inproc,ifname=soak-tap0,depth=128`
  opens and closes repeatedly;
- sandbox build rejects the same spec before host fd creation;
- `strace` shows TAP open only in trusted mode.

Exit gate:

- `ip link set vec2 up` reaches `RUNNING` on TAP without packet
  movement.

R4 implementation note:

- `20-uml-vector-driver-v2-r4-tap-backend.md` records the trusted TAP
  open/close backend, TAP fd ownership rule, backend-specific close
  routing, `ndo_open()` success for TAP mode, sandbox rejection of
  direct `ifname=`, `strace` evidence that sandbox mode does not open
  `/dev/net/tun`, manual runtime checks, build evidence, and 59-test
  KUnit result.
- R4 still has no packet movement.  The TAP-backed netdev opens with
  `NO-CARRIER` and stopped queues until the datapath phase.

### V2-R5 - Single-Queue RX/TX Data Path

Goal: move packets through v2 queues.

Deliverables:

- `ndo_start_xmit()` maps an skb to a v2 TX descriptor.
- TX uses v2 ring ownership and completes skb ownership exactly once.
- RX uses v2 batch ownership and delivers packets through NAPI.
- `sendmsg`/`sendmmsg` and `recvmsg`/`recvmmsg` assembly is separated
  from queue ownership.
- BQL accounting is correct on success, partial send, drop, and hard
  error.
- NAPI poll never touches a queue that is absent for the selected mode.

Validation:

- KUnit fake-host tests cover partial TX, EAGAIN, ENOBUFS, fd death,
  short RX, allocation failure, and reset;
- manual TAP `ping` works;
- guest-to-host and host-to-guest TCP smoke works;
- KASAN/KFENCE clean if available;
- 10,000 open/close cycles pass.

Exit gate:

- Tier 3 stdlib HTTP smoke reaches `SERVER_READY` with v2 TAP on both
  seccomp and kvm-v2.

R5 implementation note:

- `21-uml-vector-driver-v2-r5-tap-datapath.md` records the first
  trusted TAP packet path: queue allocation, nonblocking TAP fd,
  channel-owned NAPI/read IRQ, `ndo_start_xmit()` enqueue, TAP TX/RX
  host ops, vnet-header normalization, KUnit TX/RX pipe-backed tests,
  and a guest-to-host ping smoke with 3/3 replies.
- R5 was single-queue trusted TAP only.  Later checkpoints add fd
  datapath, ethtool, `umlctl`, and TAP multiqueue pieces.  Sandbox
  helper/proxy, fd multiqueue, performance, KCSAN, kvm-v2, and full
  Tier 3 replacement gates remain open.

R5 fd follow-up note:

- `27-uml-vector-driver-v2-fd-datapath.md` records the first trusted
  direct-fd packet path.
- Implemented:
  - shared runtime queue-pair allocation/free helpers used by TAP and
    fd;
  - per-channel `rx_fd` / `tx_fd` ownership so netdev datapath startup
    is backend-neutral;
  - single-queue fd TX over raw Ethernet frames from the v2 TX ring;
  - single-queue fd RX into the v2 RX batch;
  - nonblocking duplicate-fd handling;
  - NAPI/read-IRQ/write-IRQ startup for fd channels;
  - KUnit coverage for fd TX and RX packet movement.
- Evidence collected:
  - targeted object build for `vector2_runtime.o`,
    `vector2_host_fd.o`, `vector2_host_tap.o`,
    `vector2_netdev.o`, and fd/TAP host tests;
  - rebuilt runtime and KUnit UML kernels;
  - `um_vector2_*` KUnit: 68/68 passed;
  - no-root manual fd datapath smoke using an inherited UNIX datagram
    fd, a host ARP/ICMP responder, `ping -c 3`, and ethtool queue
    counters showing both TX and RX movement.
  - sandbox-only fd datapath smoke with
    `# CONFIG_UML_NET_VECTOR_V2_INPROC is not set`, inherited
    `transport=fd,fd=<n>`, 3/3 ping success, queue0 TX/RX counters,
    and no config rejection.
- Therefore direct-fd packet movement exists for the trusted
  single-queue development path and for inherited-fd sandbox builds.
  Launcher-owned fd manifests, fd multiqueue, fd performance profiles,
  and kvm-v2 fd evidence remain open.

### V2-R6 - ethtool, Stats, And Feature Policy

Goal: make v2 inspectable, safe while stopped, and harder to wedge.

Deliverables:

- ethtool driver info, ring params, coalesce params, and stats.
- stopped-state ethtool queries never dereference runtime-only queues.
- per-queue stats fold into device stats.
- feature changes that alter buffer shape require stopped state or an
  explicit quiesce/reopen path.
- write-side `-EAGAIN` handling cannot leave a full TX ring stuck.

Validation:

- ethtool queries pass before open, while running, and after close;
- stats read during traffic does not race under KCSAN;
- feature toggles fail closed when unsafe.

Exit gate:

- v2 is operational enough for routine debugging.

R6 implementation note:

- `22-uml-vector-driver-v2-r6-ethtool-hardening.md` records the first
  ethtool hardening checkpoint: explicit v2 runtime counters,
  stopped-safe stats sampling, queue-counter locking, ring reporting,
  stopped-only ring resizing, coalesce policy reporting, TAP write IRQ
  wakeup, KUnit ethtool coverage, repeated trusted TAP up/ping/down
  evidence, and sandbox rejection evidence.
- R6 is still single-queue trusted TAP only.  It does not satisfy fd
  datapath, sandbox helper/proxy, host-to-guest TCP, Tier 3 soak,
  multiqueue, real coalescing, or performance gates.

### V2-R7 - Tier 3 And Soak Eligibility

Goal: prove the single-queue TAP/fd implementation under the workload
that exposed the legacy problem.

Deliverables:

- `umlctl` or soak-template support for selecting v2 networking.
- Tier 3 Django and FastAPI template variants for v2.
- crash scraper records v2 netdev name, backend, transport, queue
  count, and host mode.

Validation:

- Tier 3 Django stdlib shim passes 30/30 on seccomp and kvm-v2;
- Tier 3 FastAPI or uvicorn variant passes if dependencies are present;
- 2h soak includes v2 Tier 3 without panic, OOM, or stuck TAP teardown.

Exit gate:

- v2 can be used as an experimental Tier 3 path, but legacy is still
  the production `vecN:` path.

R7 implementation note:

- `23-uml-vector-driver-v2-r7-umlctl-tier3-integration.md` records
  the first operator-facing integration checkpoint.
- Implemented:
  - `[network] driver = "vector2"` in Umlfiles;
  - `umlctl up --network-driver vector2`;
  - `umlctl up --dry-run` network-plan output;
  - `umlctl gate loop --network-driver vector2`;
  - `umlctl gate loop --sweep network.driver=vector,vector2`;
  - soak aliases `tier3-django-v2` and `tier3-fastapi-v2`;
  - v2 metadata in soak scoreboard rows;
  - `gate loop` cleanup through `down --force --rm`;
  - `gate loop --timeout` propagation to inner
    `umlctl up --ready-timeout`;
  - `down --force` host teardown after not-running or missing runtime
    state;
  - ready-timeout child reaping in `supervise::start()`.
- R7 follow-up usability:
  - generated init scripts export reserved `UMLCTL_NETWORK_*`
    metadata, including the selected driver and guest netdev;
  - workload phases can use `$UMLCTL_NETDEV` instead of hard-coding
    `vec0` or `vec2.0`;
  - ready-timeout errors print `pid`, `run_id`, and `init_log`;
  - `gate loop` failed-start logs include the `umlctl up` output and
    the failed bundle's `init.log` when available.
- Evidence collected:
  - `cargo test` for `uml-launcher`;
  - `kunit.py parse` over `um_vector2_*`: 64/64 passed;
  - seccomp v2 Tier 3 stdlib smoke: 1/1 pass through
    `--sweep network.driver=vector2`;
  - seccomp repeated cleanup smoke: 2/2 pass, TAP absent after loop;
  - kvm-v2 Tier 3 smoke still fails `umlctl up` readiness after the
    full 180-second budget, but TAP is absent after failure cleanup;
  - kvm-v2 no-network isolation also fails readiness before the Linux
    boot banner while the same kernel boots under seccomp, so the
    current kvm-v2 R7 blocker is not vector2-specific.
- Therefore R7 is partially satisfied.  Selection, observability, and
  teardown safety landed; the full 30/30 seccomp+kvm-v2 and long-soak
  eligibility gates remain open, and kvm-v2 baseline readiness must be
  fixed before vector2-specific kvm-v2 datapath claims are meaningful.

### V2-R8 - Multiqueue

Goal: scale without changing ownership semantics.

Deliverables:

- `alloc_etherdev_mqs()` uses parsed `queues=N`.
- one queue pair per queue;
- one NAPI instance per RX queue;
- one fd or fd pair per queue for fd/TAP where supported;
- per-queue IRQ registration and teardown;
- queue-to-CPU mapping and stats.

Validation:

- `queues=2` and `queues=num_online_cpus()` pass ping/TCP/Tier 3;
- KCSAN clean under parallel traffic;
- queue counters show distribution;
- no global driver lock in steady-state TX/RX profiles.

Exit gate:

- v2 has the SMP shape required for the long-term replacement.

R8a implementation note:

- `25-uml-vector-driver-v2-r8a-tap-multiqueue.md` records the first
  trusted TAP multiqueue checkpoint.
- Implemented:
  - v2 netdev registration uses parsed `queues=N`;
  - trusted TAP opens one channel, queue pair, TAP fd, NAPI instance,
    read IRQ, and write IRQ per configured queue;
  - TAP fds use `IFF_MULTI_QUEUE` when `queues > 1`;
  - TX maps `skb_get_queue_mapping()` to a v2 channel and uses subqueue
    stop/wake for backpressure;
  - ethtool queue counters aggregate across open channels;
  - `umlctl` exposes `[network] queues`, `--network-queues`, and
    `--sweep network.queues=...`;
  - `umlctl` host setup and teardown use matching `multi_queue` TAP
    flags for vector2 multiqueue.
- Evidence collected:
  - targeted vector2 object build;
  - rebuilt runtime and KUnit UML kernels;
  - `um_vector2_*` KUnit: 66/66 passed;
  - `cargo test` for `uml-launcher`;
  - direct `queues=2` TAP ping smoke: 3/3 ping, qdisc `mq`,
    `MQ_OK`, `PASS=1/1`;
  - `umlctl --network-queues 2` Tier 3 Django seccomp smoke:
    `SERVER_READY`, `TIER3_OK`, `REPRO_DONE rc=0`,
    `PASS=1/1`, TAP absent after teardown.
- Therefore R8 is partially satisfied.  TAP has the first real
  multiqueue runtime shape; fd multiqueue, KCSAN, queue-to-CPU
  policy, and kvm-v2 evidence remain open.

R8b implementation note:

- `26-uml-vector-driver-v2-r8b-queue-observability.md` records the
  per-queue ethtool statistics checkpoint.
- Implemented:
  - dynamic `ethtool -S` stat count based on configured, registered,
    and open queue counts;
  - stable `queue<N>_<counter>` stat names for TX ring and RX batch
    counters;
  - stopped devices report zero-valued per-queue counters for their
    configured queue count.
- Evidence collected:
  - rebuilt KUnit UML kernel;
  - `um_vector2_*` KUnit: 66/66 passed;
  - live `queues=2` seccomp smoke with `ethtool -S vec2.0`
    showing `queue0_*` and `queue1_*` stats, 3/3 ping success,
    `QUEUE_STATS_OK`, `PASS=1/1`, and TAP absent after teardown.
  - live parallel traffic smoke with `queues=2` where both queue0 and
    queue1 TX/RX counters were non-zero, `DISTRIBUTION_DONE`,
    `PASS=1/1`, and TAP absent after teardown.
- Therefore the R8 observability surface is usable for distribution
  experiments and has first TAP distribution evidence, but KCSAN,
  fairness/performance profiles, fd multiqueue, queue-to-CPU policy,
  and kvm-v2 evidence remain open.

### V2-R9 - Transport Parity

Goal: move non-TAP transports onto safe v2 transport ops.

Deliverables:

- raw trusted transport;
- GRE transport around `vector2_transport` helpers;
- L2TPv3 transport around `vector2_transport` helpers;
- hybrid and BESS decisions: implement, defer, or explicitly drop.
- transport-specific feature flags and validation.

Validation:

- old happy-path examples pass or fail with documented migration
  errors;
- short header and mismatch cases remain covered by KUnit/fuzz;
- transport state is immutable while running.

Exit gate:

- maintainers agree the remaining compatibility gaps are acceptable.

### V2-R10 - Sandbox Integration

Goal: enforce the UML v2 security boundary.

Deliverables:

- sandbox profile disables in-process TAP/raw/GRE/L2TP host creation;
- fd-only path accepts launcher-owned fds;
- optional proxy/helper path only if it beats virtio-net on a measured
  requirement;
- guest-triggered helper execution and BPF loading are unavailable in
  sandbox builds.

Validation:

- `strace` of the UML process in sandbox mode shows no `/dev/net/tun`,
  raw socket, helper execution, or BPF file load;
- helper or launcher owns privileged fds;
- policy rejection messages name the option and profile.

Exit gate:

- vector v2 is safe to document next to UML v2 sandboxing without
  creating privilege confusion.

### V2-R11 - Replacement And Legacy Removal

Goal: make v2 the implementation behind `CONFIG_UML_NET_VECTOR`.

Deliverables:

- compatibility mode maps old `vecN:` specs to v2 typed config;
- `CONFIG_UML_NET_VECTOR_LEGACY` remains available for one transition
  cycle if maintainers want it;
- docs update `Documentation/virt/uml/user_mode_linux_howto_v2.rst`;
- old driver files are deleted or archived behind `BROKEN`;
- patch series describes migration and performance evidence.

Validation:

- old command-line examples work or fail with documented diagnostics;
- CI matrix passes;
- Tier 1/2/3, LTP selected subset, and 24h soak pass;
- performance baseline accepted.

Exit gate:

- old `vector_kern.c` and `vector_user.c` no longer own the production
  vector networking path.

## Test Matrix

Required KUnit suites:

- config parser;
- lifecycle state model;
- TX/RX queue ownership;
- fake host;
- fd host;
- TAP host policy;
- transport headers;
- netdev open/close failure injection;
- ethtool stopped/running behavior;
- multiqueue queue selection and stats.

Required manual tests:

```text
backend=force=seccomp  transport=fd   queues=1  ip link up/down
backend=force=kvm-v2   transport=fd   queues=1  ip link up/down
backend=force=seccomp  transport=tap  queues=1  ping + TCP
backend=force=kvm-v2   transport=tap  queues=1  ping + TCP
backend=force=seccomp  transport=tap  queues=1  Tier 3 Django
backend=force=kvm-v2   transport=tap  queues=1  Tier 3 Django
backend=force=seccomp  transport=tap  queues=2  parallel TCP
backend=force=kvm-v2   transport=tap  queues=2  parallel TCP
```

Required static and dynamic checks:

- `git diff --check`;
- `scripts/checkpatch.pl --strict --file` for every new file;
- GCC UML build;
- Clang UML build before replacement;
- sparse before replacement;
- KCSAN on multiqueue;
- KASAN/KFENCE where UML build support allows it;
- `strace` sandbox audit;
- perf or tracepoint profile for single-queue and multiqueue TAP.

## Performance Gates

Record old driver and v2 numbers on the same host, same kernel config,
same TAP setup:

- TCP throughput guest to host;
- TCP throughput host to guest;
- UDP packet rate;
- CPU cycles per packet if perf data is practical;
- syscall batch sizes;
- NAPI poll budget utilization;
- TX partial-send rate;
- RX allocation failure rate under pressure.

Replacement is blocked if v2 has an unexplained large regression in
the accepted replacement scope.  A regression can be accepted only if
the safety/security benefit is documented and the old driver remains
available for one transition cycle.

## First Runtime Patch Series

The next concrete work should be:

1. **Legacy crash fix and baseline.**
   Fix the existing TAP NULL dereference, add a repro note, and collect
   old-driver TAP/fd smoke and performance baseline.

2. **v2 runtime skeleton.**
   Add `CONFIG_UML_NET_VECTOR_V2`, `vector2_internal.h`,
   `vector2_cmdline.c`, and `vector2_core.c`.  Register an inspectable
   netdev under a v2-only command-line syntax.  No packets yet.
   R1 and R2 have implemented this in two bisectable steps.

3. **fd backend plus open/close.**
   Wire `vector2_host_fd.c` to real fds, implement `ndo_open` and
   `ndo_stop` through the lifecycle model, and pass repeated open/close
   failure injection.

4. **TAP backend plus open/close.**
   Wire `vector2_host_tap.c` to trusted TAP fd creation, keep sandbox
   builds fail-closed before `/dev/net/tun`, and validate repeated
   `ip link set vec2.0 up/down` without packet movement.

5. **Single-queue TAP packet path.**
   Wire TAP TX/RX through v2 queues, add NAPI/read-IRQ ownership, and
   prove guest-to-host ping before claiming any soak or replacement
   readiness.

6. **Ethtool and backpressure hardening.**
   Add stopped-safe stats, ring reporting, stopped-only ring resizing,
   coalesce policy reporting, and TAP write-side wakeups before trying
   Tier 3 workloads.

Those first seven series have now taken v2 from parked scaffolding to an
experimental inspectable netdev with trusted fd and TAP packet paths,
ethtool observability, and TAP write-side wakeups, plus an
operator-facing `umlctl` selection path for Tier 3 experiments.  The
next work is KVM-v2 Tier 3 readiness, host-to-guest TCP validation,
long-soak proof, launcher-owned fd manifests, sandbox helper plumbing,
and deeper multiqueue validation.

## Workstream Exit Summary

The long-term path is not "patch the scaffold until it happens to
work."  It is:

1. keep the old driver alive long enough to unblock validation;
2. build a separate v2 runtime driver under explicit experimental
   Kconfig and command-line surfaces;
3. prove fd and TAP single-queue correctness;
4. scale to multiqueue;
5. enforce sandbox policy;
6. replace the old driver only after compatibility, soak, and
   performance gates pass.

That is the right long-term shape because it lets us use the existing
foundation work without converting a test scaffold into production code
by accident.
