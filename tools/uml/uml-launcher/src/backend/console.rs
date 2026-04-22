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

use std::io::{Read, Write};
use std::sync::{Arc, Mutex, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock, VringT};
use virtio_bindings::bindings::virtio_config::{
    VIRTIO_F_NOTIFY_ON_EMPTY, VIRTIO_F_VERSION_1,
};
use virtio_bindings::bindings::virtio_ring::{
    VIRTIO_RING_F_EVENT_IDX, VIRTIO_RING_F_INDIRECT_DESC,
};
use virtio_queue::{QueueOwnedT, QueueT};
use vm_memory::{GuestAddressSpace, GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;

use crate::backend::seccomp::FilterBuilder;
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

/// Where guest TX bytes go. Boxed so tests can swap stdout for
/// an in-memory `Vec<u8>` and verify end-to-end behavior without
/// spawning a process.
pub type ConsoleSink = Box<dyn Write + Send>;

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

    /// Sink that the TX path writes guest bytes to. Defaults to
    /// stdout when the backend is constructed for the production
    /// `run()` path; tests substitute an in-memory buffer.
    ///
    /// Wrapped in `Mutex` so `ConsoleBackend` stays `Send + Sync`
    /// (required by the `VhostUserBackend: Send + Sync` bound via
    /// the `VhostUserBackendMut` blanket impl). `std::io::Stdout`
    /// is `Send` but not `Sync`; the `Mutex` bridges the gap.
    /// Contention is nonexistent in practice — the vhost-user
    /// daemon serializes TX batches through `handle_event()`.
    sink: Mutex<ConsoleSink>,
}

impl ConsoleBackend {
    pub fn new() -> Result<Self> {
        Ok(Self::with_sink(Box::new(std::io::stdout())))
    }

    /// Construct with a caller-supplied sink. Used by tests to
    /// swap stdout for an in-memory buffer; the `new()` entry
    /// point is the one the production `run()` path uses.
    pub fn with_sink(sink: ConsoleSink) -> Self {
        Self {
            mem: None,
            event_idx: false,
            sink: Mutex::new(sink),
        }
    }

    /// Drain the TX virtqueue: for each descriptor chain the
    /// guest published, read the bytes it wrote into guest
    /// memory, forward them to the backend's sink (stdout by
    /// default), and return the chain to the used ring.
    ///
    /// Follows the pattern used by rust-vmm/vhost-device-console's
    /// `process_tx_queue()` with the `event_idx`-aware notification
    /// that crate omits: we consult `needs_notification()` on the
    /// queue state before calling `signal_used_queue()` so the
    /// guest's event-index suppression kicks in as negotiated.
    fn process_tx_queue(&mut self, vring: &VringRwLock) -> std::io::Result<()> {
        let atomic_mem = match self.mem.as_ref() {
            Some(m) => m,
            None => {
                // Guest kicked TX before SET_MEM_TABLE landed.
                // Shouldn't happen with a well-behaved frontend;
                // silently skip rather than panic.
                log::warn!("console: TX kick before memory table was set; skipping");
                return Ok(());
            }
        };

        // Collect descriptor chains up front; draining the queue
        // iterator while the vring is borrowed mutably by
        // `add_used()` would tangle the borrow checker.
        // `.memory()` returns a short-lived LoadGuard; pass it
        // directly into `.iter()` per the rust-vmm/vhost-device
        // console reference.
        let requests = {
            let mut guard = vring.get_mut();
            let queue = guard.get_queue_mut();
            queue
                .iter(atomic_mem.memory())
                .map_err(|e| {
                    std::io::Error::other(format!("iter TX queue: {e:?}"))
                })?
                .collect::<Vec<_>>()
        };

        if requests.is_empty() {
            return Ok(());
        }

        let mut any_used = false;
        for chain in requests {
            let head = chain.head_index();
            let chain_mem = atomic_mem.memory();
            let mut reader = chain
                .clone()
                .reader(&chain_mem)
                .map_err(|e| std::io::Error::other(format!("chain reader: {e:?}")))?;

            let available = reader.available_bytes();
            if available == 0 {
                // Zero-length chain — mark used for the guest's
                // bookkeeping and move on.
                vring.add_used(head, 0).map_err(|e| {
                    std::io::Error::other(format!("add_used (empty): {e:?}"))
                })?;
                any_used = true;
                continue;
            }

            // Stream the bytes straight to the sink. Use a small
            // stack buffer so we avoid one allocation per chain;
            // 4096 is the max descriptor-chain size virtio-console
            // guests typically use, but we loop if the chain is
            // bigger.
            let mut buf = [0u8; 4096];
            let mut remaining = available;
            while remaining > 0 {
                let take = remaining.min(buf.len());
                reader.read_exact(&mut buf[..take]).map_err(|e| {
                    std::io::Error::other(format!("read TX bytes: {e:?}"))
                })?;
                self.sink
                    .lock()
                    .map_err(|_| std::io::Error::other("sink mutex poisoned"))?
                    .write_all(&buf[..take])?;
                remaining -= take;
            }

            let written = reader.bytes_read() as u32;
            vring
                .add_used(head, written)
                .map_err(|e| std::io::Error::other(format!("add_used: {e:?}")))?;
            any_used = true;
        }

        // Best-effort flush; if the sink is a stdout the guest
        // kernel expected its line to appear promptly.
        if let Ok(mut sink) = self.sink.lock() {
            let _ = sink.flush();
        }

        if any_used {
            // Honor event-index suppression when the guest
            // negotiated VIRTIO_RING_F_EVENT_IDX. The guest sets
            // a threshold in the avail ring's `used_event` field
            // below which the backend must not signal; calling
            // `needs_notification()` checks that and the basic
            // "is notification suppressed" bit in one place.
            let notify_mem = atomic_mem.memory();
            let needs = {
                let mut guard = vring.get_mut();
                let queue = guard.get_queue_mut();
                queue
                    .needs_notification(&*notify_mem)
                    .map_err(|e| std::io::Error::other(format!("needs_notification: {e:?}")))?
            };
            if needs {
                vring
                    .signal_used_queue()
                    .map_err(|e| std::io::Error::other(format!("signal_used_queue: {e:?}")))?;
            } else {
                log::trace!("console: TX batch completed; guest suppressed notification");
            }
        }

        // Silence "unused field" warning about event_idx on paths
        // where trace-level logging is compiled out.
        let _ = self.event_idx;

        Ok(())
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
        vrings: &[VringRwLock],
        _thread_id: usize,
    ) -> std::io::Result<()> {
        if evset != EventSet::IN {
            log::warn!("console: unexpected evset {evset:?}; ignoring");
            return Ok(());
        }

        match device_event {
            TX_QUEUE => {
                // Guest → host. Drain the TX queue into the
                // backend's stdout.
                let vring = &vrings[TX_QUEUE as usize];
                self.process_tx_queue(vring)?;
            }
            RX_QUEUE => {
                // Host → guest. Not yet implemented — the RX
                // path requires a backend-side eventfd tied to
                // the launcher's stdin (see the rust-vmm
                // vhost-device-console reference). Kicks on the
                // RX queue alone aren't sufficient; the backend
                // needs stdin-readiness to know when to fill
                // guest buffers. Tracked for a follow-on commit
                // in the C-10 v2 series per D52.
                log::debug!("console: RX queue kick (no-op until RX path lands)");
            }
            other => {
                log::warn!("console: unexpected device_event={other}; ignoring");
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

    // Lock down the syscall surface before entering the event
    // loop. The console class needs nothing beyond the shared
    // vhost-user event-loop baseline; any deviation → SIGSYS via
    // SECCOMP_RET_KILL_PROCESS. This is the "fd plumbing done,
    // attack surface about to open" moment — Firecracker/crosvm
    // both apply their filters at the analogous boundary.
    tracing::debug!("console backend: applying seccomp filter");
    FilterBuilder::new()
        .with_vhost_user_event_loop()
        .apply()
        .context("locking down console backend with seccomp filter")?;

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

/// End-to-end tests for the TX data path. Build a real
/// `GuestMemoryMmap` + `VringRwLock`, place a descriptor that
/// points into guest memory, wire it into the queue's avail
/// ring, let `process_tx_queue()` drain it, and assert the
/// bytes arrive on the backend's sink.
///
/// This exercises the actual flow bytes take from guest to host —
/// `process_tx_queue()` is the only thing that changes per
/// backend class, and without this harness the anti-pattern-6
/// concern ("land a commit whose tests you haven't actually
/// run") would be real: the handler either works byte-for-byte
/// or not. The real UML integration test still belongs with the
/// orchestration commit, but the data-path correctness is
/// provable here without it.
#[cfg(test)]
mod tx_path_tests {
    use super::*;
    use std::sync::Mutex;
    use vm_memory::{GuestAddress, GuestMemoryAtomic, GuestMemoryMmap};

    /// Allocate a 64 KiB guest-memory region at guest-physical
    /// address 0, suitable for holding both the virtqueue
    /// structures and the TX data buffers.
    fn make_mem() -> GuestMemoryAtomic<GuestMemoryMmap<()>> {
        let gmm = GuestMemoryMmap::<()>::from_ranges(&[(GuestAddress(0), 0x10000)])
            .expect("guest memory");
        GuestMemoryAtomic::new(gmm)
    }

    /// Build a vring backed by @mem at queue-size @size and
    /// return the `VringRwLock` the backend will consume plus
    /// the descriptor-table base address so the test can write
    /// descriptors directly.
    ///
    /// Layout in guest memory (chosen to give clean alignment):
    ///   - 0x0000..0x1000 : descriptor table (16 entries × 16 B)
    ///   - 0x1000..0x1800 : avail ring + used ring
    ///   - 0x2000..       : data buffers the TX descriptors point at
    fn setup_vring(
        mem: &GuestMemoryAtomic<GuestMemoryMmap<()>>,
        size: u16,
    ) -> VringRwLock {
        let vring = VringRwLock::new(mem.clone(), size).expect("new vring");
        // Enable + configure. The addresses match the layout
        // above.
        vring.set_queue_info(0x0, 0x1000, 0x1800).expect("queue info");
        vring.set_queue_ready(true);
        vring.set_queue_size(size);
        vring
    }

    /// Write a raw split-ring descriptor at index @desc_idx.
    /// Fields per virtio 1.x spec § 2.7.5.
    fn write_desc(
        mem: &GuestMemoryMmap<()>,
        desc_table: u64,
        desc_idx: u16,
        addr: u64,
        len: u32,
        flags: u16,
        next: u16,
    ) {
        use vm_memory::Bytes;
        let desc = desc_table + (desc_idx as u64) * 16;
        mem.write_obj::<u64>(addr, GuestAddress(desc)).unwrap();
        mem.write_obj::<u32>(len, GuestAddress(desc + 8)).unwrap();
        mem.write_obj::<u16>(flags, GuestAddress(desc + 12)).unwrap();
        mem.write_obj::<u16>(next, GuestAddress(desc + 14)).unwrap();
    }

    /// Publish descriptor @desc_idx into the avail ring at
    /// ring slot @avail_idx, and bump the avail-ring `idx`
    /// field. Layout per virtio 1.x spec § 2.7.6.
    fn publish_avail(
        mem: &GuestMemoryMmap<()>,
        avail_ring: u64,
        avail_idx: u16,
        desc_idx: u16,
    ) {
        use vm_memory::Bytes;
        // avail.ring[avail_idx]
        mem.write_obj::<u16>(
            desc_idx,
            GuestAddress(avail_ring + 4 + (avail_idx as u64) * 2),
        )
        .unwrap();
        // avail.idx (bump)
        mem.write_obj::<u16>(avail_idx + 1, GuestAddress(avail_ring + 2))
            .unwrap();
    }

    #[test]
    fn tx_single_descriptor_delivers_bytes_to_sink() {
        // Capturing sink. Mutex wraps the Vec because
        // ConsoleBackend::with_sink takes `Box<dyn Write + Send>`
        // — we need the test to reach inside after the call.
        struct CaptureSink {
            out: Arc<Mutex<Vec<u8>>>,
        }
        impl Write for CaptureSink {
            fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
                self.out.lock().unwrap().extend_from_slice(buf);
                Ok(buf.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let captured = Arc::new(Mutex::new(Vec::<u8>::new()));
        let sink = CaptureSink {
            out: captured.clone(),
        };

        let mut backend = ConsoleBackend::with_sink(Box::new(sink));

        let mem = make_mem();
        backend.update_memory(mem.clone()).expect("update_memory");

        // Place the payload "hello" at guest-physical 0x2000.
        let payload = b"hello";
        {
            use vm_memory::Bytes;
            mem.memory()
                .write_slice(payload, GuestAddress(0x2000))
                .unwrap();
        }

        // Stand up a TX vring. Write a single descriptor at
        // index 0 pointing at the payload, then publish it via
        // the avail ring.
        let vring = setup_vring(&mem, 16);
        {
            let g = mem.memory();
            write_desc(
                &g,
                /* desc_table */ 0x0000,
                /* desc_idx   */ 0,
                /* addr       */ 0x2000,
                /* len        */ payload.len() as u32,
                /* flags      */ 0, // no NEXT, no WRITE — guest→host
                /* next       */ 0,
            );
            publish_avail(&g, /* avail_ring */ 0x1000, /* avail_idx */ 0, 0);
        }

        backend.process_tx_queue(&vring).expect("process tx");

        let got = captured.lock().unwrap().clone();
        assert_eq!(
            got, payload,
            "TX path should have delivered 'hello' to the sink"
        );
    }

    #[test]
    fn tx_multiple_chains_in_one_batch() {
        let captured = Arc::new(Mutex::new(Vec::<u8>::new()));
        struct CaptureSink(Arc<Mutex<Vec<u8>>>);
        impl Write for CaptureSink {
            fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
                self.0.lock().unwrap().extend_from_slice(b);
                Ok(b.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let mut backend =
            ConsoleBackend::with_sink(Box::new(CaptureSink(captured.clone())));
        let mem = make_mem();
        backend.update_memory(mem.clone()).expect("update_memory");

        // Two payloads, at different guest addresses.
        let a = b"one\n";
        let b = b"two\n";
        {
            use vm_memory::Bytes;
            let g = mem.memory();
            g.write_slice(a, GuestAddress(0x2000)).unwrap();
            g.write_slice(b, GuestAddress(0x2100)).unwrap();
        }

        let vring = setup_vring(&mem, 16);
        {
            let g = mem.memory();
            // descriptor 0 → payload A
            write_desc(&g, 0x0000, 0, 0x2000, a.len() as u32, 0, 0);
            // descriptor 1 → payload B
            write_desc(&g, 0x0000, 1, 0x2100, b.len() as u32, 0, 0);
            // Publish both in one avail-ring bump. avail.idx
            // ends at 2, and ring[0]=0, ring[1]=1.
            use vm_memory::Bytes;
            g.write_obj::<u16>(0, GuestAddress(0x1000 + 4)).unwrap();
            g.write_obj::<u16>(1, GuestAddress(0x1000 + 4 + 2)).unwrap();
            g.write_obj::<u16>(2, GuestAddress(0x1000 + 2)).unwrap();
        }

        backend.process_tx_queue(&vring).expect("process tx batch");

        let got = captured.lock().unwrap().clone();
        let mut expected = Vec::new();
        expected.extend_from_slice(a);
        expected.extend_from_slice(b);
        assert_eq!(got, expected);
    }

    #[test]
    fn tx_without_memory_no_ops() {
        // Suppress stdout noise by redirecting the sink.
        let sunk = Arc::new(Mutex::new(Vec::<u8>::new()));
        struct CaptureSink(Arc<Mutex<Vec<u8>>>);
        impl Write for CaptureSink {
            fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
                self.0.lock().unwrap().extend_from_slice(b);
                Ok(b.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }
        let mut backend =
            ConsoleBackend::with_sink(Box::new(CaptureSink(sunk.clone())));

        let mem = make_mem();
        // Don't call update_memory() — backend.mem stays None.
        let vring = setup_vring(&mem, 16);

        // Should not panic even though no memory is set.
        backend.process_tx_queue(&vring).expect("no-op");
        assert!(sunk.lock().unwrap().is_empty());
    }

}
