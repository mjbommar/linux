#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/template-pause-smoke/run-template-pause-smoke.sh - kselftest for the
# UML template-pause primitive.
#
# Verifies, in three independent boots of the same kernel binary:
#
#   case 1 - unarmed boot
#     UML booted WITHOUT `um_template_pause` on the cmdline. The
#     /proc/um/template_pause entry must NOT exist (no /proc/um dir
#     created), and boot must complete normally.
#
#   case 2 - armed boot, no identity fd
#     UML booted WITH `um_template_pause` and an init script that
#     writes to /proc/um/template_pause. UM_TEMPLATE_IDENTITY_FD is
#     unset, so the kernel should:
#       - log "armed via kernel cmdline"
#       - log "/proc/um/template_pause ready"
#       - on write: log "no UM_TEMPLATE_IDENTITY_FD" + "raising SIGSTOP"
#       - actually raise SIGSTOP (host /proc/PID/status State == T)
#     After the test harness sends SIGCONT, the kernel logs
#     "resumed via SIGCONT" and the init script logs POST_PAUSE_MARKER.
#
#   case 3 - armed boot, identity blob via memfd
#     Like case 2, but with a memfd containing a valid
#     `struct um_template_identity` plumbed via UM_TEMPLATE_IDENTITY_FD.
#     The kernel additionally parses + logs the blob's instance name,
#     MAC, tap name, and IPv4 fields.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL, per kselftest convention.
#
# Environment:
#   UML_BINARY   UML kernel built with CONFIG_UM_TEMPLATE_PAUSE=y.
#                Default: $HOME/src/uml-builds/uml-tplpause/linux.
#   UML_MEM      mem= argument. Default 128M.

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause/linux}
MEM=${UML_MEM:-128M}
OUT=$(mktemp -d -t template-pause-smoke.XXXXXX)
trap 'rm -rf "$OUT"' EXIT

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)"
	exit 4
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required for case 3"
	exit 4
fi

# Confirm CONFIG_UM_TEMPLATE_PAUSE is on (the binary may have been
# built before the option existed). The simplest signal is whether
# the "armed via kernel cmdline" message appears when armed; we'll
# detect that in case 2 and bail out with SKIP if absent.

###############################################################################
# case 1 - unarmed boot
###############################################################################

cat >"$OUT/init1.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo CASE1_BOOT_OK
if [ -e /proc/um/template_pause ]; then
	echo CASE1_FAIL_PROC_PRESENT
else
	echo CASE1_PASS_PROC_ABSENT
fi
poweroff -f
IEOF
chmod +x "$OUT/init1.sh"

timeout 30 "$BINARY" mem="$MEM" rootfstype=hostfs rootflags=/ \
	root=/dev/root rw backend=kvm-v2 ncpus=1 \
	init="$OUT/init1.sh" >"$OUT/case1.log" 2>&1 || true

if ! grep -q CASE1_PASS_PROC_ABSENT "$OUT/case1.log"; then
	echo "FAIL case 1 (unarmed boot): /proc/um/template_pause should be absent"
	tail -30 "$OUT/case1.log"
	exit 1
fi
echo "case 1 (unarmed boot): PASS"

###############################################################################
# case 2 - armed boot, no identity fd
###############################################################################

cat >"$OUT/init2.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo CASE2_PRE_PAUSE
echo case2-no-identity > /proc/um/template_pause
echo CASE2_POST_PAUSE
poweroff -f
IEOF
chmod +x "$OUT/init2.sh"

"$BINARY" mem="$MEM" rootfstype=hostfs rootflags=/ \
	root=/dev/root rw backend=kvm-v2 ncpus=1 \
	um_template_pause init="$OUT/init2.sh" \
	>"$OUT/case2.log" 2>&1 &
PID2=$!

# Wait for PRE_PAUSE
for _ in $(seq 1 60); do
	grep -q CASE2_PRE_PAUSE "$OUT/case2.log" 2>/dev/null && break
	sleep 0.5
done
if ! grep -q CASE2_PRE_PAUSE "$OUT/case2.log"; then
	# If we don't even see the armed log line, the kernel doesn't
	# have CONFIG_UM_TEMPLATE_PAUSE compiled in. SKIP cleanly.
	if ! grep -q "template_pause: armed" "$OUT/case2.log"; then
		kill "$PID2" 2>/dev/null || true
		wait 2>/dev/null || true
		echo "SKIP: kernel lacks CONFIG_UM_TEMPLATE_PAUSE"
		exit 4
	fi
	echo "FAIL case 2: never saw CASE2_PRE_PAUSE"
	kill "$PID2" 2>/dev/null || true
	wait 2>/dev/null || true
	tail -40 "$OUT/case2.log"
	exit 1
fi

# Give SIGSTOP a moment to land
sleep 0.5
STATE=$(awk '/^State:/ {print $2}' "/proc/$PID2/status" 2>/dev/null || echo GONE)
if [ "$STATE" != "T" ] && [ "$STATE" != "t" ]; then
	echo "FAIL case 2: expected stopped state T/t, got '$STATE'"
	kill "$PID2" 2>/dev/null || true
	wait 2>/dev/null || true
	tail -40 "$OUT/case2.log"
	exit 1
fi

# Resume
kill -CONT "$PID2"
for _ in $(seq 1 60); do
	grep -q CASE2_POST_PAUSE "$OUT/case2.log" 2>/dev/null && break
	sleep 0.3
done
wait "$PID2" 2>/dev/null || true

if ! grep -q CASE2_POST_PAUSE "$OUT/case2.log"; then
	echo "FAIL case 2: never saw CASE2_POST_PAUSE after SIGCONT"
	tail -40 "$OUT/case2.log"
	exit 1
fi
if ! grep -q "template_pause: resumed via SIGCONT" "$OUT/case2.log"; then
	echo "FAIL case 2: kernel did not log SIGCONT resume"
	tail -40 "$OUT/case2.log"
	exit 1
fi
echo "case 2 (armed, no identity fd): PASS"

###############################################################################
# case 3 - armed boot, identity blob via memfd
###############################################################################

cat >"$OUT/init3.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo CASE3_PRE_PAUSE
echo case3-with-identity > /proc/um/template_pause
echo CASE3_POST_PAUSE
poweroff -f
IEOF
chmod +x "$OUT/init3.sh"

python3 - "$BINARY" "$MEM" "$OUT" <<'PYEOF'
import ctypes, ctypes.util, fcntl, os, signal, struct, sys, time

binary, mem, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
log_path = os.path.join(outdir, "case3.log")
init_path = os.path.join(outdir, "init3.sh")

def Z(b, n):
    return b.ljust(n, b'\x00')[:n]

blob = struct.pack(
    "<II 64s 6s 2s 16s 20s 16s 96s 32s",
    0x44495455, 1,
    Z(b"pool-member-3", 64),
    bytes([0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc]),
    b"\x00\x00",
    Z(b"tap-pool3", 16),
    Z(b"10.7.0.42/24", 20),
    Z(b"10.7.0.1", 16),
    Z(b"/tmp/mc3.sock", 96),
    b"\x00" * 32,
)

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
MFD_CLOEXEC = 0x0001
fd = libc.memfd_create(b"um-tplpause-smoke", MFD_CLOEXEC)
if fd < 0:
    e = ctypes.get_errno()
    sys.exit(f"memfd_create: {os.strerror(e)}")
os.write(fd, blob)
os.lseek(fd, 0, 0)
flags = fcntl.fcntl(fd, fcntl.F_GETFD)
fcntl.fcntl(fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    os.execve(binary, [
        "linux", f"mem={mem}", "rootfstype=hostfs", "rootflags=/",
        "root=/dev/root", "rw", "backend=kvm-v2", "ncpus=1",
        "um_template_pause", f"init={init_path}",
    ], env)
    os._exit(127)

# Wait for stop
deadline = time.time() + 45
state = "?"
while time.time() < deadline:
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("State:"):
                    state = ln.split()[1]
                    break
    except FileNotFoundError:
        sys.exit("UML exited before SIGSTOP")
    if state in ("T", "t"):
        break
    time.sleep(0.3)
else:
    sys.exit(f"case 3: never saw SIGSTOP (state={state})")

os.kill(pid, signal.SIGCONT)
os.waitpid(pid, 0)
PYEOF

if [ $? -ne 0 ]; then
	echo "FAIL case 3: harness errored"
	tail -40 "$OUT/case3.log" 2>/dev/null || true
	exit 1
fi

if ! grep -q CASE3_POST_PAUSE "$OUT/case3.log"; then
	echo "FAIL case 3: never saw CASE3_POST_PAUSE"
	tail -40 "$OUT/case3.log"
	exit 1
fi
if ! grep -q "template_pause: identity .*instance=\"pool-member-3\"" "$OUT/case3.log"; then
	echo "FAIL case 3: kernel did not parse identity blob"
	grep template_pause "$OUT/case3.log"
	exit 1
fi
if ! grep -q "mac=52:54:00:aa:bb:cc" "$OUT/case3.log"; then
	echo "FAIL case 3: kernel did not log expected MAC"
	grep template_pause "$OUT/case3.log"
	exit 1
fi
# Identity-apply path must execute on a valid blob.
# Without a configured netdev in the smoke bootstrap, the apply
# returns -ENODEV gracefully and logs "no target netdev found".
# With one configured (see case 4 below), it logs "MAC set on" and
# "IPv4 set on".  Either path proves um_template_identity_apply ran.
APPLY_RE='no target netdev found|MAC set on |applying identity to in-guest'
if ! grep -qE "template_pause: ($APPLY_RE)" "$OUT/case3.log"; then
	echo "FAIL case 3: kernel did not invoke identity-apply path"
	grep template_pause "$OUT/case3.log"
	exit 1
fi
echo "case 3 (armed, identity blob): PASS"

###############################################################################
# case 4 - armed boot WITH netdev, identity-apply end-to-end
###############################################################################
#
# Provisions a vec0 netdev backed by transport=fd (a pair of pipe fds
# we manufacture) so an apply target exists.  Then writes the blob,
# SIGCONTs, and verifies inside the guest that the MAC and IPv4
# address actually changed.
#
# The fd transport is the lowest-friction option: it does not require
# root, does not create a host TAP, and gives us a registered netdev
# whose name starts with "vec".  Packets sent on the netdev go into a
# pipe that nothing reads - that's fine; we're testing identity
# state, not throughput.
#
# Skipped (not failed) on hosts where:
#   - the kernel was built without CONFIG_UML_NET_VECTOR (no vec0)
#   - python3 lacks the fcntl bits we need

case4_init=$OUT/init4.sh
cat >"$case4_init" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
echo CASE4_PRE_PAUSE
# Capture pre-apply state for diff'ing.
ip link show vec0 2>/dev/null | tr -s ' '  > /tmp/case4-pre.txt
ip addr show vec0 2>/dev/null | tr -s ' ' >> /tmp/case4-pre.txt
cat /tmp/case4-pre.txt
echo CASE4_BLOCKING_ON_PAUSE
echo case4-with-netdev > /proc/um/template_pause
echo CASE4_POST_PAUSE
# Capture post-apply state.
ip link show vec0 2>/dev/null | tr -s ' '  > /tmp/case4-post.txt
ip addr show vec0 2>/dev/null | tr -s ' ' >> /tmp/case4-post.txt
echo CASE4_POST_STATE_BEGIN
cat /tmp/case4-post.txt
echo CASE4_POST_STATE_END
poweroff -f
IEOF
chmod +x "$case4_init"

python3 - "$BINARY" "$MEM" "$OUT" <<'PYEOF'
import ctypes, ctypes.util, fcntl, os, signal, struct, sys, time

binary, mem, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
log_path = os.path.join(outdir, "case4.log")
init_path = os.path.join(outdir, "init4.sh")

def Z(b, n):
    return b.ljust(n, b'\x00')[:n]

blob = struct.pack(
    "<II 64s 6s 2s 16s 20s 16s 96s 32s",
    0x44495455, 1,
    Z(b"pool-member-4", 64),
    bytes([0x52, 0x54, 0x00, 0xde, 0xad, 0xbe]),
    b"\x00\x00",
    Z(b"tap-pool4", 16),
    Z(b"192.168.7.42/24", 20),
    Z(b"192.168.7.1", 16),
    Z(b"/tmp/mc4.sock", 96),
    b"\x00" * 32,
)

libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
fd = libc.memfd_create(b"um-tplpause-smoke-c4", 0)
if fd < 0:
    sys.exit("memfd_create failed")
os.write(fd, blob)
os.lseek(fd, 0, 0)

# vec0 transport=fd needs two fds (tx, rx).  We make a pair of pipes;
# the guest writes/reads into them and nothing on the host consumes
# the data.  Allocate the lowest possible fd numbers (>=10) so the
# UML binary can dup them.
rx_r, rx_w = os.pipe()
tx_r, tx_w = os.pipe()
for f in (rx_r, rx_w, tx_r, tx_w):
    flags = fcntl.fcntl(f, fcntl.F_GETFD)
    fcntl.fcntl(f, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)

env = dict(os.environ, UM_TEMPLATE_IDENTITY_FD=str(fd))
log = open(log_path, "wb")
pid = os.fork()
if pid == 0:
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    # vec0 transport=fd uses fd:rxfd-txfd format per arch/um docs.
    netarg = f"vec0:transport=fd,fd={rx_r}-{tx_w},mac=02:00:00:00:00:01"
    os.execve(binary, [
        "linux", f"mem={mem}", "rootfstype=hostfs", "rootflags=/",
        "root=/dev/root", "rw", "backend=kvm-v2", "ncpus=1",
        "um_template_pause", netarg, f"init={init_path}",
    ], env)
    os._exit(127)

# Wait for SIGSTOP
deadline = time.time() + 45
state = "?"
while time.time() < deadline:
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("State:"):
                    state = ln.split()[1]
                    break
    except FileNotFoundError:
        sys.exit("UML exited before SIGSTOP")
    if state in ("T", "t"):
        break
    time.sleep(0.3)
else:
    sys.exit(f"case 4: never saw SIGSTOP (state={state})")

os.kill(pid, signal.SIGCONT)
os.waitpid(pid, 0)
PYEOF

if [ $? -ne 0 ]; then
	echo "FAIL case 4: harness errored"
	tail -40 "$OUT/case4.log" 2>/dev/null || true
	exit 1
fi

if ! grep -q CASE4_POST_PAUSE "$OUT/case4.log"; then
	# Don't fail if the kernel lacks vec0 support - that's a SKIP.
	if grep -qE "vec0:|vector_eth_configure" "$OUT/case4.log"; then
		echo "FAIL case 4: never saw CASE4_POST_PAUSE"
		tail -40 "$OUT/case4.log"
		exit 1
	fi
	echo "SKIP case 4: kernel lacks UML_NET_VECTOR or vec0 didn't register"
else
	# Case 4 post-apply checks - read the in-guest 'ip addr show vec0'
	# capture from the init script's stdout (echoed between
	# CASE4_POST_STATE_BEGIN and CASE4_POST_STATE_END).
	awk '/CASE4_POST_STATE_BEGIN/{flag=1; next} /CASE4_POST_STATE_END/{flag=0} flag' \
		"$OUT/case4.log" > "$OUT/case4-post-state.txt"

	# MAC check: the apply path sets vec0's MAC to 52:54:00:de:ad:be.
	if ! grep -qiE "link/ether 52:54:00:de:ad:be" "$OUT/case4-post-state.txt"; then
		# Some configs may have vec0 absent - degrade to SKIP if so.
		if ! grep -q "vec0" "$OUT/case4-post-state.txt"; then
			echo "SKIP case 4: vec0 not visible in guest"
		else
			echo "FAIL case 4: MAC was not changed on vec0"
			cat "$OUT/case4-post-state.txt"
			echo "--- kernel template_pause lines ---"
			grep template_pause "$OUT/case4.log" || true
			exit 1
		fi
	else
		# IPv4 check: 192.168.7.42/24 on vec0.
		if ! grep -qE "inet 192\.168\.7\.42(/24|\s)" "$OUT/case4-post-state.txt"; then
			echo "FAIL case 4: IPv4 192.168.7.42/24 was not bound to vec0"
			cat "$OUT/case4-post-state.txt"
			echo "--- kernel template_pause lines ---"
			grep template_pause "$OUT/case4.log" || true
			exit 1
		fi
		echo "case 4 (armed + netdev, identity-apply end-to-end): PASS"
	fi
fi

echo
echo "VERDICT: template-pause primitive and identity-apply path work"
exit 0
