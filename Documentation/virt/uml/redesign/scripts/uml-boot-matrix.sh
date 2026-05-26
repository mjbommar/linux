#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# uml-boot-matrix.sh — build + boot UML across the backend dispatch
# matrix and verify each variant comes up cleanly.
#
# Run from the kernel source root, e.g.:
#     Documentation/virt/uml/redesign/scripts/uml-boot-matrix.sh
#
# For each of {PTRACE_ONLY, SECCOMP_ONLY, DYNAMIC} the script:
#   1. Configures a side build dir under /tmp/uml-matrix-<mode>/
#   2. Runs `make ARCH=um -j$(nproc)` and captures the warning count
#   3. Boots `linux init=/bin/true ...` and grabs the dmesg
#      `um: backend = ...` line
#   4. Reports pass/fail per row
#
# Exit non-zero if any row fails.

set -u

SRC=$(cd "$(dirname "$0")/../../../../.." && pwd)
JOBS=$(nproc)
RESULTS=()
FAILED=0

cleanup() {
	# Kill any lingering linux processes from a hung boot.
	pkill -P $$ -- linux 2>/dev/null || true
}
trap cleanup EXIT

build_mode() {
	local mode=$1
	local build_dir=/tmp/uml-matrix-${mode,,}
	local logfile=/tmp/uml-matrix-${mode,,}.log

	rm -rf "$build_dir"
	mkdir -p "$build_dir"
	make -C "$SRC" O="$build_dir" ARCH=um defconfig >/dev/null 2>&1

	# Set $UML_MATRIX_KUNIT=1 to also enable the KUnit conformance
	# suite — backend-contract tests then run on each boot and
	# emit KTAP output at end-of-boot.
	if [ "${UML_MATRIX_KUNIT:-0}" = "1" ]; then
		"$SRC/scripts/config" --file "$build_dir/.config" \
			-e KUNIT \
			-e KUNIT_DEFAULT_ENABLED \
			-e KUNIT_AUTORUN_ENABLED \
			-e UM_BACKEND_CONTRACT_TEST
	fi

	case "$mode" in
		PTRACE_ONLY)
			"$SRC/scripts/config" --file "$build_dir/.config" \
				-e UM_BACKEND_PTRACE_ONLY \
				-e UM_BACKEND_PTRACE \
				-d UM_BACKEND_SECCOMP_ONLY \
				-d UM_BACKEND_SECCOMP \
				-d UM_BACKEND_DYNAMIC ;;
		SECCOMP_ONLY)
			"$SRC/scripts/config" --file "$build_dir/.config" \
				-e UM_BACKEND_SECCOMP_ONLY \
				-e UM_BACKEND_SECCOMP \
				-d UM_BACKEND_PTRACE_ONLY \
				-d UM_BACKEND_PTRACE \
				-d UM_BACKEND_DYNAMIC ;;
		DYNAMIC)
			"$SRC/scripts/config" --file "$build_dir/.config" \
				-e UM_BACKEND_DYNAMIC \
				-e UM_BACKEND_PTRACE \
				-e UM_BACKEND_SECCOMP \
				-d UM_BACKEND_PTRACE_ONLY \
				-d UM_BACKEND_SECCOMP_ONLY ;;
	esac
	make -C "$SRC" O="$build_dir" ARCH=um olddefconfig >/dev/null 2>&1
	# Build output goes to stderr (still visible to the terminal)
	# so the caller's `warn=$(build_mode ...)` captures ONLY the
	# warning count on stdout. Previously `| tail -1` printed
	# make's "Leaving directory" line to stdout, which made
	# `[ "$warn" -gt 0 ]` fail with "integer expected".
	make -C "$SRC" O="$build_dir" ARCH=um -j"$JOBS" 2>&1 \
		| tee "$logfile" >&2
	# Case-insensitive so modpost's uppercase `WARNING:` lines are
	# counted (they slipped past the lowercase-only pattern and
	# masked a section-mismatch regression — see the init_backend
	# __init commit).
	grep -iE "warning:" "$logfile" | grep -vE "UM_KERN_|cow_user" | wc -l
}

# boot_test <label> <build_dir> <expect_backend> <extra args ...>
boot_test() {
	local label=$1 build_dir=$2 expect=$3
	shift 3
	# sanitize label for use as a filename: spaces, slashes, parens
	local safe=${label//[\ \/\(\)=]/_}
	local boot_log=/tmp/uml-matrix-${safe}.log
	timeout 12 "$build_dir/linux" init=/bin/true mem=64M \
		con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs \
		"$@" >"$boot_log" 2>&1 || true
	local backend_line panic_line boot_rc=99
	backend_line=$(grep "um: backend =" "$boot_log" | head -1)
	panic_line=$(grep -E "Kernel panic|BUG:" "$boot_log" | head -1)
	if echo "$panic_line" | grep -q "Attempted to kill init"; then
		boot_rc=0
		panic_line="(expected init-exit panic)"
	fi
	local got
	if [ -n "$backend_line" ]; then
		got=$(echo "$backend_line" | sed -E 's/.*backend = ([^ ]+).*/\1/')
	else
		got="(none)"
	fi
	local ok="ok"
	if [ "$expect" = "PANIC" ]; then
		# Expected to die before init; should NOT have hit
		# "Attempted to kill init".
		[ "$boot_rc" -eq 0 ] && ok="FAIL: expected panic, got clean boot"
	else
		[ "$boot_rc" -ne 0 ] && ok="FAIL: rc=$boot_rc"
		[ "$got" != "$expect" ] && ok="FAIL: backend=$got, want=$expect"
	fi
	local row="$label  expect=$expect  got=$got  rc=$boot_rc  $ok"
	RESULTS+=("$row")
	echo "$row"
	[[ "$ok" == FAIL* ]] && FAILED=$((FAILED + 1))
}

echo "===== PTRACE_ONLY ====="
warn=$(build_mode PTRACE_ONLY)
[ "$warn" -gt 0 ] && { echo "PTRACE_ONLY: $warn warnings"; FAILED=$((FAILED+1)); }
boot_test "PTRACE_ONLY/default" /tmp/uml-matrix-ptrace_only ptrace
boot_test "PTRACE_ONLY/force=ptrace" /tmp/uml-matrix-ptrace_only ptrace backend=force=ptrace
boot_test "PTRACE_ONLY/force=seccomp(panic)" /tmp/uml-matrix-ptrace_only PANIC backend=force=seccomp

echo "===== SECCOMP_ONLY ====="
warn=$(build_mode SECCOMP_ONLY)
[ "$warn" -gt 0 ] && { echo "SECCOMP_ONLY: $warn warnings"; FAILED=$((FAILED+1)); }
boot_test "SECCOMP_ONLY/default" /tmp/uml-matrix-seccomp_only seccomp seccomp=on
boot_test "SECCOMP_ONLY/force=seccomp" /tmp/uml-matrix-seccomp_only seccomp seccomp=on backend=force=seccomp
boot_test "SECCOMP_ONLY/force=ptrace(panic)" /tmp/uml-matrix-seccomp_only PANIC seccomp=on backend=force=ptrace

echo "===== DYNAMIC ====="
warn=$(build_mode DYNAMIC)
[ "$warn" -gt 0 ] && { echo "DYNAMIC: $warn warnings"; FAILED=$((FAILED+1)); }
# DYNAMIC/default now expects seccomp: the probe runs for every
# DYNAMIC build that compiled seccomp in, matching prod-fast's
# documented "backend=auto picks seccomp where available" promise.
# Prior behavior ("ptrace unless seccomp= is set") was silently
# contradicting prod-fast and making the profile benchmark badly.
boot_test "DYNAMIC/default" /tmp/uml-matrix-dynamic seccomp
boot_test "DYNAMIC/seccomp=on(legacy)" /tmp/uml-matrix-dynamic seccomp seccomp=on
boot_test "DYNAMIC/backend=ptrace" /tmp/uml-matrix-dynamic ptrace backend=ptrace
# backend=seccomp now triggers the probe on its own (no seccomp=on needed)
boot_test "DYNAMIC/backend=seccomp" /tmp/uml-matrix-dynamic seccomp backend=seccomp
boot_test "DYNAMIC/force=ptrace" /tmp/uml-matrix-dynamic ptrace backend=force=ptrace
# force=seccomp on a host that supports seccomp now succeeds (probe runs
# unconditionally). Hosts without seccomp would panic; not testable here.
boot_test "DYNAMIC/force=seccomp" /tmp/uml-matrix-dynamic seccomp backend=force=seccomp

echo "===== summary ====="
for r in "${RESULTS[@]}"; do echo "  $r"; done

if [ "$FAILED" -gt 0 ]; then
	echo "FAILED: $FAILED of ${#RESULTS[@]} configs failed"
	exit 1
fi

echo "OK: all ${#RESULTS[@]} configs built clean and booted"
