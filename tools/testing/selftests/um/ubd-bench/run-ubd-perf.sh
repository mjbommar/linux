#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Compare the legacy `do_io` synchronous helper-thread path with the
# io_uring path under fio and parallel-dd workloads.  The same kernel
# binary runs both sides, flipped by the `um_ubd_no_uring=1` cmdline knob.
#
# Usage:
#   tools/testing/selftests/um/ubd-bench/run-ubd-perf.sh \
#       [--kernel PATH] [--out DIR]
#
# Outputs:  $OUT/verdict.txt with a side-by-side table.

set -euo pipefail

KERNEL=${KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
OUT=${OUT:-$HOME/src/ubd-perf-bench}

while [ $# -gt 0 ]; do
    case "$1" in
        --kernel) KERNEL="$2"; shift 2 ;;
        --out)    OUT="$2";    shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
which fio >/dev/null || { echo "error: fio not in PATH"; exit 1; }

# Guest will use this for the ext4 mount point (hostfs root=/, so the
# mount happens at the host path).  Must be writable by $USER, since
# guest UML inherits host UID 0 mapping but file ops translate to
# host UID/GID.
GUEST_MNT=$HOME/src/ubd-perf-mnt
mkdir -p "$GUEST_MNT"

# ----- single-image, two-run helper -----------------------------

setup_image() {
    local img=$1
    rm -f "$img"
    qemu-img create -f raw "$img" 256M >/dev/null 2>&1
    mkfs.ext4 -F -q "$img"
}

run_uml() {
    local label=$1     # "legacy" or "io_uring"
    local extra_cmd=$2 # e.g. "um_ubd_no_uring=1" or ""
    local img=$3
    local init=$4
    local out=$OUT/boot-$label.log

	# ubd0D=... opens the backing file with O_DIRECT, bypassing
	# the host page cache so the queue-depth difference between
	# the io_uring path and the legacy depth-1 helper actually
	# shows up in measured throughput.
    timeout 240 "$KERNEL" \
        mem=1024M rootfstype=hostfs rootflags=/ root=/dev/root rw \
        backend=kvm-v2 ncpus=4 \
        ubd0D="$img" \
        $extra_cmd \
        init="$init" >"$out" 2>&1 || true

    # Echo throughput markers back to the caller.
    grep -E "BENCH_|fio: |IOPS=|BW=|WRITE:|READ:|read: IOPS|write: IOPS|Run status" "$out" || true
}

# Init script runs inside the guest.  Mounts /dev/ubda, runs fio
# + parallel-dd, prints results.
write_init() {
    local init=$1
    cat > "$init" <<'INIT'
#!/bin/sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
dmesg | grep -E "ubd:" | head -3

MNT=__GUEST_MNT__
mount -t ext4 /dev/ubda $MNT || { echo "BENCH_FAIL mount"; exit 1; }

# Workload A: parallel-dd, 4 streams, 64 MiB each, fsync at end.
echo "BENCH_BEGIN parallel-dd"
T0=$(date +%s.%N)
(dd if=/dev/urandom of=$MNT/a bs=4M count=16 conv=fsync 2>/dev/null) &
(dd if=/dev/urandom of=$MNT/b bs=4M count=16 conv=fsync 2>/dev/null) &
(dd if=/dev/urandom of=$MNT/c bs=4M count=16 conv=fsync 2>/dev/null) &
(dd if=/dev/urandom of=$MNT/d bs=4M count=16 conv=fsync 2>/dev/null) &
wait
sync
T1=$(date +%s.%N)
DT=$(awk "BEGIN{print $T1 - $T0}")
BYTES=$((4 * 16 * 4 * 1024 * 1024))
MBPS=$(awk "BEGIN{printf \"%.1f\", $BYTES * 8 / $DT / 1e6}")
MBS=$(awk "BEGIN{printf \"%.1f\", $BYTES / $DT / 1e6}")
echo "BENCH_RESULT workload=parallel-dd bytes=$BYTES dt=$DT mbps=$MBPS mbs=$MBS"
rm -f $MNT/a $MNT/b $MNT/c $MNT/d
sync

# Workload B: fio randwrite via 8 parallel sync writers against
# /dev/ubda raw.  Guest UML has no io_uring/libaio host syscalls,
# so ioengine=psync + numjobs=8 exercises queue depth.  Each job is
# a single-threaded sync writer; together they put up to 8 concurrent
# UBD requests in flight, which is what UBD_REQ_BUFFER_SIZE/sizeof(*req)
# caps the io_thread batch at.
echo "BENCH_BEGIN fio-randwrite-8jobs"
umount $MNT
fio --rw=randwrite --bs=4k \
    --filename=/dev/ubda --direct=0 --ioengine=psync \
    --numjobs=8 --group_reporting --thread \
    --runtime=15 --time_based --name=ubd-randwrite \
    --output-format=normal 2>&1 |
    tail -25
echo "BENCH_DONE"

# Workload C: multi-file fio variant.  This spreads I/O across 8
# separate files on the in-guest ext4 fs.  If io_uring wins here but
# loses on Workload B, single-file host locking is likely the bottleneck.
# If io_uring loses on both, the io_uring path has per-request overhead
# unrelated to file-level contention.
echo "BENCH_BEGIN fio-randwrite-multifile"
mount -t ext4 /dev/ubda $MNT 2>/dev/null
mkdir -p $MNT/fio-jobs
fio --rw=randwrite --bs=4k \
    --directory=$MNT/fio-jobs \
    --filename_format='job.\$jobnum.dat' \
    --nrfiles=1 --size=8M \
    --direct=0 --ioengine=psync \
    --numjobs=8 --group_reporting --thread \
    --runtime=15 --time_based --name=ubd-multifile \
    --output-format=normal 2>&1 |
    tail -25
rm -rf $MNT/fio-jobs
umount $MNT 2>/dev/null
echo "BENCH_DONE"

poweroff -f
INIT
    # Substitute the host-resolved mount path into the script.
    sed -i "s|__GUEST_MNT__|$GUEST_MNT|g" "$init"
    chmod +x "$init"
}

# ----- run both sides -------------------------------------------

IMG=$OUT/ubd.img
INIT=$OUT/init.sh
setup_image "$IMG"
write_init "$INIT"

echo "=== run 1: legacy do_io (um_ubd_no_uring=1) ==="
run_uml legacy "um_ubd_no_uring=1" "$IMG" "$INIT" | tee "$OUT/legacy.summary"

echo
setup_image "$IMG"     # fresh image to avoid carry-over effects
echo "=== run 2: io_uring do_io_ring_batch (default) ==="
run_uml io_uring "" "$IMG" "$INIT" | tee "$OUT/io_uring.summary"

# ----- verdict --------------------------------------------------

extract_mbps() {
    grep "workload=parallel-dd" "$1" 2>/dev/null | head -1 | \
        sed -E 's/.*mbs=([0-9.]+).*/\1/'
}
extract_iops() {
    # fio "WRITE: bw=A.BMiB/s (X), IOPS=N.Nk, ..." line
    grep -E "^\s+WRITE: " "$1" 2>/dev/null | head -1 | \
        sed -E 's/.*IOPS=([0-9.]+k?).*/\1/'
}
extract_bw() {
    grep -E "^\s+WRITE: " "$1" 2>/dev/null | head -1 | \
        sed -E 's/.*bw=([0-9.]+[KMG]iB\/s).*/\1/'
}

LEG_DD=$(extract_mbps "$OUT/legacy.summary"   || echo "n/a")
RING_DD=$(extract_mbps "$OUT/io_uring.summary" || echo "n/a")
LEG_FIO=$(extract_iops "$OUT/legacy.summary"   || echo "n/a")
RING_FIO=$(extract_iops "$OUT/io_uring.summary" || echo "n/a")
LEG_BW=$(extract_bw   "$OUT/legacy.summary"   || echo "n/a")
RING_BW=$(extract_bw   "$OUT/io_uring.summary" || echo "n/a")

cat > "$OUT/verdict.txt" <<EOF
UBD io_uring performance comparison

Kernel:  $KERNEL

| workload                          | legacy     | io_uring   |
|-----------------------------------|------------|------------|
| parallel-dd 4x 64 MiB (MB/s)      | $LEG_DD    | $RING_DD   |
| fio randwrite 8 jobs IOPS         | $LEG_FIO   | $RING_FIO  |
| fio randwrite 8 jobs aggregate BW | $LEG_BW    | $RING_BW   |

Full per-side logs in $OUT/{legacy,io_uring}.summary and
$OUT/boot-{legacy,io_uring}.log.
EOF

cat "$OUT/verdict.txt"
