#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/userspace-smoke/run-userspace-smoke.sh — host-side launcher
# for the baseline UML userspace regression guard.
#
# Boots the UML binary, runs userspace-smoke.sh as init, looks
# for the terminal "USERSPACE_SMOKE: PASS|FAIL|SKIP" line.
# Exits 0 / 1 / 4 per kselftest convention.
#
# Pattern mirrors um/kprobes-stress/run-kprobes-stress.sh and
# um/snapshot-smoke/run-snapshot-smoke.sh.
#
# Environment:
#   UML_BINARY  path to the UML binary (default: /tmp/uml-research/linux)
#   UML_MEM     mem=N argument (default: 256M — python3's heap needs
#               a bit more headroom than kprobes-stress's 64M)
#   USERSPACE_SMOKE_BACKEND  optional `backend=force=<kind>` value.
#               When set, the runner adds the cmdline arg + asserts
#               that backend was selected at boot. Audit P0 #1
#               called out that the prior "no backend forced" mode
#               left dynamically-linked-binary KVM coverage entirely
#               implicit. Set to "kvm" to force-test KVM coverage of
#               the python3 / dash / coreutils stack.

set -u

BINARY=${UML_BINARY:-/tmp/uml-research/linux}
MEM=${UML_MEM:-256M}
BACKEND=${USERSPACE_SMOKE_BACKEND:-}
DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/userspace-smoke.sh"

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi
if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: $GUEST_SCRIPT not executable" >&2
	exit 1
fi

# Self-heal /dev/kvm ACL when forcing kvm (task #267).
ensure_kvm_readable() {
	local i
	for i in 1 2 3; do
		if [ -r /dev/kvm ]; then
			return 0
		fi
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}

CMDLINE_BACKEND=""
if [ -n "$BACKEND" ]; then
	if [ "$BACKEND" = "kvm" ]; then
		if [ ! -e /dev/kvm ]; then
			echo "SKIP: USERSPACE_SMOKE_BACKEND=kvm but no /dev/kvm" >&2
			exit 4
		fi
		if ! ensure_kvm_readable; then
			echo "SKIP: USERSPACE_SMOKE_BACKEND=kvm but /dev/kvm not readable" >&2
			exit 4
		fi
	fi
	CMDLINE_BACKEND="backend=force=$BACKEND"
fi

OUT=$(timeout --kill-after=10 60 "$BINARY" \
	$CMDLINE_BACKEND \
	init="$GUEST_SCRIPT" mem="$MEM" \
	con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

if [ -n "$BACKEND" ]; then
	OBSERVED=$(echo "$OUT" |
		sed -n 's/^um: backend = \([a-z]*\).*/\1/p' | head -1)
	if [ "$OBSERVED" != "$BACKEND" ]; then
		printf 'FAIL: USERSPACE_SMOKE_BACKEND=%s but observed=%s\n' \
			"$BACKEND" "$OBSERVED"
		exit 1
	fi
fi

LINE=$(echo "$OUT" | grep -E '^USERSPACE_SMOKE: (PASS|FAIL|SKIP)' | tail -1)

if [ -z "$LINE" ]; then
	echo "FAIL: no USERSPACE_SMOKE PASS/FAIL/SKIP line in output"
	echo "$OUT" | grep '^USERSPACE_SMOKE:' | tail -10
	echo "---"
	echo "$OUT" | tail -40
	exit 1
fi

if [ -n "$BACKEND" ]; then
	# Tag the summary line with the backend so downstream
	# parsers can distinguish per-backend USERSPACE_SMOKE rows.
	echo "${LINE/USERSPACE_SMOKE:/USERSPACE_SMOKE($BACKEND):}"
else
	echo "$LINE"
fi
case "$LINE" in
*PASS*) exit 0 ;;
*SKIP*) exit 4 ;;
*FAIL*) exit 1 ;;
*)      exit 1 ;;
esac
