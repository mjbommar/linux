#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Regression gate for fork-tree exit-code propagation.
#
# Boots UML, runs an init shell that fork()s the test binary which
# itself fork()s a child and waits for it. The shell then echoes
# its $? - which encodes the test process's exit code as the kernel
# reported it.
#
# Failure signature: shell sees rc=255 instead of 0 when the test's main
# returned 0. Init kernel-panic exitcode = 0x0000ff00 instead of 0.
# Use seccomp as the baseline when comparing backend behavior.
#
# Exits 0 on PASS, 4 on SKIP, 1 on FAIL - kselftest convention.

set -u

BINARY=${UML_BINARY:-$HOME/src/uml-builds/uml-clean/linux}
BACKEND=${BACKEND:-seccomp}
MEM=${UML_MEM:-256M}
TIMEOUT=${TIMEOUT:-30}

if [ ! -x "$BINARY" ]; then
    echo "SKIP: $BINARY not found (set UML_BINARY)" >&2
    exit 4
fi

DIR=$(cd "$(dirname "$0")" && pwd)
TEST_BIN="$DIR/fork_tree_3level"
if [ ! -x "$TEST_BIN" ]; then
    if [ -f "$DIR/fork_tree_3level.c" ]; then
        gcc -static -O0 -o "$TEST_BIN" "$DIR/fork_tree_3level.c" \
            >/dev/null 2>&1 || {
            echo "SKIP: failed to build $TEST_BIN" >&2
            exit 4
        }
    else
        echo "SKIP: $TEST_BIN missing and no source to build" >&2
        exit 4
    fi
fi

INIT_SCRIPT=$(mktemp /tmp/ft3l-init.XXXXXX.sh)
trap 'rm -f "$INIT_SCRIPT"' EXIT
chmod +x "$INIT_SCRIPT"
cat > "$INIT_SCRIPT" <<EOF
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
$TEST_BIN
RC=\$?
echo "FORK_TREE_3LEVEL: shell_rc=\$RC"
sync
EOF

OUTPUT=$(timeout --kill-after=5 "$TIMEOUT" "$BINARY" \
    backend=force="$BACKEND" mem="$MEM" \
    rootfstype=hostfs root=/dev/root rw \
    con=null con0=fd:0,fd:1 panic=-1 \
    init="$INIT_SCRIPT" </dev/null 2>&1 | tr -d '\r' || true)

# What we expect: test prints "FORK_TREE_3LEVEL: PASS ..." AND shell
# sees rc=0. Anything else is a fail.
if echo "$OUTPUT" | grep -qE '^FORK_TREE_3LEVEL: PASS' && \
   echo "$OUTPUT" | grep -qE '^FORK_TREE_3LEVEL: shell_rc=0'; then
    echo "FORK_TREE_3LEVEL: backend=$BACKEND PASS"
    exit 0
fi

# On FAIL, dump the relevant lines so the grep harness layer above
# us can capture both the "PASS"/"FAIL" trailer and the shell_rc.
echo "FORK_TREE_3LEVEL: backend=$BACKEND FAIL"
echo "$OUTPUT" | grep -E '^FORK_TREE_3LEVEL:|exitcode=' | head -10
exit 1
