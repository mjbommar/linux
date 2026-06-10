#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/syzkaller-shim-smoke - validate the syzkaller UML shim wire path.
#
# This does not need a full syzkaller checkout.  It verifies the in-tree
# reference shim's command-contract source, then drives the same
# take/exec/port-forward/status/destroy sequence through umlctl that
# vm/uml would use from syzkaller.
#
# Exit codes: 0 PASS, 4 SKIP, 1 FAIL.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SHIM_GO="$SCRIPT_DIR/../../../../uml/syzkaller-vm-shim/uml.go"
KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
UMLCTL=${UMLCTL:-}

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

if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
	echo "SKIP: umlctl not built (set UMLCTL=...)"
	exit 4
fi
if [ ! -x "$KERNEL" ]; then
	echo "SKIP: fork-mode UML kernel $KERNEL not found (set UM_FORK_KERNEL)"
	exit 4
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "SKIP: python3 required"
	exit 4
fi
if [ ! -f "$SHIM_GO" ]; then
	echo "FAIL: syzkaller shim source not found at $SHIM_GO"
	exit 1
fi

POOL_NAME="syz-shim-$$"
OUT=$(mktemp -d -t syzkaller-shim-smoke.XXXXXX)
RUNTIME=$(mktemp -d -t syzkaller-shim-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"
TAKEN_PID=0

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
	rm -rf "$OUT" "$RUNTIME"
}
trap cleanup EXIT

python3 - "$SHIM_GO" <<'PY' >"$OUT/static.out" 2>"$OUT/static.err"
import sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
checks = {
    "runtime_dir config": 'RuntimeDir string `json:"runtime_dir"`' in src,
    "runtime_dir helper": '"--runtime-dir", cfg.RuntimeDir' in src,
    "auto_serve mem": 'fmt.Sprintf("%dM", cfg.MemMB)' in src,
    "take uses helper": 'umlctlArgs(p.cfg,' in src,
    "run uses helper": 'umlctlArgs(inst.pool.cfg, args...)' in src,
    "destroy uses helper": '"pool", "destroy",' in src and
                           'umlctlArgs(inst.pool.cfg,' in src,
}
bad = [name for name, ok in checks.items() if not ok]
if bad:
    print("STATIC_FAIL", ", ".join(bad))
    sys.exit(1)
print("STATIC_OK")
PY
if [ $? -ne 0 ]; then
	echo "FAIL: shim source contract check failed"
	cat "$OUT/static.out"
	cat "$OUT/static.err"
	exit 1
fi
echo "shim source contract: PASS ($(cat "$OUT/static.out"))"

"$UMLCTL" --runtime-dir "$RUNTIME" pool serve \
	--name "$POOL_NAME" \
	--kernel "$KERNEL" \
	--mem 128M \
	--background \
	>"$OUT/serve.out" 2>"$OUT/serve.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: umlctl pool serve --background exited $RC"
	cat "$OUT/serve.err"
	exit 1
fi

DEADLINE=$(( $(date +%s) + 60 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt "$DEADLINE" ]; then
		echo "FAIL: socket $SOCK not created in 60s"
		cat "$OUT/serve.err"
		exit 1
	fi
	sleep 0.1
done
echo "daemon socket up: PASS"

TAKE_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" pool take \
	--name "$POOL_NAME" --json \
	--instance "uml-0" \
	--mac "52:54:00:00:00:00" \
	--tap "tap-syz-uml-0" \
	--ipv4 "10.7.0.10/24" \
	--gateway "10.7.0.1" \
	2>"$OUT/take.err")
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: syzkaller-style pool take exited $RC"
	cat "$OUT/take.err"
	exit 1
fi
printf '%s\n' "$TAKE_JSON" >"$OUT/take.json"
TAKEN_PID=$(python3 - "$OUT/take.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
need = {
    "instance": "uml-0",
    "mac": "52:54:00:00:00:00",
    "tap": "tap-syz-uml-0",
    "ipv4_cidr": "10.7.0.10/24",
    "ipv4_gateway": "10.7.0.1",
}
for key, val in need.items():
    if r.get(key) != val:
        raise SystemExit(f"{key} mismatch: {r.get(key)!r} != {val!r}")
pid = int(r.get("pid", 0))
if pid <= 0:
    raise SystemExit(f"bad pid: {pid}")
print(pid)
PY
)
if [ $? -ne 0 ]; then
	echo "FAIL: take JSON did not match syzkaller identity"
	cat "$OUT/take.json"
	exit 1
fi
echo "syzkaller-style take pid=$TAKEN_PID: PASS"

set +e
"$UMLCTL" --runtime-dir "$RUNTIME" exec \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --json -- \
	/bin/sh -c 'printf "syz-stdout\n"; printf "syz-stderr\n" >&2' \
	>"$OUT/exec.out" 2>"$OUT/exec.err"
EXEC_RC=$?
if [ $EXEC_RC -ne 0 ]; then
	echo "FAIL: syzkaller-style exec exited $EXEC_RC"
	cat "$OUT/exec.out"
	cat "$OUT/exec.err"
	exit 1
fi

python3 - "$OUT/exec.out" "$TAKEN_PID" <<'PY' >"$OUT/parse-exec.out" 2>"$OUT/parse-exec.err"
import json, sys
frames = [json.loads(line) for line in open(sys.argv[1]) if line.strip()]
pid = int(sys.argv[2])
if not frames or frames[0].get("type") != "start":
    raise SystemExit(f"first frame is not start: {frames[:1]}")
if frames[0].get("schema_version") != "exec/1":
    raise SystemExit(f"bad schema: {frames[0]}")
if frames[0].get("pid") != pid:
    raise SystemExit(f"wrong pid: {frames[0]}")
if frames[-1].get("type") != "exit" or frames[-1].get("code") != 0:
    raise SystemExit(f"bad exit frame: {frames[-1] if frames else None}")
merged = "".join(
    f.get("data", "") for f in frames
    if f.get("type") in ("stdout", "stderr", "console")
)
if "syz-stdout\n" not in merged or "syz-stderr\n" not in merged:
    raise SystemExit(f"merged output missing expected text: {merged!r}")
print(f"EXEC_FRAMES_OK {len(frames)}")
PY
if [ $? -ne 0 ]; then
	echo "FAIL: syzkaller-style exec frame validation failed"
	cat "$OUT/parse-exec.out"
	cat "$OUT/parse-exec.err"
	exit 1
fi
echo "syzkaller-style exec: PASS ($(cat "$OUT/parse-exec.out"))"

FORWARD_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" port-forward \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --host-port 31337 --json \
	2>"$OUT/forward.err")
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: port-forward exited $RC"
	cat "$OUT/forward.err"
	exit 1
fi
printf '%s\n' "$FORWARD_JSON" >"$OUT/forward.json"
python3 - "$OUT/forward.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
addr = r.get("guest_address", "")
if not addr.endswith(":31337"):
    raise SystemExit(f"bad guest_address: {addr!r}")
PY
if [ $? -ne 0 ]; then
	echo "FAIL: port-forward JSON invalid"
	cat "$OUT/forward.json"
	exit 1
fi
echo "syzkaller-style port-forward: PASS"

STATUS_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" pool status \
	--name "$POOL_NAME" --json 2>"$OUT/status.err")
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: pool status exited $RC"
	cat "$OUT/status.err"
	exit 1
fi
printf '%s\n' "$STATUS_JSON" >"$OUT/status.json"
python3 - "$OUT/status.json" "$TAKEN_PID" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
pid = int(sys.argv[2])
if int(r.get("master_pid", 0)) <= 0:
    raise SystemExit(f"bad master_pid: {r}")
if int(r.get("taken", 0)) < 1:
    raise SystemExit(f"taken count did not include pid {pid}: {r}")
PY
if [ $? -ne 0 ]; then
	echo "FAIL: pool status JSON invalid"
	cat "$OUT/status.json"
	exit 1
fi
echo "syzkaller-style status: PASS"

"$UMLCTL" --runtime-dir "$RUNTIME" pool destroy \
	--name "$POOL_NAME" "$TAKEN_PID" \
	>"$OUT/destroy.out" 2>"$OUT/destroy.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: pool destroy exited $RC"
	cat "$OUT/destroy.err"
	exit 1
fi
if [ -r "/proc/$TAKEN_PID/stat" ]; then
	state=$(awk '{print $3}' "/proc/$TAKEN_PID/stat" 2>/dev/null || echo "?")
	if [ "$state" != "Z" ] && [ "$state" != "X" ]; then
		echo "FAIL: destroyed pid $TAKEN_PID is still runnable (state=$state)"
		exit 1
	fi
fi
echo "syzkaller-style destroy: PASS"
TAKEN_PID=0

echo
echo "VERDICT: syzkaller UML shim take/exec/destroy wire path works"
exit 0
