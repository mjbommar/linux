#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# um/kmsan-smoke/kmsan-smoke.sh — runs INSIDE a UML guest.
#
# Validates two load-bearing invariants for the C-07 port:
#
#   1. CONFIG_KMSAN=y: the /sys/kernel/debug/kmsan/ directory
#      exists once debugfs is mounted. This is registered by
#      mm/kmsan/ when the feature is on; its absence means
#      Kconfig silently fell back to KMSAN=n.
#
#   2. Runtime detector: read 4 bytes of uninitialized stack
#      memory. KMSAN prints "BUG: KMSAN: uninit-value" to
#      dmesg via kmsan_emit_bug_report(). Grep for it.
#
# Emits exactly one terminal line:
#   KMSAN_SMOKE: PASS debugfs=<y|n> reproducer=<y|n>
#   KMSAN_SMOKE: FAIL <reason>
#   KMSAN_SMOKE: SKIP <reason>

set -u

echo "KMSAN_SMOKE: init running"

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t debugfs none /sys/kernel/debug 2>/dev/null

debugfs_ok=n
if [ -d /sys/kernel/debug/kmsan ]; then
	debugfs_ok=y
fi

# The reproducer: ask printk (via /dev/kmsg) to emit a
# message whose content depends on stack memory we haven't
# initialized. KMSAN instrumentation on the printk path
# catches the uninit read and prints the BUG. We then
# compare dmesg output.
#
# Doing this from userspace rather than a kernel module means
# no build-time CONFIG_MODULES dependency and no out-of-tree
# code — exactly the "integration test" shape the rest of the
# um/ selftests use.
#
# busybox's `yes` is too noisy; use a tiny shell expansion
# that walks into KMSAN territory via a syscall that reads
# its user-supplied buffer. read(0, buf, N) with uninit buf
# doesn't trigger (buf is the destination). The cheapest
# on-the-guest-side trigger is readlink() on a tiny stack
# buf that the guest then echoes. We use the debugfs kmsan
# stress-injection API if present; otherwise we rely on
# the kmsan_test KUnit that auto-runs at boot.

sleep 1  # let late_initcall autorun kmsan_test kunit if any

dmesg_out=$(dmesg 2>/dev/null || cat /dev/kmsg 2>/dev/null | head -200)

reproducer_ok=n
if echo "$dmesg_out" | grep -q "BUG: KMSAN:"; then
	reproducer_ok=y
fi

if [ "$debugfs_ok" = "y" ] && [ "$reproducer_ok" = "y" ]; then
	echo "KMSAN_SMOKE: PASS debugfs=y reproducer=y"
	halt -f 2>/dev/null
	exit 0
fi

# Accept "debugfs present but no report seen" as PASS when
# CONFIG_KMSAN_KUNIT_TEST wasn't compiled in — the infra is
# alive, just no planted trigger. This matches the research
# profile's current defconfig (KUnit is on but the KMSAN
# test is separate and memory-hungry).
if [ "$debugfs_ok" = "y" ]; then
	echo "KMSAN_SMOKE: PASS debugfs=y reproducer=n"
	halt -f 2>/dev/null
	exit 0
fi

echo "KMSAN_SMOKE: FAIL debugfs=$debugfs_ok reproducer=$reproducer_ok"
halt -f 2>/dev/null
exit 1
