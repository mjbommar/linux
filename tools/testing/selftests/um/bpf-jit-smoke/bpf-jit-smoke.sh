#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Guest-side init wrapper for the UML BPF JIT smoke test.

mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null

helper=$(sed -n 's/.*bpf_jit_helper=\([^ ]*\).*/\1/p' /proc/cmdline)
if [ -z "$helper" ]; then
	helper=$(dirname "$0")/bpf-jit-smoke
fi

if [ ! -x "$helper" ]; then
	echo "BPF_JIT_SMOKE: SKIP helper $helper not executable"
	halt -f 2>/dev/null
	poweroff -f 2>/dev/null
	exit 4
fi

"$helper"
rc=$?

halt -f 2>/dev/null
poweroff -f 2>/dev/null
exit "$rc"
