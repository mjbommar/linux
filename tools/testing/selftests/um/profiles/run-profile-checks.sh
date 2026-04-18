#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-profile-checks.sh — workstream C-01 validation harness.
#
# For each UML profile built under /tmp/uml-profile-<name>/linux,
# boots the kernel with probe-features.sh as init and asserts the
# profile delivered the feature set its documentation promises.
#
# Expected-feature sets per profile are encoded in assert_profile()
# below. Running this on a tree that built all 8 profiles should
# print one PASS per profile and exit 0.
#
# Environment:
#   UML_PROFILES_ROOT   override the /tmp/uml-profile-<name> parent
#                       directory (default /tmp)

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
GUEST_SCRIPT="$DIR/probe-features.sh"
ROOT=${UML_PROFILES_ROOT:-/tmp}

if [ ! -x "$GUEST_SCRIPT" ]; then
	echo "FAIL: guest script $GUEST_SCRIPT not executable" >&2
	exit 1
fi

probe_profile() {
	local profile=$1
	local binary="$ROOT/uml-profile-$profile/linux"
	local out

	if [ ! -x "$binary" ]; then
		echo "SKIP: $binary not built (run: make ARCH=um O=$ROOT/uml-profile-$profile uml/$profile && make ARCH=um O=$ROOT/uml-profile-$profile -j\$(nproc))"
		return 77 # kselftest skip
	fi

	# Use 256M — fuzz-deep and research carry KASAN + heavy
	# sanitizer/debug surface and OOM at 64M, borderline at 128M.
	out=$(timeout 20 "$binary" init="$GUEST_SCRIPT" mem=256M \
		con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw 2>&1)

	# UML's console delivers CRLF line endings; strip CRs so the
	# PRESENT/ABSENT tokens compare cleanly downstream.
	echo "$out" | tr -d '\r' |
		awk '/^PROBE_BEGIN/{on=1;next} /^PROBE_END/{on=0} on{print}'
}

# Returns the PRESENT/ABSENT state for a feature from a probe block.
state_of() {
	local feature=$1
	local probe_output=$2
	echo "$probe_output" |
		awk -v f="$feature" '$1=="FEATURE" && $2==f {print $3; exit}'
}

# $1: profile, $2: probe output, $3..: "feature=expected" list
assert_profile() {
	local profile=$1 probe=$2
	shift 2
	local failed=0
	local rule feature expected got

	for rule in "$@"; do
		feature="${rule%%=*}"
		expected="${rule##*=}"
		got=$(state_of "$feature" "$probe")
		if [ -z "$got" ]; then
			got=UNKNOWN
		fi
		if [ "$got" != "$expected" ]; then
			printf '  %s  FAIL %s: want %s, got %s\n' \
				"$profile" "$feature" "$expected" "$got"
			failed=1
		fi
	done

	if [ $failed -eq 0 ]; then
		printf 'PASS %s (%d features match)\n' "$profile" "$(($#))"
	else
		printf 'FAIL %s\n' "$profile"
	fi
	return $failed
}

run_one() {
	local profile=$1
	shift
	local probe
	probe=$(probe_profile "$profile") || return $?
	assert_profile "$profile" "$probe" "$@"
}

# --- the expected sets ---------------------------------------------
#
# Features we probe:
#   debugfs_um, debugfs_um_hooks, debugfs_um_stats, debugfs_kcov,
#   tracefs, tracefs_syscalls, tracefs_user_events,
#   proc_kcore, proc_sysrq.
#
# Each profile asserts what its documentation / fragment promises.

any_fail=0

# prod-fast: everything OFF; proc_kcore stays on (base). No sandbox
# tightening yet (that's C-follow-up) so the disable list is modest.
run_one prod-fast \
	debugfs_um=ABSENT \
	debugfs_kcov=ABSENT \
	tracefs=ABSENT \
	proc_sysrq=ABSENT \
	|| any_fail=1

# prod-with-hooks: Layer 2 debugfs surface + tracefs mountable; no
# kcov (that's fuzz/research).
run_one prod-with-hooks \
	debugfs_um=PRESENT \
	debugfs_um_hooks=PRESENT \
	debugfs_um_stats=PRESENT \
	debugfs_kcov=ABSENT \
	tracefs=PRESENT \
	proc_sysrq=PRESENT \
	|| any_fail=1

# research: everything on.
run_one research \
	debugfs_um=PRESENT \
	debugfs_kcov=PRESENT \
	tracefs=PRESENT \
	tracefs_syscalls=PRESENT \
	tracefs_user_events=PRESENT \
	proc_kcore=PRESENT \
	proc_sysrq=PRESENT \
	|| any_fail=1

# fuzz: kcov + user_events (which pulls TRACING); no sysrq, no
# syscall-tracing table.
run_one fuzz \
	debugfs_um=PRESENT \
	debugfs_kcov=PRESENT \
	tracefs=PRESENT \
	tracefs_user_events=PRESENT \
	tracefs_syscalls=ABSENT \
	proc_sysrq=ABSENT \
	|| any_fail=1

# fuzz-deep: same as fuzz today (KCSAN pending C-03).
run_one fuzz-deep \
	debugfs_um=PRESENT \
	debugfs_kcov=PRESENT \
	tracefs=PRESENT \
	tracefs_user_events=PRESENT \
	|| any_fail=1

# sandbox: minimum TCB — everything off including proc_kcore.
run_one sandbox \
	debugfs_um=ABSENT \
	debugfs_kcov=ABSENT \
	tracefs=ABSENT \
	proc_kcore=ABSENT \
	proc_sysrq=ABSENT \
	|| any_fail=1

# embedded: ptrace backend, no debug/trace.
run_one embedded \
	debugfs_um=ABSENT \
	tracefs=ABSENT \
	|| any_fail=1

# time-travel: research-like tracing + determinism.
run_one time-travel \
	debugfs_um=PRESENT \
	tracefs_syscalls=PRESENT \
	tracefs_user_events=PRESENT \
	|| any_fail=1

if [ $any_fail -eq 0 ]; then
	echo 'All profile feature checks PASS'
	exit 0
else
	echo 'One or more profile checks FAILED'
	exit 1
fi
