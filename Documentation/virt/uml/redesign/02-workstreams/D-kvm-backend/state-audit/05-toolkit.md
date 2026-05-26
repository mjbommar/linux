# UML KVM v2 — State-Audit Toolkit (recipes)

**Created:** 2026-05-01
**Tip at creation:** `e5f231656ead`
**Last refresh:** 2026-05-02 (post SMP-T22, tip `1a714fdbad84`) — added
§7 (in-tree runtime instrumentation), §8 (umlctl observability spine),
§9 (per-bug-class diagnostic patterns + memo cross-reference), §10
(multi-agent investigation playbook).

**Audience:** anyone (human or subagent) filling in the matrix
([03](03-state-ownership-matrix.md)), chasing a fresh mt-mini /
threaded-subprocess-wait flake, or arriving at a flake-investigation
turn with five minutes to context-load. Every command was verified
against the live tree — the `rg` patterns hit ≥1 line, the
bpftrace/ftrace recipes use real tracepoint names from
`arch/x86/kvm/trace.h`, every state-trace API call has a real call
site in `arch/um/backend/kvm-v2/`.

**§1-6 are static + bpftrace recipes (the "read-the-tree" toolkit).
§7-10 are the in-tree runtime instrumentation + observability +
historical investigation pattern.** When debugging a fresh flake,
START at §10 (multi-agent investigation), then drop into §7-8 for
specific tools, then §1-6 for static cross-checks.

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

## §7 — In-tree runtime instrumentation (added post-2026-05-01)

These ship in the kernel image when `CONFIG_UM_BACKEND_KVM_V2=y`
and either auto-fire on specific anomalies or are toggled at boot
or via debugfs. Use them BEFORE inventing new bpftrace recipes —
they capture v2-specific state at exactly the granularity v2's
state-evolution model requires.

### 7a. State-trace ring (`arch/um/backend/kvm-v2/state_trace.c`)

A 3-layer-shielded per-CPU ring buffer that captures ~30 v2 state
fields (KVM regs/sregs, `arch_thread.kvm_v2.*`, mm tlb_gen,
vCPU last_seen_tlb_gen + current_mm + kick_pending, and the LIVE
IST page top-48..top-8 bytes) at every operation transition the
state-audit Layer 2 inventoried.

Three layers shield production:

1. `CONFIG_UM_BACKEND_KVM_V2_STATE_TRACE=n` — every hook compiles
   to `do {} while (0)`. Zero text bytes, no struct sizes change.

2. `CONFIG=y` but disabled at runtime — each hook is a single
   `static_branch_unlikely` test → 5-byte NOP when disabled.

3. Per-CPU ring storage; no locks, no cross-CPU cacheline traffic.
   Default 2 MB / CPU (~5400 entries × ~384 B); adjustable via
   debugfs `ring_bytes`.

**Boot-time enable** (`d43bb3423aa0`):
```bash
# Append to UML kernel cmdline. Note: this slows v2 by ~36us per
# dispatch (18 ops × ~2us each). Strong empirical signal: with this
# enabled, the SMP fork-stress race from threaded-subprocess-wait.py
# changes from ~30% fail rate to <10% (Heisenbug suppression — see
# state-audit/11 for the analysis).
$UML_BINARY backend=kvm-v2 ... kvm_v2_trace_enable
```

**Runtime control** (debugfs):
```bash
# Inside the running UML guest, or from host via /sys/kernel/debug:
echo 1 > /sys/kernel/debug/um_kvm_v2_trace/enabled    # turn on
echo 0 > /sys/kernel/debug/um_kvm_v2_trace/enabled    # turn off (also dumps)
echo 1 > /sys/kernel/debug/um_kvm_v2_trace/dump       # one-shot dump to dmesg
echo 1 > /sys/kernel/debug/um_kvm_v2_trace/clear      # zero the ring
cat   /sys/kernel/debug/um_kvm_v2_trace/ring_bytes    # current capacity
echo 4194304 > /sys/kernel/debug/um_kvm_v2_trace/ring_bytes  # 4 MB / CPU
```

**Programmatic dump** from kernel code (post-mortem on detected
anomaly):
```c
kvm_v2_state_trace_dump("Bug B: user_rip in handlers range");
```

**Auto-freeze patterns** (`state_trace.c::kvm_v2_state_trace_capture`):

The ring atomically freezes on first hit of any of these conditions
so subsequent dispatches don't overwrite the moment-of-failure
context:

- `KVMV2_OP_HANDLE_SYSCALL_POST` + `regs->gp[HOST_ORIG_AX] == __NR_mmap`
  + `regs->gp[HOST_AX] == 0` — mt-mini MMAP_NULL anomaly (mmap
  returned 0 to user, indicating Bug A class).
- `KVMV2_OP_TRACE_TRIGGER` — fired by `handle_io_pf` when
  `frame.user_rip` lands in `KVM_V2_HANDLERS_GVA` range (Bug B class —
  iretq popped a kernel-half RIP).

Both emit `pr_emerg("KVMV2T_ANOMALY ... — froze trace ring")` so
log scrapers can find them.

**Reading a dump:** the dumper at `dump_one()` prints two lines per
entry — `KVMV2T-S` (sync-regs / sregs / per-task / per-mm) and
`KVMV2T-I` (live IST top-48..top-8). A Python parser exists at
`tools/uml/state_trace_parser.py` (commit `e9a54318e301`) that
re-orders and diffs across cpus.

### 7b. Per-task `arch_thread.kvm_v2` snapshot fields

These are the only mutable per-task v2 state outside `regs->gp[]`.
When a flake fingers a per-task issue, dump these:

| Field | Purpose | Set by | Cleared by |
|---|---|---|---|
| `fpu` + `fpu_valid` | fork-time FPU snapshot, restored on first KVM_RUN | `kvm_v2_fpu_capture_for_fork` (vcpu.c:2063) | `arch_flush_thread` (execve) |
| `iotrap_fpu` + `iotrap_fpu_valid` | post-vmexit FPU snapshot, restored before next KVM_RUN | `kvm_v2_vcpu_run` post-KVM_RUN (vcpu.c:1782) | post-restore in load path (vcpu.c:1743) |
| `ist_frame[0..5]` + `ist_pending` | per-task IST iretq frame snapshot/restore | `kvm_v2_ist_frame_write`, `kvm_v2_ist_frame_snapshot_raw` | `kvm_v2_ist_frame_restore_pending` consumes |
| `saved_cr2_at_eintr` + `saved_cr2_valid` | preserve sregs.cr2 across EINTR-mid-IDT (Bug A T16) | EINTR-with-stub-RIP path (vcpu.c:1894) | `load_user_sregs` consumes (vcpu.c:1697) |
| `nm_ts_bypass` | one-shot CR0.TS arming bypass after #NM (T22) | `handle_io_nm`, `handle_nm_eintr_inline` | `load_user_sregs` (vcpu.c:1416) |

Reset on `arch_flush_thread` (execve) and `arch_copy_thread` (fork) —
see `arch/x86/um/asm/processor_64.h:127-172`.

### 7c. Auto-firing diagnostic emitters in dmesg

Search dmesg for these prefixes when triaging:

| Prefix | Source | What it means |
|---|---|---|
| `KVMV2T_DUMP_BEGIN` ... `KVMV2T_DUMP_END` | state-trace ring dump | Post-mortem capture of recent dispatches |
| `KVMV2T_ANOMALY` | state_trace.c:307,323 | Auto-freeze fired (mmap-NULL or trace-trigger) |
| `KVM_V2_TLB_LAG cpu=N pid=P mm=M last=X cur=Y lag=Z` | vcpu.c:1355 | Remote vCPU's `last_seen_tlb_gen` is `lag` generations behind `cur_gen`. Lag > 1000 indicates page-recycling-class risk; lag < 100 is normal. |
| `um: kvm-v2 BUG_B user_rip=...` | syscall_trap.c:1232 | iretq landed user mode at a kernel-half RIP. Bug B class — see state-audit/10, 11 |
| `um: kvm-v2 IST_FRAME_WRITE_KERNEL_RIP[N]` | syscall_trap.c:887 | Host code attempted to write a kernel-half RIP into IST top-40. Caught by SMP-T19c guard. |
| `um: kvm-v2 #PF[N] cr2=0 captured=...` | syscall_trap.c:1160 | Bug A diagnostic — sregs.cr2 was 0 but stub captured a non-zero cr2 from the IST page slot (cr2-clobber by intervening vCPU swap). |
| `um: kvm-v2 LOW-PF[N] pid=...` | syscall_trap.c:1183 | Low-address (1..0xff) #PF from mt- task — T>=N stress flake signature. |
| `um: DIAG[N] t=... nr=... addr=... len=... prot=... ret=...` | mm-syscall ring (separate from state-trace) | Last 512 mmap/munmap/mprotect events before panic. Dumped from panic path. |

### 7d. Stub-internal diagnostics (#PF stub captures)

The #PF stub at `KVM_V2_HANDLERS_GVA + 0x140` writes to the IST
page above the iretq frame:

| Slot offset (from `vcpu->ist_stack_kva + PAGE_SIZE`) | Captured |
|---|---|
| -64 | `captured_cr2` — `mov %cr2, %rax` then store |
| -72 | `captured_rdx` — user RDX at fault time |
| -80 | sentinel (proves stub ran) |

`handle_io_pf` reads these with priority over `sregs.cr2` when
`sregs.cr2 == 0` (Bug A defense). See `syscall_trap.c:1149-1200`.

---

## §8 — umlctl observability spine

`tools/uml/uml-launcher/` provides the canonical "boot UML, run a
workload, capture what happened, decide pass/fail" interface.
Always use this rather than direct `$UML_BINARY init=...` invocation
when the workload needs env (PATH, HOME, TERM), pseudo-FS mounts
(/proc, /sys, /dev/pts, /dev/shm, /tmp), or networking — the auto-
generated init script handles them. Direct invocation reproducers
break in subtle ways that look like kernel bugs but are env bugs;
see commits `0cc99f99b6dd`, `e9d6249a6c18`.

### 8a. Umlfile (the input)

Declarative TOML at `tools/uml/uml-launcher/examples/`. Canonical
templates: `cpython-test.toml`, `dev.toml`, `fastapi.toml`,
`fuzz.toml`, `research.toml`, `sandbox.toml`. Schema doc in the
example file headers; here's the minimal kvm-v2 SMP form:

```toml
schema_version = 1
[instance]
name = "my-test"
[kernel]
path    = "${UML_KERNEL}"
backend = "kvm-v2"          # or "seccomp" for A/B
append  = []                # add "kvm_v2_trace_enable" for §7a
[runtime]
mem    = "2048M"
ncpus  = 4
[network]
mode = "none"
[env]
TMPDIR = "/tmp"
PYTHONHASHSEED = "0"
[[init.phases]]
name = "smoke"
cmd  = "python3 -c 'import sys; print(repr(sys.executable))'"
[[init.phases]]
name = "repro"
cmd  = "python3 /path/to/your/reproducer.py"
[[init.phases]]
name = "report"
cmd  = "echo REPRO_DONE rc=$?"
[debug]
strace                   = false   # set true to host-side strace UML
gdb                      = false   # set true to drop a gdb stub on UML
log_dir                  = ""      # capture artifacts here
keep_running_on_failure  = false   # leave UML running for post-mortem
```

### 8b. Verbs

| Verb | What it does |
|---|---|
| `umlctl up -f FILE` | Compile Umlfile → spawn UML; init.phases run sequentially; abort on first phase rc != 0 |
| `umlctl down -f FILE` | Tear down (signal + wait + reap) |
| `umlctl ps` | List running instances |
| `umlctl logs NAME [--tail N]` | Tail per-instance console (init.log) |
| `umlctl dmesg NAME` | Filter to kernel-printk only |
| `umlctl events NAME` | Tail structured `events.jsonl` from observability spine |
| `umlctl metrics NAME` | One-shot host-side scrape of `/proc/<pid>/*` + cgroup v2 |
| `umlctl assert NAME PREDICATE` | CI-style predicate check against `events.jsonl`; exits 1 on violation |
| `umlctl gate -g GATE_FILE` | Sealed-wrapper test gate; appends scoreboard.jsonl row |
| `umlctl schema` | List declared observability event schemas |
| `umlctl export NAME` | Archive run bundle as `.umlbundle.tar.zst` |

### 8c. The 20-boot loop pattern (canonical for flake characterization)

```bash
UMLCTL=$PWD/tools/uml/uml-launcher/target/release/umlctl
export UML_KERNEL=$HOME/src/uml-builds/uml-smp/linux
OUT=/tmp/my-flake-test
mkdir -p $OUT

PASS=0 FAIL=0
for i in $(seq 1 20); do
  $UMLCTL down -f /tmp/my-test.toml > /dev/null 2>&1 || true
  sleep 1
  $UMLCTL up -f /tmp/my-test.toml > $OUT/up-$i.log 2>&1
  for j in $(seq 1 60); do                                # 120s max wait
    sleep 2
    if $UMLCTL logs my-test 2>&1 | grep -q "REPRO_DONE\|panic"; then
      break
    fi
  done
  LOG=$OUT/run-$i.log
  $UMLCTL logs my-test > $LOG 2>&1
  if grep -q "REPRO_DONE rc=0" $LOG; then
    PASS=$((PASS+1)); echo "run $i: PASS"
  else
    FAIL=$((FAIL+1))
    SIG=$(grep -E "(segfault|FAIL|PYRC1|BUG_B)" $LOG | head -2 | tr '\n' '|')
    echo "run $i: FAIL [$SIG]"
  fi
  $UMLCTL down -f /tmp/my-test.toml > /dev/null 2>&1 || true
done
echo "==> PASS=$PASS/20 FAIL=$FAIL/20"
```

### 8d. A/B vs seccomp in one command

Identical Umlfile, swap `[kernel].backend = "seccomp"`. Run both
loops, diff the FAIL signature distributions. If a signature shows
ONLY under `kvm-v2`, it's v2-specific (the bug's domain). If it
shows under both, the workload itself is racy — file a separate
bug, don't chase it through v2 changes.

---

## §9 — Per-bug-class diagnostic patterns + memo cross-reference

Investigation history of each closed (or still-open) bug class. When
a new flake's signature looks adjacent, start at the relevant memo
to avoid re-deriving the analysis.

| Bug class | Signature | Memo | Status | Key fix commit |
|---|---|---|---|---|
| Bug A — cr2 clobber | `mt-init.X[]: segfault at 0 ip e_entry error 14` | [09](09-smp-t16-bug-a-FIXED.md) | FIXED | `2f0c87a47dcb` (T16) |
| Bug B — iretq pops kernel-half RIP (EINTR variant) | `mt-mmap-stress[]: segfault at HANDLERS_GVA ip HANDLERS_GVA error 15` | [10](10-smp-t17-bug-b-FIXED.md) | FIXED | `85244f068ad3` (T17) |
| Bug B class — non-EINTR iretq | `python3[]: segfault at 0 ip ffffe000000021c2 error 20` | [11](11-smp-t22-bug-b-class-FIXED.md) | FIXED via vmexit-on-#NM | `ddf3cfe5cf31` (T22) |
| Bug class — page recycling under fork+exec | `python3[]: segfault at <high cr2> ip <legit user ip> error 4/6` | [11](11-smp-t22-bug-b-class-FIXED.md) §residual | partially addressed via T20 RCU defer; still investigating | `d9ed9e14c7b6` |
| Bug class — child-startup faults (post-T22 residual) | `[wN iter=N] PYRC1: rc=-11/1 ...` (decomposes into ld-linux 0x10896 SIGSEGV / Python init OSError ESPIPE) | [11](11-smp-t22-bug-b-class-FIXED.md) §residual | OPEN (task #193) | TBD |
| TLB-lag / cross-vCPU shootdown | `KVM_V2_TLB_LAG ... lag=1000+` | [08](08-smp-t13-FIXED.md) | mitigated | `7e1c255a09ad` + activated `ad18db7c3768` |
| First-KVM_RUN-of-new-exec NULL deref | `MMAP_NULL` printk | [09](09-smp-t16-bug-a-FIXED.md) | FIXED | `2f0c87a47dcb` |
| EINTR mid-stub RAW snapshot/replay | `KVMV2T_ANOMALY` + Bug B signature | [10](10-smp-t17-bug-b-FIXED.md) | FIXED | `85244f068ad3` |

For each closed bug, the memo includes: signature, root cause,
state-trace ring smoking-gun example, fix, regression-free
verification. **If the memo says "FIXED" but you observe the
signature**, FLAG immediately — a regression has been introduced
somewhere in the post-fix chain.

---

## §10 — Multi-agent investigation playbook (for hard non-deterministic bugs)

**When to use:** the bug is non-deterministic, spans multiple state
machines, has ≥2 plausible root-cause families, and a single agent
spent more than two iteration cycles without convergence.

**Pattern (validated 2026-05-02 on the v2 SMP fork-stress flake):**

1. **Codex 5.5 xhigh + search** — kicks off in the background.
   Best for: code-walking the flow end-to-end and proposing
   architecturally-plausible reorders (e.g., "move unblock_signals
   AFTER the IST read"). Sample prompt at `/tmp/flake-sweep-2026-05-02/codex-prompt.md`.

2. **Three Opus subagents in parallel**, each on ONE angle. Pattern
   (from this session):
   - **Angle 1**: per-task state corruption (IST frame, FPU,
     arch_thread.kvm_v2 fields). Reads syscall_trap.c, processor_64.h.
   - **Angle 2**: page-recycling / mmu_gather lifecycle. Reads
     tlb.c, kvm_v2_backend.h, skas/mmu.c.
   - **Angle 3**: mm_id / CR3 / kernel-half PT propagation. Reads
     exception.c (kernel-half install), mem.c (pgd_alloc).

   Each angle's prompt MUST include the failure signature, the
   already-attempted fixes (so the agent doesn't re-propose them),
   absolute file paths, and the line "be skeptical of your own
   hypothesis."

3. **Synthesis turn**: collate the four reports, identify the angles
   where ≥2 agents converge (high-confidence), the angles where one
   stands alone (test the prediction first), and the angles ruled
   out (deprioritize). Implement the converged fix as Phase A,
   gate-test, then iterate.

**Cost discipline:** an Opus subagent run averages ~$5-8 in input
tokens. Three in parallel + one synthesis is ~$25-30. Worth it for
a >1-day-stuck flake; overkill for a same-day fix.

**Output artifacts to keep:**
- `/tmp/flake-sweep-YYYY-MM-DD/codex-prompt.md` and `codex-output.md`.
- `~/.claude/projects/.../subagents/agent-*.jsonl` for the Opus
  transcripts — these are your historical record, NOT ephemeral.

The state-audit/11 memo cites the 2026-05-02 instance as the
template for this pattern.

---

## Closing note

Every recipe here is **read-only against the kernel source** (§1)
or **observation-only against a running UML** (§2-§5, §7-§8) or
**comment-by-default in the source** (§6) or
**post-mortem-only narrative** (§9-§10). The audit is
deliberately conservative: surface bugs by mechanical comparison,
do not patch until layer 3's matrix has a specific cell flagged
incorrect. See the anti-pattern note in
[00-overview.md](00-overview.md).
