#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/vector2-inproc-tap-smoke - validate vector2 trusted in-process TAP mode.
#
# This drives `umlctl up` through the explicit trusted host path:
# `network.host_mode = "inproc"` generates a vector2 TAP kernel argument,
# does not inherit launcher-owned TAP fds, and lets the UML process open the
# host TAP itself. The guest then verifies metadata, address assignment, and
# host TAP reachability.
#
# Exit codes: 0 PASS, 4 SKIP, 1 FAIL.

set -u

KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../../../.." && pwd)
KERNEL=${UML_KERNEL:-${UM_FORK_KERNEL:-$ROOT/linux}}
UMLCTL=${UMLCTL:-}
IP=${IP:-}
OWNER=${UML_TAP_USER:-$(id -un)}

if [ -z "$UMLCTL" ]; then
	for cand in \
		"$ROOT/tools/uml/uml-launcher/target/release/umlctl" \
		"$ROOT/tools/uml/uml-launcher/target/debug/umlctl"; do
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
	echo "SKIP: UML kernel $KERNEL not found (set UML_KERNEL)"
	exit $KSFT_SKIP
fi
if [ ! -f "$(dirname "$KERNEL")/.config" ]; then
	echo "SKIP: no .config alongside UML kernel $KERNEL"
	exit $KSFT_SKIP
fi
if ! grep -q "^CONFIG_UML_NET_VECTOR_V2=y" "$(dirname "$KERNEL")/.config"; then
	echo "SKIP: UML kernel lacks CONFIG_UML_NET_VECTOR_V2=y"
	exit $KSFT_SKIP
fi
if ! grep -q "^CONFIG_UML_NET_VECTOR_V2_INPROC=y" \
	"$(dirname "$KERNEL")/.config"; then
	echo "SKIP: UML kernel lacks CONFIG_UML_NET_VECTOR_V2_INPROC=y"
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
	echo "SKIP: sudo required for TAP setup"
	exit $KSFT_SKIP
fi
if ! command -v iptables >/dev/null 2>&1; then
	echo "SKIP: iptables required for umlctl network setup"
	exit $KSFT_SKIP
fi

SUFFIX=$$
OCTET=$(( (SUFFIX % 180) + 40 ))
NAME="v2inproc-$SUFFIX"
TAP="v2ip$SUFFIX"
PROBE_TAP="v2ipp$SUFFIX"
HOST_IP="10.93.$OCTET.1"
GUEST_IP="10.93.$OCTET.2"
OUT=$(mktemp -d -t vector2-inproc-tap.XXXXXX)
STATE=$(mktemp -d -t vector2-inproc-state.XXXXXX)
RUNTIME=$(mktemp -d -t vector2-inproc-run.XXXXXX)
UMLFILE="$OUT/vector2-inproc-tap.toml"

cleanup() {
	"$UMLCTL" --state-dir "$STATE" --runtime-dir "$RUNTIME" down \
		-f "$UMLFILE" --rm --force >/dev/null 2>&1 || true
	sudo -n "$IP" link delete "$TAP" >/dev/null 2>&1 || true
	sudo -n "$IP" link delete "$PROBE_TAP" >/dev/null 2>&1 || true
	rm -rf "$OUT" "$STATE" "$RUNTIME"
}
trap cleanup EXIT

if ! sudo -n "$IP" tuntap add mode tap dev "$PROBE_TAP" user "$OWNER" \
	>"$OUT/probe.out" 2>"$OUT/probe.err"; then
	echo "SKIP: no passwordless sudo/tuntap access for $OWNER"
	cat "$OUT/probe.err"
	exit $KSFT_SKIP
fi
sudo -n "$IP" link delete "$PROBE_TAP" >/dev/null 2>&1 || true

cat >"$UMLFILE" <<EOF
schema_version = 1
volumes = []

[instance]
name = "$NAME"
labels = { service = "network-smoke", driver = "vector2", transport = "tap", host_mode = "inproc" }

[kernel]
path = "$KERNEL"
backend = "seccomp"
append = []

[runtime]
mem = "256M"
ncpus = 1

[network]
mode = "tap"
driver = "vector2"
host_mode = "inproc"
queues = 1
tap_name = "$TAP"
guest_ip = "$GUEST_IP/24"
host_ip = "$HOST_IP/24"
gateway = "$HOST_IP"
nameservers = []
masquerade_via = "auto"
ports = []

[env]
PATH = "/usr/bin:/bin:/sbin:/usr/sbin"

[[init.phases]]
name = "network-metadata"
cmd = "env | grep '^UMLCTL_NETWORK_' | sort; echo UMLCTL_NETDEV=\$UMLCTL_NETDEV"

[[init.phases]]
name = "link"
cmd = "ip -d link show \"\$UMLCTL_NETDEV\"; ip addr show \"\$UMLCTL_NETDEV\""
timeout_secs = 20

[[init.phases]]
name = "address"
cmd = "ip addr show \"\$UMLCTL_NETDEV\" | grep -q 'inet $GUEST_IP/'"
timeout_secs = 20

[[init.phases]]
name = "gateway"
cmd = "ping -c 1 -W 1 \"\$UMLCTL_GATEWAY\""
timeout_secs = 20

[[init.phases]]
name = "ready"
cmd = "echo VECTOR2_INPROC_TAP_OK"
EOF

set +e
"$UMLCTL" --state-dir "$STATE" --runtime-dir "$RUNTIME" up \
	-f "$UMLFILE" \
	--wait-for VECTOR2_INPROC_TAP_OK \
	--wait-timeout 150 \
	>"$OUT/up.out" 2>"$OUT/up.err"
UP_RC=$?
set -u

if [ $UP_RC -ne 0 ]; then
	echo "FAIL: umlctl up exited $UP_RC"
	cat "$OUT/up.out"
	cat "$OUT/up.err"
	exit $KSFT_FAIL
fi
if ! grep -q "network: driver=vector2 .* transport=tap host_mode=inproc queues=1" \
	"$OUT/up.err"; then
	echo "FAIL: umlctl did not report vector2 in-process TAP plan"
	cat "$OUT/up.err"
	exit $KSFT_FAIL
fi
if grep -q "network-fd:" "$OUT/up.err"; then
	echo "FAIL: in-process TAP path unexpectedly reported fd inheritance"
	cat "$OUT/up.err"
	exit $KSFT_FAIL
fi

RUN_DIR=$(find "$STATE/runs" -mindepth 1 -maxdepth 1 -type d | head -n 1)
INIT_LOG="$RUN_DIR/init.log"
if [ -z "$RUN_DIR" ] || [ ! -f "$INIT_LOG" ]; then
	echo "FAIL: init.log not found under $STATE/runs"
	find "$STATE" -maxdepth 4 -type f -print
	exit $KSFT_FAIL
fi
for pat in \
	"UMLCTL_NETWORK_DRIVER=vector2" \
	"UMLCTL_NETWORK_TRANSPORT=tap" \
	"UMLCTL_NETWORK_HOST_MODE=inproc" \
	"UMLCTL_NETWORK_FD_COUNT=0" \
	"UMLCTL_NETWORK_QUEUES=1" \
	"UMLCTL_NETDEV=vec2.0" \
	"$GUEST_IP" \
	"VECTOR2_INPROC_TAP_OK"; do
	if ! grep -q "$pat" "$INIT_LOG"; then
		echo "FAIL: init.log missing '$pat'"
		cat "$INIT_LOG"
		exit $KSFT_FAIL
	fi
done

echo "vector2 in-process TAP: PASS (tap=$TAP guest=$GUEST_IP)"
exit $KSFT_PASS
