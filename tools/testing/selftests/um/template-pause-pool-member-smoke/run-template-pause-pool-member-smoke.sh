#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pool-member-smoke - pool-member entry smoke test.
#
# What this proves:
#
#   1. um_template_pause_pool_member=1 arms the pool-member mode.
#   2. Master pauses at /proc/um/template_pause, SIGSTOPs itself,
#      resumes on SIGCONT, and forks via
#      os_template_pause_fork_clone_to(child_entry_pool_member).
#   3. The M-fork child runs child_entry_pool_member on a private
#      MAP_PRIVATE stack, restores kernel/user state, and re-enters
#      userspace().
#   4. Init.sh's /proc write returns rc=0 (POST_PAUSE).
#   5. Init.sh executes subsequent commands (MEMBER_ALIVE_1).
#   6. sleep(2) wakeups succeed - multiple MEMBER_TICK markers
#      confirm timer firing in the child.
#   7. Init.sh reaches MEMBER_DONE without panic.
#
# Exit codes:
#   0  PASS  - POOL_ENTER + MEMBER_DONE seen, >=3 MEMBER_TICKs,
#              no kernel panic.
#   1  FAIL  - child never reached MEMBER_DONE.
#   4  SKIP  - kernel binary missing or python3 unavailable.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause-fork/linux}

if [ ! -x "$KERNEL" ]; then
	echo "SKIP: UML binary $KERNEL not found (set UML_BINARY)"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

OUT=$(mktemp -d -t template-pause-pool-member-smoke.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo TPPM_PRE_PAUSE pid=$$
echo fork-smoke > /proc/um/template_pause
echo TPPM_POST_PAUSE pid=$$ rc=$?
echo TPPM_MEMBER_ALIVE_1 pid=$$
i=0
while [ $i -lt 5 ]; do
	sleep 1
	i=$((i+1))
	echo "TPPM_MEMBER_TICK $i pid=$$"
done
# Read back applied identity (if any) - proves master ran
# um_template_identity_apply on this iteration's blob.
if [ -r /proc/sys/kernel/hostname ]; then
	HN=$(cat /proc/sys/kernel/hostname 2>/dev/null)
	echo "TPPM_HOSTNAME=$HN"
fi
echo TPPM_MEMBER_DONE pid=$$
sleep 9999
IEOF
chmod +x "$OUT/init.sh"

PYRC=0
python3 - "$KERNEL" "$OUT/init.sh" "$OUT/boot.log" <<'PYEOF' || PYRC=$?
import ctypes, ctypes.util, fcntl, os, signal, struct, sys, time

kernel, init_path, log_path = sys.argv[1], sys.argv[2], sys.argv[3]

def Z(b, n):
    return b.ljust(n, b'\x00')[:n]

# Identity blob - supervisor stamps the pool-member identity.
# Master reads this at fork iteration start and calls
# um_template_identity_apply() before forking the child.
blob = struct.pack(
    "<II 64s 6s 2s 16s 20s 16s 96s 32s",
    0x44495455, 1,                   # magic, version
    Z(b"pool-member-1", 64),         # instance_name
    bytes([0x52, 0x54, 0x00, 0xa1, 0xb2, 0x01]),
    b"\x00\x00",
    Z(b"tap-pool-1", 16),
    Z(b"10.7.0.42/24", 20),
    Z(b"10.7.0.1", 16),
    Z(b"", 96), b"\x00" * 32,
)

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
fd = libc.memfd_create(b"um-tplpause-pool-member-smoke", 0x0001)
if fd < 0:
    sys.exit(f"memfd_create: {os.strerror(ctypes.get_errno())}")
os.ftruncate(fd, 264)
os.lseek(fd, 0, 0)
os.write(fd, blob)
# Zero the child-pid write-back slot at offset 260
os.lseek(fd, 260, 0)
os.write(fd, b"\x00\x00\x00\x00")
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

libc.prctl(36, 1, 0, 0, 0)

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    os.execve(kernel, [
        "linux", "mem=128M", "rootfstype=hostfs", "rootflags=/",
        "root=/dev/root", "rw", "ncpus=1",
        "um_template_pause=fork",
        "um_template_pause_pool_member=1",
        f"init={init_path}",
    ], env)
    os._exit(127)

def state(p):
    try:
        with open(f"/proc/{p}/status") as fh:
            for ln in fh:
                if ln.startswith("State:"):
                    return ln.split()[1]
    except FileNotFoundError:
        return "X"
    return "?"

# Wait for master's first SIGSTOP.
for _ in range(120):
    s = state(pid)
    if s in ("T", "X"):
        break
    time.sleep(0.1)

if state(pid) == "X":
    log.close()
    sys.exit("master exited before reaching SIGSTOP")

# One SIGCONT - drive a single fork iteration to completion.
if state(pid) == "T":
    os.kill(pid, signal.SIGCONT)

# Watch for up to 20s for MEMBER_DONE to appear.
deadline = time.time() + 20
done_seen = False
while time.time() < deadline and not done_seen:
    try:
        rpid, _ = os.waitpid(-1, os.WNOHANG)
    except ChildProcessError:
        pass
    time.sleep(0.2)
    try:
        with open(log_path, errors="replace") as fh:
            if "TPPM_MEMBER_DONE" in fh.read():
                done_seen = True
                break
    except FileNotFoundError:
        pass

if state(pid) not in ("X", "?"):
    os.kill(pid, signal.SIGKILL)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
log.close()

while True:
    try:
        rpid, _ = os.waitpid(-1, os.WNOHANG)
        if rpid == 0:
            break
    except ChildProcessError:
        break

with open(log_path, errors="replace") as fh:
    content = fh.read()

pool_enter = content.count("POOL_ENTER")
post_pause = content.count("TPPM_POST_PAUSE")
alive_1    = content.count("TPPM_MEMBER_ALIVE_1")
ticks      = content.count("TPPM_MEMBER_TICK")
done       = content.count("TPPM_MEMBER_DONE")
panic      = "Kernel panic" in content
ceiling    = "um_template_pause_enter+0xf" in content
identity_logged = ("identity_fd=" in content or
                   "identity blob parsed" in content or
                   "identity-parsed" in content)
identity_parsed = "identity-parsed" in content and \
                  'name="pool-member-1"' in content

print(f"POOL_ENTER       : {pool_enter}")
print(f"TPPM_POST_PAUSE  : {post_pause}")
print(f"TPPM_MEMBER_ALIVE_1: {alive_1}")
print(f"TPPM_MEMBER_TICK : {ticks}")
print(f"TPPM_MEMBER_DONE : {done}")
print(f"identity_fd seen : {identity_logged}")
print(f"identity-parsed  : {identity_parsed}")
print(f"Kernel panic     : {panic}")
print(f"v1 ceiling IP    : {ceiling}")

if panic or ceiling:
    print("FAIL: panic or v1-ceiling regression")
    sys.exit(1)
if pool_enter < 1:
    print("FAIL: no POOL_ENTER from child entry")
    sys.exit(1)
if post_pause < 1:
    print("FAIL: init.sh did not return from /proc write")
    sys.exit(1)
if alive_1 < 1:
    print("FAIL: init.sh did not execute next shell command")
    sys.exit(1)
if ticks < 3:
    print(f"FAIL: too few timer wakeups ({ticks} < 3)")
    sys.exit(1)
if done < 1:
    print("FAIL: init.sh did not reach MEMBER_DONE")
    sys.exit(1)
if not identity_parsed:
    print("FAIL: identity blob parse marker not found")
    sys.exit(1)
print("PASS")
PYEOF

case $PYRC in
0) exit 0;;
4) exit 4;;
*) echo "FAIL: harness rc=$PYRC; see $OUT/boot.log"; cp "$OUT/boot.log" /tmp/tppm-smoke-fail.log 2>/dev/null || true; exit 1;;
esac
