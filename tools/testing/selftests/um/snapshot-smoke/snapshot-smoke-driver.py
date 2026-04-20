#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
um/snapshot-smoke/snapshot-smoke-driver.py — minimal AFL-compatible
forkserver driver for the C-09 selftest. Launches the UML binary
with host fds 198 (ctl, driver -> kernel) and 199 (status, kernel
-> driver) pre-plumbed, exchanges the 12-byte handshake for one
iteration, checks the returned pid is positive and the status byte
arrives, then closes the control fd to trigger a clean disconnect.

Terminal line on stdout, one of:
    DRV: PASS pid=<pid> status=0x<status>
    DRV: SKIP <reason>
    DRV: FAIL <reason>

Consumed by run-snapshot-smoke.sh. Not meant to be run directly.

Protocol (matches arch/um/kernel/snapshot.c):
  - parent -> 199: 4 bytes "AFL\\0"              (handshake)
  - LOOP:
      - 198 -> parent: 4 bytes testcase descriptor  (driver writes any 4 bytes)
      - parent -> 199: 4 bytes worker host pid
      - parent -> 199: 4 bytes worker exit status (placeholder 0 today)
  - driver closes 198 to end the loop
"""
import os
import struct
import subprocess
import sys
import threading
import time


def main():
    if len(sys.argv) < 3:
        print("DRV: FAIL usage: snapshot-smoke-driver.py UML_BINARY INIT_PATH [MEM]",
              file=sys.stderr)
        return 1

    uml = sys.argv[1]
    init = sys.argv[2]
    mem = sys.argv[3] if len(sys.argv) > 3 else "128M"

    if not os.access(uml, os.X_OK):
        print(f"DRV: SKIP UML binary {uml} not executable")
        return 0

    # Plumb the two forkserver fds into the child as fds 198/199.
    ctl_r, ctl_w = os.pipe()
    status_r, status_w = os.pipe()
    os.dup2(ctl_r, 198)
    os.dup2(status_w, 199)
    os.close(ctl_r)
    os.close(status_w)

    proc = subprocess.Popen(
        [uml, "rootfstype=hostfs", "rootflags=/", f"init={init}",
         f"mem={mem}", "con=null", "con0=fd:0,fd:1", "console=tty"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        close_fds=False,
    )
    os.close(198)
    os.close(199)

    status_fd = os.fdopen(status_r, "rb", buffering=0)
    ctl_fd = os.fdopen(ctl_w, "wb", buffering=0)

    # Drain the UML stdout in the background so it doesn't block on
    # a full pipe. We don't print it — the kselftest output is
    # already noisy enough.
    def drain():
        try:
            for _line in proc.stdout:
                pass
        except Exception:
            pass

    threading.Thread(target=drain, daemon=True).start()

    try:
        # 4-byte handshake from the kernel.
        hs = status_fd.read(4)
        if hs != b"AFL\x00":
            print(f"DRV: FAIL handshake expected b'AFL\\x00', got {hs!r}")
            return 1

        # Request one iteration.
        ctl_fd.write(b"\x00\x00\x00\x00")

        pid_bytes = status_fd.read(4)
        if len(pid_bytes) != 4:
            print(f"DRV: FAIL short pid read ({len(pid_bytes)} bytes)")
            return 1
        pid = struct.unpack("<i", pid_bytes)[0]
        if pid <= 0:
            print(f"DRV: FAIL non-positive pid {pid}")
            return 1

        status_bytes = status_fd.read(4)
        if len(status_bytes) != 4:
            print(f"DRV: FAIL short status read ({len(status_bytes)} bytes)")
            return 1
        status = struct.unpack("<i", status_bytes)[0]

        # Clean disconnect: close ctl pipe; kernel loop exits.
        ctl_fd.close()

        # Give the kernel some breathing room to unwind, then terminate.
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        print(f"DRV: PASS pid={pid} status=0x{status:x}")
        return 0
    except Exception as e:
        print(f"DRV: FAIL exception: {e}")
        try:
            proc.kill()
            proc.wait(timeout=5)
        except Exception:
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
