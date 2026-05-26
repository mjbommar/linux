#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Open a TAP device and exec UML with the fd inherited at the requested
slot.  Used by run-tcp-throughput.sh to feed vector2 a pre-opened tap
fd (the production "mode=fd" handoff path; the alternative
"mode=inproc,ifname=…" path requires CONFIG_UML_NET_VECTOR_V2_INPROC
and is slower).

Usage:
    exec-uml-fd.py TAP_NAME FD_NUMBER KERNEL [kernel args...]
"""
import fcntl
import os
import struct
import sys

if len(sys.argv) < 4:
    print(__doc__, file=sys.stderr)
    sys.exit(2)

TAP_NAME = sys.argv[1]
FD_NUMBER = int(sys.argv[2])
KERNEL_ARGV = sys.argv[3:]

TUNSETIFF = 0x400454CA
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

# No IFF_VNET_HDR — vec2's fd transport reads raw Ethernet frames,
# same shape as the umlctl deploy fd-handoff (tools/uml/uml-launcher/
# src/backend/net.rs comment block).
fd = os.open("/dev/net/tun", os.O_RDWR)
flags = IFF_TAP | IFF_NO_PI
ifr = struct.pack("16sH", TAP_NAME.encode(), flags)
fcntl.ioctl(fd, TUNSETIFF, ifr)
os.dup2(fd, FD_NUMBER)
os.set_inheritable(FD_NUMBER, True)
os.execvp(KERNEL_ARGV[0], KERNEL_ARGV)
