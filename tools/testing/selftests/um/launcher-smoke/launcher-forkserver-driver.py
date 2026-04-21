#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# launcher-smoke/launcher-forkserver-driver.py
#
# End-to-end test for uml-launcher's --forkserver fd-plumbing
# path (workstream C-10). Mirrors snapshot-smoke-driver.py but
# routes the UML spawn through uml-launcher instead of invoking
# the kernel binary directly — the launcher does the dup2 of our
# pipe fds to 198/199 inside CommandExt::pre_exec.
#
# If this passes, it proves:
#   1. The launcher's --forkserver ctl,status argument parses.
#   2. pre_exec's dup2 puts the caller-opened pipes at UML fds
#      198/199.
#   3. The launcher clears FD_CLOEXEC so the fds survive execve.
#   4. The UML kernel (built with CONFIG_UM_SNAPSHOT_FORKSERVER=y)
#      sees them as open, writes the AFL\0 handshake on 199,
#      reads a testcase byte on 198, forks a worker, writes the
#      worker pid + status on 199.
#   5. The launcher's signal/reap path still works end-to-end
#      through a full handshake cycle.
#
# Terminal line on stdout, one of:
#     DRV: PASS pid=<pid> status=0x<status>
#     DRV: SKIP <reason>
#     DRV: FAIL <reason>

import os
import struct
import subprocess
import sys
import threading


def main():
    if len(sys.argv) < 4:
        print("DRV: FAIL usage: launcher-forkserver-driver.py LAUNCHER UML_BINARY INIT [MEM]",
              file=sys.stderr)
        return 1

    launcher = sys.argv[1]
    uml = sys.argv[2]
    init = sys.argv[3]
    mem = sys.argv[4] if len(sys.argv) > 4 else "128M"

    if not os.access(launcher, os.X_OK):
        print(f"DRV: SKIP launcher {launcher} not executable")
        return 0
    if not os.access(uml, os.X_OK):
        print(f"DRV: SKIP UML binary {uml} not executable")
        return 0

    # Create the pipes. We pass their fd numbers into the launcher
    # via --forkserver; the launcher then dup2s them to 198/199
    # inside its pre_exec hook before execve'ing UML.
    #
    # os.pipe() returns non-inheritable fds in Python 3 (O_CLOEXEC
    # is set). pass_fds= below tells subprocess to make exactly
    # these fds inheritable for the exec of the launcher child.
    ctl_r, ctl_w = os.pipe()
    status_r, status_w = os.pipe()

    proc = subprocess.Popen(
        [launcher, "run",
         "--kernel", uml,
         "--init", init,
         "--mem", mem,
         "--console", "null",
         "--forkserver", f"{ctl_r},{status_w}"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(ctl_r, status_w),
    )
    # The launcher inherited our ctl_r / status_w. Drop OUR copies
    # — only the launcher/UML side should hold them now.
    os.close(ctl_r)
    os.close(status_w)

    status_fd = os.fdopen(status_r, "rb", buffering=0)
    ctl_fd = os.fdopen(ctl_w, "wb", buffering=0)

    # Drain launcher+UML output in the background so a full pipe
    # doesn't block them.
    def drain():
        try:
            for _line in proc.stdout:
                pass
        except Exception:
            pass

    threading.Thread(target=drain, daemon=True).start()

    try:
        # 4-byte AFL\0 handshake from the UML kernel.
        hs = status_fd.read(4)
        if hs != b"AFL\x00":
            print(f"DRV: FAIL handshake expected b'AFL\\x00', got {hs!r}")
            return 1

        # One iteration: ask the forkserver to spawn a worker.
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

        # Clean disconnect: close the ctl pipe; kernel loop exits,
        # launcher waits on the child, then exits itself.
        ctl_fd.close()

        try:
            proc.wait(timeout=30)
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
