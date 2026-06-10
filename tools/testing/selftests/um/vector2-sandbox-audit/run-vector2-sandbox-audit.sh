#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Selftest: drive `umlctl gate loop --audit-vector-sandbox` against
# a vector2 auto-queue fd boot and assert no forbidden host
# operations were issued.
#
# Skip semantics (kselftest exit 4):
#   - missing UML_KERNEL with CONFIG_UML_NET_VECTOR_V2=y;
#   - missing umlctl Rust binary;
#   - missing iptables / sudo / tuntap capabilities on the
#     runner.
#
# Exit codes:
#   0 - PASS (audit gate ran, no forbidden ops, scoreboard PASS)
#   1 - FAIL (audit detected /dev/net/tun open, TUNSETIFF, AF_PACKET,
#       bpf(), or UML network-helper exec in vector host path)
#   4 - SKIP

set -u

# kselftest exit conventions
KSFT_PASS=0
KSFT_FAIL=1
KSFT_SKIP=4

err() {
	echo "$@" >&2
}

UML_KERNEL="${UML_KERNEL:-}"
# Prefer in-tree umlctl over a packaged binary: the audit gate needs
# the --network-driver + --audit-vector-sandbox flags that only the
# current umlctl source supports.  If the operator pre-sets UMLCTL,
# honour that.
UMLCTL="${UMLCTL:-}"
if [ -z "$UMLCTL" ]; then
	for cand in \
		"$(pwd)/tools/uml/uml-launcher/target/release/umlctl" \
		"$(pwd)/tools/uml/uml-launcher/target/debug/umlctl" \
		"/home/mjbommar/bench-bundle/bin/umlctl"
	do
		if [ -x "$cand" ]; then
			UMLCTL=$cand
			break
		fi
	done
fi

# 1. Locate kernel.  Look in default places if not pre-set.
if [ -z "$UML_KERNEL" ]; then
	for cand in \
		"$HOME/projects/personal/.build/um-vector-r1-kvmv2/linux" \
		"$HOME/src/uml-builds/uml-smp-t41fix/linux" \
		"$HOME/projects/personal/.build/um-vector-r1-kunit/linux"
	do
		if [ -x "$cand" ]; then
			UML_KERNEL=$cand
			break
		fi
	done
fi

if [ -z "$UML_KERNEL" ] || [ ! -x "$UML_KERNEL" ]; then
	err "SKIP: no UML kernel binary found (set UML_KERNEL=)"
	exit $KSFT_SKIP
fi

# 2. Confirm kernel has CONFIG_UML_NET_VECTOR_V2.  The .config
# lives next to the linux binary in the build dir.
build_dir=$(dirname "$UML_KERNEL")
if [ ! -f "$build_dir/.config" ]; then
	err "SKIP: no .config alongside UML_KERNEL=$UML_KERNEL"
	exit $KSFT_SKIP
fi
if ! grep -q "^CONFIG_UML_NET_VECTOR_V2=y" "$build_dir/.config"; then
	err "SKIP: UML_KERNEL was not built with CONFIG_UML_NET_VECTOR_V2=y"
	exit $KSFT_SKIP
fi

# 3. Confirm umlctl exists AND supports the audit flag.
if [ -z "$UMLCTL" ] || [ ! -x "$UMLCTL" ]; then
	err "SKIP: no umlctl binary (set UMLCTL=)"
	exit $KSFT_SKIP
fi
if ! "$UMLCTL" gate loop --help 2>&1 | grep -q "audit-vector-sandbox"; then
	err "SKIP: umlctl at $UMLCTL does not support --audit-vector-sandbox"
	exit $KSFT_SKIP
fi

# 4. Confirm sudo + tuntap capability.  The vector2 TAP path
# needs CAP_NET_ADMIN-equivalent privileges.  CI runners that
# don't have passwordless sudo + tuntap can't run this gate.
if ! sudo -n /usr/sbin/ip tuntap add mode tap dev v2-audit-probe 2>/dev/null; then
	err "SKIP: no sudo / tuntap access on this runner"
	exit $KSFT_SKIP
fi
sudo -n /usr/sbin/ip tuntap del mode tap dev v2-audit-probe 2>/dev/null || true

# 5. Write a minimal Umlfile for an auto-queue fd boot.
out_dir=$(mktemp -d)
trap "rm -rf $out_dir; sudo -n /usr/sbin/ip link delete v2-audit-tap 2>/dev/null || true" EXIT

cat > "$out_dir/v2-audit.toml" <<EOF
schema_version = 1
volumes = []

[instance]
name = "v2-audit"

[kernel]
path = "$UML_KERNEL"
backend = "seccomp"

[runtime]
mem = "512M"
ncpus = 1

[network]
mode = "tap"
driver = "vector2"
tap_name = "v2-audit-tap"
guest_ip = "192.168.42.2/30"
host_ip = "192.168.42.1/30"
gateway = "192.168.42.1"
nameservers = []
masquerade_via = "auto"
ports = []

[env]
TMPDIR = "/tmp"

[[init.phases]]
name = "smoke"
cmd = "/bin/true && echo SMOKE_OK && echo REPRO_DONE rc=0"
expect = ""
timeout_secs = 30
EOF

# 6. Run the audit gate.  umlctl's exit code is the signal:
# - exit 0 + "PASS=1/1" in stdout: audit gate ran clean, no forbidden ops.
# - non-zero or FAIL/TIMEOUT count: audit caught forbidden host op,
#   or boot failed for another reason.
echo "Running umlctl gate loop --audit-vector-sandbox W=1 M=1 ..."
loop_out="$out_dir/gate.out"
"$UMLCTL" gate loop \
	-f "$out_dir/v2-audit.toml" \
	--network-driver vector2 \
	--audit-vector-sandbox \
	-W 1 -M 1 \
	--timeout 120 \
	--out "$out_dir/loop" \
	2>&1 | tee "$loop_out"
umlctl_rc=${PIPESTATUS[0]}

if [ "$umlctl_rc" -ne 0 ]; then
	err "FAIL: umlctl gate loop exited rc=$umlctl_rc"
	for strace_log in "$out_dir"/loop/p0_default/w0/strace-audit-*.log; do
		[ -f "$strace_log" ] || continue
		err "--- audit strace log: $strace_log ---"
		tail -80 "$strace_log" >&2 || true
	done
	exit $KSFT_FAIL
fi

# Belt-and-suspenders: check stdout for the explicit PASS rate too.
if ! grep -q "PASS=1/1 FAIL=0 TIMEOUT=0" "$loop_out"; then
	err "FAIL: gate loop didn't report PASS=1/1 FAIL=0 TIMEOUT=0"
	tail -10 "$loop_out" >&2
	exit $KSFT_FAIL
fi

echo "PASS: vector2 audit-sandbox gate passed; no forbidden host operations detected"
exit $KSFT_PASS
