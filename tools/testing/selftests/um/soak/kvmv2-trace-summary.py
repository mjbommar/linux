#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Summarize UML KVM-v2 state-trace dumps from copied run logs.

The KVM-v2 state trace prints each entry as tagged KVMV2T-* lines so
kernel printk interleaving cannot corrupt a whole record.  This helper
reassembles those sections by (cpu, seq), emits a compact text summary,
and also reports adjacent non-dump diagnostics such as TLB-lag and
Python/server failure markers.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import sys
from pathlib import Path
from typing import Any


KV_RE = re.compile(r"([A-Za-z0-9_]+)=((?:\[[^\]]*\])|(?:\S+))")
BEGIN_RE = re.compile(r"KVMV2T_DUMP_BEGIN\s+reason=(\S+)\s+entries=(\d+)")
END_RE = re.compile(r"KVMV2T_DUMP_END\s+entries=(\d+)")
LINE_RE = re.compile(r"KVMV2T-([HRSTFMI])\s+(.*)")
TLB_RE = re.compile(
    r"KVM_V2_TLB_LAG\s+cpu=(\d+)\s+pid=(\d+)\s+mm=(\S+)\s+"
    r"last=(\d+)\s+cur=(\d+)\s+lag=(\d+)"
)

SECTION_NAMES = {
    "H": "header",
    "R": "regs",
    "S": "sregs",
    "T": "task",
    "F": "flags",
    "M": "mmvcpu",
    "I": "ist_live",
}


def parse_scalar(value: str) -> Any:
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1]
        if not inner:
            return []
        return [parse_scalar(part) for part in inner.split(",")]
    try:
        return int(value, 0)
    except ValueError:
        return value


def parse_kv(rest: str) -> dict[str, Any]:
    return {key: parse_scalar(value) for key, value in KV_RE.findall(rest)}


def find_kvmv2_payload(line: str) -> str | None:
    idx = line.find("KVMV2T")
    if idx < 0:
        return None
    return line[idx:].strip()


def parse_file(path: Path) -> dict[str, Any]:
    entries: dict[tuple[int, int], dict[str, dict[str, Any]]] = {}
    begins: list[dict[str, Any]] = []
    ends: list[int] = []
    tlb_lags: list[dict[str, Any]] = []
    markers = collections.Counter()
    fatal_context: list[str] = []
    capture_fatal_context = 0
    # Round 2 (2026-05-17, Django investigation): capture the trace
    # auto-freeze anomaly lines emitted by state_trace.c. The
    # fatal-signal (HANDLE_SYSCALL_PRE tgkill+SIGABRT/SIGSEGV) and
    # fatal-segv (post-segv_handler kernel-delivered SIGSEGV/SIGBUS)
    # triggers both write a single dmesg line of the form
    #   `KVMV2T_ANOMALY <reason> ...`
    # that pinpoints the moment the trace ring was frozen.
    anomalies: list[str] = []

    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            raw = line.rstrip("\n")

            if "SERVER_READY" in raw:
                markers["server_ready"] += 1
            if "SERVER_FAIL" in raw:
                markers["server_fail"] += 1
            if "TIER3_OK" in raw:
                markers["tier3_ok"] += 1
            if "Fatal Python error:" in raw:
                markers["fatal_python"] += 1
                capture_fatal_context = 24
            if "Segmentation fault" in raw:
                markers["python_segv"] += 1
            if "Aborted" in raw and "python3" in raw:
                markers["python_abort"] += 1
            if "KVM_V2_TRACE_DUMP_BEGIN" in raw:
                markers["trace_dump_begin"] += 1
            if "KVM_V2_TRACE_DUMP_END" in raw:
                markers["trace_dump_end"] += 1
            if "KVMV2T_ANOMALY" in raw:
                markers["trace_anomaly"] += 1
                anomalies.append(raw)

            if capture_fatal_context > 0:
                fatal_context.append(raw)
                capture_fatal_context -= 1

            m = TLB_RE.search(raw)
            if m:
                tlb_lags.append(
                    {
                        "cpu": int(m.group(1)),
                        "pid": int(m.group(2)),
                        "mm": m.group(3),
                        "last": int(m.group(4)),
                        "cur": int(m.group(5)),
                        "lag": int(m.group(6)),
                    }
                )

            payload = find_kvmv2_payload(raw)
            if payload is None:
                continue

            m = BEGIN_RE.search(payload)
            if m:
                begins.append({"reason": m.group(1), "entries": int(m.group(2))})
                continue
            m = END_RE.search(payload)
            if m:
                ends.append(int(m.group(1)))
                continue

            m = LINE_RE.search(payload)
            if not m:
                continue
            section = m.group(1)
            data = parse_kv(m.group(2))
            cpu = int(data.get("cpu", -1))
            seq = int(data.get("seq", -1))
            if cpu < 0 or seq < 0:
                continue
            entries.setdefault((cpu, seq), {})[section] = data

    flattened = []
    for (cpu, seq), sections in entries.items():
        row: dict[str, Any] = {"cpu": cpu, "seq": seq, "sections": sorted(sections)}
        for section, data in sections.items():
            prefix = SECTION_NAMES[section]
            for key, value in data.items():
                if key in ("cpu", "seq"):
                    continue
                row[f"{prefix}.{key}"] = value
        flattened.append(row)
    flattened.sort(key=lambda row: (row.get("header.ts", -1), row["cpu"], row["seq"]))

    op_counts = collections.Counter(
        row.get("header.op", "MISSING") for row in flattened
    )
    pid_counts = collections.Counter(row.get("header.pid", "MISSING") for row in flattened)
    port_counts = collections.Counter(row.get("header.port", "MISSING") for row in flattened)
    exit_counts = collections.Counter(row.get("header.exit", "MISSING") for row in flattened)

    complete_sections = set(SECTION_NAMES)
    complete_entries = sum(1 for row in flattened if set(row["sections"]) == complete_sections)
    incomplete_entries = [
        row for row in flattened if set(row["sections"]) != complete_sections
    ][:10]

    max_mm_lag = None
    for row in flattened:
        mmgen = row.get("mmvcpu.mmgen")
        vlast = row.get("mmvcpu.vlast")
        if (
            isinstance(mmgen, int)
            and isinstance(vlast, int)
            and vlast > 0
            and mmgen >= vlast
        ):
            lag = mmgen - vlast
            if max_mm_lag is None or lag > max_mm_lag["lag"]:
                max_mm_lag = {
                    "lag": lag,
                    "cpu": row["cpu"],
                    "seq": row["seq"],
                    "pid": row.get("header.pid"),
                    "op": row.get("header.op"),
                    "mmgen": mmgen,
                    "vlast": vlast,
                }

    max_tlb_lag = max((item["lag"] for item in tlb_lags), default=0)
    tlb_by_pid = collections.Counter(item["pid"] for item in tlb_lags)
    dispatch_switches = summarize_dispatch_switches(flattened)
    syscall_switches = summarize_syscall_switches(flattened)
    post_syscall_mismatches = summarize_post_syscall_mismatches(flattened)
    mm_backsteps = summarize_mm_backsteps(flattened)
    regs_owner_mismatches = summarize_regs_owner_mismatches(flattened)
    has_regs_owner = any("task.rmatch" in row for row in flattened)

    return {
        "path": str(path),
        "markers": dict(markers),
        "fatal_context": fatal_context,
        "anomalies": anomalies,
        "dumps": begins,
        "dump_ends": ends,
        "entries": len(flattened),
        "complete_entries": complete_entries,
        "incomplete_entries": incomplete_entries,
        "seq_min": min((row["seq"] for row in flattened), default=None),
        "seq_max": max((row["seq"] for row in flattened), default=None),
        "op_counts": dict(op_counts),
        "pid_counts": dict(pid_counts),
        "port_counts": dict(port_counts),
        "exit_counts": dict(exit_counts),
        "max_mm_lag": max_mm_lag,
        "tlb_lag_count": len(tlb_lags),
        "tlb_lag_max": max_tlb_lag,
        "tlb_lag_by_pid": dict(tlb_by_pid),
        "dispatch_switches": dispatch_switches,
        "syscall_switches": syscall_switches,
        "post_syscall_mismatches": post_syscall_mismatches,
        "mm_backsteps": mm_backsteps,
        "has_regs_owner": has_regs_owner,
        "regs_owner_mismatches": regs_owner_mismatches,
        "last_entries": flattened[-12:],
    }


def fmt_hex(value: Any) -> str:
    if isinstance(value, int):
        return hex(value)
    return str(value)


def numeric_value(value: Any) -> int | None:
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        return None
    try:
        return int(value, 0)
    except ValueError:
        try:
            return int(value, 16)
        except ValueError:
            return None


def summarize_dispatch_switches(flattened: list[dict[str, Any]]) -> list[dict[str, Any]]:
    active: dict[int, dict[str, Any]] = {}
    switches: list[dict[str, Any]] = []

    for row in flattened:
        op = row.get("header.op")
        cpu = row["cpu"]
        if op == "VCPU_RUN_ENTRY":
            active[cpu] = row
            continue
        if op != "VCPU_RUN_EXIT":
            continue

        entry = active.pop(cpu, None)
        if entry is None:
            continue

        entry_pid = entry.get("header.pid")
        exit_pid = row.get("header.pid")
        entry_tmm = entry.get("task.tmm")
        exit_tmm = row.get("task.tmm")
        if entry_pid != exit_pid or entry_tmm != exit_tmm:
            switches.append(
                {
                    "cpu": cpu,
                    "entry_seq": entry["seq"],
                    "exit_seq": row["seq"],
                    "entry_pid": entry_pid,
                    "exit_pid": exit_pid,
                    "entry_tmm": entry_tmm,
                    "exit_tmm": exit_tmm,
                    "exit_op": op,
                }
            )

    return switches


def summarize_post_syscall_mismatches(
    flattened: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    mismatches: list[dict[str, Any]] = []

    for row in flattened:
        if row.get("header.op") != "HANDLE_SYSCALL_POST":
            continue

        # Fixed kernels intentionally trace HANDLE_SYSCALL_POST without
        # the shared kvm_run payload after handle_syscall() returns.  Only
        # old-style rows with the syscall IO port still attached can prove
        # a stale run-vs-task mismatch.
        port = numeric_value(row.get("header.port"))
        if port != 0xF4:
            continue

        run_rax = numeric_value(row.get("regs.rax"))
        task_horax = numeric_value(row.get("task.horax"))
        if run_rax is None or task_horax is None or run_rax == task_horax:
            continue

        mismatches.append(
            {
                "cpu": row["cpu"],
                "seq": row["seq"],
                "pid": row.get("header.pid"),
                "port": row.get("header.port"),
                "run_rax": row.get("regs.rax"),
                "task_horax": row.get("task.horax"),
                "task_hax": row.get("task.hax"),
                "task_tmm": row.get("task.tmm"),
                "vcpu_mm": row.get("mmvcpu.vmm"),
            }
        )

    return mismatches


def summarize_syscall_switches(flattened: list[dict[str, Any]]) -> list[dict[str, Any]]:
    active: dict[int, dict[str, Any]] = {}
    switches: list[dict[str, Any]] = []

    for row in flattened:
        op = row.get("header.op")
        cpu = row["cpu"]
        if op == "HANDLE_SYSCALL_PRE":
            active[cpu] = row
            continue
        if op != "HANDLE_SYSCALL_POST":
            continue

        entry = active.pop(cpu, None)
        if entry is None:
            continue

        entry_pid = entry.get("header.pid")
        post_pid = row.get("header.pid")
        entry_tmm = entry.get("task.tmm")
        post_tmm = row.get("task.tmm")
        if entry_pid != post_pid or entry_tmm != post_tmm:
            switches.append(
                {
                    "cpu": cpu,
                    "entry_seq": entry["seq"],
                    "post_seq": row["seq"],
                    "entry_pid": entry_pid,
                    "post_pid": post_pid,
                    "entry_tmm": entry_tmm,
                    "post_tmm": post_tmm,
                    "entry_syscall": entry.get("regs.rax"),
                    "post_horax": row.get("task.horax"),
                    "post_hax": row.get("task.hax"),
                }
            )

    return switches


def summarize_mm_backsteps(flattened: list[dict[str, Any]]) -> list[dict[str, Any]]:
    backsteps: list[dict[str, Any]] = []

    for row in flattened:
        mmgen = numeric_value(row.get("mmvcpu.mmgen"))
        vlast = numeric_value(row.get("mmvcpu.vlast"))
        if mmgen is None or vlast is None or vlast <= 0 or mmgen >= vlast:
            continue

        backsteps.append(
            {
                "cpu": row["cpu"],
                "seq": row["seq"],
                "pid": row.get("header.pid"),
                "op": row.get("header.op"),
                "mmgen": mmgen,
                "vlast": vlast,
                "task_tmm": row.get("task.tmm"),
                "vcpu_mm": row.get("mmvcpu.vmm"),
            }
        )

    return backsteps


def summarize_regs_owner_mismatches(
    flattened: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    mismatches: list[dict[str, Any]] = []

    for row in flattened:
        rmatch = numeric_value(row.get("task.rmatch"))
        rptr = numeric_value(row.get("task.rptr"))
        if rmatch is None or rmatch != 0 or not rptr:
            continue

        mismatches.append(
            {
                "cpu": row["cpu"],
                "seq": row["seq"],
                "pid": row.get("header.pid"),
                "op": row.get("header.op"),
                "task_tmm": row.get("task.tmm"),
                "rptr": row.get("task.rptr"),
                "crptr": row.get("task.crptr"),
            }
        )

    return mismatches


def format_counter_key(title: str, key: Any) -> str:
    if isinstance(key, int) and title == "ports":
        return hex(key)
    return str(key)


def print_counter(title: str, data: dict[Any, int], limit: int) -> None:
    print(f"{title}:")
    if not data:
        print("  none")
        return
    items = sorted(data.items(), key=lambda item: (-item[1], str(item[0])))
    for key, count in items[:limit]:
        print(f"  {format_counter_key(title, key)}: {count}")


def print_text(summary: dict[str, Any], limit: int) -> None:
    print(f"file: {summary['path']}")
    print(f"entries: parsed={summary['entries']} complete={summary['complete_entries']}")
    if summary["seq_min"] is not None:
        print(f"seq_range: {summary['seq_min']}..{summary['seq_max']}")
    if summary["dumps"]:
        for dump in summary["dumps"]:
            print(f"dump_begin: reason={dump['reason']} declared_entries={dump['entries']}")
    if summary["dump_ends"]:
        print("dump_end_entries: " + ",".join(str(v) for v in summary["dump_ends"]))
    print_counter("markers", summary["markers"], limit)
    print(
        f"tlb_lag: count={summary['tlb_lag_count']} max={summary['tlb_lag_max']}"
    )
    print_counter("tlb_lag_by_pid", summary["tlb_lag_by_pid"], limit)
    print_counter("ops", summary["op_counts"], limit)
    print_counter("ports", summary["port_counts"], limit)
    print_counter("exit_reasons", summary["exit_counts"], limit)
    print_counter("pids", summary["pid_counts"], limit)
    if summary["max_mm_lag"]:
        lag = summary["max_mm_lag"]
        print(
            "max_mm_lag: "
            f"lag={lag['lag']} cpu={lag['cpu']} seq={lag['seq']} "
            f"pid={lag['pid']} op={lag['op']} mmgen={lag['mmgen']} vlast={lag['vlast']}"
        )
    if summary["dispatch_switches"]:
        print(f"dispatch_switches: count={len(summary['dispatch_switches'])}")
        for item in summary["dispatch_switches"][:limit]:
            print(
                "  "
                f"cpu={item['cpu']} entry_seq={item['entry_seq']} "
                f"entry_pid={item['entry_pid']} entry_tmm={item['entry_tmm']} "
                f"exit_seq={item['exit_seq']} exit_pid={item['exit_pid']} "
                f"exit_tmm={item['exit_tmm']}"
            )
    if summary["syscall_switches"]:
        print(f"syscall_switches: count={len(summary['syscall_switches'])}")
        for item in summary["syscall_switches"][:limit]:
            print(
                "  "
                f"cpu={item['cpu']} entry_seq={item['entry_seq']} "
                f"entry_pid={item['entry_pid']} entry_tmm={item['entry_tmm']} "
                f"entry_syscall={item['entry_syscall']} "
                f"post_seq={item['post_seq']} post_pid={item['post_pid']} "
                f"post_tmm={item['post_tmm']} post_horax={item['post_horax']} "
                f"post_hax={item['post_hax']}"
            )
    if summary["post_syscall_mismatches"]:
        print(
            "post_syscall_mismatches: "
            f"count={len(summary['post_syscall_mismatches'])}"
        )
        for item in summary["post_syscall_mismatches"][:limit]:
            print(
                "  "
                f"cpu={item['cpu']} seq={item['seq']} pid={item['pid']} "
                f"port={fmt_hex(item['port'])} run_rax={item['run_rax']} "
                f"task_horax={item['task_horax']} "
                f"task_hax={item['task_hax']} "
                f"task_tmm={item['task_tmm']} "
                f"vcpu_mm={item['vcpu_mm']}"
            )
    if summary["mm_backsteps"]:
        print(f"mm_backsteps: count={len(summary['mm_backsteps'])}")
        for item in summary["mm_backsteps"][:limit]:
            print(
                "  "
                f"cpu={item['cpu']} seq={item['seq']} pid={item['pid']} "
                f"op={item['op']} mmgen={item['mmgen']} vlast={item['vlast']} "
                f"task_tmm={item['task_tmm']} vcpu_mm={item['vcpu_mm']}"
            )
    if summary["has_regs_owner"]:
        print(f"regs_owner_mismatches: count={len(summary['regs_owner_mismatches'])}")
        for item in summary["regs_owner_mismatches"][:limit]:
            print(
                "  "
                f"cpu={item['cpu']} seq={item['seq']} pid={item['pid']} "
                f"op={item['op']} task_tmm={item['task_tmm']} "
                f"rptr={item['rptr']} crptr={item['crptr']}"
            )
    if summary["incomplete_entries"]:
        print("incomplete_entries:")
        for row in summary["incomplete_entries"]:
            print(f"  cpu={row['cpu']} seq={row['seq']} sections={','.join(row['sections'])}")
    if summary["fatal_context"]:
        print("fatal_context:")
        for line in summary["fatal_context"]:
            print(f"  {line}")
    if summary.get("anomalies"):
        print("trace_anomalies:")
        for line in summary["anomalies"]:
            print(f"  {line}")
    if summary["last_entries"]:
        print("last_entries:")
        for row in summary["last_entries"]:
            print(
                "  "
                f"cpu={row['cpu']} seq={row['seq']} pid={row.get('header.pid')} "
                f"op={row.get('header.op')} port={fmt_hex(row.get('header.port'))} "
                f"rip={fmt_hex(row.get('regs.rip'))} cr2={fmt_hex(row.get('sregs.cr2'))} "
                f"hip={fmt_hex(row.get('task.hip'))} hsp={fmt_hex(row.get('task.hsp'))} "
                f"mmgen={row.get('mmvcpu.mmgen')} vlast={row.get('mmvcpu.vlast')}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="run-*.log files")
    parser.add_argument("--json", action="store_true", help="emit JSON summaries")
    parser.add_argument("--limit", type=int, default=12, help="counter rows to print")
    args = parser.parse_args()

    summaries = [parse_file(path) for path in args.logs]
    if args.json:
        json.dump(summaries, sys.stdout, indent=2, sort_keys=True)
        print()
        return 0

    for i, summary in enumerate(summaries):
        if i:
            print()
        print_text(summary, args.limit)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
