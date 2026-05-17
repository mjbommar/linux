#!/usr/bin/env python3
"""
KVM v2 state-trace parser + invariant checker.

Reads dmesg output from a UML run with CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE
enabled, parses the KVMV2T-* lines (each tagged with cpu+seq for
re-assembly even when printk interleaves output across CPUs), and:

  (1) reports the per-(pid, cpu) timeline,
  (2) checks invariants from state-audit Layer 3,
  (3) emits the FIRST invariant violation per pid as a candidate
      bug site.

Each entry is split across 7 prefixed lines:
  KVMV2T-H cpu=C seq=S ts=N pid=N op=NAME exit=N port=0xN
  KVMV2T-R cpu=C seq=S rax=N rbx=N ...
  KVMV2T-S cpu=C seq=S cr0=N ...
  KVMV2T-T cpu=C seq=S tmm=N ...
  KVMV2T-F cpu=C seq=S tfpuh=N ... tist=[N,...]
  KVMV2T-M cpu=C seq=S mmgen=N ...
  KVMV2T-I cpu=C seq=S list=[N,...]

Lines are merged by (cpu, seq).

Usage:
  parse-trace.py summary <log>
  parse-trace.py pid <PID> <log> [--limit N]
  parse-trace.py invariants <log> [--limit N]
  parse-trace.py diff <log_A> <log_B>
"""
import argparse
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Dict


HDR_RE = re.compile(
    r'KVMV2T-H cpu=(\d+) seq=(\d+) ts=(\d+) pid=(\d+) op=(\w+) '
    r'exit=(\d+) port=(0x[0-9a-f]+)'
)
GPR_RE = re.compile(
    r'KVMV2T-R cpu=(\d+) seq=(\d+) '
    r'rax=([0-9a-f]+) rbx=([0-9a-f]+) rcx=([0-9a-f]+) rdx=([0-9a-f]+) '
    r'rsi=([0-9a-f]+) rdi=([0-9a-f]+) rbp=([0-9a-f]+) rsp=([0-9a-f]+) '
    r'r8=([0-9a-f]+) r9=([0-9a-f]+) r10=([0-9a-f]+) r11=([0-9a-f]+) '
    r'r12=([0-9a-f]+) r13=([0-9a-f]+) r14=([0-9a-f]+) r15=([0-9a-f]+) '
    r'rip=([0-9a-f]+) rfl=([0-9a-f]+)'
)
SREGS_RE = re.compile(
    r'KVMV2T-S cpu=(\d+) seq=(\d+) '
    r'cr0=([0-9a-f]+) cr2=([0-9a-f]+) cr3=([0-9a-f]+) '
    r'cr4=([0-9a-f]+) fsb=([0-9a-f]+) gsb=([0-9a-f]+)'
)
TASK_RE = re.compile(
    r'KVMV2T-T cpu=(\d+) seq=(\d+) '
    r'tmm=([0-9a-f]+) tamm=([0-9a-f]+) tscr2=([0-9a-f]+) '
    r'hax=([0-9a-f]+) horax=([0-9a-f]+) hip=([0-9a-f]+) hsp=([0-9a-f]+)'
    r'(?: rmatch=(\d+) rptr=([0-9a-f]+) crptr=([0-9a-f]+))?'
)
TFLAG_RE = re.compile(
    r'KVMV2T-F cpu=(\d+) seq=(\d+) '
    r'tfpuh=([0-9a-f]+) tscv=(\d+) tistp=(\d+) tiofv=(\d+) tfpuv=(\d+) '
    r'tist=\[([^\]]+)\]'
)
MMVCPU_RE = re.compile(
    r'KVMV2T-M cpu=(\d+) seq=(\d+) '
    r'mmgen=(\d+) vlast=(\d+) vmm=([0-9a-f]+) vkick=(\d+)'
)
IST_RE = re.compile(
    r'KVMV2T-I cpu=(\d+) seq=(\d+) list=\[([^\]]+)\]'
)


@dataclass
class Snap:
    cpu: int = 0
    seq: int = 0
    ts: int = 0
    pid: int = 0
    op: str = ''
    exit_reason: int = 0
    port: int = 0
    rax: int = 0; rbx: int = 0; rcx: int = 0; rdx: int = 0
    rsi: int = 0; rdi: int = 0; rbp: int = 0; rsp: int = 0
    r8: int = 0; r9: int = 0; r10: int = 0; r11: int = 0
    r12: int = 0; r13: int = 0; r14: int = 0; r15: int = 0
    rip: int = 0; rfl: int = 0
    cr0: int = 0; cr2: int = 0; cr3: int = 0; cr4: int = 0
    fsb: int = 0; gsb: int = 0
    tmm: int = 0; tamm: int = 0; tscr2: int = 0
    hax: int = 0; horax: int = 0; hip: int = 0; hsp: int = 0
    rmatch: int = 0; rptr: int = 0; crptr: int = 0
    tfpuh: int = 0; tscv: int = 0; tistp: int = 0
    tiofv: int = 0; tfpuv: int = 0
    tist: List[int] = field(default_factory=lambda: [0]*6)
    mmgen: int = 0; vlast: int = 0; vmm: int = 0; vkick: int = 0
    ist_live: List[int] = field(default_factory=lambda: [0]*6)
    sections_seen: int = 0  # bitmask of H/R/S/T/F/M/I


SECT_H = 1 << 0
SECT_R = 1 << 1
SECT_S = 1 << 2
SECT_T = 1 << 3
SECT_F = 1 << 4
SECT_M = 1 << 5
SECT_I = 1 << 6
SECT_ALL = SECT_H|SECT_R|SECT_S|SECT_T|SECT_F|SECT_M|SECT_I


def parse_log(path: str) -> List[Snap]:
    """Parse all KVMV2T-* lines into Snap entries keyed by (cpu, seq)."""
    entries: Dict[Tuple[int, int], Snap] = {}

    with open(path, errors='replace') as f:
        for line in f:
            m = HDR_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                e.ts          = int(m.group(3))
                e.pid         = int(m.group(4))
                e.op          = m.group(5)
                e.exit_reason = int(m.group(6))
                e.port        = int(m.group(7), 16)
                e.sections_seen |= SECT_H
                continue
            m = GPR_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                vals = [int(m.group(i), 16) for i in range(3, 21)]
                (e.rax, e.rbx, e.rcx, e.rdx, e.rsi, e.rdi,
                 e.rbp, e.rsp, e.r8, e.r9, e.r10, e.r11,
                 e.r12, e.r13, e.r14, e.r15, e.rip, e.rfl) = vals
                e.sections_seen |= SECT_R
                continue
            m = SREGS_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                vals = [int(m.group(i), 16) for i in range(3, 9)]
                e.cr0, e.cr2, e.cr3, e.cr4, e.fsb, e.gsb = vals
                e.sections_seen |= SECT_S
                continue
            m = TASK_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                vals = [int(m.group(i), 16) for i in range(3, 10)]
                (e.tmm, e.tamm, e.tscr2, e.hax,
                 e.horax, e.hip, e.hsp) = vals
                if m.group(10) is not None:
                    e.rmatch = int(m.group(10))
                    e.rptr = int(m.group(11), 16)
                    e.crptr = int(m.group(12), 16)
                e.sections_seen |= SECT_T
                continue
            m = TFLAG_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                e.tfpuh = int(m.group(3), 16)
                e.tscv  = int(m.group(4))
                e.tistp = int(m.group(5))
                e.tiofv = int(m.group(6))
                e.tfpuv = int(m.group(7))
                e.tist  = [int(x, 16) for x in m.group(8).split(',')]
                e.sections_seen |= SECT_F
                continue
            m = MMVCPU_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                e.mmgen = int(m.group(3))
                e.vlast = int(m.group(4))
                e.vmm   = int(m.group(5), 16)
                e.vkick = int(m.group(6))
                e.sections_seen |= SECT_M
                continue
            m = IST_RE.search(line)
            if m:
                cpu, seq = int(m.group(1)), int(m.group(2))
                e = entries.setdefault((cpu, seq), Snap())
                e.cpu, e.seq = cpu, seq
                e.ist_live = [int(x, 16) for x in m.group(3).split(',')]
                e.sections_seen |= SECT_I
                continue

    out = list(entries.values())
    return out


def sort_key(s: Snap):
    """Best-effort timeline order: ts, then cpu, then seq."""
    return (s.ts, s.cpu, s.seq)


# --- Invariants (from state-audit Layer 3) ---------------------------

def check_invariants(snaps: List[Snap]) -> List[Tuple[str, Snap, str]]:
    """Returns list of (severity, snap, message) for each violation."""
    violations: List[Tuple[str, Snap, str]] = []
    snaps = sorted(snaps, key=sort_key)

    per_cpu = defaultdict(list)
    for s in snaps:
        per_cpu[s.cpu].append(s)

    for cpu, cpu_snaps in per_cpu.items():
        # INV1: PRE_KVM_RUN.pid == POST_KVM_RUN.pid (preempt held)
        prev_pre: Optional[Snap] = None
        for s in cpu_snaps:
            if s.op == 'PRE_KVM_RUN':
                prev_pre = s
            elif s.op == 'POST_KVM_RUN' and prev_pre:
                if prev_pre.pid != s.pid:
                    violations.append((
                        'CRITICAL', s,
                        f'INV1: PRE_KVM_RUN pid={prev_pre.pid} (seq={prev_pre.seq}) '
                        f'!= POST_KVM_RUN pid={s.pid} (seq={s.seq})'
                    ))
                prev_pre = None

        # INV2: VCPU_RUN_ENTRY.pid == VCPU_RUN_EXIT.pid
        entry_stack: List[Snap] = []
        for s in cpu_snaps:
            if s.op == 'VCPU_RUN_ENTRY':
                entry_stack.append(s)
            elif s.op == 'VCPU_RUN_EXIT' and entry_stack:
                ent = entry_stack.pop()
                if ent.pid != s.pid:
                    violations.append((
                        'CRITICAL', s,
                        f'INV2: VCPU_RUN_ENTRY pid={ent.pid} (seq={ent.seq}) '
                        f'!= VCPU_RUN_EXIT pid={s.pid} (seq={s.seq})'
                    ))

        # INV3: POST_LOAD_SREGS — vcpu->current_mm == current->mm
        for s in cpu_snaps:
            if s.op == 'POST_LOAD_SREGS':
                if s.tmm and s.vmm != s.tmm:
                    violations.append((
                        'CRITICAL', s,
                        f'INV3: vcpu->current_mm={s.vmm:x} != current->mm={s.tmm:x}'
                    ))

        # INV4: POST_LOAD_SREGS — vlast == mmgen (we just synced)
        for s in cpu_snaps:
            if s.op == 'POST_LOAD_SREGS':
                if s.tmm and s.vlast != s.mmgen:
                    violations.append((
                        'WARNING', s,
                        f'INV4: vlast={s.vlast} != mmgen={s.mmgen} '
                        f'(diff={s.mmgen - s.vlast})'
                    ))

        # INV5: PRE_KVM_RUN — RIP must be in valid user, kernel, or
        #       trampoline range. Out-of-range = corruption.
        VALID_LO = 0x400000
        VALID_HI = 0x600000
        TRAMP    = 0xffffe00000000000
        KERN_LO  = 0xffffffff80000000
        for s in cpu_snaps:
            if s.op == 'PRE_KVM_RUN':
                rip = s.rip
                # Allow user code, trampoline, kernel-half, and
                # PML4[508] handler stubs.
                if not (VALID_LO <= rip <= VALID_HI or
                        rip >= TRAMP):
                    violations.append((
                        'WARNING', s,
                        f'INV5: PRE_KVM_RUN rip={rip:x} outside valid range'
                    ))

        # INV6: POST_KVM_RUN — exit_reason should be IO (=2). We
        #       panic on others, but still flag it.
        for s in cpu_snaps:
            if s.op == 'POST_KVM_RUN' and s.exit_reason not in (0, 2):
                violations.append((
                    'WARNING', s,
                    f'INV6: POST_KVM_RUN unusual exit_reason={s.exit_reason}'
                ))

        # INV7: HANDLE_SYSCALL_POST — hax (syscall return) consistency
        #       check. mmap (sysno=9) returning 0 with prot != 0 is
        #       the MMAP_NULL bug. We can't see syscall_nr without
        #       capturing it, but we DO see HOST_AX before+after.
        # Defer. Need PT_SYSCALL_NR field added.

        # INV8: pid stability across consecutive ops on same CPU
        #       between VCPU_RUN_ENTRY and VCPU_RUN_EXIT (preempt
        #       disable is held). Only flagged within a dispatch.
        in_dispatch = False
        dispatch_pid = -1
        dispatch_ent: Optional[Snap] = None
        for s in cpu_snaps:
            if s.op == 'VCPU_RUN_ENTRY':
                in_dispatch = True
                dispatch_pid = s.pid
                dispatch_ent = s
            elif in_dispatch and s.pid != dispatch_pid:
                violations.append((
                    'CRITICAL', s,
                    f'INV8: pid={s.pid} mid-dispatch (started pid={dispatch_pid} '
                    f'at ENTRY seq={dispatch_ent.seq if dispatch_ent else -1})'
                ))
                # Only report first violation per dispatch
                in_dispatch = False
            elif s.op == 'VCPU_RUN_EXIT':
                in_dispatch = False

        # INV9: POST_LOAD_SREGS / POST_FPU_INSTALL — consistency of
        #       fs_base in sregs vs HOST_FS_BASE we set. (fs_base
        #       stored as full 64-bit base.)
        # Per-task TLS lives at fs:0; corruption means cross-thread
        # leakage. Not directly observable here without capturing
        # regs->gp[HOST_FS_BASE].

        # INV10: tmm should not change between VCPU_RUN_ENTRY and
        #        VCPU_RUN_EXIT for the same dispatch (preempt held).
        tmm_at_entry: Optional[int] = None
        for s in cpu_snaps:
            if s.op == 'VCPU_RUN_ENTRY':
                tmm_at_entry = s.tmm
            elif s.op == 'VCPU_RUN_EXIT' and tmm_at_entry is not None:
                if tmm_at_entry and s.tmm and tmm_at_entry != s.tmm:
                    violations.append((
                        'CRITICAL', s,
                        f'INV10: tmm changed mid-dispatch: '
                        f'entry={tmm_at_entry:x} exit={s.tmm:x}'
                    ))
                tmm_at_entry = None

    return violations


# --- Reporting -------------------------------------------------------

def cmd_summary(args):
    snaps = parse_log(args.log)
    print(f'Parsed {len(snaps)} snapshots from {args.log}')
    op_count = defaultdict(int)
    incomplete = 0
    for s in snaps:
        op_count[s.op] += 1
        if s.sections_seen != SECT_ALL:
            incomplete += 1
    print(f'Incomplete entries (missing sections): {incomplete}')
    print('\nOps distribution:')
    for op, n in sorted(op_count.items(), key=lambda x: -x[1]):
        print(f'  {n:6d}  {op}')
    pid_count = defaultdict(int)
    for s in snaps:
        pid_count[s.pid] += 1
    print('\nPer-pid distribution (top 20):')
    for pid, n in sorted(pid_count.items(), key=lambda x: -x[1])[:20]:
        print(f'  pid={pid:5d}  {n:6d} entries')
    cpu_count = defaultdict(int)
    for s in snaps:
        cpu_count[s.cpu] += 1
    print('\nPer-cpu distribution:')
    for cpu in sorted(cpu_count):
        print(f'  cpu={cpu}  {cpu_count[cpu]:6d} entries')


def cmd_pid(args):
    snaps = parse_log(args.log)
    pid_snaps = [s for s in snaps if s.pid == args.pid_id]
    pid_snaps.sort(key=sort_key)
    print(f'pid={args.pid_id}: {len(pid_snaps)} entries')
    for s in pid_snaps[:args.limit]:
        print(f'  ts={s.ts} seq={s.seq} cpu={s.cpu} op={s.op:22s} '
              f'rip={s.rip:12x} hax={s.hax:6x} '
              f'tmm={s.tmm:9x} mmgen={s.mmgen:4d} vlast={s.vlast:4d}')


def cmd_invariants(args):
    snaps = parse_log(args.log)
    print(f'Parsed {len(snaps)} snapshots')
    violations = check_invariants(snaps)
    if not violations:
        print('No invariant violations detected.')
        return
    crit = [v for v in violations if v[0] == 'CRITICAL']
    warn = [v for v in violations if v[0] == 'WARNING']
    print(f'\n{len(violations)} violations: {len(crit)} CRITICAL, {len(warn)} WARNING')
    print()
    print('=== CRITICAL ===')
    for sev, s, msg in crit[:args.limit]:
        print(f'  cpu={s.cpu} pid={s.pid} ts={s.ts} seq={s.seq} op={s.op}')
        print(f'    -> {msg}')
    print()
    print('=== WARNING (first 5) ===')
    for sev, s, msg in warn[:5]:
        print(f'  cpu={s.cpu} pid={s.pid} ts={s.ts} seq={s.seq} op={s.op}')
        print(f'    -> {msg}')


def cmd_find_mmap_zero(args):
    """Find HANDLE_SYSCALL_POST entries where horax=9 (mmap) and
    hax=0 (returned NULL — the MMAP_NULL bug)."""
    snaps = parse_log(args.log)
    snaps.sort(key=sort_key)
    found = []
    for s in snaps:
        if s.op == 'HANDLE_SYSCALL_POST' and s.horax == 9 and s.hax == 0:
            found.append(s)
    print(f'Found {len(found)} mmap-returned-zero events')
    for s in found[:args.limit]:
        print(f'  ts={s.ts} cpu={s.cpu} seq={s.seq} pid={s.pid} '
              f'rip={s.rip:x} rsi(len)={s.rsi:x} rdx(prot)={s.rdx:x}')


def cmd_pid_window(args):
    """Show the N entries before and after the first MMAP_NULL event
    for the failing pid."""
    snaps = parse_log(args.log)
    snaps.sort(key=sort_key)
    # Find first mmap-zero
    fail_idx = None
    for i, s in enumerate(snaps):
        if (s.op == 'HANDLE_SYSCALL_POST' and s.horax == 9 and s.hax == 0):
            fail_idx = i
            break
    if fail_idx is None:
        print('No MMAP-zero event found')
        return
    fail = snaps[fail_idx]
    print(f'FAIL @ ts={fail.ts} cpu={fail.cpu} seq={fail.seq} pid={fail.pid}')
    pid = fail.pid
    pid_snaps = [s for s in snaps if s.pid == pid]
    pid_snaps.sort(key=sort_key)
    pid_idx = pid_snaps.index(fail)
    lo = max(0, pid_idx - args.window)
    hi = min(len(pid_snaps), pid_idx + args.window + 1)
    print(f'\npid={pid} window [{lo}:{hi}] of {len(pid_snaps)} entries:')
    for i, s in enumerate(pid_snaps[lo:hi]):
        marker = ' <-- FAIL' if (lo + i == pid_idx) else ''
        print(f'  ts={s.ts} cpu={s.cpu} seq={s.seq} op={s.op:22s} '
              f'horax={s.horax:>3} hax={s.hax:9x} rip={s.rip:9x}'
              + marker)


def cmd_diff(args):
    a_snaps = parse_log(args.log_a)
    b_snaps = parse_log(args.log_b)
    a_inv = check_invariants(a_snaps)
    b_inv = check_invariants(b_snaps)
    print(f'A ({args.log_a}): {len(a_snaps)} snaps, {len(a_inv)} violations')
    print(f'B ({args.log_b}): {len(b_snaps)} snaps, {len(b_inv)} violations')

    # Group violations by INV-id (e.g. "INV1", "INV3")
    def group(vs):
        d = defaultdict(int)
        for sev, s, msg in vs:
            tag = msg.split(':')[0]
            d[tag] += 1
        return d
    ga, gb = group(a_inv), group(b_inv)
    print('\nViolation counts by invariant:')
    print(f"  {'INV':<8} {'A':>8} {'B':>8} {'B-A':>8}")
    for k in sorted(set(ga) | set(gb)):
        print(f'  {k:<8} {ga.get(k,0):>8} {gb.get(k,0):>8} {gb.get(k,0)-ga.get(k,0):>+8}')

    print('\n=== first 10 CRITICAL violations in B ===')
    crit_b = [v for v in b_inv if v[0] == 'CRITICAL']
    for sev, s, msg in crit_b[:10]:
        print(f'  cpu={s.cpu} pid={s.pid} ts={s.ts} seq={s.seq} op={s.op}')
        print(f'    -> {msg}')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('summary')
    s.add_argument('log')
    s.set_defaults(func=cmd_summary)
    s = sub.add_parser('pid')
    s.add_argument('pid_id', type=int)
    s.add_argument('log')
    s.add_argument('--limit', type=int, default=50)
    s.set_defaults(func=cmd_pid)
    s = sub.add_parser('invariants')
    s.add_argument('log')
    s.add_argument('--limit', type=int, default=20)
    s.set_defaults(func=cmd_invariants)
    s = sub.add_parser('diff')
    s.add_argument('log_a')
    s.add_argument('log_b')
    s.set_defaults(func=cmd_diff)
    s = sub.add_parser('mmap-zero')
    s.add_argument('log')
    s.add_argument('--limit', type=int, default=20)
    s.set_defaults(func=cmd_find_mmap_zero)
    s = sub.add_parser('pid-window')
    s.add_argument('log')
    s.add_argument('--window', type=int, default=20)
    s.set_defaults(func=cmd_pid_window)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
