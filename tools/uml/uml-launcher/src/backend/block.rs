// SPDX-License-Identifier: GPL-2.0
//
// virtio-blk vhost-user backend.
//
// Data path: opens a file-backed disk image at startup, handles
// VIRTIO_BLK_T_IN / OUT / FLUSH / GET_ID request types, enforces
// `--read-only` by returning VIRTIO_BLK_S_UNSUPP on writes /
// flushes, writes the 1-byte status descriptor back to the
// guest. Single request queue, no multi-queue.
//
// Implemented request set:
//   * IN  (read)   : preadv from image fd into virtqueue writer.
//   * OUT (write)  : pwritev from virtqueue reader into image fd.
//   * FLUSH        : fdatasync. Write-only, returns UNSUPP on
//                    --read-only.
//   * GET_ID       : 20-byte device identifier (the image path's
//                    basename truncated; lets guest tools like
//                    `blkid` distinguish disks).
//   * status byte  : VIRTIO_BLK_S_OK / _IOERR / _UNSUPP.
//
// Unsupported feature-bit-gated request set:
//   * DISCARD, WRITE_ZEROES, SECURE_ERASE - each adds its own
//     request type, response struct, and feature bit.
//   * ZONED - append-only disk model.
//   * O_DIRECT - optimization; the initial data path uses the
//     host page cache. O_DIRECT requires sector-aligned user
//     buffers which need posix_memalign-equivalent allocation.
//
// Reference:
//   - virtio 1.1 section5.2 (virtio-blk device)
//   - cloud-hypervisor/vhost_user_block/ (different vhost-user-
//     backend version, same shape)

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::{FileExt, OpenOptionsExt};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, RwLock};

use anyhow::{Context, Result};
use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock, VringT};
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use virtio_bindings::virtio_blk::{
    VIRTIO_BLK_F_BLK_SIZE, VIRTIO_BLK_F_FLUSH, VIRTIO_BLK_F_RO, VIRTIO_BLK_F_SEG_MAX,
    VIRTIO_BLK_F_SIZE_MAX, VIRTIO_BLK_S_IOERR, VIRTIO_BLK_S_OK, VIRTIO_BLK_S_UNSUPP,
    VIRTIO_BLK_T_FLUSH, VIRTIO_BLK_T_GET_ID, VIRTIO_BLK_T_IN, VIRTIO_BLK_T_OUT,
};
use virtio_queue::{QueueOwnedT, QueueT};
use vm_memory::{ByteValued, GuestAddressSpace, GuestMemoryAtomic, GuestMemoryMmap, Le32, Le64};
use vmm_sys_util::epoll::EventSet;
use vmm_sys_util::event::{
    new_event_consumer_and_notifier, EventConsumer, EventFlag, EventNotifier,
};

use crate::backend::seccomp::FilterBuilder;
use crate::cli::BackendBlockArgs;

const REQ_QUEUE: u16 = 0;
const NUM_QUEUES: usize = 1;
const QUEUE_SIZE: usize = 256;

/// virtio-blk sector size. Hard-coded at 512 by the spec - the
/// protocol uses `sector` as a 512-byte offset regardless of
/// the logical block size exposed via VIRTIO_BLK_F_BLK_SIZE.
const SECTOR_SIZE: u64 = 512;

/// Largest single-segment transfer we advertise via
/// VIRTIO_BLK_F_SIZE_MAX. 128 KiB is the conventional cap for
/// virtio-blk + a comfortable fit for guest page-cache chunks.
const MAX_SEG_SIZE: u32 = 128 * 1024;

/// Largest number of descriptors per request (VIRTIO_BLK_F_SEG_MAX).
/// Matches the queue depth minus reserved slots for hdr+status.
const MAX_SEG_COUNT: u32 = (QUEUE_SIZE as u32) - 2;

/// Length of the VIRTIO_BLK_T_GET_ID response buffer. Spec-
/// mandated; guest tools (blkid, udev) read exactly 20 bytes.
const VIRTIO_BLK_ID_BYTES: usize = 20;

/// virtio_blk request header, guest -> host. Bytes laid out per
/// virtio 1.1 section5.2.6. Backed by ByteValued so vm_memory can
/// `read_obj` it straight out of guest memory with no copying.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct VirtioBlkReqHeader {
    req_type: Le32,
    reserved: Le32,
    sector: Le64,
}
// SAFETY: #[repr(C)] + all fields are POD (Le32/Le64 wrappers
// over u32/u64). No padding between the three fields at these
// sizes + alignments. Matches the on-wire virtio_blk_outhdr
// layout exactly.
unsafe impl ByteValued for VirtioBlkReqHeader {}

/// virtio_blk config space, advertised via
/// VhostUserBackend::get_config(). Fields not explicitly
/// populated are zero, which is the spec-allowed "field not
/// meaningful" signal for the feature bits we *don't* set.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
struct VirtioBlkConfig {
    capacity: Le64,              // in 512-byte sectors
    size_max: Le32,              // VIRTIO_BLK_F_SIZE_MAX
    seg_max: Le32,               // VIRTIO_BLK_F_SEG_MAX
    geometry_cylinders: [u8; 2], // unused (no GEOMETRY feature)
    geometry_heads: u8,
    geometry_sectors: u8,
    blk_size: Le32, // VIRTIO_BLK_F_BLK_SIZE
                    // Trailing fields (topology, writeback, num_queues, discard,
                    // write_zeroes) are all behind feature bits we don't set;
                    // leave them out so the config blob is exactly the size of
                    // the features we DO advertise. The spec allows a shorter
                    // blob when trailing features are unset.
}
// SAFETY: #[repr(C)] + all fields are POD. Layout matches
// struct virtio_blk_config truncated at blk_size.
unsafe impl ByteValued for VirtioBlkConfig {}

fn device_features(read_only: bool) -> u64 {
    let mut f = (1u64 << VIRTIO_F_VERSION_1)
        | (1u64 << VIRTIO_BLK_F_SIZE_MAX)
        | (1u64 << VIRTIO_BLK_F_SEG_MAX)
        | (1u64 << VIRTIO_BLK_F_BLK_SIZE)
        | (1u64 << VIRTIO_BLK_F_FLUSH)
        | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits();
    if read_only {
        // RO is advertised ONLY when the user asked for it;
        // it's a hint to the guest to mount read-only, not a
        // security gate (the backend's own read_only check is
        // the actual gate).
        f |= 1u64 << VIRTIO_BLK_F_RO;
    }
    f
}

fn protocol_features() -> VhostUserProtocolFeatures {
    VhostUserProtocolFeatures::MQ
        | VhostUserProtocolFeatures::REPLY_ACK
        | VhostUserProtocolFeatures::CONFIG
}

/// File-backed disk image + immutable per-disk metadata derived
/// from the image at open time.
struct ImageBacking {
    file: Mutex<File>,
    capacity_sectors: u64,
    read_only: bool,
    /// 20-byte device id returned on VIRTIO_BLK_T_GET_ID -
    /// computed from the image path's basename so two different
    /// disks attached to the same guest get different ids.
    device_id: [u8; VIRTIO_BLK_ID_BYTES],
}

impl ImageBacking {
    fn open(path: &std::path::Path, read_only: bool) -> Result<Self> {
        let mut opts = OpenOptions::new();
        opts.read(true);
        if !read_only {
            opts.write(true);
        }
        // Do NOT use O_DIRECT here. O_DIRECT requires sector-
        // aligned user buffers which the vm_memory ByteBuffer
        // path doesn't guarantee; adopting it would require
        // per-request bounce buffers allocated via posix_memalign.
        // The host page cache is the correct default for this backend.
        opts.custom_flags(libc::O_CLOEXEC);
        let file = opts
            .open(path)
            .with_context(|| format!("opening image {}", path.display()))?;
        let metadata = file
            .metadata()
            .with_context(|| format!("fstat image {}", path.display()))?;
        let bytes = metadata.len();
        let capacity_sectors = bytes / SECTOR_SIZE;

        let mut device_id = [0u8; VIRTIO_BLK_ID_BYTES];
        if let Some(basename) = path.file_name().and_then(|s| s.to_str()) {
            let src = basename.as_bytes();
            let n = src.len().min(VIRTIO_BLK_ID_BYTES);
            device_id[..n].copy_from_slice(&src[..n]);
        }

        Ok(Self {
            file: Mutex::new(file),
            capacity_sectors,
            read_only,
            device_id,
        })
    }

    fn pread_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<usize> {
        let file = self.file.lock().expect("image file mutex poisoned");
        file.read_at(buf, offset)
    }

    fn pwrite_at(&self, buf: &[u8], offset: u64) -> std::io::Result<usize> {
        let file = self.file.lock().expect("image file mutex poisoned");
        file.write_at(buf, offset)
    }

    fn fdatasync(&self) -> std::io::Result<()> {
        let file = self.file.lock().expect("image file mutex poisoned");
        file.sync_data()
    }
}

pub struct BlockBackend {
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap<()>>>,
    event_idx: bool,
    #[allow(dead_code)]
    image_path: Option<PathBuf>,
    read_only: bool,
    /// Open image fd + derived metadata. `None` only when the
    /// backend was constructed without an image (test harness
    /// paths, invalid config recovery). In production `run()`
    /// always populates this before the daemon starts.
    backing: Option<Arc<ImageBacking>>,
    exit_event: (EventConsumer, EventNotifier),
}

impl BlockBackend {
    pub fn new(image: Option<PathBuf>, read_only: bool) -> Result<Self> {
        let backing = match image.as_deref() {
            Some(path) => Some(Arc::new(ImageBacking::open(path, read_only)?)),
            None => None,
        };
        let exit_event = new_event_consumer_and_notifier(EventFlag::NONBLOCK)
            .context("exit-event consumer/notifier pair")?;
        Ok(Self {
            mem: None,
            event_idx: false,
            image_path: image,
            read_only,
            backing,
            exit_event,
        })
    }

    fn capacity_sectors(&self) -> u64 {
        self.backing
            .as_ref()
            .map(|b| b.capacity_sectors)
            .unwrap_or(0)
    }

    fn config(&self) -> VirtioBlkConfig {
        VirtioBlkConfig {
            capacity: Le64::from(self.capacity_sectors()),
            size_max: Le32::from(MAX_SEG_SIZE),
            seg_max: Le32::from(MAX_SEG_COUNT),
            geometry_cylinders: [0u8; 2],
            geometry_heads: 0,
            geometry_sectors: 0,
            blk_size: Le32::from(SECTOR_SIZE as u32),
        }
    }

    fn process_request_queue(&mut self, vring: &VringRwLock) -> std::io::Result<()> {
        let atomic_mem = match self.mem.as_ref() {
            Some(m) => m,
            None => {
                log::warn!("block: kick before memory table was set; skipping");
                return Ok(());
            }
        };

        // Collect chains before mutating the vring with add_used.
        let requests = {
            let mut guard = vring.get_mut();
            let queue = guard.get_queue_mut();
            queue
                .iter(atomic_mem.memory())
                .map_err(|e| std::io::Error::other(format!("iter REQ queue: {e:?}")))?
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
            let mut writer = chain
                .clone()
                .writer(&chain_mem)
                .map_err(|e| std::io::Error::other(format!("chain writer: {e:?}")))?;

            let (status, written) = self.handle_one(&mut reader, &mut writer);
            // The status byte lives at the LAST byte of the
            // writer region. Writer's available_bytes tracks
            // what remains; after the data fill (if any) the
            // final byte is our status.
            let _ = writer.write_obj::<u8>(status);
            // Used length = data actually written to the
            // "in" descriptors (0 for WRITE/FLUSH, N for READ,
            // 20 for GET_ID) + 1 byte for status.
            vring
                .add_used(head, written + 1)
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
                    .map_err(|e| std::io::Error::other(format!("needs_notification: {e:?}")))?
            };
            if needs {
                vring
                    .signal_used_queue()
                    .map_err(|e| std::io::Error::other(format!("signal_used_queue: {e:?}")))?;
            }
        }

        // Silence "unused field" warning about event_idx on
        // builds where trace-level logs compile out.
        let _ = self.event_idx;

        Ok(())
    }

    /// Dispatch one request. Returns (status_byte, bytes_written
    /// to writer EXCLUDING the status byte). The caller writes
    /// the status byte itself and computes add_used length.
    fn handle_one<R, W>(&self, reader: &mut R, writer: &mut W) -> (u8, u32)
    where
        R: Read,
        W: Write,
    {
        // Parse header. Any short read here -> malformed
        // request; log + IOERR.
        let mut hdr_bytes = [0u8; std::mem::size_of::<VirtioBlkReqHeader>()];
        if reader.read_exact(&mut hdr_bytes).is_err() {
            log::warn!("block: request header short-read");
            return (VIRTIO_BLK_S_IOERR as u8, 0);
        }
        let hdr = match VirtioBlkReqHeader::from_slice(&hdr_bytes) {
            Some(h) => *h,
            None => {
                log::warn!("block: malformed request header");
                return (VIRTIO_BLK_S_IOERR as u8, 0);
            }
        };
        let req_type = u32::from(hdr.req_type);
        let sector = u64::from(hdr.sector);

        let backing = match self.backing.as_ref() {
            Some(b) => b,
            None => {
                log::warn!("block: request with no image open");
                return (VIRTIO_BLK_S_IOERR as u8, 0);
            }
        };

        match req_type {
            VIRTIO_BLK_T_IN => self.handle_read(backing, sector, writer),
            VIRTIO_BLK_T_OUT => {
                if backing.read_only {
                    log::debug!("block: WRITE denied (read-only)");
                    return (VIRTIO_BLK_S_UNSUPP as u8, 0);
                }
                self.handle_write(backing, sector, reader)
            }
            VIRTIO_BLK_T_FLUSH => {
                if backing.read_only {
                    log::debug!("block: FLUSH denied (read-only)");
                    return (VIRTIO_BLK_S_UNSUPP as u8, 0);
                }
                match backing.fdatasync() {
                    Ok(()) => (VIRTIO_BLK_S_OK as u8, 0),
                    Err(e) => {
                        log::warn!("block: fdatasync failed: {e}");
                        (VIRTIO_BLK_S_IOERR as u8, 0)
                    }
                }
            }
            VIRTIO_BLK_T_GET_ID => match writer.write_all(&backing.device_id) {
                Ok(()) => (VIRTIO_BLK_S_OK as u8, VIRTIO_BLK_ID_BYTES as u32),
                Err(e) => {
                    log::warn!("block: GET_ID write failed: {e}");
                    (VIRTIO_BLK_S_IOERR as u8, 0)
                }
            },
            other => {
                log::debug!("block: unsupported request type {other}");
                (VIRTIO_BLK_S_UNSUPP as u8, 0)
            }
        }
    }

    fn handle_read<W: Write>(
        &self,
        backing: &ImageBacking,
        sector: u64,
        writer: &mut W,
    ) -> (u8, u32) {
        let offset = sector.saturating_mul(SECTOR_SIZE);
        // Size the read to fit the writer region minus the 1
        // status byte. 128 KiB is the spec cap we advertise;
        // the reader's available_bytes may be smaller, which
        // is fine.
        //
        // We don't know the writer's remaining capacity from
        // the Write trait alone; stream into a fixed-size
        // chunk buffer and write until pread returns short.
        let mut buf = vec![0u8; MAX_SEG_SIZE as usize];
        let total = match backing.pread_at(&mut buf, offset) {
            Ok(n) => n,
            Err(e) => {
                log::warn!("block: pread@{offset}: {e}");
                return (VIRTIO_BLK_S_IOERR as u8, 0);
            }
        };
        match writer.write_all(&buf[..total]) {
            Ok(()) => (VIRTIO_BLK_S_OK as u8, total as u32),
            Err(e) => {
                log::warn!("block: write to writer: {e}");
                (VIRTIO_BLK_S_IOERR as u8, 0)
            }
        }
    }

    fn handle_write<R: Read>(
        &self,
        backing: &ImageBacking,
        sector: u64,
        reader: &mut R,
    ) -> (u8, u32) {
        let offset = sector.saturating_mul(SECTOR_SIZE);
        let mut buf = Vec::with_capacity(MAX_SEG_SIZE as usize);
        // Read from the reader until EOF - virtio_queue's
        // Reader reports available_bytes via the trait
        // method on the concrete type; read_to_end is the
        // simplest portable path through the generic
        // Read interface the dispatcher uses.
        if let Err(e) = reader.read_to_end(&mut buf) {
            log::warn!("block: reader read_to_end: {e}");
            return (VIRTIO_BLK_S_IOERR as u8, 0);
        }
        match backing.pwrite_at(&buf, offset) {
            Ok(n) => {
                // Full short-write = IOERR; we don't split a
                // single request across multiple pwrite calls.
                if n == buf.len() {
                    (VIRTIO_BLK_S_OK as u8, 0)
                } else {
                    log::warn!("block: short pwrite {n}/{len}", len = buf.len());
                    (VIRTIO_BLK_S_IOERR as u8, 0)
                }
            }
            Err(e) => {
                log::warn!("block: pwrite@{offset}: {e}");
                (VIRTIO_BLK_S_IOERR as u8, 0)
            }
        }
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
        device_features(self.read_only)
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

    fn get_config(&self, offset: u32, size: u32) -> Vec<u8> {
        let cfg = self.config();
        let bytes = cfg.as_slice();
        let start = (offset as usize).min(bytes.len());
        let end = (offset.saturating_add(size) as usize).min(bytes.len());
        bytes[start..end].to_vec()
    }

    fn handle_event(
        &mut self,
        device_event: u16,
        evset: EventSet,
        vrings: &[VringRwLock],
        _thread_id: usize,
    ) -> std::io::Result<()> {
        if evset != EventSet::IN {
            log::warn!("block: unexpected evset {evset:?}; ignoring");
            return Ok(());
        }
        match device_event {
            REQ_QUEUE => {
                let vring = &vrings[REQ_QUEUE as usize];
                self.process_request_queue(vring)?;
            }
            other => log::warn!("block: unexpected device_event={other}; ignoring"),
        }
        Ok(())
    }

    fn exit_event(&self, _thread_index: usize) -> Option<(EventConsumer, EventNotifier)> {
        let (c, n) = &self.exit_event;
        Some((
            c.try_clone().expect("clone exit consumer"),
            n.try_clone().expect("clone exit notifier"),
        ))
    }
}

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

    // Constructor opens the image fd if an image path was
    // provided; that open must happen BEFORE seccomp (which
    // doesn't allow `open` / `openat` in the event-loop
    // baseline).
    let backend = Arc::new(RwLock::new(
        BlockBackend::new(args.image, args.read_only).context("constructing block backend")?,
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

    // Block-class seccomp filter: event-loop baseline + the
    // three syscalls the data path does:
    //   preadv / pwritev - user-data movement
    //   fdatasync        - VIRTIO_BLK_T_FLUSH
    //   pread64/pwrite64 - File::read_at/write_at on x86_64
    //                      actually lower to SYS_pread64 +
    //                      SYS_pwrite64, not the preadv/pwritev
    //                      pair. Add both so either glibc path
    //                      works across versions.
    tracing::debug!("block backend: applying seccomp filter");
    FilterBuilder::new()
        .with_vhost_user_event_loop()
        .allow_many(&[
            libc::SYS_preadv,
            libc::SYS_pwritev,
            libc::SYS_pread64,
            libc::SYS_pwrite64,
            libc::SYS_fdatasync,
            libc::SYS_fsync,
        ])
        .apply()
        .context("locking down block backend with seccomp filter")?;

    daemon
        .serve(&socket)
        .map_err(|e| anyhow::anyhow!("VhostUserDaemon::serve({}): {e:?}", socket.display()))?;

    tracing::info!("block backend: daemon exited cleanly");
    // Discard dead reads.
    let _ = image_display;
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Cursor, Seek, SeekFrom};

    #[test]
    fn features_has_size_max_and_blk_size() {
        let f = device_features(false);
        assert!(f & (1u64 << VIRTIO_F_VERSION_1) != 0);
        assert!(f & (1u64 << VIRTIO_BLK_F_SIZE_MAX) != 0);
        assert!(f & (1u64 << VIRTIO_BLK_F_SEG_MAX) != 0);
        assert!(f & (1u64 << VIRTIO_BLK_F_BLK_SIZE) != 0);
        assert!(f & (1u64 << VIRTIO_BLK_F_FLUSH) != 0);
        assert!(f & (1u64 << VIRTIO_BLK_F_RO) == 0);
        assert!(f & VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits() != 0);
    }

    #[test]
    fn features_ro_toggled_by_flag() {
        let rw = device_features(false);
        let ro = device_features(true);
        assert_eq!(rw & (1u64 << VIRTIO_BLK_F_RO), 0);
        assert!(ro & (1u64 << VIRTIO_BLK_F_RO) != 0);
    }

    #[test]
    fn protocol_features_advertise_config() {
        let pf = protocol_features();
        assert!(pf.contains(VhostUserProtocolFeatures::MQ));
        assert!(pf.contains(VhostUserProtocolFeatures::REPLY_ACK));
        assert!(pf.contains(VhostUserProtocolFeatures::CONFIG));
    }

    /// Build a BlockBackend wrapping a temp file. Returns the
    /// backend + the tempfile so the caller controls both
    /// lifetimes.
    fn backend_for_image(bytes: &[u8], ro: bool) -> (BlockBackend, tempfile::NamedTempFile) {
        let mut tmp = tempfile::NamedTempFile::new().expect("tempfile");
        tmp.write_all(bytes).expect("write");
        tmp.flush().expect("flush");
        // ImageBacking::open will reopen it with R/W by path.
        let path = tmp.path().to_path_buf();
        let b = BlockBackend::new(Some(path), ro).expect("construct");
        (b, tmp)
    }

    fn build_req(req_type: u32, sector: u64) -> Vec<u8> {
        let h = VirtioBlkReqHeader {
            req_type: Le32::from(req_type),
            reserved: Le32::from(0),
            sector: Le64::from(sector),
        };
        h.as_slice().to_vec()
    }

    #[test]
    fn config_reports_capacity_in_sectors() {
        // 8 sectors x 512 = 4 KiB.
        let (b, _tmp) = backend_for_image(&vec![0u8; 4096], false);
        let cfg = b.config();
        assert_eq!(u64::from(cfg.capacity), 8);
        assert_eq!(u32::from(cfg.blk_size), SECTOR_SIZE as u32);
        assert_eq!(u32::from(cfg.size_max), MAX_SEG_SIZE);
    }

    #[test]
    fn read_returns_image_bytes() {
        let mut img = vec![0u8; 4096];
        for (i, b) in img.iter_mut().enumerate() {
            *b = (i & 0xff) as u8;
        }
        let (b, _tmp) = backend_for_image(&img, false);

        let req = build_req(VIRTIO_BLK_T_IN, 2); // sector 2 -> offset 1024
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();

        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_OK as u8);
        // Read covers from offset 1024 to EOF (4096).
        assert_eq!(written, (4096 - 1024) as u32);
        assert_eq!(writer.len(), (4096 - 1024) as usize);
        // Spot-check bytes at start + end of the returned range.
        assert_eq!(writer[0], 0 /* img[1024] = 1024 & 0xff */);
        assert_eq!(writer[writer.len() - 1], ((4095) & 0xff) as u8);
    }

    #[test]
    fn write_round_trips() {
        let (b, tmp) = backend_for_image(&vec![0u8; 4096], false);

        // Payload = 512 bytes of 0xAB, written at sector 1.
        let mut req = build_req(VIRTIO_BLK_T_OUT, 1);
        req.extend(vec![0xAB_u8; 512]);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();

        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_OK as u8);
        assert_eq!(written, 0);
        assert!(writer.is_empty());

        // Verify on-disk contents.
        let mut file = std::fs::File::open(tmp.path()).expect("reopen");
        let mut got = [0u8; 512];
        file.seek(SeekFrom::Start(512)).unwrap();
        file.read_exact(&mut got).unwrap();
        assert_eq!(&got[..], &[0xAB; 512]);
    }

    #[test]
    fn write_rejected_in_read_only() {
        let (b, tmp) = backend_for_image(&vec![0u8; 4096], true);

        let mut req = build_req(VIRTIO_BLK_T_OUT, 0);
        req.extend(vec![0xAB_u8; 512]);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();

        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_UNSUPP as u8);
        assert_eq!(written, 0);

        // Verify nothing was written.
        let mut file = std::fs::File::open(tmp.path()).expect("reopen");
        let mut got = [0u8; 512];
        file.read_exact(&mut got).unwrap();
        assert_eq!(&got[..], &[0u8; 512]);
    }

    #[test]
    fn flush_rejected_in_read_only() {
        let (b, _tmp) = backend_for_image(&vec![0u8; 4096], true);
        let req = build_req(VIRTIO_BLK_T_FLUSH, 0);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();
        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_UNSUPP as u8);
        assert_eq!(written, 0);
    }

    #[test]
    fn flush_succeeds_when_writable() {
        let (b, _tmp) = backend_for_image(&vec![0u8; 4096], false);
        let req = build_req(VIRTIO_BLK_T_FLUSH, 0);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();
        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_OK as u8);
        assert_eq!(written, 0);
    }

    #[test]
    fn get_id_returns_basename_padded() {
        let (b, tmp) = backend_for_image(&vec![0u8; 4096], false);
        let req = build_req(VIRTIO_BLK_T_GET_ID, 0);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();
        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_OK as u8);
        assert_eq!(written, VIRTIO_BLK_ID_BYTES as u32);
        assert_eq!(writer.len(), VIRTIO_BLK_ID_BYTES);

        // Prefix should match the basename.
        let basename = tmp.path().file_name().unwrap().to_str().unwrap();
        let cmp_len = basename.len().min(VIRTIO_BLK_ID_BYTES);
        let want = &basename.as_bytes()[..cmp_len];
        assert_eq!(&writer[..cmp_len], want);
    }

    #[test]
    fn unsupported_request_type_returns_unsupp() {
        let (b, _tmp) = backend_for_image(&vec![0u8; 4096], false);
        // Pick a type we don't implement (DISCARD = 11).
        let req = build_req(11, 0);
        let mut reader = Cursor::new(req);
        let mut writer: Vec<u8> = Vec::new();
        let (status, written) = b.handle_one(&mut reader, &mut writer);
        assert_eq!(status, VIRTIO_BLK_S_UNSUPP as u8);
        assert_eq!(written, 0);
    }

    #[test]
    fn queues_and_size_match_constants() {
        let b = BlockBackend::new(None, false).expect("construct backend");
        assert_eq!(b.num_queues(), NUM_QUEUES);
        assert_eq!(b.max_queue_size(), QUEUE_SIZE);
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

    #[test]
    fn new_backend_has_no_memory() {
        let b = BlockBackend::new(None, false).expect("construct backend");
        assert!(b.mem.is_none());
        assert!(b.backing.is_none());
    }

    #[test]
    fn new_backend_carries_image_path() {
        // Construct with an image path that exists.
        let tmp = tempfile::NamedTempFile::new().unwrap();
        tmp.as_file().set_len(4096).unwrap();
        let b =
            BlockBackend::new(Some(tmp.path().to_path_buf()), true).expect("construct with image");
        assert!(b.backing.is_some());
        assert!(b.read_only);
        assert_eq!(b.capacity_sectors(), 8);
    }
}
