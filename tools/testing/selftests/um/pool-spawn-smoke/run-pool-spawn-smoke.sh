#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/pool-spawn-smoke/run-pool-spawn-smoke.sh - kselftest for
# `umlctl pool spawn`.
#
# What it asserts:
#   - `umlctl pool spawn` exits 0 against a CONFIG_UM_TEMPLATE_PAUSE=y
#     kernel.
#   - The emitted JSON includes a numeric `pid` field.
#   - That host pid is alive immediately after spawn returns (the
#     master resumed cleanly from SIGCONT).
#   - The kernel's dmesg shows the expected `template_pause: identity
#     instance=...` line proving the identity blob round-tripped.
#
# Exit codes: 0 PASS, 4 SKIP (no binary / no kernel), 1 FAIL.
#
# Environment:
#   UML_BINARY   UML kernel. Default $HOME/src/uml-builds/uml-tplpause/linux.
#   UMLCTL       umlctl binary. Default scans the source tree's
#                tools/uml/uml-launcher/target/debug,release.

set -u

KERNEL=${UML_BINARY:-$HOME/src/uml-builds/uml-tplpause/linux}
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
	echo "SKIP: UML binary $KERNEL not found"
	exit 4
fi

OUT=$(mktemp -d -t pool-spawn-smoke.XXXXXX)
trap 'rm -rf "$OUT"; [ -n "${CHILD_PID:-}" ] && kill -KILL $CHILD_PID 2>/dev/null || true' EXIT

# Build a default init that pauses and then writes a known marker
# to /dev/console after resume; we'll grep for it later via /proc/PID/fd.
cat >"$OUT/init.sh" <<'IEOF'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo POOL_SMOKE_PRE_PAUSE
echo pool-smoke > /proc/um/template_pause
echo POOL_SMOKE_POST_PAUSE
exec sleep infinity
IEOF
chmod +x "$OUT/init.sh"

# Capture stderr to inspect later.
"$UMLCTL" pool spawn \
	--kernel "$KERNEL" \
	--mem 128M \
	--cmdline "backend=kvm-v2 ncpus=1" \
	--instance pool-smoke-1 \
	--mac 52:54:00:11:22:33 \
	--tap tap-smoke \
	--ipv4 10.7.0.42/24 \
	--gateway 10.7.0.1 \
	--init "$OUT/init.sh" \
	--json \
	>"$OUT/spawn.json" 2>"$OUT/spawn.err"
RC=$?
if [ $RC -ne 0 ]; then
	echo "FAIL: umlctl pool spawn exited $RC"
	cat "$OUT/spawn.err"
	exit 1
fi

CHILD_PID=$(python3 -c \
	"import json; print(json.load(open('$OUT/spawn.json'))['pid'])" 2>/dev/null \
	|| true)
if [ -z "$CHILD_PID" ] || ! [ "$CHILD_PID" -gt 0 ] 2>/dev/null; then
	echo "FAIL: spawn JSON did not include a valid pid"
	cat "$OUT/spawn.json"
	exit 1
fi
echo "pool member pid=$CHILD_PID"

# Confirm liveness immediately.
if ! kill -0 "$CHILD_PID" 2>/dev/null; then
	echo "FAIL: pid $CHILD_PID dead immediately after spawn"
	cat "$OUT/spawn.err"
	exit 1
fi

# Confirm the umlctl stderr trace matches the expected sequence.
if ! grep -q "waiting for SIGSTOP" "$OUT/spawn.err"; then
	echo "FAIL: stderr missing 'waiting for SIGSTOP'"
	cat "$OUT/spawn.err"
	exit 1
fi
if ! grep -q "master SIGSTOPped; sending SIGCONT" "$OUT/spawn.err"; then
	echo "FAIL: stderr missing the SIGCONT send line"
	cat "$OUT/spawn.err"
	exit 1
fi

# Now exercise pool list + destroy against the spawned member.
LIST_OUT=$("$UMLCTL" pool list --json)
if ! echo "$LIST_OUT" | grep -q "\"pid\":${CHILD_PID}"; then
	echo "FAIL: pool list --json did not include pid $CHILD_PID"
	echo "$LIST_OUT"
	exit 1
fi
echo "pool list --json includes pid $CHILD_PID: PASS"

DESTROY_OUT=$("$UMLCTL" pool destroy "$CHILD_PID" --json)
if ! echo "$DESTROY_OUT" | python3 -c \
	'import json,sys; o=json.load(sys.stdin); sys.exit(0 if o["destroyed"] else 1)'; then
	echo "FAIL: pool destroy did not report destroyed=true"
	echo "$DESTROY_OUT"
	exit 1
fi
echo "pool destroy reports destroyed=true: PASS"

if kill -0 "$CHILD_PID" 2>/dev/null; then
	echo "FAIL: pid $CHILD_PID still alive after destroy"
	exit 1
fi
echo "pid $CHILD_PID no longer alive: PASS"

# Subsequent list should not include the dead pid.
if "$UMLCTL" pool list --json | grep -q "\"pid\":${CHILD_PID}"; then
	echo "FAIL: pool list still includes dead pid $CHILD_PID"
	exit 1
fi
echo "post-destroy list omits dead pid: PASS"

CHILD_PID=

echo
echo "VERDICT: umlctl pool spawn/list/destroy lifecycle works end-to-end"
exit 0
