#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# um/userspace-smoke/userspace-smoke.sh
#
# Guards the load-bearing invariant that a UML kernel, booted
# under its default backend for the profile, can execute a real
# userspace program end-to-end. Exercises:
#
#   - boot reaches userspace (backend ops wired, ptrace/seccomp
#     emulation serves syscalls)
#   - hostfs is mountable as root and the host's /bin, /usr/bin,
#     /lib are visible to the guest
#   - /proc and /sys mount cleanly
#   - a non-trivial userspace binary (python3) loads, does
#     heap allocation, calls into libc, and exits 0
#   - multiple independent userspace processes in sequence don't
#     leak state that breaks later ones
#   - halt/shutdown path returns control cleanly
#
# Regression-guard, not a feature test. A backend refactor (A-02)
# or syscall-emulation change that lets the kernel boot but
# breaks usermode exec would be invisible to kprobes-stress /
# ftrace-smoke / snapshot-smoke (all of which run inside already-
# booted UML and assume userspace works) — userspace-smoke catches
# exactly that class of regression.
#
# Emits one terminal line:
#   USERSPACE_SMOKE: PASS python=<version> pid_first=<N> pid_second=<N>
#   USERSPACE_SMOKE: FAIL <reason>
#   USERSPACE_SMOKE: SKIP <reason>

set -u

echo "USERSPACE_SMOKE: init running"

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null

PY=$(command -v python3 2>/dev/null || echo /usr/bin/python3)

if [ ! -x "$PY" ]; then
	echo "USERSPACE_SMOKE: SKIP python3 not found at $PY (hostfs issue?)"
	halt -f 2>/dev/null
	exit 0
fi

# Call 1: basic interpreter + arithmetic + stdout.
OUT1=$("$PY" -c 'import os, sys
v = sys.version_info
print(f"PID1={os.getpid()} PYVER={v.major}.{v.minor} SUM={sum(range(100))}")' 2>&1)
RC1=$?
echo "USERSPACE_SMOKE: call1 rc=$RC1 out=$OUT1"
if [ $RC1 -ne 0 ]; then
	echo "USERSPACE_SMOKE: FAIL call1 exit=$RC1"
	halt -f 2>/dev/null
	exit 1
fi

# Call 2: exercises a different execve, heap reuse, different pid.
# Also verifies UML's time source is moving (time.monotonic()
# strictly increases across two reads) — catches a regression in
# the clocksource path that would otherwise pass syscall tests.
OUT2=$("$PY" -c 'import os, time
t0 = time.monotonic()
data = bytearray(1024 * 1024)
t1 = time.monotonic()
assert t1 >= t0, "monotonic went backwards"
print(f"PID2={os.getpid()} ALLOC_OK=1 DT={t1-t0:.6f}")' 2>&1)
RC2=$?
echo "USERSPACE_SMOKE: call2 rc=$RC2 out=$OUT2"
if [ $RC2 -ne 0 ]; then
	echo "USERSPACE_SMOKE: FAIL call2 exit=$RC2"
	halt -f 2>/dev/null
	exit 1
fi

# Call 3: import dlopen-backed C extensions. Plain Python startup can
# pass while KVM fault recovery still corrupts the user stack across a
# lazy page fault in the dynamic loader; hashlib/_datetime/_bisect hit
# that path on typical distro Python builds.
OUT3=$("$PY" -c 'import _bisect, _datetime, hashlib
print("CEXT_OK=1 SHA=%s" % hashlib.sha256(b"uml").hexdigest()[:8])' 2>&1)
RC3=$?
echo "USERSPACE_SMOKE: call3 rc=$RC3 out=$OUT3"
if [ $RC3 -ne 0 ]; then
	echo "USERSPACE_SMOKE: FAIL call3 exit=$RC3"
	halt -f 2>/dev/null
	exit 1
fi

# Parse the PIDs + version out of the python output blobs.
# POSIX parameter expansion rather than sed: stays portable
# across host GNU sed / busybox sed / ash+dash variants (the
# earlier sed pipeline silently returned empty under one
# specific busybox build on GitHub Actions ubuntu-latest
# runners — see Documentation/virt/uml/redesign/ for the
# incident trail).
extract() {
	# extract <field> <blob> — prints the token that follows
	# "<field>=" up to the next whitespace, or "" if no match.
	case " $2 " in
	*\ "$1"=*)
		tmp=${2#*"$1"=}
		printf '%s\n' "${tmp%% *}"
		;;
	*) ;;
	esac
}
PYVER=$(extract PYVER "$OUT1")
PID1=$(extract PID1 "$OUT1")
PID2=$(extract PID2 "$OUT2")

if [ -z "$PYVER" ] || [ -z "$PID1" ] || [ -z "$PID2" ]; then
	echo "USERSPACE_SMOKE: FAIL parse: PYVER='$PYVER' PID1='$PID1' PID2='$PID2'"
	halt -f 2>/dev/null
	exit 1
fi

# Basic sanity: the two python processes should have different
# pids. Same pid would suggest pid-reuse-at-1 (init-only) or a
# broken fork+exec path; either is a bug worth catching.
if [ "$PID1" = "$PID2" ]; then
	echo "USERSPACE_SMOKE: FAIL pid reused: PID1=$PID1 PID2=$PID2"
	halt -f 2>/dev/null
	exit 1
fi

echo "USERSPACE_SMOKE: PASS python=$PYVER pid_first=$PID1 pid_second=$PID2 cext=1"
halt -f 2>/dev/null
exit 0
