// SPDX-License-Identifier: GPL-2.0
//
// Host TAP fd opener for umlctl-owned vector2 fd handoff.
//
// umlctl creates and configures the host TAP device with sudo before the
// UML kernel starts. Once the TAP is owned by the invoking user, umlctl can
// open /dev/net/tun itself, attach to the named TAP, and pass that fd into
// the UML process. The vector2 guest then sees only an inherited fd number,
// not authority to open host networking resources from inside the sandbox.

use std::io;
use std::os::fd::{FromRawFd, OwnedFd};

const IFF_TAP: libc::c_int = 0x0002;
const IFF_MULTI_QUEUE: libc::c_int = 0x0100;
const IFF_NO_PI: libc::c_int = 0x1000;
const IFF_VNET_HDR: libc::c_int = 0x4000;
const TUNSETIFF: libc::c_ulong = 0x400454ca;
const TUNSETOFFLOAD: libc::c_ulong = 0x400454d0;
const TUN_F_CSUM: libc::c_uint = 0x01;
const TUN_F_TSO4: libc::c_uint = 0x02;
const TUN_F_TSO6: libc::c_uint = 0x04;
const IFNAMSIZ: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
struct IfReq {
    ifr_name: [u8; IFNAMSIZ],
    ifr_flags: libc::c_short,
    _pad: [u8; 22],
}

pub fn open_tap(ifname: &str, multi_queue: bool) -> io::Result<OwnedFd> {
    if ifname.len() >= IFNAMSIZ {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("TAP interface name too long: {ifname}"),
        ));
    }

    let path = std::ffi::CString::new("/dev/net/tun").expect("/dev/net/tun is static ascii");
    let fd = unsafe {
        libc::open(
            path.as_ptr(),
            libc::O_RDWR | libc::O_CLOEXEC | libc::O_NONBLOCK,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }

    let owned = unsafe { OwnedFd::from_raw_fd(fd) };
    attach_tap(&owned, ifname, multi_queue)?;
    // Request TSO/CSUM offload.  The vec2 fd transport probes
    // IFF_VNET_HDR via TUNGETIFF and dispatches accordingly, so an
    // older kernel without that detect logic still works (we just
    // hand back to per-MTU-frame writes — slow but correct).
    set_offload(&owned).ok();
    Ok(owned)
}

fn set_offload(fd: &OwnedFd) -> io::Result<()> {
    use std::os::fd::AsRawFd;

    let offload: libc::c_uint = TUN_F_CSUM | TUN_F_TSO4 | TUN_F_TSO6;
    let rc = unsafe { libc::ioctl(fd.as_raw_fd(), TUNSETOFFLOAD, offload as libc::c_ulong) };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn attach_tap(fd: &OwnedFd, ifname: &str, multi_queue: bool) -> io::Result<()> {
    use std::os::fd::AsRawFd;

    let mut req = IfReq {
        ifr_name: [0u8; IFNAMSIZ],
        ifr_flags: tap_flags(multi_queue) as libc::c_short,
        _pad: [0u8; 22],
    };
    req.ifr_name[..ifname.len()].copy_from_slice(ifname.as_bytes());

    let rc = unsafe {
        libc::ioctl(
            fd.as_raw_fd(),
            TUNSETIFF,
            &mut req as *mut _ as *mut libc::c_void,
        )
    };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

fn tap_flags(multi_queue: bool) -> libc::c_int {
    // IFF_VNET_HDR is always requested.  The vec2 fd transport probes
    // TUNGETIFF at fd-inherit time and uses virtio_net_hdr framing
    // when set, so guest→host TCP can ride TSO instead of paying
    // per-MTU-frame syscall cost.
    let mut flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
    if multi_queue {
        flags |= IFF_MULTI_QUEUE;
    }
    flags
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_oversized_name_before_open() {
        let err = open_tap("this_name_is_way_too_long_for_ifnamsiz", false).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::InvalidInput);
    }

    #[test]
    fn multiqueue_flag_is_opt_in() {
        assert_eq!(tap_flags(false), IFF_TAP | IFF_NO_PI | IFF_VNET_HDR);
        assert_eq!(
            tap_flags(true),
            IFF_TAP | IFF_NO_PI | IFF_VNET_HDR | IFF_MULTI_QUEUE
        );
    }
}
