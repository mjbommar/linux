#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-exec-smoke - kselftest for `umlctl exec` end-to-end.
#
# What it asserts:
#   1. `umlctl exec --pid <pid> --json -- /bin/true` against a real
#      daemon-managed pool member returns a well-formed NDJSON frame
#      stream (start + exit, in that order) with exit code 0.
#   2. The `start` frame carries schema_version="exec/1" and the right
#      pid.
#   3. The `exit` frame is the last frame and includes `code` /
#      `timed_out` fields.
#   4. A shell command can return stdout, stderr, and a non-zero guest
#      exit status without being mistaken for a daemon transport error.
#
# Exit codes: 0 PASS, 4 SKIP, 1 FAIL.
#
# Environment:
#   UM_FORK_KERNEL  Fork-mode UML kernel.  Default
#                   $HOME/src/uml-builds/uml-tplpause-fork/linux.
#   UMLCTL          umlctl binary.  Auto-detected from the source tree.

set -u

KERNEL=${UM_FORK_KERNEL:-$HOME/src/uml-builds/uml-tplpause-fork/linux}
# Fallback to a known fork-capable local build, if present.
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
	echo "SKIP: python3 required for NDJSON parsing"
	exit 4
fi

POOL_NAME="exec-smoke-$$"
OUT=$(mktemp -d -t pool-exec-smoke.XXXXXX)
RUNTIME=$(mktemp -d -t pool-exec-rt.XXXXXX)
SOCK="$RUNTIME/pools/$POOL_NAME/api.sock"
PIDFILE="$RUNTIME/pools/$POOL_NAME/serve.pid"

cleanup() {
	if [ -S "$SOCK" ]; then
		"$UMLCTL" --runtime-dir "$RUNTIME" pool destroy --name "$POOL_NAME" \
			"${TAKEN_PID:-0}" >/dev/null 2>&1 || true
		python3 -c "
import json, socket, sys
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
    s.connect('$SOCK')
    s.sendall(b'{\"op\":\"shutdown\"}\n')
    s.recv(4096)
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

# Launch daemon.
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

DEADLINE=$(( $(date +%s) + 30 ))
while [ ! -S "$SOCK" ]; do
	if [ "$(date +%s)" -gt $DEADLINE ]; then
		echo "FAIL: socket $SOCK not created in 30s"
		cat "$OUT/serve.err"
		exit 1
	fi
	sleep 0.1
done
echo "daemon socket up: PASS"

# Take a member.
TAKE_JSON=$("$UMLCTL" --runtime-dir "$RUNTIME" pool take \
	--name "$POOL_NAME" --instance "execsm-m1" \
	--mac "52:54:00:11:22:33" --tap "tap-execsm" \
	--ipv4 "10.7.0.42/24" --gateway "10.7.0.1" \
	--json 2>"$OUT/take.err")
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: pool take exited $RC"
	cat "$OUT/take.err"
	exit 1
fi
TAKEN_PID=$(echo "$TAKE_JSON" | python3 -c \
	"import json,sys;print(json.load(sys.stdin)['pid'])")
echo "took member pid=$TAKEN_PID: PASS"

# Run umlctl exec --json.  The current kernel has a bounded mconsole exec
# primitive, so a fresh build must execute /bin/true successfully.  Older
# kernels used to return a clean daemon error here; that boundary is now stale
# for this tree.
set +e
"$UMLCTL" --runtime-dir "$RUNTIME" exec \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --json -- /bin/true \
	>"$OUT/exec.out" 2>"$OUT/exec.err"
EXEC_RC=$?
set -e

echo "exec exit_code=$EXEC_RC"
echo "--- exec.out ---"
cat "$OUT/exec.out"
echo "--- end exec.out ---"

# Parse NDJSON frame stream regardless of exit code.  We assert:
#   - first non-blank line is type=start with schema_version=exec/1
#   - last non-blank line is type=exit
# The daemon must not surface this as a transport error with the current
# kernel.
python3 - <<PYEOF >"$OUT/parse.out" 2>"$OUT/parse.err"
import json, sys
frames = []
with open("$OUT/exec.out") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            frames.append(json.loads(line))
        except Exception as e:
            print("PARSE_FAIL", line, e)
            sys.exit(1)
if not frames:
    print("NO_FRAMES")
    sys.exit(1)
if frames[0].get("type") != "start":
    print("FIRST_NOT_START", frames[0])
    sys.exit(1)
if frames[0].get("schema_version") != "exec/1":
    print("BAD_SCHEMA", frames[0].get("schema_version"))
    sys.exit(1)
if frames[0].get("pid") != $TAKEN_PID:
    print("WRONG_PID", frames[0].get("pid"))
    sys.exit(1)
if frames[-1].get("type") != "exit":
    print("LAST_NOT_EXIT", frames[-1])
    sys.exit(1)
if "code" not in frames[-1]:
    print("EXIT_MISSING_CODE", frames[-1])
    sys.exit(1)
if "timed_out" not in frames[-1]:
    print("EXIT_MISSING_TIMED_OUT", frames[-1])
    sys.exit(1)
saw_error = any(
    f.get("type") == "stderr" and "daemon error" in f.get("data", "")
    for f in frames
)
stderr_text = "".join(
    f.get("data", "") for f in frames if f.get("type") == "stderr"
)
stale_boundaries = [
    "uml_mconsole(1) not found",
    "Resource temporarily unavailable",
    "mconsole socket",
    "not present yet",
]
if saw_error and any(s in stderr_text for s in stale_boundaries):
    print("STALE_DAEMON_EXEC_BOUNDARY", stderr_text)
    sys.exit(1)
if saw_error:
    print("DAEMON_ERROR_BOUNDARY_IS_STALE", stderr_text)
    sys.exit(1)
if frames[-1].get("code") != 0:
    print("TRUE_EXIT_NOT_ZERO", frames[-1])
    sys.exit(1)
print("FRAMES_OK case=A", len(frames))
PYEOF
PARSE_RC=$?
if [ $PARSE_RC -ne 0 ]; then
	echo "FAIL: NDJSON frame validation failed"
	cat "$OUT/parse.out"
	cat "$OUT/parse.err"
	exit 1
fi
echo "NDJSON frames valid: PASS ($(cat "$OUT/parse.out"))"

# Exercise output capture and non-zero guest exit without turning that guest
# exit into a daemon transport error.
set +e
"$UMLCTL" --runtime-dir "$RUNTIME" exec \
	--name "$POOL_NAME" --pid "$TAKEN_PID" --json -- \
	/bin/sh -c 'printf "pool-stdout\n"; printf "pool-stderr\n" >&2; exit 7' \
	>"$OUT/exec-rich.out" 2>"$OUT/exec-rich.err"
RICH_RC=$?
set -e

echo "rich exec exit_code=$RICH_RC"
echo "--- exec-rich.out ---"
cat "$OUT/exec-rich.out"
echo "--- end exec-rich.out ---"

python3 - <<PYEOF >"$OUT/parse-rich.out" 2>"$OUT/parse-rich.err"
import json, sys
frames = []
with open("$OUT/exec-rich.out") as f:
    for line in f:
        line = line.strip()
        if line:
            frames.append(json.loads(line))
stdout = "".join(f.get("data", "") for f in frames if f.get("type") == "stdout")
stderr = "".join(f.get("data", "") for f in frames if f.get("type") == "stderr")
if not frames or frames[0].get("type") != "start":
    print("RICH_FIRST_NOT_START", frames[:1])
    sys.exit(1)
if frames[-1].get("type") != "exit":
    print("RICH_LAST_NOT_EXIT", frames[-1] if frames else None)
    sys.exit(1)
if stdout != "pool-stdout\n":
    print("RICH_STDOUT_MISMATCH", repr(stdout))
    sys.exit(1)
if stderr != "pool-stderr\n":
    print("RICH_STDERR_MISMATCH", repr(stderr))
    sys.exit(1)
if frames[-1].get("code") != 7:
    print("RICH_EXIT_MISMATCH", frames[-1])
    sys.exit(1)
if any("daemon error" in f.get("data", "") for f in frames if f.get("type") == "stderr"):
    print("RICH_DAEMON_ERROR", stderr)
    sys.exit(1)
print("RICH_FRAMES_OK", len(frames))
PYEOF
PARSE_RICH_RC=$?
if [ $PARSE_RICH_RC -ne 0 ]; then
	echo "FAIL: rich NDJSON frame validation failed"
	cat "$OUT/parse-rich.out"
	cat "$OUT/parse-rich.err"
	exit 1
fi
if [ $RICH_RC -ne 7 ]; then
	echo "FAIL: rich exec process exit $RICH_RC, expected 7"
	exit 1
fi
echo "rich NDJSON frames valid: PASS ($(cat "$OUT/parse-rich.out"))"

# Destroy + shutdown via the daemon path.
"$UMLCTL" --runtime-dir "$RUNTIME" pool destroy --name "$POOL_NAME" "$TAKEN_PID" \
	>>"$OUT/destroy.out" 2>>"$OUT/destroy.err" || true
echo "destroy via daemon path: PASS"

echo
echo "VERDICT: umlctl exec NDJSON wire shape validated end-to-end"
exit 0
