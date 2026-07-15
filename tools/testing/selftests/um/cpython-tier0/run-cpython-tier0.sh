#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/cpython-tier0/run-cpython-tier0.sh - host-side launcher for
# the CPython tier-0 regression gate.
#
# Boots UML with the chosen backend, runs cpython-tier0.py as init,
# and asserts the terminal "CPYTHON_TIER0: TOTAL ... PASS" line.
#
# The gate guards backend regressions of the form "Python's
# C-extension dlopen path silently corrupts user memory" (the
# class of bug fixed by 96bfe20b3616 for kvm: missing
# current_mm_sync()). Hashlib alone catches the dlopen+libcrypto
# path; the wider tier-0 list catches anything else along the
# common single-process Python paths (numerics, containers, regex,
# strings, etc.).
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL - kselftest convention.
#
# Environment:
#   UML_BINARY    UML kernel built with the desired backend config
#                 (default: /tmp/uml-kvmint/linux for kvm-integrated;
#                  override for seccomp/ptrace builds).
#   UML_MEM       mem= argument. Default 1024M - CPython's tier-0
#                 sweep needs more headroom than dyn-loader because
#                 test_array / test_set / test_typing allocate
#                 substantial heap before they exit.
#   CPYTHON_TIER0_BACKEND
#                 Optional `backend=force=<kind>` for dynamic builds.
#                 If unset, the kernel's default backend is used.
#   CPYTHON_TIER0_PY
#                 Path to the Python interpreter inside the guest's
#                 hostfs view. Default: /usr/bin/python3.
#
# Hostfs note: the guest mounts the host's filesystem as root via
# `rootfstype=hostfs`. Tests run as init=python3, with cwd=/tmp so
# tests that write `@test_*_tmp` files have a writable directory
# (the host's /tmp). If your host's /tmp is non-writable from the
# guest's UID/GID, the test will fail identically under every
# backend - fix the host config, not this script.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-1024M}
PY=${CPYTHON_TIER0_PY:-/usr/bin/python3}
BACKEND_ARG=""
EXPECTED_BACKEND=""
if [ -n "${CPYTHON_TIER0_BACKEND:-}" ]; then
	BACKEND_ARG="backend=force=${CPYTHON_TIER0_BACKEND}"
	EXPECTED_BACKEND=${CPYTHON_TIER0_BACKEND}
	# `kvm` is the accepted selector alias; the backend reports its
	# canonical implementation name in the boot log.
	if [ "$EXPECTED_BACKEND" = "kvm" ]; then
		EXPECTED_BACKEND=kvm-v2
	fi
fi

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi

# Confirm the python interpreter the guest will use exists on the
# host (since hostfs makes the host's /usr/bin/python3 visible to
# the guest). If python isn't installed, skip; we can't run the
# tier-0 gate without an interpreter.
if [ ! -x "$PY" ]; then
	echo "SKIP: $PY not found on host (install python3)" >&2
	exit 4
fi

# Confirm the CPython test suite is installed on the host (Debian/
# Ubuntu strip Lib/test/ from the base python3 package; require
# libpython3.X-testsuite or equivalent).
PY_TESTDIR=$("$PY" -c 'import test, os; print(os.path.dirname(test.__file__))' 2>/dev/null)
if [ -z "$PY_TESTDIR" ] || [ ! -f "$PY_TESTDIR/test_hashlib.py" ]; then
	echo "SKIP: CPython test suite not installed (need libpython3.x-testsuite or equivalent)" >&2
	exit 4
fi

# /dev/kvm self-heal: same retry pattern as the other kvm runners.
case "${CPYTHON_TIER0_BACKEND:-}" in
kvm|kvm-v2)
	if [ ! -e /dev/kvm ]; then
		echo "SKIP: /dev/kvm not present" >&2
		exit 4
	fi
	for i in 1 2 3; do
		[ -r /dev/kvm ] && break
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		sleep 0.1
	done
	if [ ! -r /dev/kvm ]; then
		echo "SKIP: /dev/kvm not readable" >&2
		exit 4
	fi
	;;
esac

OUT=$(timeout --kill-after=15 600 "$BINARY" \
	$BACKEND_ARG \
	mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 \
	init="$PY" -- "$(realpath "$(dirname "$0")")/cpython-tier0.py" \
	</dev/null 2>&1 || true)

if [ -n "${CPYTHON_TIER0_BACKEND:-}" ]; then
	OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
	if [ "$OBSERVED" != "$EXPECTED_BACKEND" ]; then
		echo "SKIP: backend probed to '$OBSERVED' (asked for ${CPYTHON_TIER0_BACKEND}, expected $EXPECTED_BACKEND)" >&2
		exit 4
	fi
fi

# Per-module results: every line is informational, but we display
# them so a flake is diagnosable from the kselftest log alone.
echo "$OUT" | grep '^CPYTHON_TIER0:'

SUMMARY=$(echo "$OUT" | grep '^CPYTHON_TIER0: TOTAL' | tail -1)
if [ -z "$SUMMARY" ]; then
	echo "CPYTHON_TIER0: FAIL - no TOTAL line found (init crashed before completion?)"
	echo "$OUT" | tail -30
	exit 1
fi
case "$SUMMARY" in
	*PASS*)
		echo "CPYTHON_TIER0: PASS - $SUMMARY"
		exit 0
		;;
	*)
		echo "CPYTHON_TIER0: FAIL - $SUMMARY"
		echo "$OUT" | grep -E '^CPYTHON_TIER0:' | grep -v 'ok=True' | head -10
		exit 1
		;;
esac
