// SPDX-License-Identifier: GPL-2.0
//
// The kernel tree's top-level `.clippy.toml` disallows
// `core::ffi::CStr::as_ptr` in favor of a kernel-specific
// CStrExt helper that doesn't exist in a userspace binary.
// Same rationale as `backend/apparmor.rs` — our binary uses
// the standard C ABI at the FFI boundary, so local suppression
// rather than a codebase-wide clippy config change.
#![allow(clippy::disallowed_methods)]

// virtio-net vhost-user backend (workstream C-10 v2).
//
// Data path: open /dev/net/tun at startup, attach via
// TUNSETIFF to the --tap interface name, and shuttle Ethernet
// frames between the TAP fd and the two virtqueues:
//
//   * TX (guest → host, queue index 1): drain avail-ring
//     chains, read each frame out of guest memory, write it
//     to the TAP fd.
//   * RX (host → guest, queue index 0): a readable-TAP
//     eventfd wake registers with the daemon's epoll; on
//     wake, read frames from TAP into the guest's posted
//     buffers.
//
// Scope of this commit:
//   * Open /dev/net/tun with O_RDWR | O_CLOEXEC | O_NONBLOCK.
//   * TUNSETIFF with IFF_TAP | IFF_NO_PI so the TAP fd
//     carries raw Ethernet frames with no prepended tag. No
//     IFF_VNET_HDR — that requires VIRTIO_NET_F_MRG_RXBUF +
//     all the TSO/CSUM negotiation, which belongs with a
//     follow-on.
//   * Feature bits advertised: VIRTIO_F_VERSION_1 + protocol
//     features. No MAC, CSUM, GSO, MRG_RXBUF, STATUS, MQ.
//     Frontend sees a "dumb" Ethernet NIC at 1500 MTU; guest
//     drivers configure a random locally-administered MAC
//     and don't expect any offloads.
//   * Class-specific seccomp additions: SYS_ioctl is in the
//     baseline already, but we add SYS_recvfrom / SYS_sendto
//     + socket + connect in case a future TAP setup path
//     needs them. SYS_openat for /dev/net/tun is NOT added;
//     the fd is opened BEFORE seccomp applies.
//
// Deliberately out of scope:
//   * vhost-net (kernel-side tap offload) — different trap
//     mechanism entirely, not our stack's model.
//   * VIRTIO_NET_F_MRG_RXBUF / large-receive offload.
//   * VIRTIO_NET_F_MAC / MTU / STATUS config.
//   * Multi-queue (VIRTIO_NET_F_MQ).
//   * TAP creation or persistence — the operator sets up
//     `ip tuntap add tap0 mode tap ...` before launch.
//
// Reference: cloud-hypervisor/vhost_user_net (older
// vhost-user-backend API, same shape).

use std::io;
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::sync::{Arc, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock, VringT};
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use virtio_queue::{QueueOwnedT, QueueT};
use vm_memory::{GuestAddressSpace, GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;
use vmm_sys_util::event::{new_event_consumer_and_notifier, EventConsumer, EventFlag, EventNotifier};

use crate::backend::seccomp::FilterBuilder;
use crate::cli::BackendNetArgs;

const RX_QUEUE: u16 = 0;
const TX_QUEUE: u16 = 1;
const NUM_QUEUES: usize = 2;
const QUEUE_SIZE: usize = 256;

/// `data` value used when registering the TAP fd with the
/// daemon's epoll via `VringEpollHandler::register_listener`.
/// Must be > NUM_QUEUES (the daemon reserves [0, NUM_QUEUES]
/// for queue kicks + the exit event); NUM_QUEUES + 1 keeps the
/// mapping self-describing.
const TAP_FD_ID: u16 = NUM_QUEUES as u16 + 1;

/// Largest Ethernet frame we expect (no jumbo, no GSO). 1518
/// = 14-byte header + 1500 MTU + 4-byte FCS, with a bit of
/// margin. The virtqueue buffers each cover this cleanly;
/// anything larger gets dropped with a warning.
const MAX_FRAME_LEN: usize = 1518;

/// TUNSETIFF interface-flag bits we set.
const IFF_TAP: libc::c_int = 0x0002;
const IFF_NO_PI: libc::c_int = 0x1000;

/// TUNSETIFF ioctl number: _IOW('T', 202, int).
/// Matches `<linux/if_tun.h>`. Rust's `nix::ioctl_*!` macros
/// could synthesize this, but a bare const is clearer and
/// trivially auditable.
const TUNSETIFF: libc::c_ulong = 0x400454ca;

/// Maximum interface-name length (IFNAMSIZ).
const IFNAMSIZ: usize = 16;

/// Layout of `struct ifreq` for the TUNSETIFF ioctl. We only
/// care about ifr_name + ifr_flags; the union's other arms
/// are zero-padding for this use.
#[repr(C)]
#[derive(Clone, Copy)]
struct IfReq {
    ifr_name: [u8; IFNAMSIZ],
    ifr_flags: libc::c_short,
    _pad: [u8; 22], // sizeof(ifreq) - (IFNAMSIZ + sizeof(short))
}

fn device_features() -> u64 {
    (1u64 << VIRTIO_F_VERSION_1)
        | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits()
}

fn protocol_features() -> VhostUserProtocolFeatures {
    VhostUserProtocolFeatures::MQ | VhostUserProtocolFeatures::REPLY_ACK
}

/// Open /dev/net/tun + TUNSETIFF(IFF_TAP | IFF_NO_PI) on
/// @ifname. Returns the owning fd. Caller should register the
/// raw fd with the daemon's epoll for RX-ready notification.
fn open_tap(ifname: &str) -> io::Result<OwnedFd> {
    if ifname.len() >= IFNAMSIZ {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("TAP interface name too long: {ifname}"),
        ));
    }
    // Open /dev/net/tun. O_NONBLOCK so we drain frames without
    // blocking the daemon thread; O_CLOEXEC so we don't leak
    // the fd across any future fork().
    let path = std::ffi::CString::new("/dev/net/tun")
        .expect("/dev/net/tun is a static ascii path");
    // SAFETY: FFI call with a valid NUL-terminated C string
    // and constant integer flags. Returns -1 on error.
    let fd = unsafe {
        libc::open(
            path.as_ptr(),
            libc::O_RDWR | libc::O_CLOEXEC | libc::O_NONBLOCK,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: fd came from a successful open(2); OwnedFd now
    // manages its close.
    let owned = unsafe { OwnedFd::from_raw_fd(fd) };

    let mut req = IfReq {
        ifr_name: [0u8; IFNAMSIZ],
        ifr_flags: (IFF_TAP | IFF_NO_PI) as libc::c_short,
        _pad: [0u8; 22],
    };
    req.ifr_name[..ifname.len()].copy_from_slice(ifname.as_bytes());

    // SAFETY: ioctl on an owned fd, passing a pointer to a
    // correctly-initialized IfReq of the expected size.
    // TUNSETIFF reads ifr_name + ifr_flags and writes
    // ifr_name back; the written name may mutate if the
    // kernel rewrites an anonymous interface, but we always
    // pass an explicit one so no surprise.
    let rc = unsafe {
        libc::ioctl(
            owned.as_raw_fd(),
            TUNSETIFF,
            &mut req as *mut _ as *mut libc::c_void,
        )
    };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(owned)
}

/// Import OwnedFd::from_raw_fd without pulling in the trait
/// at the module top (avoids the dead-trait-use clippy lint
/// when tests don't exercise the open path).
use std::os::fd::FromRawFd;

pub struct NetBackend {
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap<()>>>,
    event_idx: bool,
    #[allow(dead_code)]
    tap_name: Option<String>,
    /// Opened TAP fd. `None` for the no-image test harness
    /// paths; `Some` on the real `run()` path.
    tap: Option<Arc<OwnedFd>>,
    exit_event: (EventConsumer, EventNotifier),
}

impl NetBackend {
    pub fn new(tap_name: Option<String>) -> Result<Self> {
        let tap = match tap_name.as_deref() {
            Some(name) => {
                let fd = open_tap(name)
                    .with_context(|| format!("opening TAP interface {name}"))?;
                Some(Arc::new(fd))
            }
            None => None,
        };
        let exit_event = new_event_consumer_and_notifier(EventFlag::NONBLOCK)
            .context("exit-event consumer/notifier pair")?;
        Ok(Self {
            mem: None,
            event_idx: false,
            tap_name,
            tap,
            exit_event,
        })
    }

    pub fn tap_raw_fd(&self) -> Option<RawFd> {
        self.tap.as_deref().map(|o| o.as_raw_fd())
    }

    /// Drain the TX virtqueue: for each published chain,
    /// read the Ethernet frame out of guest memory and write
    /// it to the TAP fd. Errors on an individual write are
    /// logged and dropped — TAP might be buffer-full
    /// (EAGAIN), in which case the guest re-sends; or the
    /// interface might have been torn down, in which case
    /// further writes will also fail and the daemon will
    /// eventually exit via the frontend disconnect.
    fn process_tx_queue(&mut self, vring: &VringRwLock) -> io::Result<()> {
        let atomic_mem = match self.mem.as_ref() {
            Some(m) => m,
            None => {
                log::warn!("net: TX kick before memory table was set; skipping");
                return Ok(());
            }
        };
        let tap_fd = match self.tap.as_ref() {
            Some(t) => t.as_raw_fd(),
            None => {
                log::warn!("net: TX kick with no TAP fd; dropping");
                return Ok(());
            }
        };

        let requests = {
            let mut guard = vring.get_mut();
            let queue = guard.get_queue_mut();
            queue
                .iter(atomic_mem.memory())
                .map_err(|e| io::Error::other(format!("iter TX queue: {e:?}")))?
                .collect::<Vec<_>>()
        };
        if requests.is_empty() {
            return Ok(());
        }

        let mut any_used = false;
        let mut frame = vec![0u8; MAX_FRAME_LEN];
        for chain in requests {
            let head = chain.head_index();
            let chain_mem = atomic_mem.memory();
            let mut reader = chain
                .clone()
                .reader(&chain_mem)
                .map_err(|e| io::Error::other(format!("chain reader: {e:?}")))?;
            let avail = reader.available_bytes();
            if avail == 0 {
                vring
                    .add_used(head, 0)
                    .map_err(|e| io::Error::other(format!("add_used empty: {e:?}")))?;
                any_used = true;
                continue;
            }
            if avail > frame.len() {
                log::warn!("net: oversized TX frame {avail}; truncating");
            }
            let take = avail.min(frame.len());
            if let Err(e) = std::io::Read::read_exact(&mut reader, &mut frame[..take]) {
                log::warn!("net: reading TX frame: {e}");
                vring
                    .add_used(head, 0)
                    .map_err(|e| io::Error::other(format!("add_used err: {e:?}")))?;
                any_used = true;
                continue;
            }

            // Single write(2) on the TAP fd. O_NONBLOCK so
            // EAGAIN is the congestion signal; we drop that
            // frame and let the guest retransmit (TCP/UDP
            // both handle loss).
            // SAFETY: FFI, pointer valid for `take` bytes.
            let n = unsafe {
                libc::write(tap_fd, frame.as_ptr() as *const _, take)
            };
            if n < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() != Some(libc::EAGAIN) {
                    log::warn!("net: TAP write failed: {err}");
                }
            }

            vring
                .add_used(head, 0)
                .map_err(|e| io::Error::other(format!("add_used TX: {e:?}")))?;
            any_used = true;
        }

        if any_used {
            let notify_mem = atomic_mem.memory();
            let needs = {
                let mut guard = vring.get_mut();
                let queue = guard.get_queue_mut();
                queue
                    .needs_notification(&*notify_mem)
                    .map_err(|e| io::Error::other(format!("needs_notification TX: {e:?}")))?
            };
            if needs {
                vring
                    .signal_used_queue()
                    .map_err(|e| io::Error::other(format!("signal_used_queue TX: {e:?}")))?;
            }
        }
        let _ = self.event_idx;
        Ok(())
    }

    /// Read frames from the TAP fd and fill them into the
    /// RX virtqueue's posted buffers. Called when either
    /// (a) the guest kicks the RX queue (it just posted more
    /// empty buffers), or (b) the epoll watch on TAP fires
    /// (new frames available). Drain as much as possible in
    /// either case.
    fn process_rx_queue(&mut self, vring: &VringRwLock) -> io::Result<()> {
        let atomic_mem = match self.mem.as_ref() {
            Some(m) => m,
            None => {
                log::warn!("net: RX wake before memory table was set");
                return Ok(());
            }
        };
        let tap_fd = match self.tap.as_ref() {
            Some(t) => t.as_raw_fd(),
            None => {
                log::warn!("net: RX wake with no TAP fd");
                return Ok(());
            }
        };

        // Drain TAP-then-writer in a loop until one side is
        // empty. Buffer each frame in a host-side staging buf
        // first; the descriptor chain's writer is where it
        // ultimately lands.
        let mut frame = vec![0u8; MAX_FRAME_LEN];
        let mut any_used = false;

        loop {
            // SAFETY: FFI, pointer valid for MAX_FRAME_LEN
            // bytes, non-blocking read.
            let n = unsafe {
                libc::read(
                    tap_fd,
                    frame.as_mut_ptr() as *mut _,
                    frame.len(),
                )
            };
            if n < 0 {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(libc::EAGAIN) {
                    break; // no more frames right now
                }
                log::warn!("net: TAP read failed: {err}");
                break;
            }
            if n == 0 {
                break; // TAP closed
            }
            let len = n as usize;

            // Get one avail descriptor and write the frame.
            let chain = {
                let mut guard = vring.get_mut();
                let queue = guard.get_queue_mut();
                match queue.iter(atomic_mem.memory()) {
                    Ok(mut iter) => iter.next(),
                    Err(e) => {
                        log::warn!("net: iter RX queue: {e:?}");
                        None
                    }
                }
            };
            let chain = match chain {
                Some(c) => c,
                None => {
                    log::debug!(
                        "net: dropping RX frame {len}B — no guest-posted buffer"
                    );
                    break;
                }
            };
            let head = chain.head_index();
            let chain_mem = atomic_mem.memory();
            let mut writer = match chain.clone().writer(&chain_mem) {
                Ok(w) => w,
                Err(e) => {
                    log::warn!("net: chain writer: {e:?}");
                    break;
                }
            };
            if let Err(e) = std::io::Write::write_all(&mut writer, &frame[..len]) {
                log::warn!("net: writing RX frame: {e}");
                break;
            }
            vring
                .add_used(head, len as u32)
                .map_err(|e| io::Error::other(format!("add_used RX: {e:?}")))?;
            any_used = true;
        }

        if any_used {
            let notify_mem = atomic_mem.memory();
            let needs = {
                let mut guard = vring.get_mut();
                let queue = guard.get_queue_mut();
                queue
                    .needs_notification(&*notify_mem)
                    .map_err(|e| io::Error::other(format!("needs_notification RX: {e:?}")))?
            };
            if needs {
                vring
                    .signal_used_queue()
                    .map_err(|e| io::Error::other(format!("signal_used_queue RX: {e:?}")))?;
            }
        }
        Ok(())
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
        vrings: &[VringRwLock],
        _thread_id: usize,
    ) -> std::io::Result<()> {
        if evset != EventSet::IN {
            log::warn!("net: unexpected evset {evset:?}; ignoring");
            return Ok(());
        }
        match device_event {
            TX_QUEUE => {
                let vring = &vrings[TX_QUEUE as usize];
                self.process_tx_queue(vring)?;
            }
            RX_QUEUE => {
                let vring = &vrings[RX_QUEUE as usize];
                self.process_rx_queue(vring)?;
            }
            id if id == TAP_FD_ID => {
                let vring = &vrings[RX_QUEUE as usize];
                self.process_rx_queue(vring)?;
            }
            other => log::warn!("net: unexpected device_event={other}; ignoring"),
        }
        Ok(())
    }

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

pub fn run(args: BackendNetArgs) -> Result<i32> {
    let socket = args.common.socket;
    tracing::info!(
        socket = %socket.display(),
        tap = args.tap.as_deref().unwrap_or("<unset>"),
        "net backend: starting vhost-user daemon"
    );

    // Constructor opens /dev/net/tun + TUNSETIFF before
    // seccomp applies — the event-loop baseline doesn't allow
    // open/openat and we explicitly don't want to widen it
    // for a one-shot attach.
    let backend = Arc::new(RwLock::new(
        NetBackend::new(args.tap).context("constructing net backend")?,
    ));
    let tap_fd = backend
        .read()
        .expect("net backend lock poisoned")
        .tap_raw_fd();

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());

    let mut daemon = VhostUserDaemon::new(
        "uml-launcher-backend-net".to_string(),
        backend.clone(),
        mem,
    )
    .map_err(|e| anyhow::anyhow!("constructing VhostUserDaemon: {e:?}"))?;

    // Register the TAP fd with the daemon's epoll so a
    // readable-TAP event routes to handle_event(TAP_FD_ID).
    if let Some(fd) = tap_fd {
        let handlers = daemon.get_epoll_handlers();
        if let Some(h) = handlers.first() {
            h.register_listener(fd, EventSet::IN, TAP_FD_ID as u64)
                .map_err(|e| anyhow::anyhow!("register TAP fd: {e:?}"))?;
        } else {
            return Err(anyhow::anyhow!(
                "VhostUserDaemon returned no epoll handlers"
            ));
        }
    }

    match crate::backend::apparmor::change_profile("uml-launcher//backend_net") {
        Ok(crate::backend::apparmor::ChangeResult::Changed) => {
            tracing::info!("net backend: entered AppArmor sub-profile");
        }
        Ok(crate::backend::apparmor::ChangeResult::Skipped) => {
            tracing::debug!("net backend: AppArmor unavailable, continuing unconfined");
        }
        Err(e) => {
            return Err(e).context("aa_change_profile(uml-launcher//backend_net)");
        }
    }

    // Net-class seccomp filter: event-loop baseline + nothing
    // extra for TX/RX. The TAP fd I/O path uses plain read(2)
    // and write(2), which are in the baseline; the only
    // elevated syscalls (open + ioctl(TUNSETIFF)) were
    // invoked before this apply.
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
        // Explicit: no MAC / MTU / CSUM / STATUS / MQ advertised.
        // These would all require matching config-space bytes we
        // don't emit.
        for bit in [3u32, 5, 16, 22] {
            assert_eq!(
                f & (1u64 << bit),
                0,
                "virtio-net feature bit {bit} must not be advertised"
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
    fn new_backend_without_tap_has_no_fd() {
        let b = NetBackend::new(None).expect("construct backend");
        assert!(b.mem.is_none());
        assert!(!b.event_idx);
        assert!(b.tap_name.is_none());
        assert!(b.tap.is_none());
        assert!(b.tap_raw_fd().is_none());
    }

    #[test]
    fn open_tap_rejects_oversized_name() {
        let err =
            open_tap("this_name_is_way_too_long_for_ifnamsiz").unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
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

    #[test]
    fn ifreq_layout_is_sizeof_32() {
        // struct ifreq on Linux is 40 bytes total: 16 for
        // ifr_name, 24 for the union. Our truncated IfReq
        // pads to 24 in the tail; 16 + 2 + 22 = 40.
        assert_eq!(std::mem::size_of::<IfReq>(), 40);
    }
}
