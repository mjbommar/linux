# uml-vector-driver-v2 - safe high-performance vector networking for UML

**Status:** PROPOSED - future phase. Parking lot (2026-05-17).
Not in the original A/B/C/D plan. This memo scopes a complete
rewrite of `CONFIG_UML_NET_VECTOR` into a driver family that can
serve two different requirements without conflating them:

- a trusted, in-process fast path for `prod-fast` and developer
  workloads that want high throughput with low overhead;
- a sandbox-safe path for UML v2 deployments where privileged host
  networking lives outside the UML kernel process in confined
  helpers.

The current driver remains the reference for behavior and command-line
compatibility, but not for structure. Its existing implementation
mixes configuration parsing, privileged host fd creation, transport
header handling, NAPI, IRQs, batching, BPF, ethtool, and lifecycle
unwind in a small set of translation units:

- `arch/um/drivers/vector_kern.c`
- `arch/um/drivers/vector_user.c`
- `arch/um/drivers/vector_transports.c`
- `arch/um/drivers/vector_kern.h`
- `arch/um/drivers/vector_user.h`

That shape was practical when the goal was "make vector networking
work", but it is a poor substrate for formal modeling, SMP scaling,
or the sandbox profile's host-side isolation model.

Runtime buildout note: this memo is the architecture reference, not a
claim that vector v2 is swap-ready.  The concrete plan for finishing a
real netdev driver lives in
`15-uml-vector-driver-v2-buildout.md`.

## Motivation

The UML redesign has made networking more important than it was in the
original A-D plan:

- Tier 3 validation wants real networked workloads, and current notes
  already record a `CONFIG_UML_NET_VECTOR=y` live-smoke crash around
  `vector_net_open`.
- `uml-launcher` v2 has per-device vhost-user helpers with seccomp
  and LSM confinement. That is the correct security boundary for
  sandbox networking, but it is separate from the older in-process
  vector driver.
- `prod-fast` still needs a high-throughput in-process option for
  trusted environments. The old driver's batching model is valuable;
  its lifecycle and security surface are the problem.
- SMP and high-packet-rate workloads need per-queue state instead of
  a single device-wide NAPI instance and a single global IRQ allocator.
- Future analysis should be able to run cscope, clangd, sparse,
  Smatch, KUnit, LKMM litmus tests, and optional state-model checks
  against clear modules with narrow contracts.

The rewrite should therefore be a safety and architecture project
first, with performance preserved by design rather than recovered
afterward.

## Goals

1. **Make state explicit.** Device, channel, queue, fd, IRQ, and NAPI
   states must be modeled directly. Boolean flags such as `opened`,
   `in_error`, and `in_write_poll` should not define the lifecycle.

2. **Separate privilege from packet movement.** The UML kernel process
   should not have to open raw sockets, attach host BPF, run helper
   scripts, or configure TAP devices in sandbox mode.

3. **Keep the fast path fast.** `sendmmsg(2)`, `recvmmsg(2)`, vnet
   headers, checksum/GSO/GRO support, BQL, and batching remain first-
   class in trusted builds.

4. **Scale across SMP.** Multiple TX/RX queues, one NAPI instance per
   RX queue, per-queue stats, and queue-to-CPU affinity should be part
   of the initial model, not an afterthought.

5. **Preserve user-visible compatibility where sane.** Existing
   `vecN:transport=...,option=value` syntax should continue to parse
   through a compatibility layer. Unsafe options may become disabled
   by default in sandbox profiles.

6. **Make the code analyzable.** The design should favor small files,
   typed data, pure validation helpers, table-driven transport ops,
   and named transition functions so clang AST and cscope queries
   answer real questions.

7. **Document contracts next to code.** Every ops table must have
   kernel-doc comments and a matching KUnit/fake-backend test.

## Non-goals

- Replacing the generic Linux networking stack.
- Replacing virtio-net for sandbox profiles. The v2 launcher net
  backend remains the preferred sandbox network implementation.
- Making raw host sockets safe inside the UML process. In-process raw
  sockets are a trusted-mode feature.
- Landing every transport in the first series. TAP and pre-opened fd
  transports are enough for the first usable milestone.
- Supporting host-kernel patches or out-of-tree host modules.

## Current Driver Problems To Remove

### Configuration is untyped

`uml_parse_vector_ifspec()` destructively tokenizes a string into
parallel token/value arrays. Call sites then repeatedly fetch strings
and parse them ad hoc. The parser has no schema, no duplicate-key
policy, no bounded string-copy policy, and no transport-specific
validation phase.

Replacement:

- parse once into `struct um_vec_config`;
- validate exactly once before netdev registration;
- reject duplicates unless a field is explicitly repeatable;
- reject unknown keys by default, with a compatibility warning mode
  available for one release if needed;
- keep raw user strings only for diagnostics, not as the primary
  representation.

### Host privilege is embedded in driver open

`uml_vector_user_open()` can create TAP devices, raw packet sockets,
GRE/L2TP sockets, UNIX sockets, VDE helpers, and fd-based transports.
Some paths run host helper commands. Some paths require host
capabilities. Some paths load BPF programs from host files.

Replacement:

- trusted in-process mode may keep host fd creation behind an explicit
  Kconfig and runtime policy gate;
- sandbox mode accepts only pre-opened fds or v2 helper sockets;
- helper command execution moves to `uml-launcher`, where it can be
  governed by manifest policy and namespace setup;
- guest-triggered ethtool flash loading of host BPF is removed from
  sandbox-capable builds.

### Lifecycle is not a state machine

Open failure unwinds by calling close while fields may be partially
initialized. Some ethtool paths assume queues exist. NAPI, IRQ, fd,
queue, transport-data, and BPF lifetimes are coupled by convention.

Replacement:

- every resource has a state and an owner;
- every transition is named, documented, and tested;
- cleanup walks the state graph backward;
- ethtool paths handle `REGISTERED` and `RUNNING` states explicitly.

### Queue ownership is hard to prove

The TX queue uses an skb pointer vector plus mmsg vector, separate
head/tail locks, and an atomic depth. RX reuses the same queue shape
but not the same ring semantics. This makes ownership hard to reason
about and harder to extend to SMP.

Replacement:

- define TX ring and RX batch objects separately;
- make descriptor ownership explicit;
- add fake-host KUnit tests that inject partial sends, `EAGAIN`,
  `ENOBUFS`, fd death, and repeated open/close;
- model the ring separately from syscall vector assembly.

### Transport parsing is not memory-safe by construction

GRE and L2TPv3 handlers read typed fields by casting into packet
header buffers. Header lengths are derived from options, but parser
helpers are not structured around safe bounds checks.

Replacement:

- use helpers that check `offset + sizeof(field) <= len` before every
  read or write;
- keep transport state immutable after open;
- make parser/build helpers pure enough to fuzz and KUnit-test
  without a netdev.

## Proposed Architecture

The rewrite should be a driver core plus independent host and transport
backends.

```text
arch/um/drivers/
+-- vector2_core.c          # netdev register/open/close/state machine
+-- vector2_config.c        # parser, schema, compatibility handling
+-- vector2_queue.c         # TX/RX queue objects and ownership helpers
+-- vector2_napi.c          # NAPI, IRQ arming, coalescing
+-- vector2_ethtool.c       # stats, ring params, feature gates
+-- vector2_host_inproc.c   # trusted mode: tap/raw/socket host syscalls
+-- vector2_host_fd.c       # pre-opened fds passed by launcher
+-- vector2_host_proxy.c    # optional v2 helper/proxy transport
+-- vector2_transport_tap.c
+-- vector2_transport_raw.c
+-- vector2_transport_gre.c
+-- vector2_transport_l2tpv3.c
+-- vector2_transport_bess.c
+-- vector2_model.h         # states, invariants, transition helpers
+-- vector2_internal.h      # private driver structs
`-- vector2_uapi.rst        # command-line/config contract doc
```

The exact prefix can be `vector_` once the old driver is replaced.
During parallel development, `vector2_` avoids symbol collision and
keeps cscope results clean.

### Core Objects

```c
enum um_vec_dev_state {
	UM_VEC_DEV_NEW,
	UM_VEC_DEV_CONFIGURED,
	UM_VEC_DEV_REGISTERED,
	UM_VEC_DEV_OPENING,
	UM_VEC_DEV_RUNNING,
	UM_VEC_DEV_QUIESCING,
	UM_VEC_DEV_DEAD,
};

enum um_vec_chan_state {
	UM_VEC_CHAN_UNINIT,
	UM_VEC_CHAN_ALLOCATED,
	UM_VEC_CHAN_FD_ATTACHED,
	UM_VEC_CHAN_IRQ_ATTACHED,
	UM_VEC_CHAN_NAPI_ENABLED,
	UM_VEC_CHAN_ACTIVE,
	UM_VEC_CHAN_QUIESCING,
	UM_VEC_CHAN_CLOSED,
};

struct um_vec_dev {
	struct net_device *ndev;
	struct platform_device *pdev;
	const struct um_vec_host_ops *host_ops;
	const struct um_vec_transport_ops *transport_ops;
	struct um_vec_config cfg;
	enum um_vec_dev_state state;
	struct um_vec_queue_pair __percpu *pcpu_qp;
	unsigned int num_queue_pairs;
	/* Immutable after OPENING succeeds. */
	u32 caps;
};
```

The state enums are not decorative. Public transition helpers should
assert legal source states and document postconditions:

```c
int um_vec_dev_configure(struct um_vec_dev *v, const char *spec);
int um_vec_dev_register(struct um_vec_dev *v);
int um_vec_dev_open(struct um_vec_dev *v);
void um_vec_dev_quiesce(struct um_vec_dev *v);
void um_vec_dev_close(struct um_vec_dev *v);
```

### Host Ops

Host ops represent how bytes reach the host. They are separate from
transport ops, which represent packet/header semantics.

```c
struct um_vec_host_ops {
	const char *name;
	u32 caps;
	int (*open)(struct um_vec_dev *v);
	void (*close)(struct um_vec_dev *v);
	int (*rx_batch)(struct um_vec_rxq *rxq, int budget);
	int (*tx_batch)(struct um_vec_txq *txq, int budget);
	int (*arm_rx)(struct um_vec_rxq *rxq);
	int (*arm_tx)(struct um_vec_txq *txq);
};
```

Initial host implementations:

- `inproc`: opens TAP/raw/socket fds inside the UML process. Disabled
  in sandbox profiles.
- `fd`: consumes already-opened fd numbers passed on the command line
  or by `uml-launcher`.
- `proxy`: talks to a v2 helper over a narrow control/data protocol.
  This is optional if virtio-net fully covers sandbox networking.

### Transport Ops

Transport ops handle only packet format and netdev feature
declaration.

```c
struct um_vec_transport_ops {
	const char *name;
	u32 caps;
	netdev_features_t hw_features;
	int (*validate)(const struct um_vec_config *cfg,
			struct netlink_ext_ack *extack);
	int (*build_tx_header)(const struct um_vec_transport *t,
			       struct sk_buff *skb,
			       struct iov_iter *hdr);
	int (*parse_rx_header)(const struct um_vec_transport *t,
			       struct sk_buff *skb,
			       const void *hdr,
			       size_t len);
};
```

Transport state is built once during open, then treated as immutable
until close. Mutable per-packet counters such as GRE sequence and
L2TPv3 counters live in per-queue transport state so SMP does not
serialize all traffic on one cacheline.

## State Model

### Device State

```text
NEW
  |
  v
CONFIGURED
  |
  v
REGISTERED <----------------------------+
  |                                     |
  v                                     |
OPENING --failure--> QUIESCING -> close-+
  |
  v
RUNNING
  |
  v
QUIESCING
  |
  v
REGISTERED
  |
  v
DEAD
```

Legal operations:

- `ndo_open` is valid only in `REGISTERED`.
- `ndo_stop` is valid in `RUNNING`, `OPENING` failure unwind, and
  `QUIESCING`.
- TX entry is valid only in `RUNNING`.
- ethtool read-only queries are valid in `REGISTERED` and `RUNNING`,
  but must report "not allocated" for runtime-only queues.
- feature changes that affect buffer shape are valid only when not
  `RUNNING`, unless the operation has an explicit quiesce/reopen path.

### Channel State

```text
UNINIT
  |
  v
ALLOCATED
  |
  v
FD_ATTACHED
  |
  v
IRQ_ATTACHED
  |
  v
NAPI_ENABLED
  |
  v
ACTIVE
  |
  v
QUIESCING
  |
  v
CLOSED
```

Required invariants:

- `ACTIVE` implies a live fd, an IRQ registration, a NAPI instance, and
  allocated queues.
- `NAPI_ENABLED` implies `napi_disable()` is required before freeing
  queues.
- `IRQ_ATTACHED` implies `um_free_irq()` is required before closing the
  fd unless the fd owner is outside UML.
- `FD_ATTACHED` has exactly one owner: in-process host ops, launcher,
  or proxy.

## Ownership Invariants

The following invariants should be written into comments, KUnit tests,
and optional model files:

1. An skb is owned by exactly one of:
   - network stack before `ndo_start_xmit`;
   - driver TX ring;
   - host syscall vector;
   - completion path;
   - freed/dropped path.

2. TX ring depth equals occupied descriptors.

3. RX buffers prepared for `recvmmsg` are either delivered to NAPI or
   freed before the next prepare cycle.

4. No packet header parser reads outside the received header length.

5. No queue is freed while NAPI can still enter its poll function.

6. No fd is closed while an IRQ registration can still report events
   for that fd.

7. No guest-controlled operation can cause host command execution.

8. No guest-controlled operation can attach host BPF unless the build
   and runtime policy explicitly enable trusted in-process mode.

9. Transport ops and immutable transport state do not change while the
   device is `RUNNING`.

10. Per-queue mutable transport state is not shared between CPUs unless
    protected by a documented lock.

## Data Path

### TX Fast Path

```text
ndo_start_xmit()
  -> select TX queue
  -> map skb into um_vec_tx_desc
  -> build optional transport/vnet header
  -> enqueue into per-queue ring
  -> if xmit_more and below threshold: defer
  -> else host_ops->tx_batch()
  -> complete descriptors returned by host
  -> BQL complete
  -> wake queue as needed
```

Design rules:

- `ndo_start_xmit` must not perform host setup or allocation beyond
  emergency fallback.
- batching depth is per queue and can adapt to recent partial sends;
- partial sends keep unsent descriptors in the ring;
- hard errors transition the channel to `QUIESCING` and schedule a
  controlled reopen if policy allows it;
- the drop path must complete BQL and free skb ownership exactly once.

### RX Fast Path

```text
IRQ_READ or host wake
  -> napi_schedule(rxq->napi)
  -> poll budget
  -> prepare batch buffers
  -> host_ops->rx_batch()
  -> parse optional transport/vnet headers
  -> eth_type_trans()
  -> napi_gro_receive()
  -> re-arm fd if work_done < budget
```

Design rules:

- one NAPI instance per RX queue;
- RX buffer allocation failures consume/drop host packets with explicit
  stats rather than wedging fd readiness;
- GRO-sized buffers are negotiated by feature state, not inferred from
  a string option at packet time;
- receive-side checksum trust must flow only from validated vnet header
  state or transport-specific policy.

## SMP And Multiqueue Plan

The initial rewrite should use multiqueue even if a single queue is the
default:

- allocate with `alloc_etherdev_mqs()`;
- default queue count is `min(num_online_cpus(), configured_limit)`;
- expose `queues=N` as a typed config field;
- create one `struct napi_struct` per RX queue;
- use `netdev_pick_tx()` / skb queue mapping rather than a global lock;
- keep queue stats per queue and fold them for ethtool;
- avoid false sharing with cacheline alignment for hot queue fields;
- keep control state and stats out of TX/RX cachelines.

Per-transport SMP mapping:

- TAP: use multiqueue TAP where available. The launcher or trusted host
  ops opens one fd per queue.
- raw packet sockets: use one socket per RX queue with packet fanout
  where supported.
- GRE/L2TPv3 UDP: use `SO_REUSEPORT` per queue where host semantics
  make sense.
- GRE/L2TPv3 raw IP: begin single-queue unless a measured host
  fanout strategy is proven.
- fd transport: support either one fd pair or an explicit fd pair per
  queue.
- proxy/helper: map vhost-user queue pairs directly to UML queues.

The first SMP milestone is correctness with multiple queues. The second
is measured throughput. Do not introduce lockless rings until the
locked per-queue model is tested and profiled; if lockless rings are
added later, write LKMM litmus tests first.

## Performance Model

Performance targets should be explicit:

- no regression for single-queue TAP/raw throughput versus the old
  vector driver once equivalent offloads are enabled;
- line-rate-ish TCP throughput on local host for trusted TAP/raw with
  vnet headers and GSO;
- packet-rate scaling with queues on multicore hosts;
- no measurable cost in sandbox builds for code that is compiled out by
  Kconfig;
- no global lock in the steady-state TX/RX fast path except Linux
  networking core locks outside this driver.

Instrumentation:

- per-queue `rx_batch_histogram` and `tx_batch_histogram`;
- partial send/receive counters;
- fd readiness wake counters;
- BQL stop/wake counters;
- drop reasons, not just `rx_dropped` and `tx_dropped`;
- optional tracepoints under `TRACE_EVENT(um_vec_*)`.

## Sandbox And UML v2

The sandbox answer is architectural, not "make raw sockets careful".

### Policy

For sandbox profile builds:

```text
CONFIG_UML_NET_VECTOR=y
CONFIG_UML_NET_VECTOR_INPROC=n
CONFIG_UML_NET_VECTOR_BPF_FLASH=n
CONFIG_UML_NET_VECTOR_HELPERS=n
```

Allowed data sources:

- `transport=fd` with fds supplied by `uml-launcher`;
- `transport=proxy` to a confined helper, if implemented;
- virtio-net via `uml-launcher --virtio net:<tap>`, which remains the
  preferred sandbox path.

Disallowed inside the UML process:

- opening `/dev/net/tun`;
- creating `AF_PACKET` raw sockets;
- creating raw GRE/L2TP sockets;
- executing `ifup=` or any host helper;
- loading host BPF from guest-visible commands;
- ethtool flash as a BPF update mechanism.

### v2 Helper Relationship

`uml-launcher` v2 already has the right pattern:

```text
UML kernel process
  |
  | virtio or narrow fd/proxy protocol
  v
uml-launcher backend net
  |
  | TAP/raw/socket fds opened before seccomp
  v
host network
```

The vector rewrite should not duplicate this security work. It should
either:

- use virtio-net for sandbox networking and keep vector-v2 as trusted
  in-process only; or
- add a narrow `host_proxy` implementation where the helper performs
  privileged host operations and the UML driver only sees a data-plane
  protocol.

The second path is worthwhile only if it preserves vector-specific
advantages that virtio-net does not provide. Otherwise, use virtio-net
and keep the vector driver out of sandbox.

### Control Plane

Guest requests that affect host networking must be policy decisions:

- MAC change;
- promiscuous mode;
- multicast filters;
- offload feature changes;
- queue count changes;
- BPF/filter changes.

In sandbox mode, the UML driver sends a request to the helper or
returns `-EOPNOTSUPP`. The helper decides based on a manifest, not on
guest authority.

## Configuration Contract

Typed config should be documented and mechanically testable.

Common fields:

```text
transport=tap|raw|gre|l2tpv3|bess|fd|proxy
mode=inproc|fd|proxy|auto
queues=<1..N>
depth=<1..4096>
mtu=<576..65535>
headroom=<0..4096>
gro=0|1
gso=0|1
csum=0|1
mac=<xx:xx:xx:xx:xx:xx>
coalesce_usecs=<0..N>
```

Trusted-only fields:

```text
ifname=<host-ifname>
src=<addr>
dst=<addr>
srcport=<port>
dstport=<port>
ifup=<helper>
bpffile=<path>
```

Compatibility:

- `vec=0` maps to `depth=1, batching=off` for one release;
- old `bpfflash` emits a deprecation warning and is rejected in
  sandbox builds;
- transport-name matching must be exact, not prefix-based;
- duplicate keys are rejected unless explicitly documented.

## Formal Modeling And Analysis Hooks

This rewrite should produce artifacts that can be consumed by tools:

### Cscope And clangd

- Prefix all new symbols with `um_vec_`.
- Keep one concept per file.
- Avoid large anonymous helper blocks.
- Keep ops tables `static const` and named by transport/host.
- Generate and document `compile_commands.json` workflow for `ARCH=um`
  builds.

### KUnit

KUnit suites:

- `um_vec_config_test`: parser, bounds, duplicate keys, compatibility.
- `um_vec_queue_test`: ring ownership, partial completion, wraparound.
- `um_vec_transport_test`: GRE/L2TP/raw header build/parse.
- `um_vec_state_test`: legal and illegal transition coverage.
- `um_vec_fake_host_test`: fake host with programmable failures.

### Static Analysis

Required checks before graduating from parking lot:

- GCC build;
- Clang build;
- sparse;
- Smatch;
- checkpatch for every series;
- KCSAN on multiqueue tests;
- KASAN/KMSAN on repeated open/close and failure injection.

### Model Files

Optional but encouraged:

```text
Documentation/virt/uml/redesign/08-future-phases/models/vector2/
+-- device_state.tla
+-- channel_state.tla
+-- tx_ring.tla
+-- rx_batch.tla
`-- README.md
```

The models do not need to describe Linux networking. They should
describe resource ownership, allowed transitions, and queue invariants.

Implementation checkpoint:

- `Documentation/virt/uml/redesign/08-future-phases/models/vector2/`
  now contains starter TLA+ models for the v2 device state machine,
  channel state machine, TX ring counters, and RX batch counters.
- The models intentionally track the same pure helper boundaries as the
  current v2 KUnit code rather than attempting to model Linux networking
  or host syscalls.

## Validation Ladder

### Tier 0 - Unit and fake-host

- KUnit parser and queue tests.
- Fake-host TX/RX with deterministic partial sends.
- Header fuzz tests for transport parsers.
- Open/close failure injection for every state.

### Tier 1 - Single queue smoke

- TAP fd supplied by launcher.
- In-process TAP trusted mode.
- `ping`, DHCP, TCP loopback workload.
- ethtool stats and feature query while stopped and running.

### Tier 2 - Compatibility

- Existing `vec0:transport=tap,...` syntax works.
- Existing raw/hybrid/gre/l2tpv3 happy paths work in trusted builds.
- Deprecated/unsafe options reject in sandbox builds with clear errors.

### Tier 3 - SMP

- `queues=2`, then `queues=num_online_cpus()`.
- parallel iperf-style TX/RX.
- KCSAN clean.
- queue affinity and per-queue stats visible.

### Tier 4 - Sandbox

- sandbox profile build contains no in-process raw/TAP open path;
- `strace` of UML process shows no `/dev/net/tun`, raw socket, or helper
  execution;
- helper process seccomp profile is narrow and verified;
- guest attempts to enable unsafe filters fail closed.

### Tier 5 - Performance

- compare old vector driver versus vector-v2 on same host;
- compare vector-v2 trusted TAP/raw versus virtio-net helper;
- collect p50/p95 throughput and packet rate;
- collect CPU cycles per packet and syscall batch histograms;
- no prod-fast regression without explicit sign-off.

## Migration Plan

### Phase V0 - Freeze Current Behavior

Deliverables:

- document current syntax and observed behavior;
- add targeted tests around parser, open failure, ethtool paths, and
  known crash reproducer;
- add current-driver perf baseline for TAP/raw/hybrid.

Validation:

- old driver still builds and passes existing network smoke;
- repro for the `vector_net_open` crash exists if still reproducible.

### Phase V1 - Typed Config

Deliverables:

- `struct um_vec_config`;
- parser with compatibility mode;
- KUnit tests;
- no data-path changes.

Validation:

- all existing command-line examples parse or fail with documented
  errors;
- malformed inputs cannot produce partially initialized devices.

Implementation checkpoint:

- `arch/um/drivers/vector2_config.{c,h}` adds the first typed parser
  under the `um_vec2_*` prefix.
- `CONFIG_UML_NET_VECTOR_V2_KUNIT=y` builds
  `arch/um/drivers/vector2_config_test.c`, a KUnit suite covering
  defaults, exact transport names, duplicate and unknown keys, numeric
  bounds, legacy `vec=0`, strict-vs-compat boolean handling, sandbox
  rejection of trusted host options, fd transport requirements, and
  paired GRE/L2TPv3 keys.
- This checkpoint intentionally does not change the runtime
  `CONFIG_UML_NET_VECTOR` data path.

### Phase V2 - Ops Boundaries

Deliverables:

- introduce host ops and transport ops wrappers around existing code;
- move transport validation into transport modules;
- add fake-host test backend.

Validation:

- behavior unchanged for single-queue TAP/raw;
- fake-host tests cover open, close, partial TX, empty RX, and fd death.

Implementation checkpoint:

- `arch/um/drivers/vector2_host.h` defines a minimal host data-plane ops
  boundary for TX and RX batch operations.  The contract states that
  transient host errors preserve queue ownership, while fd death is
  reported as `-ENODEV` for lifecycle cleanup.
- `arch/um/drivers/vector2_fake_host.{c,h}` adds a deterministic fake
  backend that is backed by the v2 TX ring and RX batch helpers rather
  than by host sockets.  It can limit TX completion, inject TX/RX
  errors, queue synthetic RX packet lengths, and model fd death.
- `CONFIG_UML_NET_VECTOR_V2_FAKE_HOST_KUNIT=y` builds
  `arch/um/drivers/vector2_fake_host_test.c`, a KUnit suite covering
  full and partial TX completion, transient TX error preservation, TX
  fd death, RX packets, empty RX, RX allocation failure unwind, and RX
  fd death.
- This checkpoint intentionally does not wrap the legacy TAP/raw host
  syscalls yet; it establishes the testable boundary first.

Transport-safety checkpoint:

- `arch/um/drivers/vector2_transport.{c,h}` adds bounds-checked GRE and
  L2TPv3 header build/parse helpers.  All multi-byte fields are read
  and written through explicit unaligned big-endian helpers after a
  `len - offset` bounds check; no transport helper casts into packet
  storage.
- The helpers use immutable host-endian specs and return `-EMSGSIZE`
  for short buffers and `-EPROTO` for transport identifier mismatches
  such as an unexpected GRE key, GRE protocol, L2TPv3 data marker,
  session, or cookie.
- `CONFIG_UML_NET_VECTOR_V2_TRANSPORT_KUNIT=y` builds
  `arch/um/drivers/vector2_transport_test.c`, a KUnit suite covering
  GRE key/sequence headers, GRE short buffers, GRE mismatch handling,
  minimal GRE, L2TPv3 UDP/cookie/counter headers, 32-bit cookies,
  L2TPv3 short buffers, and L2TPv3 mismatch handling.
- This checkpoint is not wired into the existing transport modules yet;
  it creates the safe parsing/building surface that later transport ops
  can consume.

### Phase V3 - Queue Rewrite

Deliverables:

- separate TX ring and RX batch implementations;
- explicit skb ownership;
- per-queue stats;
- KUnit ring tests.

Validation:

- KASAN/KMSAN clean under failure injection;
- no BQL accounting leaks under drops and partial sends.

Implementation checkpoint:

- `arch/um/drivers/vector2_queue.{c,h}` adds pure TX ring and RX batch
  ownership helpers under the `um_vec2_*` prefix.  The helpers use
  caller-provided descriptor storage so the state can be tested without
  a netdev, syscall vector, or host fd.
- TX descriptors move through explicit free and driver-owned states.
  Partial completion preserves unsent descriptor order, including ring
  wraparound.
- RX slots move through explicit free, prepared, and filled states.
  Unreceived prepared buffers are released during receive completion,
  while filled buffers must be consumed or reset before another prepare
  cycle.
- `CONFIG_UML_NET_VECTOR_V2_QUEUE_KUNIT=y` builds
  `arch/um/drivers/vector2_queue_test.c`, a KUnit suite covering TX
  wraparound, partial completion, invalid completion rejection, reset
  release, RX prepare/receive/consume, allocation-failure unwind,
  busy/invalid receive rejection, and filled-buffer reset.
- This checkpoint intentionally remains model-only; the live vector
  transmit and receive paths still use the existing queue implementation.

### Phase V4 - Lifecycle Rewrite

Deliverables:

- explicit device and channel state machines;
- cleanup through transition helpers;
- ethtool paths audited for stopped/running states.

Validation:

- repeated open/close stress;
- injected failure at every open step unwinds without leaks;
- NAPI/IRQ/fd ordering tests pass.

Implementation checkpoint:

- `arch/um/drivers/vector2_model.{c,h}` adds pure device and channel
  lifecycle transition helpers under the `um_vec2_*` prefix.
- `CONFIG_UML_NET_VECTOR_V2_MODEL_KUNIT=y` builds
  `arch/um/drivers/vector2_model_test.c`, a KUnit suite covering the
  normal device path, open-failure unwind, illegal device transitions,
  normal channel activation/close, channel failure unwind, illegal
  channel transitions, and state-name helpers.
- This checkpoint intentionally remains model-only; no existing vector
  netdev open/close path calls the state model yet.

### Phase V5 - Multiqueue

Deliverables:

- `alloc_etherdev_mqs()`;
- per-queue NAPI;
- per-queue fd support for TAP and fd transports;
- queue selection and stats.

Validation:

- KCSAN clean under parallel traffic;
- queue counters show distribution;
- no global fast-path lock in profiles.

### Phase V6 - Sandbox Integration

Deliverables:

- Kconfig policy gates;
- fd-only sandbox path;
- optional proxy/helper path if justified;
- documentation update for sandbox profile.

Validation:

- sandbox `strace`/seccomp audit proves UML process does not own host
  network privilege;
- helper owns host fds and is confined.

### Phase V7 - Legacy Replacement

Deliverables:

- switch `CONFIG_UML_NET_VECTOR` to vector-v2 implementation;
- remove old files or move them to archive for one release if needed;
- update `Documentation/virt/uml/user_mode_linux_howto_v2.rst`.

Validation:

- CI matrix passes;
- performance baseline accepted;
- LKML-facing patch series is bisectable and reviewable.

## Patch Series Shape

Keep each series reviewable:

1. Documentation and tests for current behavior.
2. Parser and config object.
3. Ops table split with no data-path behavior change.
4. Transport parser safety cleanup.
5. Queue rewrite.
6. Lifecycle state machine.
7. Multiqueue TAP/fd.
8. Raw/GRE/L2TP trusted transports.
9. Sandbox Kconfig/policy integration.
10. Legacy removal and docs.

Every series should compile on its own. Every behavior-changing series
needs a test or a measured benchmark update.

## Risks

### Upstream Review Risk

Maintainers may reject a parallel `vector2_` implementation if it looks
like a second driver forever.

Mitigation:

- make the parallel period short;
- keep the patch series explicitly staged toward replacement;
- delete old code when feature parity is acceptable.

### Performance Regression Risk

Safer queues and typed state may add overhead.

Mitigation:

- benchmark before and after every fast-path series;
- keep per-queue hot state cacheline-local;
- compile out sandbox-only policy checks in trusted fast builds;
- do not add tracepoints or stats that run unconditionally in the hot
  path.

### Scope Creep Risk

GRE, L2TPv3, BESS, raw sockets, TAP, proxy, and virtio overlap can turn
this into an unbounded networking project.

Mitigation:

- first milestone is TAP + fd, single queue;
- second milestone is multiqueue TAP + fd;
- raw/GRE/L2TP follow only after the core state machine is stable;
- proxy/helper is conditional on measured value over virtio-net.

### Sandbox Confusion Risk

Users may assume `CONFIG_UML_NET_VECTOR=y` means "secure networking".

Mitigation:

- docs must distinguish in-process trusted vector from sandbox helper
  networking;
- sandbox profile Kconfig disables in-process privileged paths;
- runtime diagnostics should name the rejected option and the profile
  policy that rejected it.

## Open Questions

1. Should vector-v2 remain a separate netdev family (`vec2N`) during
   development, or should it replace internals under the existing
   `vecN` names immediately?

2. Is there a real performance or UX reason to add a vector-specific
   proxy/helper protocol, or should sandbox always use virtio-net?

3. Which transports must be feature-parity before old code removal:
   TAP + raw only, or TAP + raw + GRE + L2TPv3 + BESS?

4. Should host BPF support be removed entirely, or retained only as a
   trusted-mode launcher-managed feature?

5. What is the minimum host kernel version for multiqueue TAP and raw
   fanout support in the environments UML cares about?

6. Should queue count default to online CPUs, or remain 1 unless the
   user opts in?

## Decision Criteria For Graduation

This memo should graduate from parking lot to a real workstream only
when all are true:

- a current-driver crash or blocker is confirmed to be costly enough
  that incremental repair is worse than rewrite;
- TAP/fd vector-v2 prototype passes smoke tests;
- performance baseline shows no obvious design-level regression;
- sandbox policy is agreed: virtio-only, vector-proxy, or both;
- a maintainer-facing patch sequence can be kept below roughly ten
  logical series.

Until then, this memo is the target architecture and checklist for any
incremental fixes to the existing vector driver.
