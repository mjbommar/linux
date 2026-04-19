#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# um/ftrace-smoke/ftrace-smoke.sh — workstream C-05 smoke test.
#
# Runs inside a UML guest via init=. Verifies the UML function
# tracer port is live:
#
#   1. tracefs mounts
#   2. writing `function` to current_tracer succeeds
#   3. a small syscall workload produces trace output (>100 lines)
#   4. writing `nop` to current_tracer succeeds
#   5. emits a single FTRACE_SMOKE: PASS|FAIL line and halts
#
# Requires CONFIG_FUNCTION_TRACER=y + CONFIG_DYNAMIC_FTRACE=y (the
# `research` profile enables both; see
# Documentation/virt/uml/redesign/02-workstreams/C-profiles-and-gaps/
# 05-port-ftrace.md).

echo "FTRACE_SMOKE: init running"

mount -t tracefs none /sys/kernel/tracing 2>/dev/null || \
	mount -t tracefs none /sys/kernel/debug/tracing 2>/dev/null

if [ -d /sys/kernel/tracing ]; then
	T=/sys/kernel/tracing
elif [ -d /sys/kernel/debug/tracing ]; then
	T=/sys/kernel/debug/tracing
else
	echo "FTRACE_SMOKE: FAIL no tracefs"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 1
fi

echo "FTRACE_SMOKE: tracefs at $T"

echo "function" > "$T/current_tracer"
if [ $? -ne 0 ]; then
	echo "FTRACE_SMOKE: FAIL cannot enable function tracer"
	halt -f 2>/dev/null
	exit 1
fi

echo "FTRACE_SMOKE: set function tracer OK"
echo 1 > "$T/tracing_on"
ls / > /dev/null
echo 0 > "$T/tracing_on"

lines=$(wc -l < "$T/trace")
echo "FTRACE_SMOKE: trace has $lines lines"

echo "nop" > "$T/current_tracer"

if [ "$lines" -lt 100 ]; then
	echo "FTRACE_SMOKE: FAIL trace_lines=$lines (expected >100)"
else
	echo "FTRACE_SMOKE: PASS trace_lines=$lines"
fi

halt -f 2>/dev/null
poweroff -f 2>/dev/null
