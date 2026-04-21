#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# um/kprobes-stress/kprobes-stress.sh — workstream C-04 stress test.
#
# Runs inside a UML guest via init=. Exercises the kprobes +
# kretprobes-via-rethook port under a fork-heavy workload. Intended
# as the regression guard for C-04 commits 1a-1d + 2, and as the
# empirical gate for a future HAVE_FUNCTION_GRAPH_TRACER landing
# (decisions-log D34).
#
# What it does:
#
#   1. Mount proc/sys/debugfs.
#   2. Insmod samples/kprobes/kretprobe_example.ko on kernel_clone.
#   3. Confirm the probe shows in /sys/kernel/debug/kprobes/list.
#   4. Run N iterations of /bin/true (each triggers kernel_clone).
#   5. Count kretprobe fires in dmesg. Each /bin/true is one fire;
#      allow for a few extra from kernel-side helper forks.
#   6. Check dmesg for kernel errors (BUG/Oops/WARNING/panic).
#   7. Rmmod cleanly. Confirm probe gone from list.
#   8. Emit KPROBES_STRESS: PASS|FAIL|SKIP <details>.
#
# Requires CONFIG_KPROBES=y + CONFIG_KRETPROBES=y (auto-selected
# via HAVE_RETHOOK on UML x86_64). The research profile enables
# both; see Documentation/virt/uml/profiles/research.rst.
#
# If samples/kprobes/kretprobe_example.ko is not available, the
# script emits SKIP and halts cleanly — commit 5 is a regression
# guard, not a kernel-build verifier.

ITERS=${KPROBES_STRESS_ITERS:-200}
MODULE=${UML_KRETPROBE_MODULE:-}

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

# Env vars don't propagate through UML's kernel-start → init exec
# path. Allow the host-side runner to override ITERS via the
# kernel command line (kretprobe_iters=N); the env var still
# works for manual invocations.
CMDLINE_ITERS=$(cat /proc/cmdline 2>/dev/null \
	| tr ' ' '\n' | sed -n 's/^kretprobe_iters=//p' | head -1)
if [ -n "$CMDLINE_ITERS" ] && [ "$CMDLINE_ITERS" -gt 0 ] 2>/dev/null; then
	ITERS="$CMDLINE_ITERS"
fi

echo "KPROBES_STRESS: init running iters=$ITERS"

if [ ! -d /sys/kernel/debug/kprobes ]; then
	echo "KPROBES_STRESS: SKIP debugfs/kprobes not mounted (CONFIG_KPROBES missing?)"
	halt -f 2>/dev/null
	exit 0
fi

if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	# Environment variables don't propagate from the host into
	# the UML guest's init process — kernel cmdline is the portable
	# channel. Try kretprobe_module=/path/to/ko on /proc/cmdline
	# first (set by run-kprobes-stress.sh), then fall back to a
	# few well-known build-tree locations.
	CMDLINE_MODULE=$(cat /proc/cmdline 2>/dev/null \
		| tr ' ' '\n' | sed -n 's/^kretprobe_module=//p' | head -1)
	if [ -n "$CMDLINE_MODULE" ] && [ -f "$CMDLINE_MODULE" ]; then
		MODULE="$CMDLINE_MODULE"
	fi
fi
if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	# Fall back to a couple of common build-tree locations.
	for cand in \
		/home/*/src/linux/samples/kprobes/kretprobe_example.ko \
		/tmp/uml-research/samples/kprobes/kretprobe_example.ko \
		/samples/kprobes/kretprobe_example.ko; do
		if [ -f "$cand" ]; then
			MODULE="$cand"
			break
		fi
	done
fi

if [ -z "$MODULE" ] || [ ! -f "$MODULE" ]; then
	echo "KPROBES_STRESS: SKIP kretprobe_example.ko not found (set UML_KRETPROBE_MODULE)"
	halt -f 2>/dev/null
	exit 0
fi

echo "KPROBES_STRESS: using module $MODULE"

# Load with default symbol=kernel_clone (every fork hits it).
if ! insmod "$MODULE" 2>/dev/null; then
	echo "KPROBES_STRESS: FAIL insmod failed"
	halt -f 2>/dev/null
	exit 1
fi

if ! grep -q kernel_clone /sys/kernel/debug/kprobes/list 2>/dev/null; then
	echo "KPROBES_STRESS: FAIL probe not in kprobes/list after insmod"
	rmmod kretprobe_example 2>/dev/null
	halt -f 2>/dev/null
	exit 1
fi

echo "KPROBES_STRESS: probe live on kernel_clone"

# Workload: N forks. /bin/true is the smallest process we can exec.
i=0
while [ "$i" -lt "$ITERS" ]; do
	/bin/true
	i=$((i + 1))
done

# Count kretprobe fires (sample module logs "kernel_clone returned ..."
# once per return). Kernel-side helpers (kworker etc.) also fork, so
# FIRES >= ITERS rather than == ITERS.
FIRES=$(dmesg | grep -c "kernel_clone returned" || echo 0)
echo "KPROBES_STRESS: probe fired $FIRES times for $ITERS iterations"

# Scan for kernel errors. A single BUG/Oops/WARNING/panic is FAIL.
# Match common signatures from include/asm-generic/bug.h and
# arch/um/kernel/trap.c's panic sites.
ERRORS=$(dmesg | grep -cE \
	"Kernel panic|BUG:|Oops|kernel BUG|WARNING: |Unable to handle kernel|Segfault with no mm" \
	|| echo 0)
if [ "$ERRORS" -gt 0 ]; then
	echo "KPROBES_STRESS: FAIL kernel_errors=$ERRORS"
	dmesg | grep -E "Kernel panic|BUG:|Oops|WARNING: |Segfault with no mm" \
		| head -5
	rmmod kretprobe_example 2>/dev/null
	halt -f 2>/dev/null
	exit 1
fi

# Unload cleanly.
if ! rmmod kretprobe_example 2>/dev/null; then
	echo "KPROBES_STRESS: FAIL rmmod failed"
	halt -f 2>/dev/null
	exit 1
fi

if grep -q kernel_clone /sys/kernel/debug/kprobes/list 2>/dev/null; then
	echo "KPROBES_STRESS: FAIL probe still in list after rmmod"
	halt -f 2>/dev/null
	exit 1
fi

# Require at least one fire (guard against silent "never armed").
if [ "$FIRES" -lt 1 ]; then
	echo "KPROBES_STRESS: FAIL no probe fires observed"
	halt -f 2>/dev/null
	exit 1
fi

echo "KPROBES_STRESS: PASS iters=$ITERS fires=$FIRES errors=0 graph=deferred"
halt -f 2>/dev/null
poweroff -f 2>/dev/null
