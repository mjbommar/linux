#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-serve-smoke - kselftest for `umlctl pool serve`.
#
# What it asserts end-to-end:
#   1. `umlctl pool serve --background` boots a CONFIG_UM_TEMPLATE_PAUSE_FORK
#      master, writes its pidfile, and starts listening on the api.sock.
#   2. `--min-warm=1` pre-fills one anonymous ready member.
#   3. An anonymous {"op":"take"} consumes a ready member, returns a
#      live pid, and the daemon replenishes the ready queue.
#   4. A {"op":"take", ...} JSON-line RPC over the socket returns a
#      result containing a live, runnable replicated pool-member pid.
#   5. {"op":"destroy", "pid":N} reports destroyed=true and the pid
#      is no longer alive.
#   6. {"op":"shutdown"} terminates the daemon + master cleanly
#      (pidfile gone, socket gone, master pid no longer alive).
#
# Exit codes: 0 PASS, 4 SKIP (no fork-kernel / no binary), 1 FAIL.
#
# Environment:
#   UM_FORK_KERNEL  Fork-mode UML kernel.  Default
#                   $HOME/src/uml-builds/uml-tplpause-fork/linux.
#                   Used by the mission gate's fork-server check.
#   UMLCTL          umlctl binary.  Auto-detected from the source tree.

set -u

KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
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
	echo "SKIP: python3 required for socket RPC"
	exit 4
fi

POOL_NAME="serve-smoke-$$"
OUT=$(mktemp -d -t pool-serve-smoke.XXXXXX)
RUNTIME=$(mktemp -d -t pool-serve-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"

cleanup() {
	# Best-effort: ask the daemon to shut down first; SIGKILL it if
	# it's still around; then sweep the master pid we saved.
	if [ -S "$SOCK" ]; then
		rpc shutdown >/dev/null 2>&1 || true
	fi
	if [ -f "$PIDFILE" ]; then
		dpid=$(cat "$PIDFILE" 2>/dev/null || true)
		[ -n "$dpid" ] && kill -KILL "$dpid" 2>/dev/null || true
	fi
	if [ -n "${MASTER_PID:-}" ]; then
		kill -KILL "$MASTER_PID" 2>/dev/null || true
	fi
	if [ -n "${TAKEN_PID:-}" ]; then
		kill -KILL "$TAKEN_PID" 2>/dev/null || true
	fi
	if [ -n "${WARM_PID:-}" ]; then
		kill -KILL "$WARM_PID" 2>/dev/null || true
	fi
	rm -rf "$OUT" "$RUNTIME"
}
trap cleanup EXIT

# RPC helper.  Sends one JSON line, reads one JSON line back.
rpc() {
	python3 -c "
import json, socket, sys
op = sys.argv[1]
extra = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
extra['op'] = op
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(30)
s.connect('$SOCK')
s.sendall((json.dumps(extra) + '\n').encode())
buf = b''
while not buf.endswith(b'\n'):
    chunk = s.recv(4096)
    if not chunk:
        break
    buf += chunk
print(buf.decode().rstrip())
" "$@"
}

# Launch the daemon in the background.
"$UMLCTL" --runtime-dir "$RUNTIME" pool serve \
	--name "$POOL_NAME" \
	--kernel "$KERNEL" \
	--mem 128M \
	--min-warm 1 \
	--background \
	>"$OUT/serve.out" 2>"$OUT/serve.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: umlctl pool serve --background exited $RC"
	cat "$OUT/serve.err"
	exit 1
fi

# Wait up to 30s for the socket to appear.
DEADLINE=$(( $(date +%s) + 30 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt $DEADLINE ]; then
		echo "FAIL: socket $SOCK not created in 30s"
		ls -la "$RUNTIME/pools/$POOL_NAME/" 2>/dev/null
		cat "$OUT/serve.err"
		exit 1
	fi
	sleep 0.1
done
echo "daemon socket up at $SOCK: PASS"

if [ ! -f "$PIDFILE" ]; then
	echo "FAIL: pidfile $PIDFILE missing"
	exit 1
fi
DAEMON_PID=$(cat "$PIDFILE")
echo "daemon pid=$DAEMON_PID"

# status RPC: extract master_pid for later kill.
STATUS=$(rpc status)
if ! echo "$STATUS" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('ok') else 1)"; then
	echo "FAIL: status RPC did not return ok=true"
	echo "$STATUS"
	exit 1
fi
MASTER_PID=$(echo "$STATUS" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['master_pid'])")
echo "master_pid=$MASTER_PID: PASS"
if ! kill -0 "$MASTER_PID" 2>/dev/null; then
	echo "FAIL: master pid $MASTER_PID not alive at status time"
	exit 1
fi

runnable() {
	local p=$1
	local state
	state=$(awk '{ for (i=NF; i>=1; i--) if ($i ~ /^[RSDTZXIt]$/) { print $i; exit } }' \
		"/proc/$p/stat" 2>/dev/null || true)
	[ -n "$state" ] && [ "$state" != "Z" ] && [ "$state" != "X" ]
}

MIN_WARM=$(echo "$STATUS" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['min_warm'])")
READY_COUNT=$(echo "$STATUS" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['ready'])")
FAILED_COUNT=$(echo "$STATUS" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['failed'])")
if [ "$MIN_WARM" -ne 1 ]; then
	echo "FAIL: min_warm status was $MIN_WARM, expected 1"
	echo "$STATUS"
	exit 1
fi
if [ "$FAILED_COUNT" -ne 0 ]; then
	echo "FAIL: warm replenish failed before first take"
	echo "$STATUS"
	exit 1
fi
if [ "$READY_COUNT" -lt 1 ]; then
	echo "FAIL: min_warm did not prefill ready member"
	echo "$STATUS"
	exit 1
fi
echo "warm ready prefilled: PASS"

WARM_TAKE=$("$UMLCTL" --runtime-dir "$RUNTIME" pool take \
	--name "$POOL_NAME" --ready --json)
if ! echo "$WARM_TAKE" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('pid', 0) > 0 else 1)"; then
	echo "FAIL: pool take --ready did not return a member JSON object"
	echo "$WARM_TAKE"
	exit 1
fi
WARM_PID=$(echo "$WARM_TAKE" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['pid'])")
if [ -z "$WARM_PID" ] || [ "$WARM_PID" -le 0 ]; then
	echo "FAIL: invalid warm pid=$WARM_PID"
	exit 1
fi
if ! runnable "$WARM_PID"; then
	echo "FAIL: warm pid $WARM_PID is not runnable"
	exit 1
fi
echo "warm take returned live pid=$WARM_PID: PASS"

STATUS_AFTER_WARM=$(rpc status)
READY_AFTER_WARM=$(echo "$STATUS_AFTER_WARM" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['ready'])")
TAKEN_AFTER_WARM=$(echo "$STATUS_AFTER_WARM" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['taken'])")
FAILED_AFTER_WARM=$(echo "$STATUS_AFTER_WARM" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['failed'])")
if [ "$TAKEN_AFTER_WARM" -lt 1 ]; then
	echo "FAIL: daemon did not retain warm member as taken"
	echo "$STATUS_AFTER_WARM"
	exit 1
fi
if [ "$READY_AFTER_WARM" -lt 1 ] || [ "$FAILED_AFTER_WARM" -ne 0 ]; then
	echo "FAIL: warm queue did not replenish after take"
	echo "$STATUS_AFTER_WARM"
	exit 1
fi
echo "warm queue replenished: PASS"

WARM_DESTROY=$(rpc destroy "{\"pid\":$WARM_PID}")
if ! echo "$WARM_DESTROY" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('ok') else 1)"; then
	echo "FAIL: warm destroy RPC did not return ok=true"
	echo "$WARM_DESTROY"
	exit 1
fi
echo "warm destroy RPC ok: PASS"
WARM_PID=

# take RPC with request-specific identity.
TAKE_PAYLOAD='{"instance":"serve-smoke-m1","mac":"52:54:00:11:22:33","tap":"tap-ss","ipv4":"10.7.0.42/24","gateway":"10.7.0.1","mconsole":""}'
TAKE=$(rpc take "$TAKE_PAYLOAD")
if ! echo "$TAKE" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('ok') else 1)"; then
	echo "FAIL: take RPC did not return ok=true"
	echo "$TAKE"
	exit 1
fi
TAKEN_PID=$(echo "$TAKE" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['result']['pid'])")
echo "took member pid=$TAKEN_PID: PASS"

if [ -z "$TAKEN_PID" ] || [ "$TAKEN_PID" -le 0 ]; then
	echo "FAIL: invalid taken pid=$TAKEN_PID"
	exit 1
fi
echo "taken pid valid: PASS"

if ! runnable "$TAKEN_PID"; then
	echo "FAIL: taken pid $TAKEN_PID is not a live runnable member"
	exit 1
fi
echo "taken pid live/runnable: PASS"

STATUS_AFTER_TAKE=$(rpc status)
TAKEN_COUNT=$(echo "$STATUS_AFTER_TAKE" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['taken'])")
if [ "$TAKEN_COUNT" -lt 1 ]; then
	echo "FAIL: daemon status did not retain live member"
	echo "$STATUS_AFTER_TAKE"
	exit 1
fi
echo "daemon retained live member: PASS"

# destroy RPC.
DESTROY=$(rpc destroy "{\"pid\":$TAKEN_PID}")
if ! echo "$DESTROY" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('ok') else 1)"; then
	echo "FAIL: destroy RPC did not return ok=true"
	echo "$DESTROY"
	exit 1
fi
echo "destroy RPC ok: PASS"
# The live member is the master's child, not the daemon's. The destroy contract
# is: "no longer a runnable process" - gone or Z/X state. Mirror the daemon's
# pid_runnable() check here.
not_runnable() {
	local p=$1
	local state
	state=$(awk '{ for (i=NF; i>=1; i--) if ($i ~ /^[RSDTZXIt]$/) { print $i; exit } }' \
		"/proc/$p/stat" 2>/dev/null || true)
	[ -z "$state" ] && return 0  # gone from /proc
	[ "$state" = "Z" ] || [ "$state" = "X" ]
}
if ! not_runnable "$TAKEN_PID"; then
	echo "FAIL: taken pid $TAKEN_PID still runnable after destroy"
	exit 1
fi
echo "taken pid no longer runnable: PASS"

# shutdown RPC.
SHUTDOWN=$(rpc shutdown)
if ! echo "$SHUTDOWN" | python3 -c \
	"import json,sys;o=json.load(sys.stdin);sys.exit(0 if o.get('ok') else 1)"; then
	echo "FAIL: shutdown RPC did not return ok=true"
	echo "$SHUTDOWN"
	exit 1
fi

# Wait up to 10s for the daemon to exit + clean up.
DEADLINE=$(( $(date +%s) + 10 ))
while [ -S "$SOCK" ] || kill -0 "$DAEMON_PID" 2>/dev/null; do
	if [ "$(date +%s)" -gt $DEADLINE ]; then
		echo "FAIL: daemon did not exit within 10s of shutdown"
		exit 1
	fi
	sleep 0.1
done
echo "daemon exited cleanly: PASS"

if kill -0 "$MASTER_PID" 2>/dev/null; then
	echo "FAIL: master pid $MASTER_PID still alive after shutdown"
	exit 1
fi
echo "master killed on shutdown: PASS"

# Don't double-kill on cleanup.
MASTER_PID=
TAKEN_PID=

echo
echo "VERDICT: umlctl pool serve daemon lifecycle works end-to-end"
exit 0
