#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# probe-features.sh — run INSIDE a UML guest. Mounts the usual
# pseudo-filesystems and emits one line per feature/path in the
# form:
#
#     FEATURE <name> PRESENT
#     FEATURE <name> ABSENT
#
# Host-side run-profile-checks.sh parses these lines and asserts a
# per-profile expected set. Works under every profile (including
# sandbox, where most probes should report ABSENT).

# Breadcrumb: write to /dev/kmsg so that even if init's stdout is
# wired to a dead console (seen on GHA runners where `con=null`
# is our default), the "init reached" signal still lands in the
# kernel log ring — which the harness captures via the UML
# process's stdout. First thing, before any mount/exec that
# could fail.
echo 'probe-features.sh: init running' >/dev/kmsg 2>/dev/null || true

# hostfs is root; the guest sees the host's /proc, /sys, /dev
# until it overlays its own. Mount proc and sysfs FIRST so the
# rest of the probe sees guest-kernel state, not host-kernel state.
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t tracefs none /sys/kernel/tracing 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

probe() {
	name=$1
	path=$2
	if [ -e "$path" ]; then
		echo "FEATURE $name PRESENT"
	else
		echo "FEATURE $name ABSENT"
	fi
}

echo "PROBE_BEGIN"
# debugfs: /sys/kernel/debug/um exists only if our late_initcall ran,
# which requires CONFIG_DEBUG_FS=y.
probe debugfs_um              /sys/kernel/debug/um
probe debugfs_um_hooks        /sys/kernel/debug/um/hooks
probe debugfs_um_stats        /sys/kernel/debug/um/stats
probe debugfs_kcov            /sys/kernel/debug/kcov
probe debugfs_kfence          /sys/kernel/debug/kfence/stats
probe debugfs_kcsan           /sys/kernel/debug/kcsan
# tracefs: `mount -t tracefs` fails silently above if the kernel
# doesn't register tracefs. available_tracers is created by ftrace
# init and is present iff tracing is usable.
probe tracefs                 /sys/kernel/tracing/available_tracers
probe tracefs_syscalls        /sys/kernel/tracing/events/syscalls
probe tracefs_user_events     /sys/kernel/tracing/user_events_data
probe proc_kcore              /proc/kcore
# CONFIG_MAGIC_SYSRQ=y registers /proc/sysrq-trigger.
probe proc_sysrq              /proc/sysrq-trigger
echo "PROBE_END"
halt -f
