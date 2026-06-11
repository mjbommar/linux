#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Validate the text dump from kvm_v2_state_trace_dump."""

import re
import sys


LINE_RE = re.compile(
    r"^seq=(?P<seq>[0-9]+) "
    r"ts_ns=(?P<ts>[0-9]+) "
    r"cpu=(?P<cpu>[0-9]+) "
    r"pid=(?P<pid>[0-9]+) "
    r"op=(?P<op>[a-z_]+) "
    r"exit_reason=(?P<exit>[0-9]+) "
    r"io_port=(?P<port>[0-9]+) "
    r"rip=(?P<rip>0x[0-9a-f]+|0) "
    r"sp=(?P<sp>0x[0-9a-f]+|0) "
    r"ax=(?P<ax>0x[0-9a-f]+|0)$"
)


def main(path: str) -> int:
    count = 0
    seen_enter = False
    seen_exit = False
    last_seq = -1

    with open(path, encoding="utf-8") as dump:
        for raw in dump:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            match = LINE_RE.match(line)
            if not match:
                print(f"KVM_STATE_TRACE_PARSE: FAIL bad line: {line}")
                return 1

            seq = int(match.group("seq"))
            if seq <= last_seq:
                print(
                    "KVM_STATE_TRACE_PARSE: FAIL non-increasing "
                    f"sequence {seq} after {last_seq}"
                )
                return 1
            last_seq = seq
            count += 1

            op = match.group("op")
            if op == "run_enter":
                seen_enter = True
            elif op == "run_exit":
                seen_exit = True
            elif op == "run_eintr":
                pass
            else:
                print(f"KVM_STATE_TRACE_PARSE: FAIL unknown op {op}")
                return 1

    if count <= 0:
        print("KVM_STATE_TRACE_PARSE: FAIL no entries")
        return 1
    if not seen_enter or not seen_exit:
        print(
            "KVM_STATE_TRACE_PARSE: FAIL missing enter/exit "
            f"enter={int(seen_enter)} exit={int(seen_exit)} count={count}"
        )
        return 1

    print(
        "KVM_STATE_TRACE_PARSE: PASS "
        f"entries={count} enter={int(seen_enter)} exit={int(seen_exit)}"
    )
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: parse-kvm-state-trace.py DUMP", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1]))
