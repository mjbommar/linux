#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/aperf-mperf-smoke/run-aperf-mperf-smoke.sh
#
# Boots a kvm-v2 UML kernel built with
# CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y and confirms the
# in-guest rdmsr probe at /sys/kernel/debug/um/kvm_v2/aperf_mperf
# returns non-zero, monotonically advancing values.  This is the
# direct repro target for Anderson's QEMU+libvirt bug
# (KVM_X86_DISABLE_EXITS_APERFMPERF not plumbed by the upstream
# userspace VMM): with kvm-v2 we issue the cap directly and the
# guest reads real host counters, proving the kernel-side path
# works as documented.
#
# Two read-and-print cycles are run with a short busy-loop in
# between (sleep would yield the vCPU and confuse APERF; busy keeps
# the host CPU spinning so the counters definitely advance).  The
# script asserts:
#
#   1. toggle=on in both reads.
#   2. aperf > 0 and mperf > 0 in both reads.
#   3. aperf_2 > aperf_1 AND mperf_2 > mperf_1 (counters advanced).
#   4. 0.5 < (aperf / mperf) < 2.0  in both reads (sanity bound:
#      APERF/MPERF ratio reflects frequency excursion from the
#      base; a ratio outside [0.5, 2.0] points at the counters
#      reading garbage rather than passthrough working).
#
# Exits:
#   0  PASS
#   1  FAIL (read or assertion failed)
#   4  SKIP (host requirements unmet: /dev/kvm missing, host CPU
#            lacks aperfmperf feature, UML binary not built with
#            the Kconfig, ...)
#
# Environment:
#   UML_BINARY    UML kernel built with KVM_V2 + APERFMPERF=y
#                 (default: /tmp/uml-aperf/linux)
#   UML_ROOTFS    rootfs ubd image with debugfs(8) + a shell.
#                 (default: /tmp/uml-aperf/rootfs.ext4)
#   UML_MEM       mem= arg. Default 256M.
#   BUSY_LOOP_MS  duration of the in-guest busy loop between
#                 sample 1 and sample 2.  Default 200.

set -u

BINARY=${UML_BINARY:-/tmp/uml-aperf/linux}
ROOTFS=${UML_ROOTFS:-/tmp/uml-aperf/rootfs.ext4}
MEM=${UML_MEM:-256M}
BUSY_LOOP_MS=${BUSY_LOOP_MS:-200}

skip() { echo "SKIP: $*" >&2; exit 4; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- preflight ----------------------------------------------------

if [ ! -x "$BINARY" ]; then
	skip "UML binary $BINARY not found; build with CONFIG_UM_BACKEND_KVM_V2_APERFMPERF_PASSTHROUGH=y and point UML_BINARY at it"
fi

if [ ! -r "$ROOTFS" ]; then
	skip "rootfs image $ROOTFS not readable; build with umlbuild or point UML_ROOTFS at one"
fi

if ! grep -q aperfmperf /proc/cpuinfo 2>/dev/null; then
	skip "host CPU lacks X86_FEATURE_APERFMPERF — KVM masks the disable-exits bit off in kvm_get_allowed_disable_exits()"
fi

# /dev/kvm ACL self-heal — same retry pattern as kvm-smoke.
ensure_kvm_readable() {
	local i
	for i in 1 2 3; do
		[ -r /dev/kvm ] && return 0
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}
if ! ensure_kvm_readable; then
	skip "/dev/kvm not readable by the selftest user"
fi

# ---- in-guest probe script ----------------------------------------
#
# Emitted via cmdline init= so we don't need a custom rootfs.  The
# script is short enough to live inline; it dumps two samples to
# the console with a printable separator the host-side parser
# below greps for.

PROBE='
#!/bin/sh
PROBE=/sys/kernel/debug/um/kvm_v2/aperf_mperf
if ! [ -r "$PROBE" ]; then
	echo "APERF_MPERF_NOPROBE"
	echo o > /proc/sysrq-trigger 2>/dev/null
	echo b > /proc/sysrq-trigger 2>/dev/null
	exit 1
fi
echo "APERF_MPERF_SAMPLE_1_BEGIN"
cat "$PROBE"
echo "APERF_MPERF_SAMPLE_1_END"
# busy spin — sleep would yield the vCPU and APERF wouldn'\''t advance
end=$(($(date +%s%N) / 1000000 + '"$BUSY_LOOP_MS"'))
while [ $(($(date +%s%N) / 1000000)) -lt $end ]; do : ; done
echo "APERF_MPERF_SAMPLE_2_BEGIN"
cat "$PROBE"
echo "APERF_MPERF_SAMPLE_2_END"
echo o > /proc/sysrq-trigger 2>/dev/null
echo b > /proc/sysrq-trigger 2>/dev/null
exit 0
'

PROBE_DIR=$(mktemp -d)
trap "rm -rf $PROBE_DIR" EXIT
PROBE_PATH="$PROBE_DIR/probe.sh"
printf "%s" "$PROBE" > "$PROBE_PATH"
chmod +x "$PROBE_PATH"

# ---- boot ---------------------------------------------------------

# Use a short timeout: this is a single boot + two-cat + reboot.
# 20s should be plenty even on a busy CI host.  kill-after escalates
# to SIGKILL if UML ignores SIGTERM.
LOG=$(mktemp)
trap "rm -rf $PROBE_DIR; rm -f $LOG" EXIT

timeout --kill-after=10 20 \
	"$BINARY" \
	mem="$MEM" \
	ubda="$ROOTFS" \
	backend=force=kvm-v2 \
	kvm_v2_aperfmperf=on \
	root=/dev/ubda \
	rw \
	init=/bin/sh -- -c "$PROBE_PATH" \
	con0=fd:0,fd:1 \
	con=none \
	>"$LOG" 2>&1 || true

# ---- parse --------------------------------------------------------

if ! grep -q APERF_MPERF_SAMPLE_1_BEGIN "$LOG"; then
	echo "---- guest log (first 40 lines) ----" >&2
	head -40 "$LOG" >&2
	echo "---- guest log (last 40 lines)  ----" >&2
	tail -40 "$LOG" >&2
	fail "guest did not reach the probe — boot wedged or probe init= script unreachable"
fi

if grep -q APERF_MPERF_NOPROBE "$LOG"; then
	fail "debugfs file /sys/kernel/debug/um/kvm_v2/aperf_mperf missing — Kconfig OFF or debugfs unmounted in guest"
fi

extract() {
	awk -v key="$1" -v sample="$2" '
		$0 == "APERF_MPERF_SAMPLE_" sample "_BEGIN" { in_sample = 1; next }
		$0 == "APERF_MPERF_SAMPLE_" sample "_END"   { exit }
		in_sample && $0 ~ "^"key"=" {
			split($0, a, "=")
			print a[2]
		}
	' "$LOG"
}

TOGGLE_1=$(extract toggle 1)
APERF_1=$(extract aperf 1)
MPERF_1=$(extract mperf 1)
TOGGLE_2=$(extract toggle 2)
APERF_2=$(extract aperf 2)
MPERF_2=$(extract mperf 2)

[ "$TOGGLE_1" = "on" ] || fail "toggle=$TOGGLE_1 in sample 1 (expected on)"
[ "$TOGGLE_2" = "on" ] || fail "toggle=$TOGGLE_2 in sample 2 (expected on)"
[ -n "$APERF_1" ] || fail "aperf empty in sample 1"
[ -n "$APERF_2" ] || fail "aperf empty in sample 2"
[ -n "$MPERF_1" ] || fail "mperf empty in sample 1"
[ -n "$MPERF_2" ] || fail "mperf empty in sample 2"

[ "$APERF_1" -gt 0 ] || fail "aperf=0 in sample 1 — passthrough NOT working (KVM is still intercepting)"
[ "$MPERF_1" -gt 0 ] || fail "mperf=0 in sample 1 — passthrough NOT working"
[ "$APERF_2" -gt "$APERF_1" ] || fail "aperf did not advance: $APERF_1 -> $APERF_2"
[ "$MPERF_2" -gt "$MPERF_1" ] || fail "mperf did not advance: $MPERF_1 -> $MPERF_2"

# Ratio sanity: 0.5 <= aperf/mperf <= 2.0 expressed as integer pct.
RATIO_1=$(( APERF_1 * 100 / MPERF_1 ))
RATIO_2=$(( APERF_2 * 100 / MPERF_2 ))

if [ "$RATIO_1" -lt 50 ] || [ "$RATIO_1" -gt 200 ]; then
	fail "ratio sample 1 = ${RATIO_1}%% outside [50, 200]; counters likely reading garbage"
fi
if [ "$RATIO_2" -lt 50 ] || [ "$RATIO_2" -gt 200 ]; then
	fail "ratio sample 2 = ${RATIO_2}%% outside [50, 200]; counters likely reading garbage"
fi

echo "PASS aperf_mperf_smoke"
echo "  sample 1: aperf=$APERF_1 mperf=$MPERF_1 ratio=${RATIO_1}%%"
echo "  sample 2: aperf=$APERF_2 mperf=$MPERF_2 ratio=${RATIO_2}%%"
echo "  delta:    aperf=$((APERF_2 - APERF_1)) mperf=$((MPERF_2 - MPERF_1))"
exit 0
