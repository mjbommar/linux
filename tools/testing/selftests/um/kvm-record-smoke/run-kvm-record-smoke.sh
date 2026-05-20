#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/kvm-record-smoke/run-kvm-record-smoke.sh — kselftest for the
# v2 record/replay primitives (#169 Phase 7).
#
# Boots UML with `backend=force=kvm-v2` and asserts every KUnit case
# in the kvm_v2_record suite reports PASS:
#
#   test_kvm_v2_record_basic            — lifecycle (alloc/start/stop/
#                                         replay/destroy) + gate flips.
#   test_kvm_v2_record_state_transitions— invalid-edge -EINVAL coverage.
#   test_kvm_v2_record_observe          — Phase 2 observe append.
#   test_kvm_v2_record_strict_replay    — Phase 3 consume + strict mode.
#   test_kvm_v2_record_gadget_bypass    — Phase 4 gadget RECORD byte.
#   test_kvm_v2_record_rdtsc            — Phase 5 RDTSC round-trip.
#   test_kvm_v2_record_sigalrm          — Phase 6 SIGALRM anchor round-trip.
#
# Build/boot/KUnit-shape regression guard: a kvm-v2 backend regression
# that breaks any of the record/replay primitives fails here.
#
# Pre-v2 history: this script previously targeted the v1 path
# (backend=force=kvm + kvm_record_basic_test / kvm_record_roundtrip_test
# case names + kvm_record_ctl debugfs node). Phase 7 of the v2 port
# re-plumbs against the v2 symbol surface. The debugfs runtime path
# is deferred — the v2 backend ships C/KUnit surface only; a debugfs
# control file is filed as Phase 7+ follow-on once the runtime hook
# wiring matures.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL — kselftest convention.
#
# Environment:
#   UML_BINARY  UML kernel built with CONFIG_UM_BACKEND_KVM_V2=y +
#               CONFIG_UM_BACKEND_KVM_V2_KUNIT=y. Default
#               /tmp/uml-kvmint/linux.
#   UML_MEM     mem= argument. Default 128M.

set -u

BINARY=${UML_BINARY:-/tmp/uml-kvmint/linux}
MEM=${UML_MEM:-128M}

if [ ! -x "$BINARY" ]; then
	echo "SKIP: UML binary $BINARY not found (set UML_BINARY)" >&2
	exit 4
fi

if [ ! -e /dev/kvm ]; then
	echo "SKIP: /dev/kvm not present" >&2
	exit 4
fi

ensure_kvm_readable() {
	local i
	for i in 1 2 3; do
		if [ -r /dev/kvm ]; then
			return 0
		fi
		sudo -n setfacl -m u:"$(id -un)":rw /dev/kvm 2>/dev/null || true
		[ -r /dev/kvm ] && return 0
		sleep 0.1
	done
	return 1
}

if ! ensure_kvm_readable; then
	echo "SKIP: /dev/kvm not readable" >&2
	exit 4
fi

OUT=$(timeout --kill-after=10 30 "$BINARY" \
	backend=force=kvm-v2 \
	init=/bin/true mem="$MEM" \
	con=null con0=fd:0,fd:1 \
	root=/dev/root rootfstype=hostfs rw \
	panic=-1 </dev/null 2>&1 || true)

OBSERVED=$(echo "$OUT" | sed -n 's/^um: backend = \([a-z0-9-]*\).*/\1/p' | head -1)
if [ "$OBSERVED" != "kvm-v2" ]; then
	echo "SKIP: backend probed to '$OBSERVED' (need kvm-v2)" >&2
	exit 4
fi

# The full v2 KUnit record suite — must all PASS.
CASES=(
	"test_kvm_v2_record_basic"
	"test_kvm_v2_record_state_transitions"
	"test_kvm_v2_record_observe"
	"test_kvm_v2_record_strict_replay"
	"test_kvm_v2_record_gadget_bypass"
	"test_kvm_v2_record_rdtsc"
	"test_kvm_v2_record_sigalrm"
	"test_kvm_v2_record_time_travel"
)
MISSING=()
for case in "${CASES[@]}"; do
	if ! echo "$OUT" | grep -Eq "^[[:space:]]+ok [0-9]+ ${case}\b"; then
		MISSING+=("$case")
	fi
done
if [ "${#MISSING[@]}" -gt 0 ]; then
	echo "KVM_RECORD_SMOKE: FAIL (KUnit case(s) not PASS: ${MISSING[*]})"
	echo "$OUT" | grep -E 'kvm_v2_record|backend = ' | head -10
	exit 1
fi

KU_INFO=$(echo "$OUT" | grep -E 'um: kvm-v2 record_start: armed' | head -1)

echo "KVM_RECORD_SMOKE: PASS (8/8 record cases — basic, state, observe, strict_replay, gadget_bypass, rdtsc, sigalrm, time_travel)"
if [ -n "$KU_INFO" ]; then
	echo "KVM_RECORD_SMOKE: info ${KU_INFO}"
fi

# Note: a debugfs runtime path (kvm_v2_record_ctl + state) is deferred
# to Phase 7+ once a host-side observe wiring lands that the guest
# control flow can drive. The KUnit suite above proves the full
# observe/consume contract works end-to-end.

exit 0
