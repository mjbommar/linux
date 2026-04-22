// SPDX-License-Identifier: GPL-2.0
//
// virtio-console vhost-user backend (workstream C-10 v2).
//
// This is the simplest vhost-user surface in UML's v2 device set:
// two queues (RX at index 0, TX at index 1), byte-oriented, no
// device config beyond the basic feature negotiation. UML's
// virtio_uml driver (arch/um/drivers/virtio_uml.c) connects to
// the socket we serve here and drives the queues on behalf of
// the guest.
//
// Data flow:
//   - TX (guest → host): process_tx_queue() drains each avail-ring
//     chain into the backend's sink (stdout in production).
//   - RX (host → guest): a dedicated stdin reader thread
//     (spawned in run() before seccomp applies) appends bytes to
//     rx_fifo and wakes the daemon via an EventFd registered
//     with VhostUserDaemon::get_epoll_handlers() under
//     RX_EFD_ID. process_rx_queue() drains rx_fifo into
//     avail-ring buffers.
//
// Notification discipline: both paths call needs_notification()
// before signal_used_queue(), honoring the guest's
// VIRTIO_RING_F_EVENT_IDX suppression.
//
// Design references:
//   - rust-vmm/vhost-device/vhost-device-console (0.21 API)
//   - rust-vmm/vhost-device/vhost-device-rng     (simpler, 0.22)
//   - D52 (the C-10 v2 plan these choices come from)

use std::collections::VecDeque;
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
use vmm_sys_util::event::{new_event_consumer_and_notifier, EventConsumer, EventFlag, EventNotifier};
use vmm_sys_util::eventfd::{EventFd, EFD_NONBLOCK};
use std::os::fd::AsRawFd;

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

/// `data` value used when registering the backend-side RX eventfd
/// with `VringEpollHandler::register_listener()`. Must be greater
/// than the number of queues (the [0, NUM_QUEUES] range is
/// reserved by the daemon for queue kicks + the exit event);
/// picking `NUM_QUEUES + 1` keeps the mapping self-describing.
/// `handle_event()` dispatches on this when the reader thread
/// signals that stdin bytes have arrived.
const RX_EFD_ID: u16 = NUM_QUEUES as u16 + 1;

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

    /// Buffered host → guest bytes waiting to be delivered on
    /// the next RX virtqueue kick. The stdin reader thread
    /// appends; `process_rx_queue` drains as descriptors
    /// become available. Mutex is held briefly (one per-chain
    /// drain) so contention with TX handling is bounded.
    rx_fifo: Arc<Mutex<VecDeque<u8>>>,

    /// EventFd the stdin reader thread writes to after
    /// appending bytes to `rx_fifo`. Registered with the
    /// daemon's epoll via `register_listener(RX_EFD_ID)`
    /// so the daemon calls `handle_event` with
    /// `device_event = RX_EFD_ID` as soon as stdin bytes
    /// arrive — no waiting for the next guest-initiated RX
    /// kick.
    rx_eventfd: Arc<EventFd>,

    /// Exit-event pair. The `VhostUserDaemon`'s main control
    /// loop calls `send_exit_event()` on every
    /// `VringEpollHandler` when the frontend disconnects or
    /// `serve()` otherwise finishes; that notifier writes to
    /// the consumer fd this pair registered with the epoll.
    /// Without this wiring the worker thread's `epoll_wait`
    /// has nothing to wake on and the subprocess hangs at
    /// shutdown — verified by `tests/frontend_handshake.rs`
    /// via `try_wait()` timeout.
    exit_event: (EventConsumer, EventNotifier),
}

impl ConsoleBackend {
    pub fn new() -> Result<Self> {
        Self::with_sink(Box::new(std::io::stdout()))
    }

    /// Construct with a caller-supplied sink. Used by tests to
    /// swap stdout for an in-memory buffer; the `new()` entry
    /// point is the one the production `run()` path uses.
    pub fn with_sink(sink: ConsoleSink) -> Result<Self> {
        let eventfd = EventFd::new(EFD_NONBLOCK).context("rx eventfd")?;
        let exit_event = new_event_consumer_and_notifier(EventFlag::NONBLOCK)
            .context("exit-event consumer/notifier pair")?;
        Ok(Self {
            mem: None,
            event_idx: false,
            sink: Mutex::new(sink),
            rx_fifo: Arc::new(Mutex::new(VecDeque::new())),
            rx_eventfd: Arc::new(eventfd),
            exit_event,
        })
    }

    /// Push host-to-guest bytes into the RX FIFO. The bytes are
    /// delivered to the guest on the next RX virtqueue kick (or
    /// sooner if an external eventfd is wired — that plumbing
    /// lands in a follow-on commit).
    ///
    /// `#[allow(dead_code)]` because the only current caller is
    /// the test module; production stdin plumbing arrives in
    /// the next C-10 v2 commit.
    #[allow(dead_code)]
    pub fn push_rx_bytes(&self, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        let mut fifo = self.rx_fifo.lock().expect("rx_fifo mutex poisoned");
        fifo.extend(bytes);
    }

    /// Expose a clone of the shared FIFO handle. Callers that
    /// want to push from another thread (the stdin reader) can
    /// hold this handle without keeping the whole backend alive.
    pub fn rx_fifo_handle(&self) -> Arc<Mutex<VecDeque<u8>>> {
        Arc::clone(&self.rx_fifo)
    }

    /// Handle to the RX-ready eventfd. The stdin reader writes
    /// 1 here after appending bytes to `rx_fifo` so the daemon
    /// wakes up via its epoll registration.
    pub fn rx_eventfd_handle(&self) -> Arc<EventFd> {
        Arc::clone(&self.rx_eventfd)
    }

    /// Drain the RX virtqueue: for each avail-ring descriptor
    /// the guest offered as a place to put bytes, copy as many
    /// FIFO bytes as fit, and return the chain to the used
    /// ring.
    ///
    /// Unlike `process_tx_queue`, this handler runs opportunistically
    /// — the guest posts empty buffers proactively, and we fill
    /// them only when there's something to deliver. If the FIFO
    /// is empty, we return without consuming any chains; the
    /// avail ring stays as-is and we'll pick them up on the
    /// next kick.
    fn process_rx_queue(&mut self, vring: &VringRwLock) -> std::io::Result<()> {
        let atomic_mem = match self.mem.as_ref() {
            Some(m) => m,
            None => {
                log::warn!("console: RX kick before memory table was set; skipping");
                return Ok(());
            }
        };

        // Fast-exit if there's nothing to deliver.
        if self
            .rx_fifo
            .lock()
            .map_err(|_| std::io::Error::other("rx_fifo mutex poisoned"))?
            .is_empty()
        {
            return Ok(());
        }

        let requests = {
            let mut guard = vring.get_mut();
            let queue = guard.get_queue_mut();
            queue
                .iter(atomic_mem.memory())
                .map_err(|e| {
                    std::io::Error::other(format!("iter RX queue: {e:?}"))
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
            let mut writer = chain
                .clone()
                .writer(&chain_mem)
                .map_err(|e| std::io::Error::other(format!("chain writer: {e:?}")))?;

            let cap = writer.available_bytes();
            if cap == 0 {
                vring.add_used(head, 0).map_err(|e| {
                    std::io::Error::other(format!("add_used (empty): {e:?}"))
                })?;
                any_used = true;
                continue;
            }

            // Pull the next `cap` bytes (or whatever remains)
            // from the FIFO. Release the lock before touching
            // the virtqueue writer to keep the contention
            // window tight.
            let to_write: Vec<u8> = {
                let mut fifo = self
                    .rx_fifo
                    .lock()
                    .map_err(|_| std::io::Error::other("rx_fifo mutex poisoned"))?;
                let n = cap.min(fifo.len());
                if n == 0 {
                    // FIFO emptied concurrently; stop consuming
                    // the avail ring. The chain we already
                    // pulled will be re-emitted by the next
                    // iter() call thanks to virtio-queue's
                    // cursor semantics.
                    break;
                }
                fifo.drain(..n).collect()
            };

            for b in &to_write {
                writer
                    .write_obj::<u8>(*b)
                    .map_err(|e| std::io::Error::other(format!("write_obj RX: {e:?}")))?;
            }

            vring
                .add_used(head, to_write.len() as u32)
                .map_err(|e| std::io::Error::other(format!("add_used: {e:?}")))?;
            any_used = true;
        }

        if any_used {
            let notify_mem = atomic_mem.memory();
            let needs = {
                let mut guard = vring.get_mut();
                let queue = guard.get_queue_mut();
                queue
                    .needs_notification(&*notify_mem)
                    .map_err(|e| std::io::Error::other(format!("needs_notification RX: {e:?}")))?
            };
            if needs {
                vring
                    .signal_used_queue()
                    .map_err(|e| std::io::Error::other(format!("signal_used_queue RX: {e:?}")))?;
            }
        }

        Ok(())
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
            id if id == RX_EFD_ID => {
                // Stdin reader thread signaled "bytes arrived".
                // Drain the eventfd counter so epoll doesn't
                // fire again until the next bytes, then process
                // the RX queue (it may still be empty — the
                // guest's buffer availability is independent of
                // our byte arrival).
                if let Err(e) = self.rx_eventfd.read() {
                    log::warn!("console: rx_eventfd drain failed: {e}");
                }
                let vring = &vrings[RX_QUEUE as usize];
                self.process_rx_queue(vring)?;
            }
            RX_QUEUE => {
                // Host → guest. The guest is offering empty
                // buffers we can fill on demand. Opportunistic
                // drain: if the FIFO has bytes, deliver them;
                // otherwise leave the chains in the avail ring
                // for the next kick.
                //
                // Production stdin plumbing (stdin reader thread
                // + eventfd wake-up) is queued as the next C-10
                // v2 commit. Until then, the FIFO is populated
                // via `push_rx_bytes()` — exercised by tests and
                // available for orchestration glue.
                let vring = &vrings[RX_QUEUE as usize];
                self.process_rx_queue(vring)?;
            }
            other => {
                log::warn!("console: unexpected device_event={other}; ignoring");
            }
        }
        Ok(())
    }

    /// Hand the daemon a cloned copy of our exit-event pair so
    /// the main control loop can wake the VringEpollHandler
    /// thread(s) when `serve()` returns. Without this override
    /// the default `None` leaves the worker thread blocked in
    /// `epoll_wait` forever on frontend disconnect, which in
    /// turn hangs the subprocess at shutdown.
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

/// Loop driving host → guest byte flow. Reads from `source`
/// into a small stack buffer, appends each chunk to `fifo`, and
/// writes `1` to `wake` so the daemon's epoll wakes and routes
/// to `handle_event(RX_EFD_ID)`. Returns on EOF or on a
/// non-EINTR read error.
///
/// Generic over `Read` so tests can feed a pipe or `Cursor`
/// instead of stdin. Production path wraps `std::io::stdin()`.
fn run_reader_loop<R: Read>(
    mut source: R,
    fifo: Arc<Mutex<VecDeque<u8>>>,
    wake: Arc<EventFd>,
) {
    let mut buf = [0u8; 4096];
    loop {
        match source.read(&mut buf) {
            Ok(0) => {
                log::info!("console reader: stdin EOF");
                break;
            }
            Ok(n) => {
                // Hold the lock only long enough to append.
                match fifo.lock() {
                    Ok(mut guard) => guard.extend(&buf[..n]),
                    Err(_) => {
                        log::error!("console reader: rx_fifo mutex poisoned");
                        break;
                    }
                }
                // Best-effort wake. NONBLOCK so a saturated
                // counter doesn't stall the reader; daemon
                // drains on every handle_event dispatch.
                if let Err(e) = wake.write(1) {
                    if e.raw_os_error() != Some(libc::EAGAIN) {
                        log::warn!("console reader: eventfd write failed: {e}");
                    }
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => {
                log::warn!("console reader: stdin read error: {e}");
                break;
            }
        }
    }
}

/// Entry point for `uml-launcher backend console --socket <path>`.
///
/// Creates the backend state + stdin reader thread + daemon,
/// registers the backend-side RX eventfd with the daemon's
/// epoll so stdin arrivals wake the event loop, applies the
/// seccomp filter, and calls `serve()`. Returns when the
/// frontend disconnects or the process receives a fatal signal.
pub fn run(args: BackendConsoleArgs) -> Result<i32> {
    let socket = args.common.socket;
    tracing::info!(socket = %socket.display(), "console backend: starting vhost-user daemon");

    let backend = Arc::new(RwLock::new(
        ConsoleBackend::new().context("constructing console backend")?,
    ));

    // Grab shared handles before we hand `backend` to the
    // daemon — the reader thread needs to outlive the run()
    // stack frame but must not carry the whole `Arc<RwLock>`.
    let (rx_fifo, rx_event) = {
        let guard = backend.read().expect("rx init: backend lock poisoned");
        (guard.rx_fifo_handle(), guard.rx_eventfd_handle())
    };

    // Spawn the reader BEFORE seccomp applies, so thread-
    // creation syscalls (clone3, set_robust_list, …) aren't
    // filtered. The thread's steady-state syscalls (read,
    // write-to-eventfd, futex) are in the seccomp baseline.
    let reader_thread = std::thread::Builder::new()
        .spawn(move || {
            let stdin = std::io::stdin();
            let handle = stdin.lock();
            run_reader_loop(handle, rx_fifo, rx_event);
        })
        .context("spawning console stdin reader thread")?;
    // We deliberately do not join() — the thread exits on
    // stdin EOF or when the process dies. Dropping the handle
    // detaches it.
    let _ = reader_thread;

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());

    let mut daemon = VhostUserDaemon::new(
        "uml-launcher-backend-console".to_string(),
        backend.clone(),
        mem,
    )
    .map_err(|e| anyhow::anyhow!("constructing VhostUserDaemon: {e:?}"))?;

    // Register the backend-side RX eventfd with the daemon's
    // epoll. The daemon will route events on that fd through
    // handle_event() with device_event = RX_EFD_ID.
    let handlers = daemon.get_epoll_handlers();
    let rx_event_handle = {
        let guard = backend.read().expect("rx register: backend lock poisoned");
        guard.rx_eventfd_handle()
    };
    if let Some(h) = handlers.first() {
        h.register_listener(
            rx_event_handle.as_raw_fd(),
            EventSet::IN,
            RX_EFD_ID as u64,
        )
        .map_err(|e| anyhow::anyhow!("register RX eventfd: {e:?}"))?;
    } else {
        return Err(anyhow::anyhow!(
            "VhostUserDaemon returned no epoll handlers"
        ));
    }

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

        let mut backend = ConsoleBackend::with_sink(Box::new(sink)).expect("backend");

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
            ConsoleBackend::with_sink(Box::new(CaptureSink(captured.clone()))).expect("backend");
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
    fn rx_delivers_pushed_bytes_into_descriptor_buffer() {
        // Stand up the backend with an in-memory (unused) sink.
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
            ConsoleBackend::with_sink(Box::new(CaptureSink(sunk.clone()))).expect("backend");

        let mem = make_mem();
        backend.update_memory(mem.clone()).expect("update_memory");

        // Stage host → guest bytes.
        let payload = b"hi guest!";
        backend.push_rx_bytes(payload);

        // Stand up an RX vring. Guest offers one descriptor of
        // 64 bytes at 0x3000, marked WRITE so the backend knows
        // it's a "here's a buffer for you to fill" slot.
        // Flag 0x2 = VRING_DESC_F_WRITE.
        let vring = setup_vring(&mem, 16);
        {
            let g = mem.memory();
            write_desc(
                &g,
                /* desc_table */ 0x0000,
                /* desc_idx   */ 0,
                /* addr       */ 0x3000,
                /* len        */ 64,
                /* flags      */ 0x2,
                /* next       */ 0,
            );
            publish_avail(&g, /* avail_ring */ 0x1000, /* avail_idx */ 0, 0);
        }

        backend.process_rx_queue(&vring).expect("process rx");

        // Read back the bytes the backend wrote into guest memory.
        use vm_memory::Bytes;
        let mut got = vec![0u8; payload.len()];
        mem.memory()
            .read_slice(&mut got, GuestAddress(0x3000))
            .unwrap();

        assert_eq!(got, payload);
    }

    #[test]
    fn rx_no_bytes_is_noop_on_queue() {
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
            ConsoleBackend::with_sink(Box::new(CaptureSink(sunk.clone()))).expect("backend");

        let mem = make_mem();
        backend.update_memory(mem.clone()).expect("update_memory");
        // No push_rx_bytes — fifo stays empty.

        let vring = setup_vring(&mem, 16);
        {
            let g = mem.memory();
            write_desc(&g, 0x0000, 0, 0x3000, 64, 0x2, 0);
            publish_avail(&g, 0x1000, 0, 0);
        }

        // Should complete without error and without consuming
        // the avail-ring entry. (We don't assert the avail entry
        // remains — virtio-queue's cursor semantics vary; the
        // important property is "no panic, no spurious write".)
        backend.process_rx_queue(&vring).expect("process rx no-data");
    }

    #[test]
    fn reader_loop_moves_bytes_and_wakes_eventfd() {
        // Run the reader loop against a Cursor<Vec<u8>> source.
        // It should push all bytes into the FIFO and bump the
        // eventfd counter at least once before the source
        // reports EOF.
        use std::io::Cursor;
        let wake = Arc::new(EventFd::new(EFD_NONBLOCK).unwrap());
        let fifo = Arc::new(Mutex::new(VecDeque::<u8>::new()));
        let source = Cursor::new(b"hello reader loop".to_vec());

        let wake_clone = wake.clone();
        let fifo_clone = fifo.clone();
        std::thread::spawn(move || {
            run_reader_loop(source, fifo_clone, wake_clone);
        })
        .join()
        .expect("reader thread join");

        let got: Vec<u8> = fifo.lock().unwrap().iter().copied().collect();
        assert_eq!(got, b"hello reader loop");

        // EventFd counter: NONBLOCK read returns the counter
        // and resets to 0. Should be > 0 since at least one
        // write(1) happened before the Cursor's EOF.
        let counter = wake.read().expect("read eventfd");
        assert!(counter > 0, "eventfd should have been bumped");
    }

    #[test]
    fn reader_loop_handles_empty_source() {
        // Zero-byte source: loop should exit on EOF without
        // touching the FIFO or the eventfd.
        use std::io::Cursor;
        let wake = Arc::new(EventFd::new(EFD_NONBLOCK).unwrap());
        let fifo = Arc::new(Mutex::new(VecDeque::<u8>::new()));
        let source = Cursor::new(Vec::<u8>::new());

        let wake_clone = wake.clone();
        let fifo_clone = fifo.clone();
        std::thread::spawn(move || {
            run_reader_loop(source, fifo_clone, wake_clone);
        })
        .join()
        .expect("reader thread join");

        assert!(fifo.lock().unwrap().is_empty());
        // EventFd NONBLOCK read returns EAGAIN when counter == 0;
        // that's the signal nothing was written.
        match wake.read() {
            Ok(n) => assert_eq!(n, 0, "no bytes should have been signaled"),
            Err(e) => assert_eq!(
                e.raw_os_error(),
                Some(libc::EAGAIN),
                "EAGAIN on empty eventfd"
            ),
        }
    }

    #[test]
    fn rx_partial_fill_when_buffer_smaller_than_fifo() {
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
            ConsoleBackend::with_sink(Box::new(CaptureSink(sunk.clone()))).expect("backend");
        let mem = make_mem();
        backend.update_memory(mem.clone()).expect("update_memory");

        // Push 100 bytes; offer a single 16-byte descriptor.
        let payload: Vec<u8> = (0..100u8).collect();
        backend.push_rx_bytes(&payload);

        let vring = setup_vring(&mem, 16);
        {
            let g = mem.memory();
            write_desc(&g, 0x0000, 0, 0x3000, 16, 0x2, 0);
            publish_avail(&g, 0x1000, 0, 0);
        }

        backend.process_rx_queue(&vring).expect("partial fill");

        // Guest sees the first 16 bytes.
        use vm_memory::Bytes;
        let mut got = vec![0u8; 16];
        mem.memory()
            .read_slice(&mut got, GuestAddress(0x3000))
            .unwrap();
        assert_eq!(got, &payload[..16]);

        // 84 bytes remain in the FIFO for the next kick.
        let remaining = backend.rx_fifo.lock().unwrap().len();
        assert_eq!(remaining, 84);
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
            ConsoleBackend::with_sink(Box::new(CaptureSink(sunk.clone()))).expect("backend");

        let mem = make_mem();
        // Don't call update_memory() — backend.mem stays None.
        let vring = setup_vring(&mem, 16);

        // Should not panic even though no memory is set.
        backend.process_tx_queue(&vring).expect("no-op");
        assert!(sunk.lock().unwrap().is_empty());
    }

}
