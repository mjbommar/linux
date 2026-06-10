#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/vector2-fd-multiqueue-smoke - validate launcher-owned vector2 fd
# multiqueue handoff.
#
# This uses the production `umlctl up` path to create a multiqueue TAP, open
# four launcher-owned TAP fds, inherit them into the UML process as fd
# 200..203, and verify that the guest vector2 netdev is configured with four
# queues and can ping the host-side TAP address.
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
NAME="v2mq-$SUFFIX"
TAP="v2mq$SUFFIX"
PROBE_TAP="v2mp$SUFFIX"
HOST_IP="10.92.$OCTET.1"
GUEST_IP="10.92.$OCTET.2"
OUT=$(mktemp -d -t vector2-fd-mq.XXXXXX)
STATE=$(mktemp -d -t vector2-mq-state.XXXXXX)
RUNTIME=$(mktemp -d -t vector2-mq-run.XXXXXX)
UMLFILE="$OUT/vector2-fd-multiqueue.toml"

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
labels = { service = "network-smoke", driver = "vector2", transport = "fd", queues = "4" }

[kernel]
path = "$KERNEL"
backend = "seccomp"
append = []

[runtime]
mem = "384M"
ncpus = 1

[network]
mode = "tap"
driver = "vector2"
host_mode = "auto"
queues = 4
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
name = "queues"
cmd = "ip -d link show \"\$UMLCTL_NETDEV\" | grep -q 'numtxqueues 4'"
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
cmd = "echo VECTOR2_FD_MULTIQUEUE_OK"
EOF

set +e
"$UMLCTL" --state-dir "$STATE" --runtime-dir "$RUNTIME" up \
	-f "$UMLFILE" \
	--wait-for VECTOR2_FD_MULTIQUEUE_OK \
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
if ! grep -q "ip tuntap add dev $TAP mode tap .* multi_queue" "$OUT/up.err"; then
	echo "FAIL: umlctl did not create a multiqueue TAP"
	cat "$OUT/up.out"
	cat "$OUT/up.err"
	exit $KSFT_FAIL
fi
if ! grep -q "network-fd: open tap=$TAP and inherit fds=200..203" \
	"$OUT/up.err"; then
	echo "FAIL: umlctl did not report fd 200..203 TAP handoff"
	cat "$OUT/up.err"
	exit $KSFT_FAIL
fi
if ! grep -q "network: driver=vector2 .* transport=fd host_mode=fd queues=4" \
	"$OUT/up.err"; then
	echo "FAIL: umlctl did not report vector2 multiqueue fd plan"
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
	"UMLCTL_NETWORK_TRANSPORT=fd" \
	"UMLCTL_NETWORK_HOST_MODE=fd" \
	"UMLCTL_NETWORK_FD=200" \
	"UMLCTL_NETWORK_FD_COUNT=4" \
	"UMLCTL_NETWORK_QUEUES=4" \
	"UMLCTL_NETDEV=vec2.0" \
	"numtxqueues 4" \
	"$GUEST_IP" \
	"VECTOR2_FD_MULTIQUEUE_OK"; do
	if ! grep -q "$pat" "$INIT_LOG"; then
		echo "FAIL: init.log missing '$pat'"
		cat "$INIT_LOG"
		exit $KSFT_FAIL
	fi
done

echo "vector2 fd multiqueue: PASS (tap=$TAP fds=200..203 guest=$GUEST_IP)"
exit $KSFT_PASS
