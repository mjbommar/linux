# 10 — vhost-net for the network datapath

**Sprint:** post-2026-05-19 (next-next)
**Priority:** MEDIUM-HIGH (closes the last ~13 % gap to legacy
vector single-stream TCP)
**Effort:** medium (~300–500 LoC across umlctl backends + arch/um/
drivers/vector2_host_*)
**Status:** designed 2026-05-19, awaiting implementation
**Depends on:** vector2 TSO/vnet_hdr (`9f5fca43fc2a` — DONE),
fd-handoff (`a73377ac4b9a` — DONE)

## Why this matters

Post-TSO patch, vector2 single-stream guest→host TCP is at
**0.877 of legacy** (memo 01 Step 2 gate is 0.85 — we pass, but
barely).  The remaining ~13 % gap is the per-skb userspace hop:

```
guest kernel  →  TAP fd write  →  host kernel (tap driver)
                                          ↓
                                    host network stack
```

Each skb costs one `write()` syscall from the UML process's
context.  Firecracker / cloud-hypervisor use **vhost-net** (the
host kernel's vhost-driver targeting tap) which eliminates the
userspace hop entirely:

```
guest kernel  →  virtqueue (shared mem)  →  vhost-net kthread
                                                  ↓
                                            host network stack
```

The vhost-net kthread runs in the host kernel, polls the
virtqueue, and sends directly into the tap.  No syscalls from
the UML process in the data path.  Expected win: another **1.5–2 ×**
on guest→host throughput, crossing into multi-Gbps with zero CPU
spent on userspace skb shuffling.

## Current state

| Piece | Path | State |
|---|---|---|
| vector2 fd transport | `arch/um/drivers/vector2_host_fd.c` | DONE — handles vnet_hdr after `9f5fca43fc2a` |
| vector2 inproc transport | `arch/um/drivers/vector2_host_tap.c` | DONE — opens tap with IFF_VNET_HDR |
| vhost-user-net Rust backend | `tools/uml/uml-launcher/src/backend/net.rs` | DONE — but it's a *user-space* backend, not host-kernel vhost-net |
| **Host vhost-net wiring** | — | MISSING |
| **virtio-net + vhost-net cmdline** | — | MISSING |

Note: tools/uml/uml-launcher/src/backend/net.rs is a vhost-user
backend that *speaks vhost-user to a virtio_uml frontend*.  That's
the user-space rust-vmm style.  vhost-net is fundamentally
different: it's the host kernel's `/dev/vhost-net` device that
the userspace VMM points at a tap fd, after which the kernel does
the data shuffling itself.

## Proposed change — three-phase build

### Phase 1 — vhost-net handshake helper

```rust
// New: tools/uml/uml-launcher/src/backend/vhost_net.rs
struct VhostNetSetup {
    vhost_fd: OwnedFd,   // /dev/vhost-net
    tap_fd: OwnedFd,     // opened tap (IFF_VNET_HDR)
    mem_table: VhostMem, // physmem regions visible to vhost-net
}

impl VhostNetSetup {
    fn open(tap_name: &str) -> Result<Self>;
    fn set_features(&self, features: u64) -> Result<()>;
    fn set_mem_table(&self, regions: &[MemRegion]) -> Result<()>;
    fn set_vring(&self, qid: u16, queue: &VirtQueue) -> Result<()>;
    fn set_backend(&self, qid: u16, tap_fd: &OwnedFd) -> Result<()>;
}
```

The handshake uses the standard ioctls:
`VHOST_SET_FEATURES`, `VHOST_SET_MEM_TABLE`, `VHOST_SET_VRING_*`,
`VHOST_NET_SET_BACKEND`.

### Phase 2 — virtio-net device in the guest

UML already has `virtio_uml.c` (the virtio_uml frontend) talking
vhost-user to a backend over a UNIX socket.  Add a new transport
that uses vhost-net instead:

```c
/* arch/um/drivers/virtio_uml_vhost_net.c (new) */
static int virtio_uml_vhost_net_probe(struct virtio_device *vdev)
{
    /* The guest sees a virtio-net device.  The host-side
     * vhost-net kthread does the actual packet shuffling.
     * Configure virtio_uml so its TX/RX virtqueues are wired
     * to vhost-net's queues at attach time.
     */
}
```

### Phase 3 — umlctl wiring

```toml
[network]
mode = "tap"
driver = "vhost-net"   # NEW — alongside vector / vector2
tap_name = "uml-tap0"
```

umlctl's `apply_network_section` opens the tap, opens
`/dev/vhost-net`, does the handshake, and passes the configured
vhost-net fd to the kernel via `virtio_uml.device=`.

## Effort breakdown

  * Phase 1 (vhost-net ioctl wrapper): ~150 LoC Rust + ~100 LoC
    tests.
  * Phase 2 (in-kernel virtio-net glue): ~200 LoC — most logic
    is in virtio_uml's existing vhost-user code, mostly re-wiring
    the queue endpoint.
  * Phase 3 (umlctl): ~50 LoC plus a per-driver code path in
    `deploy.rs::tap_network_plan`.

Total: ~500 LoC.

## Acceptance criteria

  * **Functional gate**: a guest with `driver = "vhost-net"` boots
    and can reach the host via TCP (curl against a host server
    on the gateway IP).
  * **Throughput gate**: single-stream guest→host TCP via
    `tools/testing/selftests/um/net-bench/run-tcp-throughput-via-
    umlctl.sh` measures **≥ 0.95 ratio vs legacy vector** (currently
    0.877 with vector2 fd-handoff).
  * **CPU gate**: at the same achieved throughput, UML process
    CPU drops by ≥ 30 % vs vector2 (the vhost-net kthread takes
    over the per-skb work).

## Risk notes

  * **vhost-net + non-root**: `/dev/vhost-net` is normally root-only.
    umlctl already opens privileged things (tap fd) under sudo;
    the same wrapper handles vhost-net.
  * **vCPU memory layout**: vhost-net needs to know the guest's
    physical memory layout via `VHOST_SET_MEM_TABLE`.  UML's
    physmem is one contiguous mmap region, so the table is a
    single entry — simple.
  * **Rollback**: per-Umlfile setting; existing `driver = "vector2"`
    keeps working unchanged.

## Cross-references

  * memo 01: vector2 default flip — sets the baseline this builds
    on.
  * `9f5fca43fc2a + a73377ac4b9a`: TSO/vnet_hdr patches that
    closed the gap from 0.126 to 0.877.
  * Firecracker's vhost-net wiring:
    https://github.com/firecracker-microvm/firecracker/blob/main/src/vmm/src/devices/virtio/net/device.rs
