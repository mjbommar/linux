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
- `umlctl` `network.host_mode` / `--network-host-mode` selection;
- launcher-owned vector2 single-queue TAP fd handoff through manifest
  labels and inherited fd 200;
- live `umlctl up` single-queue vector2 fd-handoff smoke with
  launcher-owned TAP inherited as fd 200, successful gateway ping,
  ethtool queue counters, and clean TAP teardown;
- fd open preflight diagnostics for missing and wrong-type inherited
  fds, with closed-state unwind covered by KUnit;
- fd failure-stress KUnit coverage for 1000 repeated netdev open/stop
  cycles, bad-fd `ndo_open()` unwind to closed state, and 10,000 direct
  missing-config fd-open failures with no attached channels;
- fd multiqueue core for contiguous inherited fd ranges, with KUnit
  coverage for two-queue open/close and missing-later-fd unwind;
- launcher-owned vector2 fd multiqueue handoff through `umlctl`:
  `queues=N` maps to a contiguous inherited fd range starting at fd
  200, with live 4-queue gateway ping and clean TAP teardown;
- `umlctl` automatic vector2 queue sizing: `queues = "auto"` and
  `--network-queues auto` resolve from `[runtime].ncpus` before
  rendering numeric `queues=N`, inherited fd counts, manifest labels,
  and guest `UMLCTL_NETWORK_*` metadata;
- explicit vector2 queue-to-CPU policy through `ndo_select_queue` and
  XPS setup, with KUnit coverage for the deterministic CPU/queue
  modulo rules;
- short KCSAN-instrumented seccomp smoke for `queues = "auto"` over
  launcher-owned vector2 fd multiqueue: `PASS=1/1`, no TAP leak,
  `requested_queues=4 runtime_queues=4`, and no KCSAN data-race
  signatures in the captured run log;
- queue lockdep follow-up for that KCSAN path: ethtool stats and the
  netdev transmit path now disable bottom halves while taking queue
  locks shared with NAPI, `um_vector2_*` KUnit passes 72/72, and the
  rebuilt KCSAN auto-queue fd multiqueue gate passes
  `PASS=10/10 FAIL=0 TIMEOUT=0` with no warning, panic, KCSAN,
  data-race, or TAP leak;
- short `umlctl gate loop` vector2 fd-handoff repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0`;
- short `umlctl gate loop` vector2 fd-multiqueue repetition:
  `PASS=3/3 FAIL=0 TIMEOUT=0`;
- `umlctl gate loop` cleanup audit for TAP-backed Umlfiles, so a
  leaked host TAP turns the iteration into a failure;
- live vector2 lifecycle stress harness through `umlctl gate loop`:
  the guest repeatedly drives `vec2.0` through `ip link down/up`,
  verifies `open_attempts` and `closes` ethtool counter deltas, and a
  25-cycle smoke passed with clean TAP/process teardown;
- live Tier 3 Django stdlib-shim success on seccomp through v2 TAP;
- 30/30 Tier 3 Django stdlib-shim success on seccomp through v2 TAP;
- live Tier 3 Django stdlib-shim success on seccomp through v2
  `queues=2` TAP;
- live real FastAPI + uvicorn success on seccomp through
  vector2 launcher-owned fd handoff with `queues = "auto"` resolving
  to two queues: one-shot gateway ping, `SERVER_READY`,
  `FASTAPI_HTTP ok=51 fail=0`, `VECTOR2_FASTAPI_OK`,
  `REPRO_DONE rc=0`, clean TAP teardown, and a short
  `PASS=10/10 FAIL=0 TIMEOUT=0` repetition;
- both-drivers kernel compatibility for `vec2.*`: the legacy `vec`
  setup path now leaves `vec2.` and `vec2=` command-line specs for
  vector2 while preserving legacy `vec2:` as old-driver unit 2;
- initial `umlctl` legacy-vs-vector2 TCP baseline helper and evidence:
  guest-to-host 32 MiB legacy vector TAP measured 662 MiB/s guest-side
  while vector2 fd multiqueue measured 220 MiB/s; host-to-guest
  32 MiB legacy vector TAP measured 3.2 MiB/s guest-side while vector2
  fd multiqueue measured 456 MiB/s on the same host, making
  performance a measured open blocker rather than an unmeasured
  unknown;
- repeated-size perf harness support: the helper accepts byte-count
  lists and repeat counts, writes per-size/per-repeat output
  directories plus a `repeat` summary column, and a vector2-only smoke
  passed guest-to-host and host-to-guest 1 MiB/2 MiB transfers with no
  lingering TAP;
- `umlctl up --strace` wiring for audited vector2 fd-handoff runs:
  the supervisor records the UML tracee PID rather than the strace
  wrapper PID, `down --force --rm` removes the traced auto-queue run
  cleanly, and a narrowed strace scan of the vector2 fd boot found no
  actual `/dev/net/tun` open, `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML
  network-helper exec from the traced vector host path;
- `umlctl gate loop --audit-vector-sandbox`, which implies `--strace`,
  preserves per-iteration strace/audit logs, and fails the iteration on
  actual vector host TAP open, `TUNSETIFF`, `AF_PACKET`, `bpf()`, or UML
  network-helper exec; the live auto-queue fd smoke passed
  `PASS=1/1 FAIL=0 TIMEOUT=0` through this gate with no TAP or UML
  process leak;
- the real FastAPI + uvicorn vector2 fd smoke also passed the same
  audit gate once: `PASS=1/1 FAIL=0 TIMEOUT=0`, `FASTAPI_HTTP ok=51
  fail=0`, `VECTOR2_FASTAPI_OK`, no TAP or UML process leak, and a
  2068786-line strace with no forbidden vector host operation;
- TAP teardown hardening after successful loops and failed starts.

Those pieces attach v2 to the Linux networking stack for inspection.
They still do not provide replacement-ready networking.

Missing runtime pieces:

- no real timer-driven coalescing;
- no feature negotiation;
- no 30/30 Tier 3 workload proof on kvm-v2;
- no repeated long soak loop;
- no live 10,000-cycle `ip link up/down` failure-injection proof; the
  fd path has bounded KUnit stress and a 25-cycle runtime smoke, but
  not full runtime repetition evidence;
- no full multiqueue validation story: heavier KCSAN traffic, long SMP
  traffic, and broader fairness/performance profiles remain open;
- no repeated or CI-enforced sandbox syscall audit gate, and no final
  policy for guest userspace raw/netlink sockets visible in UML host
  traces; a local `umlctl gate loop --audit-vector-sandbox` gate exists
  for the vector host path, including one FastAPI run, but longer
  workload coverage remains open;
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
  datapath, ethtool, `umlctl`, TAP multiqueue, and launcher-owned fd
  multiqueue pieces.  Sandbox helper/proxy, performance, KCSAN,
  kvm-v2, and full Tier 3 replacement gates remain open.

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
  - fd multiqueue KUnit and live `umlctl` fd multiqueue smoke:
    `fd=200,queues=4`, runtime queues 4, `numtxqueues 4`, 3/3 ping,
    per-queue ethtool counters, and clean TAP teardown.
- Therefore direct-fd packet movement exists for the trusted
  single-queue development path, inherited-fd sandbox builds, and the
  launcher-owned fd multiqueue path.  fd performance profiles, KCSAN,
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
  - seccomp v2 Tier 3 stdlib repetition: 30/30 pass through
    `--network-driver vector2`, every run reached `SERVER_READY`,
    `TIER3_OK`, and `REPRO_DONE rc=0`, and `soak-tap0` was absent
    after teardown;
  - seccomp repeated cleanup smoke: 2/2 pass, TAP absent after loop;
  - kvm-v2 Tier 3 smoke still fails `umlctl up` readiness after the
    full 180-second budget, but TAP is absent after failure cleanup;
  - kvm-v2 no-network isolation also fails readiness before the Linux
    boot banner while the same kernel boots under seccomp, so the
    current kvm-v2 R7 blocker is not vector2-specific.
- Therefore R7 is partially satisfied.  Selection, observability,
  teardown safety, and the seccomp 30/30 Tier 3 gate landed; kvm-v2
  30/30 and long-soak eligibility remain open, and kvm-v2 baseline
  readiness must be fixed before vector2-specific kvm-v2 datapath
  claims are meaningful.

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
  - `umlctl` exposes `[network] queues`, `queues = "auto"`,
    `--network-queues`, `--network-queues auto`, and
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
  multiqueue runtime shape, fd multiqueue now has core KUnit plus live
  `umlctl` launch evidence, and queue-to-CPU policy now has explicit
  XPS setup plus KUnit coverage; KCSAN and kvm-v2 evidence remain open.

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
  fairness/performance profiles, and kvm-v2 evidence remain open.

R8c implementation note:

- `31-uml-vector-driver-v2-r8c-queue-cpu-policy.md` records the
  explicit queue-to-CPU checkpoint.
- Implemented:
  - vector2 owns `.ndo_select_queue` and caps the selected queue with
    `netdev_cap_txqueue()`;
  - vector2 configures XPS at open for multiqueue devices;
  - the pure policy is defined over online CPU ordinals: CPUs spread
    across queues by modulo when CPUs are plentiful, and queues share
    CPUs by modulo when queue count exceeds online CPU count;
  - netdev KUnit covers both sides of the modulo policy and invalid
    bounds.
- Evidence collected:
  - targeted object build for `vector2_netdev.o` and
    `vector2_netdev_test.o`;
  - rebuilt KUnit UML kernel;
  - `um_vector2_*` KUnit: 72/72 passed.
- Therefore the queue-to-CPU policy item has an implementation and
  unit-level model coverage.  Long SMP traffic, performance profiles,
  and kvm-v2 evidence remain open.

R8d/R8e implementation notes:

- `33-uml-vector-driver-v2-r8d-kcsan-auto-queue-smoke.md` records the
  first KCSAN-instrumented `queues = "auto"` vector2 fd multiqueue
  smoke.
- `35-uml-vector-driver-v2-r8e-kcsan-lockdep.md` records the repeated
  KCSAN follow-up.
- Implemented in R8e:
  - process-context queue users in ethtool stats and netdev transmit
    now disable bottom halves when taking queue locks shared with NAPI;
  - vector2 fd multiqueue was rebuilt and rerun under KCSAN.
- Evidence collected:
  - targeted object build for `vector2_ethtool.o` and
    `vector2_netdev.o`;
  - rebuilt KCSAN runtime UML kernel;
  - rebuilt KUnit UML kernel;
  - `um_vector2_*` KUnit: 72/72 passed;
  - post-fix KCSAN auto-queue fd multiqueue gate:
    `PASS=10/10 FAIL=0 TIMEOUT=0`, no lingering `v2autoq0`, and no
    `WARNING`, `BUG`, `KCSAN`, `data-race`, panic, or lockdep
    signatures in the captured run logs.
- Therefore the specific queue-lock lockdep bug found by repeated
  KCSAN smoke is closed.  Heavier KCSAN traffic, fairness/performance
  profiles, and kvm-v2 evidence remain open.

Performance baseline follow-up:

- `36-uml-vector-driver-v2-perf-baseline.md` records the first
  repeatable `umlctl` legacy-vs-vector2 performance baseline.
- Implemented:
  - `tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh`;
  - both-drivers command-line coexistence fix so the legacy `vec`
    setup path does not consume `vec2.` or `vec2=`;
  - temporary per-driver Umlfile generation with a host Python TCP
    sink, guest Python TCP sink, and direction-selectable sender.
- Evidence collected:
  - rebuilt both-drivers runtime UML kernel;
  - legacy vector `vec0` guest-to-host TCP over TAP: 32 MiB at
    662 MiB/s guest-side;
  - vector2 `vec2.0` guest-to-host TCP over launcher-owned fd
    multiqueue: 32 MiB at 220 MiB/s guest-side;
  - legacy vector `vec0` host-to-guest TCP over TAP: 32 MiB at
    3.2 MiB/s guest-side;
  - vector2 `vec2.0` host-to-guest TCP over launcher-owned fd
    multiqueue: 32 MiB at 456 MiB/s guest-side;
  - direction `both` vector2 smoke with 1 MiB each way;
  - repeated-size vector2-only smoke for 1 MiB and 2 MiB in both
    directions;
  - no lingering baseline TAP device.
- Therefore the performance gate now has an initial comparison harness
  and an explicit vector2 guest-to-host regression to investigate.
  UDP packet rate, syscall profiles, repeated legacy-vs-vector2 transfer
  sizes, and CPU profiles remain open.

Fd failure-stress follow-up:

- `38-uml-vector-driver-v2-fd-failure-stress.md` records the focused fd
  lifecycle KUnit checkpoint.
- Implemented:
  - repeated netdev open/stop over an inherited fd for 1000 iterations;
  - invalid-fd `ndo_open()` unwind with closed-state, carrier, channel,
    and counter checks;
  - direct missing-config fd-open failure for 10,000 iterations with no
    channel attachment.
- Evidence collected:
  - targeted object build for `vector2_host_fd_test.o`;
  - rebuilt KUnit UML kernel;
  - `um_vector2_*` KUnit: 75/75 passed;
  - no `not ok`, `FAILED`, `panic`, `BUG`, `WARNING`, `KCSAN`,
    `data-race`, or lockdep signatures in the captured KUnit log.
- Therefore the unit-level fd failure-injection story is stronger, but
  the replacement gate still needs a live 10,000-cycle `ip link up/down`
  failure-injection run with leak checks.

Lifecycle stress gate follow-up:

- `39-uml-vector-driver-v2-lifecycle-stress-gate.md` records the first
  reusable runtime lifecycle harness.
- Implemented:
  - `tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml`;
  - default 10,000-cycle live `ip link down/up` loop over vector2 fd
    handoff;
  - `umlctl gate loop --sweep UML_VECTOR2_LIFECYCLE_CYCLES=N` support
    through the existing env sweep path for short smoke runs;
  - ethtool `open_attempts` and `closes` delta checks before printing
    `VECTOR2_LIFECYCLE_STRESS_OK`.
- Evidence collected:
  - TOML syntax parse passed;
  - `umlctl up --dry-run` rendered vector2 fd handoff over fd 200;
  - 25-cycle live smoke passed:
    `PASS=1/1 FAIL=0 TIMEOUT=0`,
    `open_delta=25 close_delta=25`,
    `VECTOR2_LIFECYCLE_STRESS_OK`, no lingering `v2life0`, and no UML
    process leak.
- Therefore the lifecycle gate now has a reusable runtime harness and
  smoke evidence.  The full 10,000-cycle run remains open.

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

- `umlctl gate loop --audit-vector-sandbox` shows no vector host
  `/dev/net/tun` open, `TUNSETIFF`, `AF_PACKET`, helper execution, or
  `bpf()` syscall in sandbox mode;
- guest userspace raw/netlink socket policy is explicitly documented
  separately from vector host attach behavior;
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
long-soak proof, repeated launcher-owned fd gates, sandbox helper
plumbing, and deeper multiqueue validation.

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
