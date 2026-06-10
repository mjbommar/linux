#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-mconsole-path-probe - bounded probe for pool-member mconsole paths.
#
# Expected result: the member reaches userspace, the requested per-member
# mconsole socket exists, and the socket answers `version`. A panic before
# MEMBER_DONE is a regression.
#
# Exit codes:
#   0 PASS  - requested socket exists and answers `version`.
#   1 FAIL  - kernel panic, timeout before MEMBER_DONE, absent socket, or bad
#             mconsole reply.
#
# Environment:
#   UML_BINARY  UML kernel. Default $HOME/src/uml-builds/uml-tplpause-fork/linux.
#   KEEP_OUT=1 preserves the temporary directory and full boot log.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause-fork/linux}

if [ ! -x "$KERNEL" ]; then
	echo "SKIP: UML binary $KERNEL not found"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

OUT=$(mktemp -d -t pool-mconsole-path-probe.XXXXXX)
trap 'if [ "${KEEP_OUT:-0}" = "1" ]; then echo "kept: $OUT" >&2; else rm -rf "$OUT"; fi' EXIT

cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc none /proc 2>/dev/null || true
echo PMCON_PRE_PAUSE pid=$$
echo fork-smoke > /proc/um/template_pause
echo PMCON_POST_PAUSE pid=$$ rc=$?
echo PMCON_MEMBER_ALIVE pid=$$
sleep 0.1
echo PMCON_MEMBER_DONE pid=$$
sleep 9999
IEOF
chmod +x "$OUT/init.sh"

PYRC=0
python3 - "$KERNEL" "$OUT/init.sh" "$OUT/boot.log" "$OUT/member.mconsole" <<'PYEOF' || PYRC=$?
import ctypes
import ctypes.util
import fcntl
import os
import signal
import socket
import struct
import sys
import time

kernel, init_path, log_path, mconsole_path = sys.argv[1:5]

MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_MAX_DATA = 512

def zpad(value, size):
    return value.ljust(size, b"\x00")[:size]

def build_blob():
    return struct.pack(
        "<II 64s 6s 2s 16s 20s 16s 96s 32s",
        0x44495455,
        1,
        zpad(b"mconsole-probe", 64),
        bytes([0x52, 0x54, 0x00, 0x31, 0x32, 0x33]),
        b"\x00\x00",
        zpad(b"tap-mcon", 16),
        zpad(b"10.7.0.50/24", 20),
        zpad(b"10.7.0.1", 16),
        zpad(mconsole_path.encode(), 96),
        b"\x00" * 32,
    )

def state(pid):
    try:
        with open(f"/proc/{pid}/status") as fh:
            for line in fh:
                if line.startswith("State:"):
                    return line.split()[1]
    except FileNotFoundError:
        return "X"
    return "?"

def read_log():
    try:
        with open(log_path, errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""

def kill_tree(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError:
        os.kill(pid, signal.SIGKILL)

def mconsole_command(sock_path, command):
    data = command.encode()
    if len(data) >= MCONSOLE_MAX_DATA:
        raise RuntimeError("mconsole command too long")
    packet = struct.pack(
        "=III512s",
        MCONSOLE_MAGIC,
        MCONSOLE_VERSION,
        len(data),
        data + b"\x00" * (MCONSOLE_MAX_DATA - len(data)),
    )
    client = f"{sock_path}.client.{os.getpid()}"
    try:
        os.unlink(client)
    except FileNotFoundError:
        pass
    out = []
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.bind(client)
        s.settimeout(5.0)
        s.connect(sock_path)
        s.send(packet)
        for _ in range(64):
            reply = s.recv(12 + MCONSOLE_MAX_DATA)
            if len(reply) < 12:
                raise RuntimeError(f"short mconsole reply: {len(reply)} bytes")
            err, more, length = struct.unpack("=III", reply[:12])
            payload = reply[12:12 + min(max(length - 1, 0), MCONSOLE_MAX_DATA)]
            out.append(payload.decode(errors="replace"))
            if err:
                raise RuntimeError("mconsole error: " + "".join(out).strip())
            if not more:
                return "".join(out)
        raise RuntimeError("mconsole reply exceeded 64 packets")
    finally:
        s.close()
        try:
            os.unlink(client)
        except FileNotFoundError:
            pass

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
fd = libc.memfd_create(b"um-pool-mconsole-probe", 0x0001)
if fd < 0:
    raise SystemExit(f"memfd_create: {os.strerror(ctypes.get_errno())}")
os.ftruncate(fd, 264)
os.write(fd, build_blob())
os.pwrite(fd, b"\x00" * 4, 260)
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.setsid()
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    cmdline = [
        "linux",
        "mem=128M",
        "rootfstype=hostfs",
        "rootflags=/",
        "root=/dev/root",
        "rw",
        "ncpus=1",
        "um_template_pause=fork",
        "um_template_pause_pool_member=1",
        "um_template_pause_pool_replicate=1",
        f"init={init_path}",
    ]
    os.execve(kernel, cmdline, env)
    os._exit(127)

try:
    deadline = time.time() + 20
    while time.time() < deadline:
        if state(pid) in ("T", "X"):
            break
        time.sleep(0.05)
    if state(pid) != "T":
        print(f"FAIL: master did not reach SIGSTOP; state={state(pid)}")
        raise SystemExit(1)

    os.kill(pid, signal.SIGCONT)

    done = False
    panic = False
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            while os.waitpid(-1, os.WNOHANG)[0]:
                pass
        except ChildProcessError:
            pass
        content = read_log()
        if "Kernel panic" in content or "segfault at" in content:
            panic = True
            break
        if "PMCON_MEMBER_DONE" in content:
            done = True
            break
        time.sleep(0.1)

    content = read_log()
    print(f"PMCON_MEMBER_DONE : {content.count('PMCON_MEMBER_DONE')}")
    print(f"POOL_ENTER        : {content.count('POOL_ENTER')}")
    print(f"POOL_REPLICATE_OK : {content.count('POOL_REPLICATE_OK')}")
    print(f"Kernel panic      : {'Kernel panic' in content}")
    print(f"mconsole path     : {mconsole_path}")
    print(f"mconsole exists   : {os.path.exists(mconsole_path)}")

    if panic:
        print("FAIL: kernel panicked while probing non-empty mconsole_path")
        print("--- boot log tail ---")
        for line in content.splitlines()[-80:]:
            print(line)
        raise SystemExit(1)
    if not done:
        print("FAIL: member did not reach PMCON_MEMBER_DONE")
        print("--- boot log tail ---")
        for line in content.splitlines()[-80:]:
            print(line)
        raise SystemExit(1)
    if not os.path.exists(mconsole_path):
        print("FAIL: member reached userspace but requested mconsole socket is absent")
        raise SystemExit(1)

    reply = mconsole_command(mconsole_path, "version")
    print("mconsole version reply:")
    print(reply.strip())
    print("PASS: per-member mconsole socket answers version")
    raise SystemExit(0)
finally:
    if state(pid) not in ("X", "?"):
        kill_tree(pid)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    log.close()
PYEOF

case $PYRC in
0) exit 0;;
4) exit 4;;
*) exit 1;;
esac
