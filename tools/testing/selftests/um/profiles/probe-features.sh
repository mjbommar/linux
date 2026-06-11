#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# probe-features.sh - run INSIDE a UML guest. Mounts the usual
# pseudo-filesystems and emits one line per feature/path in the
# form:
#
#     FEATURE <name> PRESENT
#     FEATURE <name> ABSENT
#
# Output is written to both stdout (for interactive / local
# runs where `timeout $BIN ... 2>&1` is captured cleanly) and
# to a hostfs file at `/tmp/uml-probe-output` (for
# environments where UML's tty driver discards the pending
# queue when halt_skas unwinds the process - e.g. GitHub
# Actions runners with `con=fd:0,fd:1` piped to a non-tty fd).
# The host-side run-profile-checks.sh prefers the file and
# falls back to the stdout capture.
#
# Host-side run-profile-checks.sh parses these lines and asserts a
# per-profile expected set. Works under every profile (including
# sandbox, where most probes should report ABSENT).

# File-based output: hostfs maps /tmp to the host's /tmp, so
# the host can read the result after UML exits. Opening the
# file here (truncate) also serves as an "init reached this
# point" signal.
PROBE_OUT=/tmp/uml-probe-output
: >"$PROBE_OUT" 2>/dev/null || PROBE_OUT=

# Write to /dev/kmsg so that even if init's stdout is wired to a
# dead console (seen on GHA runners where `con=null` is our default),
# the "init reached" signal still lands in the kernel log ring, which
# the harness captures via the UML process's stdout. First thing,
# before any mount or exec that could fail.
echo 'probe-features.sh: init running' >/dev/kmsg 2>/dev/null || true

# hostfs is root; the guest sees the host's /proc, /sys, /dev
# until it overlays its own. Mount proc and sysfs FIRST so the
# rest of the probe sees guest-kernel state, not host-kernel state.
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t tracefs none /sys/kernel/tracing 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

# Write to stdout (legacy contract) and to $PROBE_OUT (the
# reliable path). Both destinations see the same lines so
# either reader can parse.
emit() {
	echo "$1"
	if [ -n "$PROBE_OUT" ]; then
		echo "$1" >>"$PROBE_OUT"
	fi
}

probe() {
	name=$1
	path=$2
	if [ -e "$path" ]; then
		emit "FEATURE $name PRESENT"
	else
		emit "FEATURE $name ABSENT"
	fi
}

emit "PROBE_BEGIN"
# debugfs: /sys/kernel/debug/um exists only if our late_initcall ran,
# which requires CONFIG_DEBUG_FS=y.
probe debugfs_um              /sys/kernel/debug/um
probe debugfs_um_hooks        /sys/kernel/debug/um/hooks
probe debugfs_um_stats        /sys/kernel/debug/um/stats
probe debugfs_kcov            /sys/kernel/debug/kcov
probe debugfs_kfence          /sys/kernel/debug/kfence/stats
probe debugfs_kcsan           /sys/kernel/debug/kcsan
probe debugfs_kmsan           /sys/kernel/debug/kmsan
# tracefs: `mount -t tracefs` fails silently above if the kernel
# doesn't register tracefs. available_tracers is created by ftrace
# init and is present iff tracing is usable.
probe tracefs                 /sys/kernel/tracing/available_tracers
probe tracefs_syscalls        /sys/kernel/tracing/events/syscalls
probe tracefs_user_events     /sys/kernel/tracing/user_events_data
probe proc_kcore              /proc/kcore
# CONFIG_MAGIC_SYSRQ=y registers /proc/sysrq-trigger.
probe proc_sysrq              /proc/sysrq-trigger
emit "PROBE_END"
halt -f
