#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-port-forward-smoke - kselftest for `umlctl port-forward`
# metadata.
#
# What it asserts:
#   1. After taking a member with --gateway 10.7.0.1, `umlctl
#      port-forward --pid <pid> --host-port 35419 --json` returns a
#      well-typed envelope: schema_version="port-forward/1",
#      guest_address ends in ":35419", scheme="tap-direct".
#   2. Querying a non-existent pid surfaces a clean error (non-zero
#      exit, error string mentions "not found in daemon's member
#      table").
#   3. Querying without --host-port is a clap-level error (exit 2).
#
# The verb is metadata-only; vector2 pool-member TAP connectivity is covered by
# vector2-pool-tap-smoke.
#
# Exit codes: 0 PASS, 4 SKIP, 1 FAIL.

set -u

KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
if [ ! -x "$KERNEL" ]; then
	KERNEL=$HOME/src/uml-builds/uml-smp-t41fix/linux
fi
UMLCTL=${UMLCTL:-}

if [ -z "$UMLCTL" ]; then
	for cand in \
		"$(dirname "$0")/../../../../uml/uml-launcher/target/release/umlctl" \
		"$(dirname "$0")/../../../../uml/uml-launcher/target/debug/umlctl"; do
		if [ -x "$cand" ]; then
			UMLCTL=$cand
			break
		fi
	done
fi

if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
	echo "SKIP: umlctl not built (set UMLCTL=...)"
	exit 4
fi
if [ ! -x "$KERNEL" ]; then
	echo "SKIP: fork-mode UML kernel $KERNEL not found (set UM_FORK_KERNEL)"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required for JSON parsing"
	exit 4
fi

POOL_NAME="pf-smoke-$$"
OUT=$(mktemp -d -t pf-smoke.XXXXXX)
RUNTIME=$(mktemp -d -t pf-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"

cleanup() {
	if [ -S "$SOCK" ]; then
		python3 -c "
import json, socket
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
    s.connect('$SOCK')
    s.sendall(b'{\"op\":\"shutdown\"}\n'); s.recv(4096)
except Exception:
    pass
" 2>/dev/null || true
	fi
	if [ -f "$PIDFILE" ]; then
		dpid=$(cat "$PIDFILE" 2>/dev/null || true)
		[ -n "$dpid" ] && kill -KILL "$dpid" 2>/dev/null || true
	fi
	rm -rf "$OUT" "$RUNTIME"
}
trap cleanup EXIT

"$UMLCTL" --runtime-dir "$RUNTIME" pool serve \
	--name "$POOL_NAME" --kernel "$KERNEL" --mem 128M --background \
	>"$OUT/serve.out" 2>"$OUT/serve.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: pool serve exited $RC"; cat "$OUT/serve.err"; exit 1
fi

DEADLINE=$(( $(date +%s) + 30 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt $DEADLINE ]; then
		echo "FAIL: socket $SOCK not created"; cat "$OUT/serve.err"; exit 1
	fi
	sleep 0.1
done
echo "daemon up: PASS"

TAKE_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" pool take \
	--name "$POOL_NAME" --instance "pf-m1" \
	--mac "52:54:00:55:55:55" --tap "tap-pf" \
	--ipv4 "10.7.0.42/24" --gateway "10.7.0.1" --json)
TAKEN_PID=$(echo "$TAKE_JSON" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['pid'])")
echo "took pid=$TAKEN_PID: PASS"

# Case 1: well-typed envelope for a real member.
PF_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" port-forward \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --host-port 35419 --json)
echo "port-forward output: $PF_JSON"
python3 - <<PYEOF
import json, sys
o = json.loads('''$PF_JSON''')
assert o.get("ok") is True, o
assert o.get("schema_version") == "port-forward/1", o
assert o.get("scheme") == "tap-direct", o
assert o.get("host_port") == 35419, o
assert o.get("guest_port") == 35419, o
assert o.get("pid") == $TAKEN_PID, o
assert o.get("guest_address", "").endswith(":35419"), o
assert "10.7.0.1" in o.get("guest_address", ""), o
print("envelope_ok")
PYEOF
echo "port-forward envelope shape: PASS"

# Case 2: explicit --guest-port differs from --host-port.
PF_JSON2=$("$UMLCTL" --runtime-dir "$RUNTIME" port-forward \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --host-port 35419 \
	--guest-port 22 --json)
python3 - <<PYEOF
import json
o = json.loads('''$PF_JSON2''')
assert o.get("guest_port") == 22, o
assert o.get("host_port") == 35419, o
print("split_ports_ok")
PYEOF
echo "split host/guest ports: PASS"

# Case 3: bogus pid surfaces clean failure.
set +e
"$UMLCTL" --runtime-dir "$RUNTIME" port-forward \
	--name "$POOL_NAME" --pid 99999999 --host-port 1234 --json \
	>"$OUT/bogus.out" 2>"$OUT/bogus.err"
BOGUS_RC=$?
set -e
if [ $BOGUS_RC -eq 0 ]; then
	echo "FAIL: bogus pid did not surface failure (rc=0)"
	cat "$OUT/bogus.out" "$OUT/bogus.err"; exit 1
fi
if ! grep -q "not found in daemon's member table" "$OUT/bogus.err"; then
	echo "FAIL: bogus pid stderr missing expected diagnostic"
	cat "$OUT/bogus.err"; exit 1
fi
echo "bogus pid surfaces clean failure: PASS"

# Case 4: missing --host-port is a clap error (exit 2).
set +e
"$UMLCTL" --runtime-dir "$RUNTIME" port-forward \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --json \
	>/dev/null 2>"$OUT/missing.err"
MISSING_RC=$?
set -e
if [ $MISSING_RC -ne 2 ]; then
	echo "FAIL: missing --host-port did not exit 2 (got $MISSING_RC)"
	cat "$OUT/missing.err"; exit 1
fi
echo "missing --host-port -> exit 2 (clap): PASS"

echo
echo "VERDICT: umlctl port-forward TAP-direct envelope works end-to-end"
exit 0
