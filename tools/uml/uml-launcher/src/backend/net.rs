// SPDX-License-Identifier: GPL-2.0
//
// virtio-net vhost-user backend (workstream C-10 v2).
//
// Scaffold: protocol surface + daemon wiring + class seccomp
// filter. No TAP handling and no data path — `handle_event` is a
// no-op log today. Follow-on commits land the TAP-fd attach,
// the per-descriptor read/write through TAP, and the
// packet-aware buffer management (vnet hdr, checksum offload).
//
// This commit mirrors what `console.rs` looked like at the end
// of its commit 2: `uml-launcher backend net --socket <path>`
// binds the socket, completes vhost-user negotiation with a
// connecting frontend, applies the class-specific seccomp
// filter, and enters the daemon's event loop. Enough to exercise
// the extension pattern without pulling in the full TAP stack.
//
// Reference for the later commits:
//   - cloud-hypervisor/cloud-hypervisor/vhost_user_net/src/lib.rs
//     — the only vhost-user-backend 0.22-era net reference (the
//     rust-vmm/vhost-device monorepo has no net crate).

use std::sync::{Arc, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock};
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use vm_memory::{GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;

use crate::backend::seccomp::FilterBuilder;
use crate::cli::BackendNetArgs;

/// RX queue index (host → guest). Frames the TAP fd reads will
/// be delivered on here once the data path lands.
const RX_QUEUE: u16 = 0;

/// TX queue index (guest → host). Frames the guest posts here
/// will be written to the TAP fd once the data path lands.
const TX_QUEUE: u16 = 1;

/// Two queues for a simple TAP backend (RX + TX). No control
/// queue (VIRTIO_NET_F_CTRL_VQ = bit 17) and no multi-queue
/// (VIRTIO_NET_F_MQ = bit 22) — both are feature-bit-gated
/// and we advertise neither.
const NUM_QUEUES: usize = 2;

/// Max ring entries per virtqueue. Cloud-hypervisor's
/// `vhost_user_net` uses 256 as well; it's also the console
/// default. A single buffer holds at most one Ethernet frame
/// (1514 bytes + optional vnet hdr), so 256 buffers covers
/// burst load without wasting memory.
const QUEUE_SIZE: usize = 256;

/// Minimum feature set for negotiation. The scaffold only
/// advertises what's required to let the guest finish the
/// vhost-user handshake; feature-bit work for checksum
/// offload, MAC config, status, etc. lands with the data path.
fn device_features() -> u64 {
    (1u64 << VIRTIO_F_VERSION_1)
        | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits()
}

fn protocol_features() -> VhostUserProtocolFeatures {
    VhostUserProtocolFeatures::MQ | VhostUserProtocolFeatures::REPLY_ACK
}

/// Net backend state. Keeps the same `Arc<RwLock<...>>` wrap
/// pattern console uses so the daemon's blanket impl picks up
/// our `VhostUserBackendMut`.
pub struct NetBackend {
    /// Guest memory set at `set_mem_table()` time.
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap<()>>>,

    /// `VIRTIO_RING_F_EVENT_IDX` negotiation result. Recorded
    /// now even though the scaffold doesn't act on it; the
    /// data-path commits will consult it in
    /// `needs_notification()`.
    event_idx: bool,

    /// Name of the host TAP device, if one was specified at
    /// launch. Not yet consumed — the data path commit opens
    /// `/dev/net/tun` and binds this name via `TUNSETIFF`.
    /// Carried here so the diff when the data path lands is
    /// strictly additive.
    #[allow(dead_code)]
    tap_name: Option<String>,
}

impl NetBackend {
    pub fn new(tap_name: Option<String>) -> Result<Self> {
        Ok(Self {
            mem: None,
            event_idx: false,
            tap_name,
        })
    }
}

impl VhostUserBackendMut for NetBackend {
    type Bitmap = ();
    type Vring = VringRwLock;

    fn num_queues(&self) -> usize {
        NUM_QUEUES
    }

    fn max_queue_size(&self) -> usize {
        QUEUE_SIZE
    }

    fn features(&self) -> u64 {
        device_features()
    }

    fn protocol_features(&self) -> VhostUserProtocolFeatures {
        protocol_features()
    }

    fn set_event_idx(&mut self, enabled: bool) {
        self.event_idx = enabled;
    }

    fn update_memory(
        &mut self,
        mem: GuestMemoryAtomic<GuestMemoryMmap<()>>,
    ) -> std::io::Result<()> {
        self.mem = Some(mem);
        Ok(())
    }

    fn handle_event(
        &mut self,
        device_event: u16,
        evset: EventSet,
        _vrings: &[VringRwLock],
        _thread_id: usize,
    ) -> std::io::Result<()> {
        if evset != EventSet::IN {
            log::warn!("net: unexpected evset {evset:?}; ignoring");
            return Ok(());
        }

        // Scaffold: recognize the queue indices but do no work.
        // Data path (TAP read/write, descriptor-chain byte
        // movement, event-idx-aware notifications) lands in a
        // follow-on commit.
        match device_event {
            RX_QUEUE => log::debug!(
                "net: RX queue kick (data-path TBD in follow-on commit)"
            ),
            TX_QUEUE => log::debug!(
                "net: TX queue kick (data-path TBD in follow-on commit)"
            ),
            other => log::warn!("net: unexpected device_event={other}; ignoring"),
        }
        Ok(())
    }
}

/// Entry point for `uml-launcher backend net --socket <path>
/// [--tap <name>]`.
///
/// Creates backend state + daemon, applies the net-class
/// seccomp filter, and calls `serve()`. The TAP fd is not yet
/// opened; the `--tap` flag is carried through for the data
/// path commit.
pub fn run(args: BackendNetArgs) -> Result<i32> {
    let socket = args.common.socket;
    tracing::info!(
        socket = %socket.display(),
        tap = args.tap.as_deref().unwrap_or("<unset>"),
        "net backend: starting vhost-user daemon"
    );

    let backend = Arc::new(RwLock::new(
        NetBackend::new(args.tap).context("constructing net backend")?,
    ));

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());

    let mut daemon = VhostUserDaemon::new(
        "uml-launcher-backend-net".to_string(),
        backend.clone(),
        mem,
    )
    .map_err(|e| anyhow::anyhow!("constructing VhostUserDaemon: {e:?}"))?;

    // Net class uses the shared vhost-user event-loop baseline
    // plus no extra syscalls for the scaffold (the data path
    // will add TUN ioctls — TUNSETIFF, TUNSETOFFLOAD, etc. —
    // and whatever packet-read syscalls TAP needs). `ioctl` is
    // already in the baseline, so scaffold-time tap attach
    // (if any) will not trip the filter; cmd-level argument
    // filtering on ioctl is a future tightening.
    tracing::debug!("net backend: applying seccomp filter");
    FilterBuilder::new()
        .with_vhost_user_event_loop()
        .apply()
        .context("locking down net backend with seccomp filter")?;

    daemon
        .serve(&socket)
        .map_err(|e| anyhow::anyhow!("VhostUserDaemon::serve({}): {e:?}", socket.display()))?;

    tracing::info!("net backend: daemon exited cleanly");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn features_minimal() {
        let f = device_features();
        assert!(f & (1u64 << VIRTIO_F_VERSION_1) != 0);
        assert!(f & VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits() != 0);
        // No feature-bit logic yet; confirm we aren't accidentally
        // advertising MAC / MTU / CSUM etc. since the scaffold
        // doesn't honor them.
        // VIRTIO_NET_F_MAC = bit 5, VIRTIO_NET_F_MTU = bit 3,
        // VIRTIO_NET_F_STATUS = bit 16.
        assert_eq!(f & (1u64 << 3), 0, "MTU must not be advertised by scaffold");
        assert_eq!(f & (1u64 << 5), 0, "MAC must not be advertised by scaffold");
        assert_eq!(f & (1u64 << 16), 0, "STATUS must not be advertised by scaffold");
    }

    #[test]
    fn protocol_features_scaffold() {
        let pf = protocol_features();
        assert!(pf.contains(VhostUserProtocolFeatures::MQ));
        assert!(pf.contains(VhostUserProtocolFeatures::REPLY_ACK));
    }

    #[test]
    fn new_backend_has_no_memory() {
        let b = NetBackend::new(None).expect("construct backend");
        assert!(b.mem.is_none());
        assert!(!b.event_idx);
        assert!(b.tap_name.is_none());
    }

    #[test]
    fn new_backend_carries_tap_name() {
        let b = NetBackend::new(Some("tap0".to_string())).expect("construct backend");
        assert_eq!(b.tap_name.as_deref(), Some("tap0"));
    }

    #[test]
    fn queues_and_size_match_constants() {
        let b = NetBackend::new(None).expect("construct backend");
        assert_eq!(b.num_queues(), NUM_QUEUES);
        assert_eq!(b.max_queue_size(), QUEUE_SIZE);
        assert_eq!(NUM_QUEUES, 2);
        assert_eq!(QUEUE_SIZE, 256);
    }

    #[test]
    fn set_event_idx_records() {
        let mut b = NetBackend::new(None).expect("construct backend");
        assert!(!b.event_idx);
        b.set_event_idx(true);
        assert!(b.event_idx);
    }
}
