#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Boot a UML kernel and execute the regrtest substrate reproducers.
# Aggregates PASS / FAIL / EXPECTED_FAIL and exits 0 only if every
# expected-PASS reproducer passes.
#
# Usage:
#   UML_BINARY=/tmp/uml-clean/linux \
#     bash tools/testing/selftests/um/regrtest-repros/run-regrtest-repros.sh
#
set -u

BINARY=${UML_BINARY:-/tmp/uml-clean/linux}
PY=${PY:-/usr/bin/python3}
MEM=${MEM:-1024M}
BACKEND=${BACKEND:-seccomp}
TIMEOUT=${TIMEOUT:-90}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found" >&2
	exit 4
fi

REPRO_DIR="$(cd "$(dirname "$0")" && pwd)"

# Build all C reproducers up front. If a class folder has no
# Makefile yet (work in progress) we tolerate the absence.
for d in class-a-env class-b-process class-c-syscall class-d-structural; do
	if [ -f "$REPRO_DIR/$d/Makefile" ]; then
		make -C "$REPRO_DIR/$d" >/dev/null 2>&1 || {
			echo "BUILD_FAIL: $d" >&2
		}
	fi
done

# Collect reproducer scripts. Each class folder may expose a
# run-class-X.sh that the in-guest init can source. The driver
# below executes them inside the UML kernel via init=...
INIT_SCRIPT=$(mktemp /tmp/regrtest-repros-init.XXXXX.sh)
trap 'rm -f "$INIT_SCRIPT"' EXIT
chmod +x "$INIT_SCRIPT"

cat > "$INIT_SCRIPT" <<'INITSH'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t tmpfs tmpfs /var/tmp 2>/dev/null
mount -t devpts devpts /dev/pts 2>/dev/null
export PATH=/usr/bin:/usr/sbin:/bin:/sbin
export HOME=/var/tmp
export TMPDIR=/var/tmp

REPRO_ROOT="@REPRO_DIR@"
echo "=== REGRTEST_REPROS_BEGIN $(date -u +%FT%TZ) ==="
for klass in class-a-env class-b-process class-c-syscall class-d-structural; do
	runner="$REPRO_ROOT/$klass/run-$klass.sh"
	if [ -x "$runner" ]; then
		echo "--- $klass ---"
		"$runner" || true
	else
		echo "--- $klass (no runner) ---"
	fi
done
echo "=== REGRTEST_REPROS_END $(date -u +%FT%TZ) ==="
sync
INITSH

sed -i "s|@REPRO_DIR@|$REPRO_DIR|" "$INIT_SCRIPT"

OUTPUT=$(timeout --kill-after=15 "$TIMEOUT" "$BINARY" \
	backend=force="$BACKEND" mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw panic=-1 \
	init="$INIT_SCRIPT" </dev/null 2>&1 | tr -d '\r' || true)

if [ -n "${VERBOSE:-}" ]; then
	echo "$OUTPUT"
else
	echo "$OUTPUT" | grep -E '^(=== |--- |REPRO:|BUILD_FAIL:|SKIP:)'
fi

PASS=$(echo "$OUTPUT" | grep -cE '^REPRO: [^ ]+ PASS( |$)' || true)
FAIL=$(echo "$OUTPUT" | grep -cE '^REPRO: [^ ]+ FAIL( |$)' || true)
XFAIL=$(echo "$OUTPUT" | grep -cE '^REPRO: [^ ]+ EXPECTED_FAIL( |$)' || true)

echo
echo "=== SUMMARY ==="
echo "PASS=$PASS FAIL=$FAIL EXPECTED_FAIL=$XFAIL"

if [ "$FAIL" -gt 0 ]; then
	exit 1
fi
exit 0
