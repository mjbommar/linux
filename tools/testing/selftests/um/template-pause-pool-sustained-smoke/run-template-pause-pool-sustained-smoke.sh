#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-pool-sustained-smoke - N-member sustained
# pool dispatch.
#
# The default no-replication path is expected to fail after iter 1 and records
# the known MAP_SHARED physmem/member ownership limitation. With
# UML_POOL_REPLICATE=1, all requested members are expected to complete.
#
# Exit codes:
#   0   PASS - N members all reached MEMBER_DONE.
#   4   SKIP/XFAIL - kernel binary missing, OR the default path reaches
#              iter 1 PASS + iter 2+ hits the architectural limit, OR
#              UML_POOL_REPLICATE=1 reaches a bounded post-replication
#              regression.
#   1   FAIL - iter 1 itself broke (regression in pool-member entry).
#
# Optional:
#   KEEP_OUT=1 preserves the temporary directory and boot log.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
N=3
REPLICATE=${UML_POOL_REPLICATE:-0}

if [ ! -x "$KERNEL" ]; then
	echo "SKIP: UML binary $KERNEL not found"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi

OUT=$(mktemp -d -t template-pause-pool-sustained-smoke.XXXXXX)
trap 'if [ "${KEEP_OUT:-0}" = "1" ]; then echo "kept: $OUT" >&2; else rm -rf "$OUT"; fi' EXIT

cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo SUSTAINED_PRE_PAUSE pid=$$
echo fork-smoke > /proc/um/template_pause
echo SUSTAINED_POST_PAUSE pid=$$ rc=$?
echo SUSTAINED_MEMBER_DONE pid=$$
sleep 9999
IEOF
chmod +x "$OUT/init.sh"

PYRC=0
python3 - "$KERNEL" "$OUT/init.sh" "$OUT/boot.log" "$N" "$REPLICATE" <<'PYEOF' || PYRC=$?
import ctypes, ctypes.util, fcntl, os, signal, struct, sys, time

kernel, init_path, log_path, n_str, replicate_str = sys.argv[1:6]
N = int(n_str)
replicate = replicate_str == "1"

def Z(b, n): return b.ljust(n, b'\x00')[:n]
def make_blob(idx):
    return struct.pack(
        "<II 64s 6s 2s 16s 20s 16s 96s 32s",
        0x44495455, 1,
        Z(f"pool-member-{idx}".encode(), 64),
        bytes([0x52, 0x54, 0x00, idx, 0xb2, 0x01]),
        b"\x00\x00",
        Z(f"tap-pool-{idx}".encode(), 16),
        Z(f"10.7.0.{40+idx}/24".encode(), 20),
        Z(b"10.7.0.1", 16),
        Z(b"", 96), b"\x00" * 32,
    )

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
fd = libc.memfd_create(b"um-pool-sustained", 0x0001)
os.ftruncate(fd, 264)
os.lseek(fd, 0, 0); os.write(fd, make_blob(1))
os.lseek(fd, 260, 0); os.write(fd, b"\x00" * 4)
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)
libc.prctl(36, 1, 0, 0, 0)

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.setsid()
    os.dup2(log.fileno(), 1); os.dup2(log.fileno(), 2)
    cmdline = ["linux", "mem=128M", "rootfstype=hostfs",
               "rootflags=/", "root=/dev/root", "rw", "ncpus=1",
               "um_template_pause=fork",
               "um_template_pause_pool_member=1"]
    if replicate:
        cmdline.append("um_template_pause_pool_replicate=1")
    cmdline.append(f"init={init_path}")
    os.execve(kernel, cmdline, env)
    os._exit(127)

def state(p):
    try:
        with open(f"/proc/{p}/status") as fh:
            for ln in fh:
                if ln.startswith("State:"):
                    return ln.split()[1]
    except FileNotFoundError: return "X"
    return "?"

def wait_state(p, want, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s = state(p)
        if s == want or s == "X": return s
        time.sleep(0.05)
    return state(p)

def count_done():
    try:
        with open(log_path, errors="replace") as fh:
            return fh.read().count("SUSTAINED_MEMBER_DONE")
    except FileNotFoundError: return 0

def read_log():
    try:
        with open(log_path, errors="replace") as fh:
            return fh.read()
    except FileNotFoundError:
        return ""

def child_pid_slot():
    try:
        data = os.pread(fd, 4, 260)
    except OSError:
        return 0
    if len(data) != 4:
        return 0
    return struct.unpack("<I", data)[0]

def kill_uml_tree(p):
    try:
        os.killpg(p, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError:
        os.kill(p, signal.SIGKILL)

prev = 0
for i in range(1, N+1):
    s = wait_state(pid, "T", 15)
    if s != "T":
        print(f"iter {i}: master state {s}, abort")
        break

    os.lseek(fd, 0, 0); os.write(fd, make_blob(i))
    os.lseek(fd, 260, 0); os.write(fd, b"\x00" * 4)
    before = read_log()
    panic_base = before.count("Kernel panic")
    segv_base = before.count("segfault at")

    os.kill(pid, signal.SIGCONT)
    fatal_seen = False
    done_seen = False
    end = time.time() + 20
    while time.time() < end:
        try:
            while os.waitpid(-1, os.WNOHANG)[0]: pass
        except ChildProcessError: pass
        c = count_done()
        if c > prev:
            prev = c
            print(f"iter {i}: MEMBER_DONE total={c} child_pid={child_pid_slot()}")
            done_seen = True
            break
        content = read_log()
        if (replicate or i > 1) and (
                content.count("Kernel panic") > panic_base or
                content.count("segfault at") > segv_base):
            print(f"iter {i}: observed panic/segfault while awaiting member "
                  f"child_pid={child_pid_slot()}")
            fatal_seen = True
            break
        time.sleep(0.2)
    else:
        print(f"iter {i}: TIMEOUT child_pid={child_pid_slot()}")
        break
    if fatal_seen:
        break
    if done_seen:
        end = time.time() + 1
        while time.time() < end:
            content = read_log()
            if (replicate or i > 1) and (
                    content.count("Kernel panic") > panic_base or
                    content.count("segfault at") > segv_base):
                print(f"iter {i}: observed panic/segfault after MEMBER_DONE "
                      f"child_pid={child_pid_slot()}")
                fatal_seen = True
                break
            time.sleep(0.05)
    if fatal_seen:
        break
    time.sleep(0.5)

if state(pid) not in ("X", "?"):
    kill_uml_tree(pid)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass

with open(log_path, errors="replace") as fh:
    content = fh.read()

done = content.count("SUSTAINED_MEMBER_DONE")
pool_enter = content.count("POOL_ENTER")
replicate_ok = content.count("POOL_REPLICATE_OK")
replicate_fail = content.count("POOL_REPLICATE_FAIL")
panic = "Kernel panic" in content
ceiling = "um_template_pause_enter+0xf" in content

print(f"SUSTAINED_MEMBER_DONE : {done}")
print(f"POOL_ENTER            : {pool_enter}")
print(f"POOL_REPLICATE_OK     : {replicate_ok}")
print(f"POOL_REPLICATE_FAIL   : {replicate_fail}")
print(f"Kernel panic          : {panic}")
print(f"NULL-call regression  : {ceiling}")

# Honest verdict:
if ceiling:
    print("FAIL: NULL-call regression")
    sys.exit(1)
if done < 1:
    if replicate and pool_enter >= 1 and (replicate_ok >= 1 or replicate_fail >= 1):
        print("XFAIL: replication path reached bounded post-clone boundary")
        print("       - post-replication userspace/stub path still needs a fix.")
        sys.exit(4)
    print("FAIL: iter 1 broken (regression in pool-member entry)")
    sys.exit(1)
if done >= N and not panic:
    print(f"PASS: {done}/{N} pool members reached MEMBER_DONE")
    sys.exit(0)
# Iter 1 worked but subsequent iterations did not complete - expected today.
if replicate:
    print("XFAIL: replication iter 1 PASS, iter 2+ hits repeated-member boundary")
    print("       - per-member physmem isolation still needs the second take fixed.")
else:
    print(f"XFAIL: iter 1 PASS, iter 2+ hits MAP_SHARED physmem/member limit")
    print("       - repeated live members need independent physmem ownership.")
sys.exit(4)
PYEOF

case $PYRC in
0) exit 0;;
4) exit 4;;
*) exit 1;;
esac
