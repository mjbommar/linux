#!/bin/sh
# Class A env repro: subprocess for a missing executable must raise
# FileNotFoundError, not silently succeed. Sanity check that PATH
# semantics behave deterministically under PID-1 init.
set -u

PY=${PY:-/usr/bin/python3}
if [ ! -x "$PY" ]; then
	echo "REPRO: env_path_subprocess EXPECTED_FAIL no_python"
	exit 0
fi

OUT=$("$PY" -c '
import subprocess, sys
try:
    subprocess.run(["nonexistent_xyz_cmd_repro"], check=False)
    print("OPENED")
except FileNotFoundError:
    print("ENOENT")
except Exception as e:
    print("OTHER:%s" % type(e).__name__)
' 2>/dev/null)

case "$OUT" in
	ENOENT)
		echo "REPRO: env_path_subprocess PASS"
		;;
	OPENED)
		echo "REPRO: env_path_subprocess FAIL silent_success"
		;;
	*)
		echo "REPRO: env_path_subprocess FAIL out=$OUT"
		;;
esac
exit 0
