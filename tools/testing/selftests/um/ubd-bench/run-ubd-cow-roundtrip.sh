#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# HONEST-AUDIT §12: UBD Phase 5 (COW bitmap drain) round-trip test.
#
# Phase 5 ships code that updates the COW bitmap via io_uring when
# the substrate is available; the existing soak workloads
# (django/fastapi) are non-COW so the path was never exercised
# under load.  This test:
#
#   1. Creates a 64 MiB ext4 backing file with known content.
#   2. Launches UML with a COW image on top, has the guest write
#      data that overlaps the backing-file content (forces
#      copy-up = bitmap update).
#   3. Verifies the guest reads back the new data correctly.
#   4. Verifies the backing file is UNCHANGED (COW invariant).
#   5. Repeats under um_ubd_no_uring=1 (legacy bitmap pwrite)
#      and the default (io_uring bitmap drain).
#
# Both paths must:
#   * pass the round-trip (data integrity)
#   * leave the backing file byte-identical
#
# Exits 0 on success, non-zero on any failure.

set -euo pipefail

KERNEL=${KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
OUT=${OUT:-$HOME/src/ubd-cow-roundtrip}
mkdir -p "$OUT"

# Step 1: build a backing file with known content + ext4.
BACKING=$OUT/backing.img
qemu-img create -f raw "$BACKING" 64M >/dev/null 2>&1
mkfs.ext4 -F -q "$BACKING"

# Pre-populate the backing file with a sentinel via loop mount.  Skipped
# here to keep the test root-free; the bitmap-update path is exercised
# regardless because any write touches at least one COW segment.

# Step 2: in-guest init script — writes data, verifies, unmounts.
GUEST_MNT=$HOME/src/ubd-cow-mnt
mkdir -p "$GUEST_MNT"

cat > "$OUT/init.sh" <<INIT
#!/bin/sh
set -e
mount -t proc proc /proc 2>/dev/null

dmesg | grep -E "ubd:" | head -3

mount -t ext4 /dev/ubda __GUEST_MNT__ && echo "[ok] mount ubda"
# Write data that will require copy-up on the COW layer.
dd if=/dev/urandom of=__GUEST_MNT__/payload bs=4k count=512 conv=fsync 2>&1 | tail -1
PRE_MD5=\$(md5sum __GUEST_MNT__/payload | awk '{print \$1}')
echo "COW_TEST_PRE_MD5=\$PRE_MD5"

# Unmount, remount, reverify (forces re-read from disk; tests that
# bitmap was correctly updated so the read goes to COW not backing).
umount __GUEST_MNT__
mount -t ext4 /dev/ubda __GUEST_MNT__
POST_MD5=\$(md5sum __GUEST_MNT__/payload | awk '{print \$1}')
echo "COW_TEST_POST_MD5=\$POST_MD5"
umount __GUEST_MNT__

if [ "\$PRE_MD5" = "\$POST_MD5" ]; then
    echo "COW_TEST_VERDICT=PASS"
else
    echo "COW_TEST_VERDICT=FAIL"
fi
poweroff -f
INIT
sed -i "s|__GUEST_MNT__|$GUEST_MNT|g" "$OUT/init.sh"
chmod +x "$OUT/init.sh"

run_one() {
    local label=$1 extra=$2 cow_img=$3
    local out=$OUT/$label.log
    # Use a fresh COW image per run (UBD creates it on first open).
    rm -f "$cow_img"
    # Snapshot the backing file's sha256 BEFORE the run.
    local pre_sha=$(sha256sum "$BACKING" | awk '{print $1}')

    timeout 60 "$KERNEL" \
        mem=512M rootfstype=hostfs rootflags=/ root=/dev/root rw \
        backend=kvm-v2 ncpus=2 \
        $extra \
        ubd0="$cow_img:$BACKING" \
        init="$OUT/init.sh" >"$out" 2>&1 || true

    local post_sha=$(sha256sum "$BACKING" | awk '{print $1}')
    if [ "$pre_sha" != "$post_sha" ]; then
        echo "  $label: FAIL — backing file mutated (COW invariant broken)"
        echo "     pre=$pre_sha"
        echo "     post=$post_sha"
        return 1
    fi
    local verdict=$(grep "^COW_TEST_VERDICT=" "$out" | sed 's/.*=//' | tr -d '\r')
    local pre=$(grep "^COW_TEST_PRE_MD5=" "$out" | sed 's/.*=//' | tr -d '\r')
    local post=$(grep "^COW_TEST_POST_MD5=" "$out" | sed 's/.*=//' | tr -d '\r')
    if [ "$verdict" != "PASS" ] || [ -z "$pre" ] || [ "$pre" != "$post" ]; then
        echo "  $label: FAIL — round-trip md5 mismatch or no verdict (verdict=$verdict pre=$pre post=$post)"
        return 1
    fi
    echo "  $label: PASS  (backing immutable; md5=$pre round-trips)"
    return 0
}

echo "=== legacy path (um_ubd_no_uring=1) ==="
run_one legacy "um_ubd_no_uring=1" "$OUT/legacy.cow"

echo "=== io_uring path (default; Phase 5 bitmap drain) ==="
run_one io_uring "" "$OUT/io_uring.cow"

echo
echo "VERDICT: all paths PASSED the COW round-trip + backing-immutable invariant"
