#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Per-class runner for Class A (environmental) reproducers.
# Invoked from the top-level run-regrtest-repros.sh inside the UML guest.
DIR="$(dirname "$0")"

for b in terminal_size openpty tty_isatty controlling_tty termios_get termios_mode_probe; do
	if [ -x "$DIR/$b" ]; then
		"$DIR/$b" 2>&1
	else
		echo "REPRO: $b EXPECTED_FAIL not_built"
	fi
done

PY=${PY:-/usr/bin/python3}
if [ -x "$PY" ]; then
	"$PY" "$DIR/ensurepip_check.py" 2>&1
else
	echo "REPRO: ensurepip_check EXPECTED_FAIL no_python"
fi

sh "$DIR/env_path_subprocess.sh" 2>&1
