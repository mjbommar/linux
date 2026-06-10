#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/vector2-pool-tap-smoke - validate vector2 TAP handoff in a pool member.
#
# This boots `umlctl pool serve` with a vector2 TAP-backed master, takes one
# member with a different TAP identity, then uses daemon-routed `umlctl exec`
# to verify that the member's vec2.0 netdev has the assigned IPv4 address and
# can reach the host-side TAP address.
#
# Exit codes: 0 PASS, 4 SKIP, 1 FAIL.

set -u

KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
UMLCTL=${UMLCTL:-}
IP=${IP:-}

if [ -z "$UMLCTL" ]; then
	for cand in \
		"$SCRIPT_DIR/../../../../uml/uml-launcher/target/release/umlctl" \
		"$SCRIPT_DIR/../../../../uml/uml-launcher/target/debug/umlctl"; do
		if [ -x "$cand" ]; then
			UMLCTL=$cand
			break
		fi
	done
fi
if [ -z "$IP" ]; then
	IP=$(command -v ip 2>/dev/null || true)
fi
if [ -z "$IP" ] && [ -x /usr/sbin/ip ]; then
	IP=/usr/sbin/ip
fi

if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
	echo "SKIP: umlctl not built (set UMLCTL=...)"
	exit $KSFT_SKIP
fi
if [ ! -x "$KERNEL" ]; then
	echo "SKIP: fork-mode UML kernel $KERNEL not found (set UM_FORK_KERNEL)"
	exit $KSFT_SKIP
fi
if [ -z "$IP" ] || [ ! -x "$IP" ]; then
	echo "SKIP: iproute2 ip(8) not found"
	exit $KSFT_SKIP
fi
if [ ! -e /dev/net/tun ]; then
	echo "SKIP: /dev/net/tun not available"
	exit $KSFT_SKIP
fi
if ! command -v sudo >/dev/null 2>&1; then
	echo "SKIP: sudo required for host TAP setup"
	exit $KSFT_SKIP
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit $KSFT_SKIP
fi

POOL_NAME="v2tap-$$"
MASTER_TAP="v2pm0-$$"
MEMBER_TAP="v2pm1-$$"
PROBE_TAP="v2pmp-$$"
HOST_IP="10.88.42.1"
GUEST_IP="10.88.42.2"
OUT=$(mktemp -d -t vector2-pool-tap.XXXXXX)
RUNTIME=$(mktemp -d -t vector2-pool-tap-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"
TAKEN_PID=0
OWNER=${UML_TAP_USER:-$(id -un)}

cleanup() {
	if [ "$TAKEN_PID" -gt 0 ] 2>/dev/null; then
		"$UMLCTL" --runtime-dir "$RUNTIME" pool destroy \
			--name "$POOL_NAME" "$TAKEN_PID" >/dev/null 2>&1 || true
	fi
	if [ -S "$SOCK" ]; then
		python3 - "$SOCK" <<'PY' >/dev/null 2>&1 || true
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(5)
s.connect(sys.argv[1])
s.sendall(b'{"op":"shutdown"}\n')
s.recv(4096)
PY
	fi
	if [ -f "$PIDFILE" ]; then
		dpid=$(cat "$PIDFILE" 2>/dev/null || true)
		[ -n "$dpid" ] && kill -KILL "$dpid" 2>/dev/null || true
	fi
	sudo -n "$IP" link delete "$MEMBER_TAP" >/dev/null 2>&1 || true
	sudo -n "$IP" link delete "$MASTER_TAP" >/dev/null 2>&1 || true
	sudo -n "$IP" link delete "$PROBE_TAP" >/dev/null 2>&1 || true
	rm -rf "$OUT" "$RUNTIME"
}
trap cleanup EXIT

if ! sudo -n "$IP" tuntap add mode tap dev "$PROBE_TAP" user "$OWNER" \
	>"$OUT/probe.out" 2>"$OUT/probe.err"; then
	echo "SKIP: no passwordless sudo/tuntap access for $OWNER"
	cat "$OUT/probe.err"
	exit $KSFT_SKIP
fi
sudo -n "$IP" link delete "$PROBE_TAP" >/dev/null 2>&1 || true

setup_tap() {
	local tap=$1
	local addr=${2:-}

	sudo -n "$IP" link delete "$tap" >/dev/null 2>&1 || true
	if ! sudo -n "$IP" tuntap add mode tap dev "$tap" user "$OWNER" \
		>"$OUT/$tap.add.out" 2>"$OUT/$tap.add.err"; then
		echo "FAIL: could not create TAP $tap for $OWNER"
		cat "$OUT/$tap.add.err"
		exit $KSFT_FAIL
	fi
	if [ -n "$addr" ]; then
		if ! sudo -n "$IP" addr add "$addr" dev "$tap" \
			>"$OUT/$tap.addr.out" 2>"$OUT/$tap.addr.err"; then
			echo "FAIL: could not assign $addr to TAP $tap"
			cat "$OUT/$tap.addr.err"
			exit $KSFT_FAIL
		fi
	fi
	if ! sudo -n "$IP" link set "$tap" up \
		>"$OUT/$tap.up.out" 2>"$OUT/$tap.up.err"; then
		echo "FAIL: could not set TAP $tap up"
		cat "$OUT/$tap.up.err"
		exit $KSFT_FAIL
	fi
}

setup_tap "$MASTER_TAP"
setup_tap "$MEMBER_TAP" "$HOST_IP/24"
echo "host TAP setup: PASS (master=$MASTER_TAP member=$MEMBER_TAP host=$HOST_IP)"

VEC2_ARG="vec2.0:transport=tap,mode=inproc,ifname=$MASTER_TAP,depth=128"
"$UMLCTL" --runtime-dir "$RUNTIME" pool serve \
	--name "$POOL_NAME" \
	--kernel "$KERNEL" \
	--mem 192M \
	--cmdline "$VEC2_ARG" \
	--background \
	>"$OUT/serve.out" 2>"$OUT/serve.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: umlctl pool serve exited $RC"
	cat "$OUT/serve.err"
	exit $KSFT_FAIL
fi

DEADLINE=$(( $(date +%s) + 60 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt "$DEADLINE" ]; then
		echo "FAIL: socket $SOCK not created in 60s"
		cat "$OUT/serve.err"
		exit $KSFT_FAIL
	fi
	sleep 0.1
done
echo "daemon socket up: PASS"

TAKE_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" pool take \
	--name "$POOL_NAME" --json \
	--instance "v2tap-m1" \
	--mac "52:54:00:66:77:88" \
	--tap "$MEMBER_TAP" \
	--ipv4 "$GUEST_IP/24" \
	--gateway "$HOST_IP" \
	--mconsole "" \
	2>"$OUT/take.err")
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: vector2 pool take exited $RC"
	cat "$OUT/take.err"
	exit $KSFT_FAIL
fi
printf '%s\n' "$TAKE_JSON" >"$OUT/take.json"
TAKEN_PID=$(python3 - "$OUT/take.json" "$MEMBER_TAP" "$GUEST_IP/24" "$HOST_IP" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
need = {
    "tap": sys.argv[2],
    "ipv4_cidr": sys.argv[3],
    "ipv4_gateway": sys.argv[4],
}
for key, val in need.items():
    if r.get(key) != val:
        raise SystemExit(f"{key} mismatch: {r.get(key)!r} != {val!r}")
if not r.get("mconsole_path"):
    raise SystemExit(f"missing mconsole_path: {r}")
pid = int(r.get("pid", 0))
if pid <= 0:
    raise SystemExit(f"bad pid: {pid}")
print(pid)
PY
)
if [ $? -ne 0 ]; then
	echo "FAIL: take JSON did not match vector2 TAP identity"
	cat "$OUT/take.json"
	exit $KSFT_FAIL
fi
echo "pool take with vector2 TAP identity pid=$TAKEN_PID: PASS"

GUEST_CHECK="$OUT/guest-check.sh"
cat >"$GUEST_CHECK" <<EOF
#!/bin/sh
if ! command -v ip >/dev/null 2>&1 || ! command -v ping >/dev/null 2>&1; then
	echo V2_POOL_TAP_SKIP_TOOLS
	exit 77
fi
if ! ip link show vec2.0 >/tmp/v2-pool-link.out 2>&1; then
	echo V2_POOL_TAP_SKIP_NO_VEC2
	cat /proc/net/dev 2>/dev/null || true
	exit 77
fi
ip -d link show vec2.0
ip addr show vec2.0
if ! ip addr show vec2.0 | grep -q 'inet $GUEST_IP/'; then
	echo V2_POOL_TAP_NO_GUEST_IP
	exit 1
fi
ip link set vec2.0 up
ip -d link show vec2.0
if ! ping -c 1 -W 1 $HOST_IP; then
	echo V2_POOL_TAP_PING_FAIL
	exit 1
fi
echo V2_POOL_TAP_OK
EOF
chmod +x "$GUEST_CHECK"

set +e
"$UMLCTL" --runtime-dir "$RUNTIME" exec \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --timeout 20 --json -- \
	/bin/sh "$GUEST_CHECK" \
	>"$OUT/exec.out" 2>"$OUT/exec.err"
EXEC_RC=$?
set -u

python3 - "$OUT/exec.out" "$TAKEN_PID" <<'PY' >"$OUT/parse-exec.out" 2>"$OUT/parse-exec.err"
import json, sys
frames = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
pid = int(sys.argv[2])
if not frames:
    raise SystemExit("no exec frames")
if frames[0].get("type") != "start":
    raise SystemExit(f"first frame is not start: {frames[:1]}")
if frames[0].get("schema_version") != "exec/1":
    raise SystemExit(f"bad schema: {frames[0]}")
if frames[0].get("pid") != pid:
    raise SystemExit(f"wrong pid: {frames[0]}")
if frames[-1].get("type") != "exit":
    raise SystemExit(f"last frame is not exit: {frames[-1]}")
merged = "".join(
    f.get("data", "") for f in frames
    if f.get("type") in ("stdout", "stderr", "console")
)
code = int(frames[-1].get("code", 0))
if code == 77 or "V2_POOL_TAP_SKIP_" in merged:
    print("SKIP", merged.strip())
    sys.exit(4)
if code != 0:
    raise SystemExit(f"exec exit {code}: {merged!r}")
if "V2_POOL_TAP_OK" not in merged:
    raise SystemExit(f"missing V2_POOL_TAP_OK marker: {merged!r}")
if "inet 10.88.42.2/" not in merged:
    raise SystemExit(f"guest address not visible in exec output: {merged!r}")
print(f"EXEC_FRAMES_OK {len(frames)}")
PY
PARSE_RC=$?
if [ $PARSE_RC -eq $KSFT_SKIP ] || [ $EXEC_RC -eq 77 ]; then
	echo "SKIP: guest lacks vector2 pool TAP prerequisites"
	cat "$OUT/parse-exec.out"
	cat "$OUT/exec.err"
	exit $KSFT_SKIP
fi
if [ $PARSE_RC -ne 0 ]; then
	echo "FAIL: vector2 pool TAP exec validation failed"
	cat "$OUT/parse-exec.out"
	cat "$OUT/parse-exec.err"
	cat "$OUT/exec.out"
	cat "$OUT/exec.err"
	exit $KSFT_FAIL
fi
if [ $EXEC_RC -ne 0 ]; then
	echo "FAIL: umlctl exec exited $EXEC_RC, expected 0"
	cat "$OUT/exec.out"
	cat "$OUT/exec.err"
	exit $KSFT_FAIL
fi
echo "vector2 pool member TAP connectivity: PASS ($(cat "$OUT/parse-exec.out"))"

"$UMLCTL" --runtime-dir "$RUNTIME" pool destroy \
	--name "$POOL_NAME" "$TAKEN_PID" \
	>"$OUT/destroy.out" 2>"$OUT/destroy.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: pool destroy exited $RC"
	cat "$OUT/destroy.err"
	exit $KSFT_FAIL
fi
echo "pool destroy: PASS"
TAKEN_PID=0

echo
echo "VERDICT: vector2 TAP handoff works through a live pool member"
exit $KSFT_PASS
