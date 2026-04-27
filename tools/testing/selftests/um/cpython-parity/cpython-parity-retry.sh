#!/usr/bin/env bash
# Parity gate with per-module retry. Each module gets up to MAX_TRIES
# attempts; module passes if any single try succeeds.
# Useful to characterize: is the failure independent per-trial (in
# which case retries dramatically improve gate) or correlated (in
# which case retries don't help)?

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
PY=${PY:-/usr/bin/python3}
MEM=${MEM:-1024M}
TIMEOUT_PER_TRY=${TIMEOUT_PER_TRY:-180}
MAX_TRIES=${MAX_TRIES:-5}

MODULES="${MODULES:-test_struct test_math test_hashlib test_bisect test_array test_decimal test_fractions test_string test_bytes test_dict test_list test_set test_tuple test_int test_float test_complex test_heapq test_itertools test_operator test_typing test_abc}"

HARNESS=$(mktemp /tmp/parity-retry-harness.XXXXX.py)
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

run_module_kvm() {
	local mod="$1"
	timeout --kill-after=15 "$TIMEOUT_PER_TRY" "$BINARY" \
		backend=force=kvm mem="$MEM" \
		con=null con0=fd:0,fd:1 \
		root=/dev/root rootfstype=hostfs rw panic=-1 \
		init="$PY" -- "$HARNESS" "test.$mod" \
		</dev/null 2>&1 | grep "^PARITY:" | head -1
}

PASS=0
FAIL=0
TOTAL_TRIES=0

echo "module | best | tries | status"
echo "-------|------|-------|-------"
for MOD in $MODULES; do
	best="?"
	tries=0
	for try in $(seq 1 $MAX_TRIES); do
		tries=$((tries+1))
		TOTAL_TRIES=$((TOTAL_TRIES+1))
		out=$(run_module_kvm "$MOD")
		ok=$(echo "$out" | grep -oE "ok=[A-Za-z]+" | sed 's/ok=//')
		if [ "$ok" = "True" ]; then
			best="True"
			break
		fi
		[ -z "$ok" ] && ok="?"
		best="$ok"
	done
	if [ "$best" = "True" ]; then
		PASS=$((PASS+1))
		STATUS="PARITY"
	else
		FAIL=$((FAIL+1))
		STATUS="DIVERGE"
	fi
	printf "%-20s | %-4s | %d | %s\n" "$MOD" "$best" "$tries" "$STATUS"
done

rm -f "$HARNESS"

echo
echo "TOTAL: parity=$PASS diverge=$FAIL  total_kernel_runs=$TOTAL_TRIES"
[ "$FAIL" -gt 0 ] && exit 1
exit 0
