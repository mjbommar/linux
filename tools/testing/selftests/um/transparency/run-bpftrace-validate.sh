#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Validate the `umlctl bpf` pre-canned scripts
# actually produce output against a live UML guest.
#
# The transparency.rs scripts were shipped without this check;
# this selftest closes that gap by booting a UML, capturing its
# PID, and running each script for 2 seconds via bpftrace -e.
#
# Exits 0 iff every script attaches its probes and produces at
# least one observable @-map entry (or, for net/io scripts that
# need active I/O, attaches probes without parse error; a clean
# "0 lines because idle UML" outcome is still considered PASS).

set -euo pipefail

KERNEL=${KERNEL:-$HOME/src/uml-builds/uml-smp-t41fix/linux}
OUT=${OUT:-$HOME/src/bpf-validate}
mkdir -p "$OUT"

which bpftrace >/dev/null 2>&1 || { echo "SKIP: bpftrace not installed"; exit 4; }
sudo -n true >/dev/null 2>&1 || { echo "SKIP: sudo required for bpftrace"; exit 4; }

# Boot a UML that idles for 30 s.
cat > "$OUT/init.sh" <<EOF
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
echo BPF_TEST_READY
sleep 30
poweroff -f
EOF
chmod +x "$OUT/init.sh"

timeout 35 "$KERNEL" mem=128M rootfstype=hostfs rootflags=/ root=/dev/root rw \
    backend=kvm-v2 ncpus=1 init="$OUT/init.sh" \
    > "$OUT/boot.log" 2>&1 &
UML_PID=$!
cleanup() { kill "$UML_PID" 2>/dev/null || true; wait 2>/dev/null; }
trap cleanup EXIT

for i in $(seq 1 30); do
    if grep -q BPF_TEST_READY "$OUT/boot.log" 2>/dev/null; then break; fi
    sleep 0.3
done
grep -q BPF_TEST_READY "$OUT/boot.log" || {
    echo "FAIL: UML did not reach BPF_TEST_READY"
    tail -5 "$OUT/boot.log"
    exit 1
}

echo "UML host PID=$UML_PID"
echo

run_one() {
    local label=$1 prog=$2 expect_data=$3
    local subst
    subst=$(echo "$prog" | sed "s/__PID__/$UML_PID/g")
    timeout 2 sudo -n bpftrace -e "$subst" >"$OUT/$label.out" 2>&1 || true
    local attached
    attached=$(grep -c "^Attached " "$OUT/$label.out" || true)
    local data
    data=$(grep -cE "^@" "$OUT/$label.out" || true)
    printf "  %-12s attached=%s data_lines=%s" "$label" "$attached" "$data"
    if [ "$attached" -eq 0 ]; then
        echo "  -> FAIL (no probes attached - script parse error)"
        return 1
    fi
    if [ "$expect_data" = "required" ] && [ "$data" -eq 0 ]; then
        echo "  -> FAIL (expected data, got none)"
        return 1
    fi
    echo "  -> PASS"
    return 0
}

# Inline copies of the scripts from transparency.rs; kept in sync
# manually (any change there should land here too).  Acceptance:
# syscalls + sched produce data on idle UML; the others attach
# cleanly and only need an active workload to produce output.
SCRIPT_SYSCALLS='
tracepoint:raw_syscalls:sys_enter
/pid == __PID__/
{
    @[args->id] = count();
}
interval:s:1
{
    print(@);
    clear(@);
}
'

SCRIPT_PAGEFAULTS='
tracepoint:exceptions:page_fault_user
/pid == __PID__/
{
    @addrs[args->address] = count();
}
interval:s:5
{
    print(@addrs, 10);
    clear(@addrs);
}
'

SCRIPT_IO='
tracepoint:block:block_rq_issue
/pid == __PID__/
{
    @start[args->dev, args->sector] = nsecs;
}
tracepoint:block:block_rq_complete
/@start[args->dev, args->sector]/
{
    @latency_us = hist((nsecs - @start[args->dev, args->sector]) / 1000);
    delete(@start[args->dev, args->sector]);
}
interval:s:5
{
    print(@latency_us);
    clear(@latency_us);
}
'

SCRIPT_NET='
tracepoint:syscalls:sys_enter_writev
/pid == __PID__/
{
    @tx_bytes = sum(args->vlen);
}
tracepoint:syscalls:sys_enter_recvfrom
/pid == __PID__/
{
    @rx_bytes = sum(args->size);
}
interval:s:1
{
    printf("tx %llu B/s    rx %llu B/s\n", (uint64)@tx_bytes, (uint64)@rx_bytes);
    clear(@tx_bytes);
    clear(@rx_bytes);
}
'

SCRIPT_SCHED='
tracepoint:sched:sched_switch
/args->prev_pid == __PID__ || args->next_pid == __PID__/
{
    @[args->prev_comm, args->next_comm] = count();
}
interval:s:5
{
    print(@);
    clear(@);
}
'

run_one syscalls   "$SCRIPT_SYSCALLS"   required
run_one pagefaults "$SCRIPT_PAGEFAULTS" optional
run_one io         "$SCRIPT_IO"         optional
run_one net        "$SCRIPT_NET"        optional
run_one sched      "$SCRIPT_SCHED"      required

echo
echo "VERDICT: all five bpftrace scripts attached cleanly; syscalls + sched produce data on idle UML"
