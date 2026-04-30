#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Multi-thread mmap stress reproducer for the H.1b residual.
#
# Boots UML and runs mt-mmap-stress with N pthreads each looping
# mmap+memset+munmap. Detects "memset corruption" — when a thread's
# private mmap'd page returns bytes from another thread or stale
# physmem after a memset. Under v2, this flakes ~50% with N=2 and
# ~100% with N>=3 because of a multi-thread mm-arbiter race that
# Phase H.1b investigation hasn't pinpointed yet (memo §H.1b
# residual, task #115).
#
# Under seccomp, 10/10 PASS — same code, same pthreads, no flake.
#
# Exits 0 on PASS, 1 on FAIL, 4 on SKIP.
#
# Usage:
#   UML_BINARY=$HOME/src/uml-builds/uml-clean/linux \
#     BACKEND=kvm-v2 NTHREADS=3 \
#     bash tools/testing/selftests/um/mt-mmap-stress/run-mt-mmap-stress.sh

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-clean/linux}
BACKEND=${BACKEND:-kvm-v2}
NTHREADS=${NTHREADS:-3}
TIMEOUT=${TIMEOUT:-30}

if [ ! -x "$BINARY" ]; then
    echo "SKIP: $BINARY not found" >&2
    exit 4
fi

DIR=$(cd "$(dirname "$0")" && pwd)
TEST_BIN="$DIR/mt-mmap-stress"
if [ ! -x "$TEST_BIN" ]; then
    if [ -f "$DIR/mt-mmap-stress.c" ]; then
        gcc -static -O0 -pthread -o "$TEST_BIN" "$DIR/mt-mmap-stress.c" \
            >/dev/null 2>&1 || {
            echo "SKIP: failed to build $TEST_BIN" >&2
            exit 4
        }
    else
        echo "SKIP: $TEST_BIN missing and no source" >&2
        exit 4
    fi
fi

INIT_SCRIPT=$(mktemp /tmp/mt-mmap-init.XXXXXX.sh)
trap 'rm -f "$INIT_SCRIPT"' EXIT
chmod +x "$INIT_SCRIPT"
cat > "$INIT_SCRIPT" <<EOSH
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
$TEST_BIN $NTHREADS
EOSH

OUTPUT=$(timeout --kill-after=5 "$TIMEOUT" "$BINARY" \
    backend=force="$BACKEND" mem=512M \
    rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 panic=-1 \
    init="$INIT_SCRIPT" </dev/null 2>&1 | tr -d '\r' || true)

if echo "$OUTPUT" | grep -q "ALL_OK n=$NTHREADS"; then
    echo "MT_MMAP_STRESS: backend=$BACKEND nthreads=$NTHREADS PASS"
    exit 0
fi

echo "MT_MMAP_STRESS: backend=$BACKEND nthreads=$NTHREADS FAIL"
echo "$OUTPUT" | grep -E "^T[0-9]+ iter|memset corruption" | head -10
exit 1
