#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run-profile-checks.sh - UML profile validation harness.
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
	local out probe

	# Use 512M: fuzz-deep and research carry KASAN + heavy
	# sanitizer/debug surface and OOM at 64M, borderline at 128M.
	# 40s timeout: KCSAN (race profile) adds several seconds of
	# lockdep/selftest init at boot before the probe runs.
	# panic=0 keeps the kernel from instantly restarting if
	# init fails to launch, so the failure path's printk output
	# has a chance to flush to our captured fd before UML exits.
	# loglevel=8 forces pr_info/debug into the console output so
	# the "Run /path/to/init as init process" line (KERN_INFO)
	# is visible.
	local rc
	# probe-features.sh writes its output to both stdout AND
	# the hostfs path `/tmp/uml-probe-output`. Clear the file
	# before each run so a stale hit from a prior profile
	# can't false-positive us.
	rm -f /tmp/uml-probe-output
	# Use 512M: fuzz-deep and research carry KASAN + heavy
	# sanitizer/debug surface and OOM at 64M, borderline at 128M.
	# 40s timeout: KCSAN (race profile) adds several seconds of
	# lockdep/selftest init at boot before the probe runs.
	# panic=0 keeps the kernel from instantly restarting if
	# init fails to launch, so the failure path's printk output
	# has a chance to flush to our captured fd before UML exits.
	# loglevel=8 forces pr_info/debug into the console output so
	# the "Run /path/to/init as init process" line (KERN_INFO)
	# is visible.
	out=$(timeout --kill-after=10 40 "$binary" init="$GUEST_SCRIPT" mem=512M \
		ncpus=2 con=null con0=fd:0,fd:1 root=/dev/root rootfstype=hostfs rw \
		panic=0 loglevel=8 2>&1)
	rc=$?

	# Prefer the hostfs file: it is written by direct host
	# write() syscalls and survives UML's exit regardless of
	# the tty driver's queue state. Fall back to the stdout
	# capture for environments that don't write to the file
	# (older probe-features.sh without that contract).
	# UML's console may deliver CRLF line endings; strip CRs
	# so the PRESENT/ABSENT tokens compare cleanly downstream.
	if [ -s /tmp/uml-probe-output ]; then
		probe=$(tr -d '\r' </tmp/uml-probe-output |
			awk '/^PROBE_BEGIN/{on=1;next} /^PROBE_END/{on=0} on{print}')
	else
		probe=$(echo "$out" | tr -d '\r' |
			awk '/^PROBE_BEGIN/{on=1;next} /^PROBE_END/{on=0} on{print}')
	fi

	# If the probe block is missing from both sources, the
	# guest never reached its init script: usually a boot
	# failure, ptrace/seccomp refusal, or OOM. Emit the raw
	# captured stream on stderr so the caller's log makes the
	# root cause visible without a second "what just happened"
	# run. The empty-probe path is what surfaces in every
	# FAIL-with-UNKNOWN mode of the assert step; showing the
	# boot log next to those UNKNOWNs is the single biggest
	# reducer of "is UML broken or is my config broken" triage
	# time.
	if [ -z "$probe" ]; then
		{
			printf '=== %s: probe block missing (UML exit=%d)\n' \
				"$profile" "$rc"
			printf '    raw boot output follows ===\n'
			# rc=124: outer `timeout` tripped (UML hung).
			# rc!=0 && rc!=124: UML exited itself, likely a
			# kernel panic that didn't flush or a host-side
			# crash (segfault in the UML host stub, etc.).
			echo "$out"
			echo "=== end raw boot output ==="
		} >&2
	fi

	echo "$probe"
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
	local binary="$ROOT/uml-profile-$profile/linux"
	local probe

	# If the operator built only a subset of profiles (as CI
	# does via the per-profile matrix), the missing ones are a
	# clean SKIP, not a test failure. Return 0 so the caller's
	# `|| any_fail=1` guard doesn't trip.
	if [ ! -x "$binary" ]; then
		local o="$ROOT/uml-profile-$profile"
		printf 'SKIP %s (binary %s not built; run:\n' "$profile" "$binary"
		printf '       make ARCH=um O=%s uml/%s &&\n' "$o" "$profile"
		printf '       make ARCH=um O=%s -j$(nproc))\n' "$o"
		return 0
	fi

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
	debugfs_kfence=PRESENT \
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

# fuzz-deep: fuzz + KFENCE + KASAN_INLINE.
run_one fuzz-deep \
	debugfs_um=PRESENT \
	debugfs_kcov=PRESENT \
	debugfs_kfence=PRESENT \
	tracefs=PRESENT \
	tracefs_user_events=PRESENT \
	|| any_fail=1

# race: KCSAN-focused, no KASAN, debugfs/kcsan present.
run_one race \
	debugfs_um=PRESENT \
	debugfs_kcsan=PRESENT \
	debugfs_kcov=ABSENT \
	tracefs=PRESENT \
	proc_sysrq=ABSENT \
	|| any_fail=1

# sandbox: minimum TCB; everything off including proc_kcore.
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
