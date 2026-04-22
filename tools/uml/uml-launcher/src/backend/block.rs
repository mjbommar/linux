// SPDX-License-Identifier: GPL-2.0
//
// virtio-blk vhost-user backend (workstream C-10 v2).
//
// Scaffold: protocol surface + daemon wiring. No image open,
// no request handling; handle_event() recognizes the queue
// index but logs and returns. Follow-on commits land the
// `O_DIRECT` image fd, the virtio_blk_req / sector-based
// request dispatcher, and the optional discard / write-zeroes
// feature bits.
//
// Reference for the later commits:
//   - rust-vmm/vhost-device/vhost-device-vsock has the closest-
//     shape block-ish byte-stream path among crates on
//     vhost-user-backend 0.22 (single queue, descriptor chain
//     → data copy). A dedicated net-style "block" crate in
//     rust-vmm/vhost-device does not exist at 0.22 yet.
//   - cloud-hypervisor/cloud-hypervisor/vhost_user_block/ uses
//     vhost-user-backend 0.20 (older API), so cross-reference
//     for request shape but not directly verbatim.

use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock};
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use vm_memory::{GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;
use vmm_sys_util::event::{new_event_consumer_and_notifier, EventConsumer, EventFlag, EventNotifier};

use crate::backend::seccomp::FilterBuilder;
use crate::cli::BackendBlockArgs;

/// Single request queue. virtio-blk carries both reads and
/// writes on one queue, disambiguated by the virtio_blk_req
/// header's `type` field (VIRTIO_BLK_T_{IN,OUT,FLUSH,DISCARD,
/// WRITE_ZEROES}). No multi-queue, no explicit ctrl queue.
const REQ_QUEUE: u16 = 0;

/// Number of virtqueues — one for block devices at the simplest
/// shape. virtio-blk can also advertise multi-queue via
/// VIRTIO_BLK_F_MQ, but v2 does not.
const NUM_QUEUES: usize = 1;

/// Max ring entries per virtqueue. 256 matches console / net /
/// the rust-vmm byte-stream baseline and covers reasonable
/// burst-IO load without wasting memory.
const QUEUE_SIZE: usize = 256;

/// Minimum feature set for negotiation — enough to let the
/// guest finish the vhost-user handshake. Capability bits for
/// RO / SIZE_MAX / SEG_MAX / BLK_SIZE / FLUSH / DISCARD /
/// WRITE_ZEROES are intentionally off; the scaffold advertises
/// no config fields and no optional behaviors.
fn device_features() -> u64 {
    (1u64 << VIRTIO_F_VERSION_1)
        | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits()
}

fn protocol_features() -> VhostUserProtocolFeatures {
    VhostUserProtocolFeatures::MQ | VhostUserProtocolFeatures::REPLY_ACK
}

/// Block backend state. Same `Arc<RwLock<...>>` wrap pattern
/// console + net use so the daemon's blanket impl of
/// `VhostUserBackend` for `RwLock<T: VhostUserBackendMut>`
/// picks us up.
pub struct BlockBackend {
    /// Guest memory set at `set_mem_table()` time.
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap<()>>>,

    /// `VIRTIO_RING_F_EVENT_IDX` negotiation result. Recorded
    /// now even though the scaffold doesn't act on it; the
    /// data-path commits will consult it in
    /// `needs_notification()`.
    event_idx: bool,

    /// Path to the disk-image file, if one was supplied at
    /// launch. Not yet opened — the data path commit will
    /// `open(O_DIRECT)` it, fstat for capacity, and then use
    /// preadv / pwritev per request. Carried now so that diff
    /// stays additive.
    #[allow(dead_code)]
    image: Option<PathBuf>,

    /// Read-only gate from the CLI flag. When true, the data
    /// path will refuse VIRTIO_BLK_T_OUT / FLUSH / DISCARD /
    /// WRITE_ZEROES requests with VIRTIO_BLK_S_UNSUPP rather
    /// than attempting a write. Recording this at scaffold
    /// time means the data-path commit is strictly additive
    /// and doesn't touch BackendBlockArgs plumbing.
    #[allow(dead_code)]
    read_only: bool,

    /// Exit-event pair — see `ConsoleBackend::exit_event` for
    /// the full rationale. Needed for clean shutdown when the
    /// frontend disconnects; without it the daemon's internal
    /// worker thread hangs in `epoll_wait`.
    exit_event: (EventConsumer, EventNotifier),
}

impl BlockBackend {
    pub fn new(image: Option<PathBuf>, read_only: bool) -> Result<Self> {
        let exit_event = new_event_consumer_and_notifier(EventFlag::NONBLOCK)
            .context("exit-event consumer/notifier pair")?;
        Ok(Self {
            mem: None,
            event_idx: false,
            image,
            read_only,
            exit_event,
        })
    }
}

impl VhostUserBackendMut for BlockBackend {
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
            log::warn!("block: unexpected evset {evset:?}; ignoring");
            return Ok(());
        }

        match device_event {
            REQ_QUEUE => log::debug!(
                "block: request queue kick (data-path TBD in follow-on commit)"
            ),
            other => log::warn!("block: unexpected device_event={other}; ignoring"),
        }
        Ok(())
    }

    /// Hand the daemon a cloned exit-event pair so its worker
    /// thread can be woken at shutdown. See the same method on
    /// `ConsoleBackend` for the rationale.
    fn exit_event(
        &self,
        _thread_index: usize,
    ) -> Option<(EventConsumer, EventNotifier)> {
        let (c, n) = &self.exit_event;
        Some((
            c.try_clone().expect("clone exit consumer"),
            n.try_clone().expect("clone exit notifier"),
        ))
    }
}

/// Entry point for `uml-launcher backend block --socket <path>
/// [--image <file>] [--read-only]`.
///
/// Creates backend state + daemon, applies the shared seccomp
/// filter, and calls `serve()`. The image fd is not yet opened
/// and request dispatch is not yet implemented; the `--image`
/// and `--read-only` flags are carried through for the data
/// path commit.
pub fn run(args: BackendBlockArgs) -> Result<i32> {
    let socket = args.common.socket;
    let image_display = args
        .image
        .as_ref()
        .map(|p| p.display().to_string())
        .unwrap_or_else(|| "<unset>".to_string());
    tracing::info!(
        socket = %socket.display(),
        image = image_display,
        read_only = args.read_only,
        "block backend: starting vhost-user daemon"
    );

    let backend = Arc::new(RwLock::new(
        BlockBackend::new(args.image, args.read_only)
            .context("constructing block backend")?,
    ));

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());

    let mut daemon = VhostUserDaemon::new(
        "uml-launcher-backend-block".to_string(),
        backend.clone(),
        mem,
    )
    .map_err(|e| anyhow::anyhow!("constructing VhostUserDaemon: {e:?}"))?;

    // Transition into the class's AppArmor sub-profile if
    // available. See console.rs for the shape rationale; same
    // graceful-skip contract here.
    match crate::backend::apparmor::change_profile("uml-launcher//backend_block") {
        Ok(crate::backend::apparmor::ChangeResult::Changed) => {
            tracing::info!("block backend: entered AppArmor sub-profile");
        }
        Ok(crate::backend::apparmor::ChangeResult::Skipped) => {
            tracing::debug!("block backend: AppArmor unavailable, continuing unconfined");
        }
        Err(e) => {
            return Err(e).context("aa_change_profile(uml-launcher//backend_block)");
        }
    }

    // Block class uses the shared vhost-user event-loop
    // baseline. The data path will add preadv / pwritev /
    // fdatasync / fallocate on top; those are all real-data-
    // movement syscalls that should be explicit in the
    // block-specific filter, not accidentally allowed via the
    // baseline. Scaffold keeps the baseline unchanged.
    tracing::debug!("block backend: applying seccomp filter");
    FilterBuilder::new()
        .with_vhost_user_event_loop()
        .apply()
        .context("locking down block backend with seccomp filter")?;

    daemon
        .serve(&socket)
        .map_err(|e| anyhow::anyhow!("VhostUserDaemon::serve({}): {e:?}", socket.display()))?;

    tracing::info!("block backend: daemon exited cleanly");
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
        // Capability bits for the scaffold must stay off until
        // the data path honors them. virtio_blk 1.x:
        //   VIRTIO_BLK_F_SIZE_MAX = 1, SEG_MAX = 2, GEOMETRY = 4,
        //   RO = 5, BLK_SIZE = 6, FLUSH = 9, DISCARD = 13,
        //   WRITE_ZEROES = 14, MQ = 12.
        for bit in [1, 2, 4, 5, 6, 9, 12, 13, 14] {
            assert_eq!(
                f & (1u64 << bit),
                0,
                "virtio-blk feature bit {bit} must not be advertised by scaffold"
            );
        }
    }

    #[test]
    fn protocol_features_scaffold() {
        let pf = protocol_features();
        assert!(pf.contains(VhostUserProtocolFeatures::MQ));
        assert!(pf.contains(VhostUserProtocolFeatures::REPLY_ACK));
    }

    #[test]
    fn new_backend_has_no_memory() {
        let b = BlockBackend::new(None, false).expect("construct backend");
        assert!(b.mem.is_none());
        assert!(!b.event_idx);
        assert!(b.image.is_none());
        assert!(!b.read_only);
    }

    #[test]
    fn new_backend_carries_image_and_ro() {
        let b = BlockBackend::new(Some(PathBuf::from("/tmp/rootfs.img")), true)
            .expect("construct backend");
        assert_eq!(b.image.as_deref(), Some(std::path::Path::new("/tmp/rootfs.img")));
        assert!(b.read_only);
    }

    #[test]
    fn queues_and_size_match_constants() {
        let b = BlockBackend::new(None, false).expect("construct backend");
        assert_eq!(b.num_queues(), NUM_QUEUES);
        assert_eq!(b.max_queue_size(), QUEUE_SIZE);
        // Sanity-check the constants themselves: one queue,
        // standard 256 ring size.
        assert_eq!(NUM_QUEUES, 1);
        assert_eq!(QUEUE_SIZE, 256);
    }

    #[test]
    fn set_event_idx_records() {
        let mut b = BlockBackend::new(None, false).expect("construct backend");
        assert!(!b.event_idx);
        b.set_event_idx(true);
        assert!(b.event_idx);
    }
}
