#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# um/umlbuild - end-to-end gate for the umlbuild mvp profile.
#
# What this proves:
#   1. `umlbuild instance --profile mvp` builds without error
#   2. the emitted kernel binary boots from the emitted ubd image
#   3. the rootfs's /sbin/init runs the baked /etc/sandbox.cmd
#   4. python3 inside the guest computes the canonical sha256 of "hello"
#
# Exit codes (kselftest convention):
#   0 PASS
#   1 FAIL
#   4 SKIP - required tooling missing on the host
#
# The host needs: cargo, gcc, make, curl, tar, e2fsprogs >= 1.43,
# and one of {apk, bubblewrap, unshare}.

set -u

WORK=$(mktemp -d -t umlbuild-mvp.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# Locate the source tree (assume we're invoked from anywhere within it).
SRC=$(cd "$(dirname "$0")" && cd ../../../../.. && pwd)
if [ ! -f "$SRC/arch/um/Kconfig" ]; then
    echo "SKIP: cannot locate kernel source root from $0" >&2
    exit 4
fi

UMLBUILD=$SRC/tools/uml/uml-launcher/target/release/umlbuild
UMLCTL=$SRC/tools/uml/uml-launcher/target/release/umlctl
if [ ! -x "$UMLBUILD" ]; then
    UMLBUILD=$SRC/tools/uml/uml-launcher/target/debug/umlbuild
    UMLCTL=$SRC/tools/uml/uml-launcher/target/debug/umlctl
fi
if [ ! -x "$UMLBUILD" ] || [ ! -x "$UMLCTL" ]; then
    echo "SKIP: umlbuild/umlctl not built; run \`make -C tools/uml/uml-launcher\`" >&2
    exit 4
fi

EXPECTED_SHA256=2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824

echo "== umlbuild instance --profile mvp =="
if ! "$UMLBUILD" instance --profile mvp --out "$WORK/inst" --source "$SRC" \
        > "$WORK/build.log" 2>&1; then
    echo "FAIL: umlbuild instance failed; see $WORK/build.log"
    tail -20 "$WORK/build.log"
    cp "$WORK/build.log" /tmp/umlbuild-mvp-build.log 2>/dev/null || true
    exit 1
fi

if [ ! -f "$WORK/inst/linux" ] || [ ! -f "$WORK/inst/rootfs.img" ] \
        || [ ! -f "$WORK/inst/Umlfile.toml" ]; then
    echo "FAIL: umlbuild instance did not emit expected artifacts"
    ls -la "$WORK/inst"
    exit 1
fi

echo "  kernel:  $(wc -c < "$WORK/inst/linux") bytes"
echo "  image:   $(wc -c < "$WORK/inst/rootfs.img") bytes"

echo "== direct kernel boot =="
# Boot the kernel directly first (bypasses umlctl); proves the
# kernel + ubd image work standalone.
timeout 60 "$WORK/inst/linux" \
    mem=256M \
    "ubd0=$WORK/inst/rootfs.img" \
    root=/dev/ubda rw \
    con=null,fd:1 > "$WORK/direct.log" 2>&1
DIRECT_RC=$?
# Kernel panics on poweroff with rc != 0 sometimes; tolerate that
# (timeout returns 124 if it had to kill).
if [ "$DIRECT_RC" -ne 0 ] && [ "$DIRECT_RC" -ne 124 ] && [ "$DIRECT_RC" -ne 134 ]; then
    echo "FAIL: direct boot exited with $DIRECT_RC; see $WORK/direct.log"
    grep -v "^um: DIAG\|^UMPTFREE" "$WORK/direct.log" | tail -20
    exit 1
fi

if ! grep -q "$EXPECTED_SHA256" "$WORK/direct.log"; then
    echo "FAIL: expected sha256 $EXPECTED_SHA256 not found in direct boot stdout"
    grep "umlbuild-init\|mvp:" "$WORK/direct.log" | tail -10
    exit 1
fi
echo "  direct boot: PASS"

# Best-effort cleanup of any UML processes the direct boot left behind.
pkill -9 -f "$WORK/inst/linux" 2>/dev/null || true

echo "== umlctl up =="
INSTANCE=umlbuild-mvp-test-$$
"$UMLCTL" rm "$INSTANCE" 2>/dev/null || true

# Substitute instance name into the emitted Umlfile so concurrent
# kselftest runs don't collide.
sed -i "s/^name = .*$/name = \"$INSTANCE\"/" "$WORK/inst/Umlfile.toml"

timeout 60 "$UMLCTL" up -f "$WORK/inst/Umlfile.toml" > "$WORK/umlctl.log" 2>&1
UP_RC=$?
sleep 6
"$UMLCTL" logs "$INSTANCE" > "$WORK/guest.log" 2>&1 || true
"$UMLCTL" stop "$INSTANCE" >/dev/null 2>&1 || true
"$UMLCTL" rm   "$INSTANCE" >/dev/null 2>&1 || true
pkill -9 -f "$WORK/inst/linux" 2>/dev/null || true

if ! grep -q "$EXPECTED_SHA256" "$WORK/guest.log"; then
    echo "FAIL: umlctl up path did not produce the expected sha256"
    echo "--- guest.log tail ---"
    tail -20 "$WORK/guest.log"
    cp "$WORK/guest.log" /tmp/umlbuild-mvp-umlctl.log 2>/dev/null || true
    exit 1
fi

echo "  umlctl up: PASS"
echo
echo "umlbuild-mvp: PASS"
exit 0
