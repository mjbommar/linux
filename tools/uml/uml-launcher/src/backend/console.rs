// SPDX-License-Identifier: GPL-2.0
//
// virtio-console vhost-user backend (C-10 v2 commit 2).
//
// This is the simplest vhost-user surface in UML's v2 device set:
// two queues (RX at index 0, TX at index 1), byte-oriented, no
// device config beyond the basic feature negotiation. UML's
// virtio_uml driver (arch/um/drivers/virtio_uml.c) connects to
// the socket we serve here and drives the queues on behalf of
// the guest.
//
// This commit lands the vhost-user **protocol** wiring — socket
// listen, feature negotiation, queue setup via the rust-vmm
// VhostUserDaemon. Actual RX/TX data flow (reading guest bytes
// off the TX queue into the backend's stdout, writing stdin
// bytes through the RX queue) is intentionally a no-op here; it
// lands in the next commit so this one stays reviewable in
// isolation. A backend with a live protocol surface + stubbed
// data path is still a bisectable unit — frontend connection +
// negotiation can be exercised before any data-movement code
// ships.
//
// Design references:
//   - rust-vmm/vhost-device/vhost-device-console (0.21 API)
//   - rust-vmm/vhost-device/vhost-device-rng     (simpler, 0.22)
//   - D52 (the C-10 v2 plan these choices come from)

use std::sync::{Arc, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock};
use virtio_bindings::bindings::virtio_config::{
    VIRTIO_F_NOTIFY_ON_EMPTY, VIRTIO_F_VERSION_1,
};
use virtio_bindings::bindings::virtio_ring::{
    VIRTIO_RING_F_EVENT_IDX, VIRTIO_RING_F_INDIRECT_DESC,
};
use vm_memory::{GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;

use crate::cli::BackendConsoleArgs;

/// RX queue index (host → guest). Follows virtio-console convention:
/// the VMM/guest sends bytes the backend should deliver TO the guest
/// here.
const RX_QUEUE: u16 = 0;

/// TX queue index (guest → host). Guest bytes arrive for the backend
/// to emit to its stdout.
const TX_QUEUE: u16 = 1;

/// Number of virtqueues. Classic single-port virtio-console is two
/// queues (RX + TX). Multiport (VIRTIO_CONSOLE_F_MULTIPORT) adds a
/// control-pair; v2 does not advertise it.
const NUM_QUEUES: usize = 2;

/// Maximum ring entries per virtqueue. 256 is the rust-vmm byte-
/// stream backend default and covers bursty interactive input
/// without wasting memory.
const QUEUE_SIZE: usize = 256;

/// Device feature bits the backend negotiates with the frontend.
/// Matches the canonical byte-stream feature set used by
/// vhost-device-console and vhost-device-rng:
///
///   * `VIRTIO_F_VERSION_1`        — modern (v1.x) virtio only.
///   * `VIRTIO_F_NOTIFY_ON_EMPTY`  — batching hint; standard.
///   * `VIRTIO_RING_F_INDIRECT_DESC` — lets the frontend chain
///     descriptors indirectly, standard for data-path devices.
///   * `VIRTIO_RING_F_EVENT_IDX`  — event-suppression optimization.
///   * vhost-user `PROTOCOL_FEATURES` bit — tells the frontend to
///     negotiate the protocol-features bitmap (for `MQ`, `REPLY_ACK`,
///     etc.).
fn device_features() -> u64 {
    (1u64 << VIRTIO_F_VERSION_1)
        | (1u64 << VIRTIO_F_NOTIFY_ON_EMPTY)
        | (1u64 << VIRTIO_RING_F_INDIRECT_DESC)
        | (1u64 << VIRTIO_RING_F_EVENT_IDX)
        | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits()
}

fn protocol_features() -> VhostUserProtocolFeatures {
    // MQ — required by the daemon to advertise more than one queue.
    // REPLY_ACK — lets the frontend request ACKs for set_* messages;
    // standard defensive choice, no perf cost unless the frontend
    // asks for it.
    VhostUserProtocolFeatures::MQ | VhostUserProtocolFeatures::REPLY_ACK
}

/// Mutable state for a live console backend. Wrapped in
/// `Arc<RwLock<...>>` so both `VhostUserDaemon` (via the blanket
/// `VhostUserBackend` impl for `RwLock<T: VhostUserBackendMut>`)
/// and any future control-path code see the same instance.
pub struct ConsoleBackend {
    /// Guest memory set at `set_mem_table()` time. `None` until the
    /// frontend calls `update_memory` the first time.
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap<()>>>,

    /// `VIRTIO_RING_F_EVENT_IDX` negotiation result. The per-queue
    /// notification path consults this; the scaffold path just
    /// records it.
    event_idx: bool,
}

impl ConsoleBackend {
    pub fn new() -> Result<Self> {
        Ok(Self {
            mem: None,
            event_idx: false,
        })
    }
}

impl VhostUserBackendMut for ConsoleBackend {
    // No guest-memory-backed bitmap tracking. Dirty-bitmap support
    // belongs on migratable backends; `console` is stateless.
    type Bitmap = ();

    // RwLock-backed vring. Thread-safety comes from the daemon's
    // per-queue handler locking pattern; a Mutex variant is also
    // available but RwLock is the conventional choice for
    // byte-stream backends where reads dominate.
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
        // Scaffold: log and drain. The rust-vmm daemon only calls
        // handle_event for device-class queues that we registered
        // (RX + TX for console). Data-path logic lands in the next
        // commit; this stub keeps the protocol surface live so
        // frontend connection + negotiation can be exercised now.
        log::debug!(
            "console: handle_event device_event={} evset={:?} (scaffold no-op)",
            device_event,
            evset
        );
        match device_event {
            RX_QUEUE => {}
            TX_QUEUE => {}
            other => {
                log::warn!("console: unexpected device_event={other}");
            }
        }
        Ok(())
    }

    // exit_event is left at its default (None). The frontend
    // disconnect path already takes the daemon out of its serve
    // loop cleanly; a future commit can plumb a proper EventConsumer
    // + EventNotifier pair here if we want externally-triggered
    // backend shutdown (e.g., an SIGTERM from the launcher
    // supervisor). Not needed for the scaffold's negotiation-only
    // path.
}

/// Entry point for `uml-launcher backend console --socket <path>`.
///
/// Creates the backend state, hands it to a `VhostUserDaemon`, and
/// calls `serve()`, which binds the Unix-domain socket, accepts a
/// single frontend connection, runs the protocol handshake, dispatches
/// queue events through `handle_event()`, and returns when the
/// frontend disconnects or our `exit_event` fires.
pub fn run(args: BackendConsoleArgs) -> Result<i32> {
    let socket = args.common.socket;
    tracing::info!(socket = %socket.display(), "console backend: starting vhost-user daemon");

    let backend = Arc::new(RwLock::new(
        ConsoleBackend::new().context("constructing console backend")?,
    ));

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());

    let mut daemon = VhostUserDaemon::new(
        "uml-launcher-backend-console".to_string(),
        backend.clone(),
        mem,
    )
    .map_err(|e| anyhow::anyhow!("constructing VhostUserDaemon: {e:?}"))?;

    daemon
        .serve(&socket)
        .map_err(|e| anyhow::anyhow!("VhostUserDaemon::serve({}): {e:?}", socket.display()))?;

    tracing::info!("console backend: daemon exited cleanly");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn features_includes_version_1_and_event_idx() {
        let f = device_features();
        assert!(f & (1u64 << VIRTIO_F_VERSION_1) != 0);
        assert!(f & (1u64 << VIRTIO_RING_F_EVENT_IDX) != 0);
        assert!(f & VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits() != 0);
    }

    #[test]
    fn protocol_features_advertises_mq_and_reply_ack() {
        let pf = protocol_features();
        assert!(pf.contains(VhostUserProtocolFeatures::MQ));
        assert!(pf.contains(VhostUserProtocolFeatures::REPLY_ACK));
    }

    #[test]
    fn new_backend_has_no_memory_until_update() {
        let b = ConsoleBackend::new().expect("construct backend");
        assert!(b.mem.is_none());
        assert!(!b.event_idx);
    }

    #[test]
    fn num_queues_and_size_match_constants() {
        let b = ConsoleBackend::new().expect("construct backend");
        assert_eq!(b.num_queues(), NUM_QUEUES);
        assert_eq!(b.max_queue_size(), QUEUE_SIZE);
        assert_eq!(NUM_QUEUES, 2);
        assert_eq!(QUEUE_SIZE, 256);
    }

    #[test]
    fn set_event_idx_records() {
        let mut b = ConsoleBackend::new().expect("construct backend");
        assert!(!b.event_idx);
        b.set_event_idx(true);
        assert!(b.event_idx);
        b.set_event_idx(false);
        assert!(!b.event_idx);
    }
}
