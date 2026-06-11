#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side init wrapper for the UML KCSAN smoke test.

set -u

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

echo "KCSAN_SMOKE: init running"

if [ ! -f /sys/kernel/debug/kcsan ]; then
	echo "KCSAN_SMOKE: SKIP debugfs_kcsan=missing"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 4
fi

selftest=$(dmesg | grep -c 'kcsan: selftest: 3/3 tests passed')
initial=$(awk '$1=="enabled:" {print $2; exit}' /sys/kernel/debug/kcsan)
initial=${initial:-unknown}

if ! echo on >/sys/kernel/debug/kcsan; then
	echo "KCSAN_SMOKE: FAIL enable_write initial=$initial"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi
enabled=$(awk '$1=="enabled:" {print $2; exit}' /sys/kernel/debug/kcsan)
enabled=${enabled:-unknown}

if ! echo microbench=200 >/sys/kernel/debug/kcsan; then
	echo "KCSAN_SMOKE: FAIL microbench enabled=$enabled selftest=$selftest"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

if ! echo off >/sys/kernel/debug/kcsan; then
	echo "KCSAN_SMOKE: FAIL disable_write enabled=$enabled selftest=$selftest"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi
disabled=$(awk '$1=="enabled:" {print $2; exit}' /sys/kernel/debug/kcsan)
disabled=${disabled:-unknown}

bench_begin=$(dmesg | grep -c 'kcsan: microbenchmark begin')
bench_end=$(dmesg | grep -c 'kcsan: microbenchmark end')
unexpected_re='Kernel panic|Oops|kernel BUG|Unable to handle kernel|'
unexpected_re="${unexpected_re}WARNING:|BUG: KCSAN:|data-race"
dump_re='kcsan: selftest|kcsan: microbenchmark|Kernel panic|Oops|'
dump_re="${dump_re}kernel BUG|WARNING:|BUG: KCSAN:|data-race"
unexpected=$(dmesg | grep -E "$unexpected_re" | wc -l)

if [ "$selftest" -lt 1 ] || [ "$initial" != 0 ] ||
	[ "$enabled" != 1 ] || [ "$disabled" != 0 ] ||
	[ "$bench_begin" -lt 1 ] || [ "$bench_end" -lt 1 ] ||
	[ "$unexpected" -gt 0 ]; then
	printf 'KCSAN_SMOKE: FAIL selftest=%s initial=%s enabled=%s ' \
		"$selftest" "$initial" "$enabled"
	printf 'disabled=%s microbench_begin=%s microbench_end=%s ' \
		"$disabled" "$bench_begin" "$bench_end"
	printf 'unexpected=%s\n' "$unexpected"
	dmesg | grep -E "$dump_re" | tail -40
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

printf 'KCSAN_SMOKE: PASS selftest=%s initial=%s enabled=%s ' \
	"$selftest" "$initial" "$enabled"
printf 'disabled=%s microbench_begin=%s microbench_end=%s ' \
	"$disabled" "$bench_begin" "$bench_end"
printf 'unexpected=%s\n' "$unexpected"
halt -f 2>/dev/null
poweroff -f 2>/dev/null
exit 0
