#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Class B process model reproducers. Each binary emits exactly one
# REPRO: <name> (PASS|FAIL|EXPECTED_FAIL) line on stdout.
DIR="$(dirname "$0")"
"$DIR/fork_exec_wait" 2>&1
"$DIR/fork_pipe_ipc" 2>&1
"$DIR/pool_workers" 2>&1
"$DIR/waitpid_wnohang" 2>&1
"$DIR/sigchld_select" 2>&1
"$DIR/asyncio_subprocess_min" 2>&1
