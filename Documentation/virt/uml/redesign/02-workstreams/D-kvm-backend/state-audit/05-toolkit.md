# UML KVM v2 — State-Audit Toolkit (recipes)

**Created:** 2026-05-01
**Tip at creation:** `e5f231656ead`
**Audience:** anyone (human or subagent) filling in the matrix
([03](03-state-ownership-matrix.md)) or chasing a fresh mt-mini
flake. Every command was verified against the live tree (or the
in-flight mt-mini soak) — the `rg` patterns hit ≥1 line, the
bpftrace/ftrace recipes use real tracepoint names from
`arch/x86/kvm/trace.h`.

All paths are repo-relative to `/home/mjbommar/projects/personal/linux`.
For tools: `rg`, `bpftrace`, `perf`, `gdb`, `crash` are required;
`ast-grep` is *optional* (see §1c). `cscope` is not used here —
`rg` is faster and zero-config.

---

## 5-minute audit recipe

When you only have five minutes and need to know "is the per-task
gp[] / per-vCPU mmap / TLB-gen state plausible?":

```bash
cd $LINUX
# (1) every gp[HOST_*] R/W in v2 + UML kernel hooks (~80 hits)
rg -nP 'gp\[HOST_[A-Z0-9_]+\]' arch/um/backend/kvm-v2/ arch/um/kernel/skas/ arch/x86/um/

# (2) every kvm_run.s.regs.* and kvm_dirty_regs flag set (~30 hits)
rg -nP '(kvm_run|run)->s\.regs\.|kvm_dirty_regs\s*[|^&]?=' arch/um/backend/kvm-v2/

# (3) every os_ioctl_generic(*, KVM_*) call (~20 hits — the v2 KVM surface)
rg -nP 'os_ioctl_generic\([^,]+,\s*KVM_' arch/um/backend/kvm-v2/

# (4) preempt/mutex/spin scope inside kvm-v2 (paired must be even)
rg -nPc 'preempt_(disable|enable)\b|mutex_(lock|unlock)\b|spin_(lock|unlock)\b' \
   arch/um/backend/kvm-v2/

# (5) live KVM stats — pick the UML pid and inspect aggregate counters
ls /sys/kernel/debug/kvm/ 2>/dev/null && \
  for f in remote_tlb_flush_requests host_state_reload signal_exits exits; do
    echo -n "$f: "; cat /sys/kernel/debug/kvm/*-*/$f 2>/dev/null
  done
```

If (4) returns an odd count for any file, an unbalanced lock
exists — start there.

---

## §1 — Static enumeration

### 1a. Per-task `regs->gp[HOST_*]` (RBP/RAX/RDX/FS_BASE & friends)

```bash
# Every read or write of any HOST_* slot, anywhere it could matter:
rg -nP 'gp\[HOST_[A-Z0-9_]+\]' \
   arch/um/backend/kvm-v2/ \
   arch/um/kernel/skas/ \
   arch/x86/um/

# Narrow to the four suspect registers from layer 4:
rg -nP 'gp\[HOST_(BP|AX|DX|FS_BASE)\]' \
   arch/um/backend/kvm-v2/ arch/um/kernel/skas/ arch/x86/um/

# Writes only (LHS — assignment on the same line):
rg -nP 'gp\[HOST_[A-Z0-9_]+\]\s*=(?!=)' \
   arch/um/backend/kvm-v2/ arch/um/kernel/skas/ arch/x86/um/

# Compound mutators (+=, |= etc):
rg -nP 'gp\[HOST_[A-Z0-9_]+\]\s*[+\-*/|&^]=' arch/um/
```

Verified: returns 34 lines for the `BP|AX|DX|FS_BASE` query — every
matrix-cell candidate.

### 1b. Per-vCPU `vcpu->kvm_run` mmap fields

```bash
# Every read/write of kvm_run sub-fields under the v2 backend
# (kvm_run is the per-CPU shared mmap; matrix needs every site):
rg -nP '\b(kvm_run|run)->(s\.regs|kvm_dirty_regs|exit_reason|io|mmio|immediate_exit|request_interrupt_window)' \
   arch/um/backend/kvm-v2/

# kvm_dirty_regs flag sets (shows when we're telling KVM to consume
# our shadow of regs/sregs/events on the next KVM_RUN):
rg -nP 'kvm_dirty_regs\s*[|^&]?=\s*' arch/um/backend/kvm-v2/
```

Verified: `kvm_dirty_regs |= ...` returns the 9 expected sites
(vcpu.c × 2, syscall_trap.c × 7).

### 1c. `vcpu->ist_stack_kva` (per-CPU IST page)

```bash
# Every direct field access:
rg -nP '\b(ist_stack_kva|ist_top|ist_frame_snapshot)\b' \
   arch/um/backend/kvm-v2/

# Byte-offset reads from the IST page (the post-stub stash window):
rg -nP 'ist_(stack_kva|top)[^;]{0,80}(PAGE_SIZE|0x[0-9a-fA-F]+|-\s*\d+)' \
   arch/um/backend/kvm-v2/
```

If you have ast-grep installed:

```bash
# `target->ist_stack_kva[ANY]` — any indexed access, two-arg pattern:
ast-grep --pattern '$T->ist_stack_kva[$X]' arch/um/backend/kvm-v2/
ast-grep --pattern 'kvm_v2_ist_frame_$_($$$)' arch/um/backend/kvm-v2/
```

### 1d. `arch_thread.kvm_v2.*` (per-task private state)

```bash
rg -nP '(thread\.arch\.kvm_v2|->thread\.arch\.kvm_v2|arch_thread\.kvm_v2)\.' \
   arch/um/ arch/x86/um/

# Per-field breakdowns — pick a field to scope a deep audit:
for f in fpu iotrap_fpu ist_frame saved_cr2 last_seen_tlb_gen current_mm; do
    echo "=== .$f ==="
    rg -nP "kvm_v2\.${f}\b" arch/um/ arch/x86/um/
done
```

### 1e. mm-context TLB / deferred-free state

```bash
# Per-mm tlb-gen + sync ranges + freelist (the cells most likely
# implicated in cross-vCPU stale-TLB symptoms):
rg -nP '\bmm->context\.(tlb_gen|sync_tlb_range_(start|end)|deferred_free)' \
   arch/um/

# Tlb-gen reads vs writes (writers should be paired with kick_others):
rg -nP 'context\.tlb_gen\s*\+\+|atomic64_inc\(&[^)]*tlb_gen' arch/um/
rg -nP '\.tlb_gen\b|->tlb_gen\b' arch/um/
```

### 1f. Synchronization primitive scope inside kvm-v2/

```bash
# Pair-balance audit (each file's open/close count must match):
rg -nPc 'preempt_disable\b' arch/um/backend/kvm-v2/
rg -nPc 'preempt_enable\b'  arch/um/backend/kvm-v2/
rg -nPc 'mutex_lock\b'      arch/um/backend/kvm-v2/
rg -nPc 'mutex_unlock\b'    arch/um/backend/kvm-v2/
rg -nPc 'spin_lock\b'       arch/um/backend/kvm-v2/
rg -nPc 'spin_unlock\b'     arch/um/backend/kvm-v2/

# All scope opens with line numbers (cross-reference manually with closes):
rg -nP 'preempt_disable\b|mutex_lock\b|spin_lock(_irqsave)?\b' \
   arch/um/backend/kvm-v2/
```

### 1g. `KVM_SET_*` / `KVM_GET_*` ioctl call sites

The wrapper is `os_ioctl_generic(fd, CMD, arg)`; this is the *only*
path v2 uses to talk to /dev/kvm:

```bash
rg -nP 'os_ioctl_generic\([^,]+,\s*KVM_[A-Z0-9_]+' arch/um/backend/kvm-v2/

# Categorize SET vs GET:
rg -nP 'os_ioctl_generic\([^,]+,\s*KVM_SET_' arch/um/backend/kvm-v2/
rg -nP 'os_ioctl_generic\([^,]+,\s*KVM_GET_' arch/um/backend/kvm-v2/
```

### 1h. Kick / IPI plumbing

```bash
rg -nP '\bos_send_ipi\(|kick_pending|IPI_SIGNAL\b' arch/um/backend/kvm-v2/ arch/um/os-Linux/
rg -nP 'kvm_v2_tlb_kick_others\b|tlb_kick_others\b' arch/um/
```

---

## §2 — Runtime verification (bpftrace + ftrace)

### 2a. Tracepoint deltas: T=8 vs T=4

The KVM module exposes ~74 `kvm:*` tracepoints (`arch/x86/kvm/trace.h`).
Per-vCPU breakdown of the most diagnostic ones:

```bash
# Save a 30-iter run's tracepoint counts (per UML host pid + vcpu_id):
sudo bpftrace -e '
  tracepoint:kvm:kvm_entry  { @entries[pid, args->vcpu_id] = count(); }
  tracepoint:kvm:kvm_exit   { @exits[pid, args->vcpu_id, args->exit_reason] = count(); }
  tracepoint:kvm:kvm_inj_exception { @inj[pid, args->exception] = count(); }
  interval:s:30 { exit(); }
' > /tmp/kvm-T8.tally &
BPID=$!
sudo /tmp/run-d15-segv.sh   # uses T=4 by default — edit to mt-mini T=8 first
wait $BPID

# Re-run with T=4 for diff:
# (same bpftrace, write to /tmp/kvm-T4.tally)
diff -u /tmp/kvm-T4.tally /tmp/kvm-T8.tally
```

Conclusion shape: any `kvm_exit` reason that grows
super-linearly (e.g. EXIT_REASON_EXCEPTION_NMI 4× higher than CPU-count
ratio) is a flag. Cross-vCPU divergence in `kvm_entry` count for the
same UML pid signals an unfair scheduler — informational, not a
bug per se.

### 2b. ftrace function_graph on the v2 hot path

```bash
F=/sys/kernel/tracing
sudo bash -c "echo function_graph > $F/current_tracer"
for fn in kvm_v2_vcpu_run kvm_v2_load_user_sregs kvm_v2_handle_io_pf \
          interrupt_end __schedule; do
    echo $fn | sudo tee -a $F/set_graph_function
done
sudo bash -c "echo 0 > $F/tracing_on; echo > $F/trace; echo 1 > $F/tracing_on"
sudo timeout 20 /tmp/run-d15-segv.sh
sudo bash -c "echo 0 > $F/tracing_on"
# Surface only inter-call gaps > 100us:
sudo grep -E '(\| {2,}[0-9]+\.[0-9]+ us)' $F/trace | awk '$2+0 > 100'
```

Expected shape: under healthy T=4 the per-call cost of
`kvm_v2_vcpu_run` is dominated by `kvm_arch_vcpu_ioctl_run`. Any
unexpected gap (e.g. `kvm_v2_load_user_sregs` taking >1ms) is an
immediate red flag — almost always signal-pending wait or
KVM_SET_SREGS deferred work.

### 2c. Per-vCPU exit-reason tally + IST writes + kicks

```bash
# vmexit reasons per (host-cpu, vcpu_id) — the cleanest cross-vCPU view:
sudo bpftrace -e '
  tracepoint:kvm:kvm_exit {
      @[cpu, args->vcpu_id, args->exit_reason] = count();
  }
  interval:s:60 { exit(); }
'

# IST-frame writes (uprobe on the UML binary; substitute your build):
UML=$HOME/src/uml-builds/uml-clean/linux
sudo bpftrace -e "
  uprobe:$UML:kvm_v2_ist_frame_snapshot_raw { @snap[tid] = count(); }
  uprobe:$UML:kvm_v2_handle_pf_eintr_inline { @pfb[tid]  = count(); }
  interval:s:30 { exit(); }
"

# Kick events fired (host-side os_send_ipi callers):
sudo bpftrace -e "
  uprobe:$UML:kvm_v2_tlb_kick_others { @kicks[tid] = count(); }
  uprobe:$UML:os_send_ipi            { @ipi[tid]   = count(); }
  interval:s:30 { exit(); }
"
```

Conclusion shape: `@kicks` should be ≪ `@ipi` (one kick fans out to N-1
vCPUs). If `@ipi == 0` while `@kicks > 0`, the dedup at vcpu.c:860
is suppressing every IPI (pre-existing `kick_pending=1`) — exactly
the "remote vCPU never gets the flush" failure mode.

### 2d. Top-level snapshots

```bash
# A short-lived perf summary across the run:
sudo perf stat -a -e \
  'kvm:kvm_entry,kvm:kvm_exit,kvm:kvm_inj_exception,kvm:kvm_inj_virq,sched:sched_switch,sched:sched_wakeup' \
  -- /tmp/run-d15-segv.sh

# Per-irq snapshot (look for sudden growth in IPI lines during failure):
cat /proc/interrupts | grep -E '^(IPI|TLB|RES|CAL)'
```

### 2e. /sys/kernel/debug/kvm spot-check at panic

```bash
# At the moment of failure (e.g. inside a wait/abort path), grab everything:
PID=$(pgrep -f 'backend=force=kvm-v2' | head -1)
sudo cp -r /sys/kernel/debug/kvm/${PID}-*/ /tmp/kvm-snap-$(date +%s)/
```

Files of interest under that directory: `vcpu*/` (one per vCPU),
each containing `vcpu_stat` (entry/exit counts), `tlb_flush`,
`signal_exits` — for the v2 dispatch model see §3.

---

## §3 — KVM debugfs spelunking

`/sys/kernel/debug/kvm/` exposes one directory per VM (named
`<host-pid>-<vm-id>`) and one subdirectory per vCPU. Verified live
files include:

| File | Meaning under v2 dispatch | Useful for |
|------|---------------------------|------------|
| `exits` | total vmexits this VM has taken | sanity vs T-count |
| `tlb_flush` | KVM_REQ_TLB_FLUSH service count | spike = guest TLB churn |
| `tlb_flush_guest` | guest-initiated flushes (INVLPG, CR3-load) | high = our CR4.PGE toggle is firing |
| `remote_tlb_flush_requests` | cross-vCPU TLB IPI requests | cross-check with `@kicks` from §2c |
| `halt_attempted_poll` | # of times KVM polled before HLT-blocking | low under v2 (we don't HLT in guest much) |
| `host_state_reload` | times host had to reload MSRs/segment regs after vmexit | every signal_exits/EINTR contributes |
| `signal_exits` | KVM_EXIT_INTR (SIGALRM/IPI_SIGNAL etc) | the v2 cooperative-preempt indicator |
| `pf_taken` / `pf_fixed` / `pf_guest` | guest #PF accounting | should track user-page-fault rate |

Per-vCPU directories (`vcpu*/`) carry the same counters scoped to
one vCPU. For the SMP T=8 flake the canonical comparison is:

```bash
PID=$(pgrep -f 'backend=force=kvm-v2' | head -1)
DIR=/sys/kernel/debug/kvm/${PID}-*
for v in $DIR/vcpu*/; do
    printf 'cpu=%s exits=%s sig=%s tlb=%s host_reload=%s\n' \
        "$(basename $v)" \
        "$(cat $v/exits 2>/dev/null)" \
        "$(cat $v/signal_exits 2>/dev/null)" \
        "$(cat $v/tlb_flush 2>/dev/null)" \
        "$(cat $v/host_state_reload 2>/dev/null)"
done
```

A vCPU that has *zero* `tlb_flush` while the others tally ≥1
proves stale-TLB on that vCPU directly — that's the smoking gun.

---

## §4 — Crash-time forensics

### 4a. The DIAG[] panic dump

`arch/um/kernel/um_diag.c:72` emits one `pr_info` per recorded
mm-syscall event (see also LOW-PF in `syscall_trap.c:1121`). Capture
it from the panic console:

```bash
$LINUX/.../linux backend=force=kvm-v2 ... 2>&1 | tee /tmp/panic.log
grep -E 'um: DIAG\[|LOW-PF|D15-' /tmp/panic.log | sort -k4n
```

mt-byteset typically yields ~512 events; the LAST few before the
SIGSEGV name the offending (cr2, gp[SP/BP/DX/AX]) tuple.

### 4b. bpftrace ringbuf tail of the last N kvm:kvm_exit events

```bash
sudo bpftrace -e '
  BEGIN { @ringsz = 64; }
  tracepoint:kvm:kvm_exit {
      @ring[@idx % @ringsz] = (pid, cpu, args->vcpu_id,
                               args->exit_reason, args->guest_rip);
      @idx++;
  }
  END {
      print(@ring);
  }
' &
sudo /tmp/run-d15-segv.sh   # let it crash; ^C bpftrace immediately after
```

### 4c. gdb attach to the UML host process

```bash
PID=$(pgrep -f 'backend=force=kvm-v2' | head -1)
sudo gdb -p $PID \
   -ex 'set pagination off' \
   -ex 'thread apply all bt' \
   -ex 'p (struct kvm_v2_vcpu) *vcpus[0]' \
   -ex 'p current->thread.regs.regs.gp' \
   -ex 'detach' -ex quit
```

For per-vCPU state, the symbol is `vcpus[NR_CPUS]` (vcpu.c:92);
walk it as a struct array. `current` is per-host-thread — `thread
apply all` will give you each pthread's gp[].

### 4d. crash(8) on a saved core

```bash
# Coredump the live host process:
sudo gcore $PID                  # → core.<pid>
# Inspect with crash-style helpers (UML host is just userspace):
gdb $UML core.$PID -ex 'thread apply all bt' -ex quit
```

There is no crash(8)-style kernel-side introspection because the
v2 backend is a *userspace* host — `crash` is the wrong tool.
`gdb` plus the printers above is the supported path.

---

## §5 — A/B test scaffolding

### 5a. Two builds (UP vs SMP) at the same git tip

```bash
GIT=$LINUX
TIP=$(git -C $GIT rev-parse HEAD)
mkdir -p ~/src/uml-builds
for cfg in up smp; do
    OUT=~/src/uml-builds/uml-${TIP:0:8}-${cfg}
    git -C $GIT worktree add $OUT $TIP
    case $cfg in
      up)  EXTRA="--disable CONFIG_SMP" ;;
      smp) EXTRA="--enable  CONFIG_SMP" ;;
    esac
    make -C $GIT O=$OUT ARCH=um defconfig
    $OUT/scripts/config --file $OUT/.config $EXTRA
    make -C $GIT O=$OUT ARCH=um -j$(nproc) >/dev/null
done
```

Build dir lives on `/`, **not** `/tmp` — see auto-memory note on
tmpfs fill silently breaking Bash stdout.

### 5b. Quick mt-mini soak script

The reference is `/tmp/run-d15-segv.sh` (read it first; it's only
27 lines). To re-target for mt-mini T=8:

```bash
cp /tmp/run-d15-segv.sh /tmp/run-mtmini-T8.sh
sed -i 's|mt-byteset 4|mt-mini 8|' /tmp/run-mtmini-T8.sh
sed -i 's|seq 1 60|seq 1 30|'      /tmp/run-mtmini-T8.sh
chmod +x /tmp/run-mtmini-T8.sh
/tmp/run-mtmini-T8.sh
```

### 5c. Decision matrix from the SUMMARY line

After each soak, the script prints `SUMMARY PASS=X FAIL=Y`. Map
to a verdict against the previous session's baseline (record it):

| Δ PASS-rate | Verdict |
|-------------|---------|
| ≥ +95% absolute, FAIL=0 over 30+ iters | **fixed** |
| +20% to +94%, FAIL > 0 | **improved** (partial; keep digging) |
| ±19% (within noise) | **unchanged** (your patch is a no-op for this signal) |
| ≤ -20% | **regressed** (revert; don't try to whittle it) |

Threshold rationale: 30 iters at 50% baseline gives a 1σ of ~9%
on the proportion, so ±19% ≈ 2σ — the right "no signal" band.

---

## §6 — Suggested instrumentation patches (commented in source)

These are pre-formed `pr_emerg` snippets used during the v2 audit.
Keep them commented in the source so future investigations can
flip them on without re-deriving the call site. All use **bounded
counters** (atomic) so a runaway bug doesn't drown the console.

### 6a. LOW-PF (already wired — `syscall_trap.c:1121`)

Captures the full register tuple when cr2 is suspiciously low
(<0x10000). The existing format is the pattern:

```c
pr_emerg("um: kvm-v2 LOW-PF[%d] pid=%d cr2=%llx err=%llx user_rip=%llx "
         "user_rsp=%llx user_rbp_or_rdx=%lx gp[BP]=%lx gp[SP]=%lx "
         "gp[DX]=%lx gp[AX]=%lx gp[DI]=%lx\n",
         atomic_inc_return(&low_pf_seq), task_pid_nr(current),
         (u64)cr2, (u64)err, (u64)user_rip, (u64)user_rsp, captured_rdx,
         regs->gp[HOST_BP], regs->gp[HOST_SP], regs->gp[HOST_DX],
         regs->gp[HOST_AX], regs->gp[HOST_DI]);
```

### 6b. D15-SAVE / D15-RESTORE (used during #121, leave commented)

Drop in around `kvm_v2_handle_pf_eintr_inline` to record cr2
snapshot/restore parity. Bound to first 64 events:

```c
static atomic_t d15_seq = ATOMIC_INIT(0);
if (atomic_inc_return(&d15_seq) <= 64)
    pr_emerg("um: D15-SAVE pid=%d cpu=%d saved_cr2=%llx live_cr2=%llx\n",
             task_pid_nr(current), smp_processor_id(),
             (u64)current->thread.arch.kvm_v2.saved_cr2,
             (u64)read_cr2_via_sregs(vcpu));
/* ... at restore: */
if (atomic_read(&d15_seq) <= 64)
    pr_emerg("um: D15-RESTORE pid=%d cpu=%d restored=%llx\n",
             task_pid_nr(current), smp_processor_id(),
             (u64)current->thread.arch.kvm_v2.saved_cr2);
```

### 6c. TLB-GEN-DELTA (the bug-candidate of the moment)

Drop into `kvm_v2_load_user_sregs` (vcpu.c near :1270) to log when
a vCPU's last-seen tlb-gen lags significantly behind `mm->context.tlb_gen`:

```c
{
    u64 cur = atomic64_read(&active_mm->context.tlb_gen);
    u64 seen = atomic64_read(&vcpu->last_seen_tlb_gen);
    static atomic_t tg_seq = ATOMIC_INIT(0);
    if (cur > seen + 1 && atomic_inc_return(&tg_seq) <= 128)
        pr_emerg("um: TLB-GEN-DELTA pid=%d cpu=%d vcpu=%d cur=%llu seen=%llu Δ=%llu\n",
                 task_pid_nr(current), smp_processor_id(), vcpu->vcpu_id,
                 cur, seen, cur - seen);
}
```

A consistent Δ ≥ 1 on the failing vCPU is direct evidence for
hypothesis (C) from layer 4.

### 6d. KICK-FIRE / KICK-SUPPRESS (the dedup audit)

Inside `kvm_v2_tlb_kick_others` (vcpu.c:820, around the cmpxchg at :860):

```c
static atomic_t kick_seq = ATOMIC_INIT(0);
if (atomic_cmpxchg(&v->kick_pending, 0, 1) == 0) {
    if (atomic_inc_return(&kick_seq) <= 256)
        pr_emerg("um: KICK-FIRE src_pid=%d src_cpu=%d dst_cpu=%d gen=%llu\n",
                 task_pid_nr(current), smp_processor_id(), cpu, cur_gen);
    (void)os_send_ipi(cpu, 0);
} else {
    if (atomic_inc_return(&kick_seq) <= 256)
        pr_emerg("um: KICK-SUPPRESS src_pid=%d src_cpu=%d dst_cpu=%d "
                 "(pending already set)\n",
                 task_pid_nr(current), smp_processor_id(), cpu);
}
```

Cross-correlate with §2c's bpftrace `@kicks` vs `@ipi` —
KICK-SUPPRESS lines with no later FIRE for the same dst_cpu mean
that target vCPU never got a fresh kick after the suppress, and
its last_seen_tlb_gen will lag.

---

## Closing note

Every recipe here is **read-only against the kernel source** (§1)
or **observation-only against a running UML** (§2-§5) or
**comment-by-default in the source** (§6). The audit is
deliberately conservative: surface bugs by mechanical comparison,
do not patch until layer 3's matrix has a specific cell flagged
incorrect. See the anti-pattern note in
[00-overview.md](00-overview.md).
