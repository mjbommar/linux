#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Per-class runner for Class D (structural) reproducers.
# Invoked from the top-level run-regrtest-repros.sh inside the UML guest.
DIR="$(dirname "$0")"

for b in itimer_virtual itimer_prof itimer_real getrusage_split; do
	if [ -x "$DIR/$b" ]; then
		"$DIR/$b" 2>&1
	else
		echo "REPRO: $b EXPECTED_FAIL not_built"
	fi
done

PY=${PY:-/usr/bin/python3}
if [ -x "$PY" ]; then
	"$PY" "$DIR/thread_excepthook.py" 2>&1
else
	echo "REPRO: thread_excepthook EXPECTED_FAIL no_python"
fi
