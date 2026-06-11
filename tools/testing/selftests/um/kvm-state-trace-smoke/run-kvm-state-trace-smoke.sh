#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-side smoke for the KVM v2 private state trace ring.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
BINARY=${UML_BINARY:-./linux}
MEM=${UML_MEM:-256M}
GUEST_SCRIPT="$DIR/kvm-state-trace-smoke.sh"
PARSER="$DIR/parse-kvm-state-trace.py"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi
if [ ! -x "$PARSER" ]; then
	echo "FAIL: $PARSER not executable" >&2
	exit 1
fi
if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi

ensure_kvm_readable() {
	local i

	for i in 1 2 3; do
		[ -r /dev/kvm ] && return 0
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 45 "$BINARY" \
	backend=force=kvm-v2 \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 | tr -d '\r' || true)

LINE=$(echo "$OUT" | grep '^KVM_STATE_TRACE_SMOKE:' | tail -1)
if [ -z "$LINE" ]; then
	echo "KVM_STATE_TRACE_SMOKE: FAIL (no guest result line)"
	echo "$OUT" |
		grep -E 'backend = |KVM_STATE_TRACE|UML: fatal|panic' |
		tail -80
	exit 1
fi

echo "$LINE"
case "$LINE" in
*PASS*) ;;
*SKIP*) exit 4 ;;
*) exit 1 ;;
esac

DUMP=$(mktemp -t kvm-state-trace-dump.XXXXXX)
trap 'rm -f "$DUMP"' EXIT

echo "$OUT" |
	awk '/^KVM_STATE_TRACE_DUMP_BEGIN$/ { in_dump=1; next }
	     /^KVM_STATE_TRACE_DUMP_END$/ { in_dump=0; next }
	     in_dump { print }' > "$DUMP"

if [ ! -s "$DUMP" ]; then
	echo "KVM_STATE_TRACE_SMOKE: FAIL (empty dump)"
	exit 1
fi

"$PARSER" "$DUMP" || exit 1
echo "KVM_STATE_TRACE_SMOKE: PASS (debugfs=1 parser=1)"
exit 0
