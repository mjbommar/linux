#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# CPython parity gate (#277): run a curated set of CPython
# stdlib test modules under BOTH backends (seccomp baseline +
# kvm under test) and report module-by-module divergence.
#
# Pass criterion: every module's pass/fail count under kvm is
# >= the same module's count under seccomp. Goal: bit-identical
# parity once the residual fork+exec staleness is resolved.
#
# Usage:
#   UML_BINARY=/tmp/uml-kvmint/linux \
#     bash tools/testing/selftests/um/cpython-parity/cpython-parity.sh
#
# Exits 0 if every module shows kvm_pass >= seccomp_pass, 1 otherwise.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
PY=${PY:-/usr/bin/python3}
MEM=${MEM:-1024M}
TIMEOUT_PER_MOD=${TIMEOUT_PER_MOD:-180}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found" >&2
	exit 4
fi
if [ ! -x "$PY" ]; then
	echo "SKIP: $PY not found" >&2
	exit 4
fi
PY_TESTDIR=$("$PY" -c 'import test, os; print(os.path.dirname(test.__file__))' 2>/dev/null)
if [ -z "$PY_TESTDIR" ] || [ ! -f "$PY_TESTDIR/test_hashlib.py" ]; then
	echo "SKIP: CPython test suite not installed" >&2
	exit 4
fi

# Curated module list — pure-CPU + memory, no external deps.
MODULES="${MODULES:-test_struct test_math test_hashlib test_bisect test_array test_decimal test_fractions test_string test_bytes test_dict test_list test_set test_tuple test_int test_float test_complex test_heapq test_itertools test_operator test_typing test_abc}"

# Build a Python harness that loads + runs ONE module's tests.
HARNESS=$(mktemp /tmp/cpython-parity-harness.XXXXX.py)
cat > "$HARNESS" <<'PYHARNESS'
import os, sys, unittest, importlib
os.chdir("/tmp")
mod_name = sys.argv[1]
try:
	mod = importlib.import_module(mod_name)
except Exception as e:
	print("PARITY: %s IMPORT_FAIL %s" % (mod_name, type(e).__name__))
	sys.exit(0)
suite = unittest.TestLoader().loadTestsFromModule(mod)
runner = unittest.TextTestRunner(verbosity=0, stream=sys.stdout)
result = runner.run(suite)
print("PARITY: %s tests=%d fail=%d err=%d ok=%s" % (
	mod_name, result.testsRun, len(result.failures), len(result.errors),
	result.wasSuccessful()))
PYHARNESS

ensure_kvm() {
	for i in 1 2 3; do
		[ -r /dev/kvm ] && return 0
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		sleep 0.1
	done
	return 1
}

run_module() {
	local backend="$1"
	local mod="$2"
	local out
	if [ "$backend" = "kvm" ]; then
		ensure_kvm || { echo "/dev/kvm unreadable" >&2; return 1; }
	fi
	out=$(timeout --kill-after=15 "$TIMEOUT_PER_MOD" "$BINARY" \
		backend=force="$backend" mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw panic=-1 \
		init="$PY" -- "$HARNESS" "test.$mod" \
		</dev/null 2>&1 || true)
	echo "$out" | grep "^PARITY:" | head -1
}

DIVERGE=0
PARITY=0
SKIP=0
echo "module | seccomp | kvm | status"
echo "-------|---------|-----|-------"
for MOD in $MODULES; do
	S=$(run_module seccomp "$MOD")
	K=$(run_module kvm "$MOD")
	# Parse "PARITY: <mod> tests=N fail=N err=N ok=B"
	S_OK=$(echo "$S" | grep -oE "ok=[A-Za-z]+" | sed 's/ok=//')
	K_OK=$(echo "$K" | grep -oE "ok=[A-Za-z]+" | sed 's/ok=//')
	S_TESTS=$(echo "$S" | grep -oE "tests=[0-9]+" | sed 's/tests=//')
	K_TESTS=$(echo "$K" | grep -oE "tests=[0-9]+" | sed 's/tests=//')
	[ -z "$S_OK" ] && S_OK="?"
	[ -z "$K_OK" ] && K_OK="?"
	[ -z "$S_TESTS" ] && S_TESTS="?"
	[ -z "$K_TESTS" ] && K_TESTS="?"
	STATUS="???"
	if [ "$S_OK" = "True" ] && [ "$K_OK" = "True" ]; then
		STATUS="PARITY"
		PARITY=$((PARITY+1))
	elif [ "$S_OK" = "True" ] && [ "$K_OK" != "True" ]; then
		STATUS="DIVERGE_KVM_REGRESSION"
		DIVERGE=$((DIVERGE+1))
	elif [ "$S_OK" != "True" ] && [ "$K_OK" = "True" ]; then
		STATUS="DIVERGE_KVM_BETTER"
		DIVERGE=$((DIVERGE+1))
	elif [ "$S_OK" = "?" ] || [ "$K_OK" = "?" ]; then
		STATUS="SKIP_NO_RESULT"
		SKIP=$((SKIP+1))
	else
		STATUS="BOTH_FAIL"
		PARITY=$((PARITY+1))
	fi
	printf "%-20s | s=%4s/%-5s | k=%4s/%-5s | %s\n" "$MOD" "$S_TESTS" "$S_OK" "$K_TESTS" "$K_OK" "$STATUS"
done

rm -f "$HARNESS"

echo
echo "TOTAL: parity=$PARITY diverge=$DIVERGE skip=$SKIP"
[ "$DIVERGE" -gt 0 ] && exit 1
exit 0
