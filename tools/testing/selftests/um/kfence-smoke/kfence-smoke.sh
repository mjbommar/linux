#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side init wrapper for the UML KFENCE smoke test.

set -u

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

echo "KFENCE_SMOKE: init running"

if [ ! -f /sys/kernel/debug/kfence/stats ]; then
	echo "KFENCE_SMOKE: SKIP kfence debugfs stats missing"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 4
fi

MODULE=${UML_KFENCE_MODULE:-}
if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	CMDLINE_MODULE=$(cat /proc/cmdline 2>/dev/null \
		| tr ' ' '\n' | sed -n 's/^kfence_test_module=//p' | head -1)
	if [ -n "$CMDLINE_MODULE" ] && [ -f "$CMDLINE_MODULE" ]; then
		MODULE="$CMDLINE_MODULE"
	fi
fi
if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	for cand in \
		/tmp/uml-research/mm/kfence/kfence_test.ko \
		/mm/kfence/kfence_test.ko; do
		if [ -f "$cand" ]; then
			MODULE="$cand"
			break
		fi
	done
fi

if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	echo "KFENCE_SMOKE: SKIP kfence_test.ko not found (set UML_KFENCE_MODULE)"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 4
fi

echo "KFENCE_SMOKE: using module $MODULE"

if ! insmod "$MODULE" 2>/dev/null; then
	if ! dmesg | grep -q 'KTAP version' 2>/dev/null &&
		! dmesg | grep -q 'BUG: KFENCE:' 2>/dev/null; then
		echo "KFENCE_SMOKE: FAIL insmod failed and no KUnit/KFENCE output"
		dmesg | tail -40
		halt -f 2>/dev/null
		poweroff -f 2>/dev/null
		exit 1
	fi
fi

BUGS=$(dmesg | grep 'BUG: KFENCE:' | wc -l)
OK_COUNT=$(dmesg | grep -E '([[:space:]]|^)ok [0-9]+[[:space:]]+test_' | wc -l)
NOT_OK_COUNT=$(dmesg | grep -E '([[:space:]]|^)not ok [0-9]+[[:space:]]+test_' | wc -l)
STATS_BUGS=$(sed -n 's/^total bugs:[[:space:]]*//p' \
	/sys/kernel/debug/kfence/stats 2>/dev/null | head -1)
STATS_BUGS=${STATS_BUGS:-0}
case "$STATS_BUGS" in
''|*[!0-9]*) STATS_BUGS=0 ;;
esac
UNEXPECTED=$(dmesg | grep -E \
	'Kernel panic|Oops|kernel BUG|Unable to handle kernel|Segfault with no mm' \
	| wc -l)

if [ "$UNEXPECTED" -gt 0 ]; then
	echo "KFENCE_SMOKE: FAIL unexpected_kernel_errors=$UNEXPECTED"
	dmesg | grep -E \
		'Kernel panic|Oops|kernel BUG|Unable to handle kernel|Segfault with no mm' \
		| head -5
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

if [ "$BUGS" -lt 1 ]; then
	echo "KFENCE_SMOKE: FAIL bugs=$BUGS stats_bugs=$STATS_BUGS"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

if [ "$STATS_BUGS" -lt 1 ]; then
	echo "KFENCE_SMOKE: FAIL bugs=$BUGS stats_bugs=$STATS_BUGS"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

if [ "$NOT_OK_COUNT" -gt 0 ]; then
	printf 'KFENCE_SMOKE: FAIL bugs=%s stats_bugs=%s ok=%s not_ok=%s\n' \
		"$BUGS" "$STATS_BUGS" "$OK_COUNT" "$NOT_OK_COUNT"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

printf 'KFENCE_SMOKE: PASS bugs=%s stats_bugs=%s ok=%s not_ok=%s\n' \
	"$BUGS" "$STATS_BUGS" "$OK_COUNT" "$NOT_OK_COUNT"
halt -f 2>/dev/null
poweroff -f 2>/dev/null
exit 0
