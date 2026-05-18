# kvm-v2 Django Flake — Deeper-Layer Debugging Plan

**Status:** Forward-looking plan. Not yet executed (Round 13+).

**Context:** After 12 rounds of investigation (see
[50-kvm-v2-django-flake-investigation-summary.md](50-kvm-v2-django-flake-investigation-summary.md)),
the bug is narrowed but not closed. Confirmed:

- Bytecode bytes read as zero in CPython's heap (ground-truth via
  patched CPython dump).
- LSTAR gadget is causally involved (`CONFIG_UM_BACKEND_KVM_V2_GADGET=n`
  gives 0 cache hits across 120 iters).
- NOT in entry-save (T60 audit, 0 SAVE-slot mismatches).
- NOT in user-syscall layer (bpftrace uprobes: zero
  munmap/mmap/madvise/mremap targeting the bytecode page).

The remaining mechanism is 1–2 layers down — in KVM's internal
TDP/SPTE management. The CPython patch + host bpftrace uprobes +
cscope cross-references have hit their resolution limit.

This document records the next deeper-instrumentation techniques,
each with rationale, exact commands, expected signal, and pitfalls.

Passwordless sudo is available on the investigation host, so all
of these techniques are runnable without escalating the access
model.

---

## Technique A — `perf record -a -g -e 'kvm:*,kvmmmu:*'`

### Rationale

This is the single highest-leverage next step. `perf record -g`
captures **kernel call stacks** at every KVM tracepoint hit. When
an SPTE for the bytecode page's GFN gets invalidated, we see
exactly which host kernel function triggered it:

- `kvm_mmu_notifier_invalidate_range_start` → some mm op fired
  mmu_notifier
- `kvm_mmu_zap_collapsible_sptes` → THP collapse merged 4 KB SPTEs
  into a 2 MB SPTE
- `kvm_mmu_invalidate_zap_pages_in_memslot` → memslot operation
- `kvm_arch_mmu_notifier_invalidate_range` → host mm reclaim

Round 11's host-uprobes ruled out the **user-mm-op layer**.
`perf record -g` reaches **one layer deeper** — the host KVM
kernel code path that mutates the SPTE. Combined with `perf script`
post-processing, we get a flat list:

```
TIMESTAMP  CPU  PID  COMM    kvmmmu:kvm_tdp_mmu_spte_changed  gfn=GGG ...
              -> __kvm_mmu_invalidate_range_start
              -> kvm_mmu_notifier_invalidate_range_start
              -> __mmu_notifier_invalidate_range_start
              -> tlb_finish_mmu
              -> ... (mm operation that originated the invalidation)
```

The originating mm operation is the **smoking gun**.

### Setup

```bash
# 1. Ensure perf has access to tracepoint events
sudo sysctl kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0

# 2. Pre-flight: list tracepoints to confirm they're reachable
sudo perf list 'kvm:*' 'kvmmmu:*' | head -40

# 3. Record with -g (call graph), -a (system-wide), filter to the
#    KVM + KVMMMU subsystems
sudo perf record -a -g \
    -e 'kvmmmu:*,kvm:kvm_unmap_gfn_range,kvm:kvm_age_hva,kvm:kvm_invalidate_range_start' \
    -o /home/mjbommar/src/r13-perf/kvm.perf.data \
    -- timeout 180 bash -c '
        UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r8-t58/linux \
        UMLCTL=/home/mjbommar/projects/personal/linux/tools/uml/uml-launcher/target/release/umlctl \
        bash tools/testing/selftests/um/soak/run-soak-daemon.sh \
            --budget-sec 150 --backends kvm-v2 \
            --workloads django-loopback-none \
            --workers 1 --iters-per-rotation 60 \
            --continue-on-fail-threshold \
            --out /home/mjbommar/src/r13-perf/dj-perf
    '
```

### Post-Processing

```bash
# Generate human-readable script. The stack frames between events
# tell us what host code chain triggered each SPTE change.
sudo perf script -i /home/mjbommar/src/r13-perf/kvm.perf.data \
    > /home/mjbommar/src/r13-perf/kvm.perf.script

# Find the bytecode GFN. CPython's heap GVA → GPA via UML guest pgd
# walk is hard to compute offline, but we can find candidates by:
# (1) reading the patched-CPython dump's instr_ptr (e.g., 0x550000818f78)
# (2) the GVA is in [heap] anon mapping
# (3) UML guest pgd entries for that VA → GPA
# (4) GFN = GPA >> 12

# Filter perf.script for SPTE changes around the cache-abort time.
# Look for kvm_unmap_gfn_range whose gfn maps to the bytecode page.
grep -B0 -A30 "kvm_unmap_gfn_range\|kvm_tdp_mmu_spte_changed" \
    /home/mjbommar/src/r13-perf/kvm.perf.script
```

### Expected Signal

If the bug is THP-collapse-related: stack ends in
`kvm_mmu_zap_collapsible_sptes` → `kvm_mmu_invalidate_zap_pages_in_memslot`.

If it's an mmu_notifier from host mm reclaim: stack ends in
`kvm_arch_mmu_notifier_invalidate_range` →
`__mmu_notifier_invalidate_range_start` → some `unmap_*` /
`__do_munmap` / `madvise_dontneed` / `wp_page_copy`.

If it's KVM's own zapping (e.g., NX-lpage recovery split): stack
ends in `kvm_recover_nx_huge_pages` / `kvm_mmu_zap_oldest_mmu_pages`.

### Pitfalls

- Volume: `kvmmmu:kvm_tdp_mmu_spte_changed` fires ~150K times per
  second during a Django soak. Even at perf's reduced cost, the
  recording will be 1–3 GB for a 3-minute soak. Stash on `~/src/`
  (`/` filesystem with 768 GB free), NOT `/tmp` (tmpfs).
- Call-graph capture has measurable overhead (~10–30 % for hot
  KVM workloads). Bug may or may not Heisenbug-suppress.
- `perf_event_paranoid=-1` is required to capture kernel stacks
  as a non-root user via sudo (the perf process itself runs as
  root via sudo, so this is more about post-processing).

---

## Technique B — `kvmmmu` kprobes filtered to the failing pid

### Rationale

The kvmmmu tracepoints fire system-wide. With a per-vCPU/per-pid
filter, we get the SPTE manipulations for OUR python's GFNs only,
shrinking the trace volume 10–100×.

Round 12's `kvm_tdp_mmu_spte_changed` capture gave 919K events.
With a `pid==target` filter, we'd narrow to events for our
umlctl/UML process specifically.

### Setup

```bash
# Identify the failing UML pid at runtime. Pre-launch soak,
# then attach bpftrace once we know the pid.

# Alternative: use bpftrace's BEGIN block + uprobe to capture pid
# at process start, then use that pid for filters.

cat > /home/mjbommar/src/r13-filtered/kvmmmu-filtered.bt <<'EOF'
#!/usr/bin/env bpftrace
// Filter kvmmmu events to the failing python's UML kernel process.
// "linux" is the UML kernel's host-side comm.

BEGIN { printf("# T63b filtered kvmmmu trace start\n"); }

tracepoint:kvmmmu:kvm_tdp_mmu_spte_changed /comm == "linux"/ {
    // Capture every SPTE change with GFN
    @spte_writes[args->gfn] = count();
    if (args->new_spte == 0) {
        printf("%llu SPTE_ZERO gfn=%lx level=%d old=%lx\n",
            nsecs, args->gfn, args->level, args->old_spte);
    }
}

tracepoint:kvmmmu:kvm_mmu_prepare_zap_page /comm == "linux"/ {
    printf("%llu PREPARE_ZAP\n", nsecs);
}

tracepoint:signal:signal_generate /args->sig == 6 && comm == "python3.14"/ {
    printf("%llu *** SIGABRT *** to pid=%d\n", nsecs, args->pid);
    // dump top 20 GFNs with most SPTE writes — narrows
    // the bytecode-page candidate set
    print(@spte_writes, 20);
}

interval:s:600 { exit(); }
EOF
sudo bpftrace /home/mjbommar/src/r13-filtered/kvmmmu-filtered.bt \
    > /home/mjbommar/src/r13-filtered/out.log 2>&1 &
```

### Post-Processing

When a SIGABRT fires for python3.14, the top-20 GFN histogram dumps.
GFNs that show up just before SIGABRT are the candidate bytecode
pages. Cross-correlate against the patched-CPython dump's
`instr_ptr / start` to identify the exact GFN.

### Expected Signal

A GFN appears MANY times in @spte_writes just before SIGABRT,
indicating repeated SPTE invalidation of the bytecode page.
Comparing the GFN's translation against the host page table
reveals what HPA it ultimately maps to, and whether the host
side replaced it with a fresh anon page.

### Pitfalls

- The `comm == "linux"` filter catches UML's kernel thread, but
  bpftrace's `comm` is the host-process comm. UML's pthreads
  all inherit "linux" comm from the parent. Should work but verify.
- Bpftrace `@spte_writes[args->gfn]` map can blow up to millions
  of GFN entries. Use a bounded LRU map or sample every Nth event.

---

## Technique C — ftrace `function_graph` on KVM internals

### Rationale

`function_graph` gives entry/exit + call duration for every host
kernel function in a filter set. With the filter narrowed to
`kvm_mmu_*`, `tdp_mmu_*`, and `mmu_notifier_*` functions, we
capture the **complete call chain** through KVM's MMU subsystem.
This is finer-grained than tracepoints: it sees private static
inline helpers, the actual recursion in the TDP walk, and the
exact return path on SPTE manipulation.

### Setup

```bash
sudo bash <<'EOF'
# 1. Switch to function_graph tracer
echo function_graph > /sys/kernel/tracing/current_tracer

# 2. Filter to KVM MMU functions only
echo > /sys/kernel/tracing/set_ftrace_filter
echo 'kvm_mmu_*' >> /sys/kernel/tracing/set_ftrace_filter
echo 'tdp_mmu_*' >> /sys/kernel/tracing/set_ftrace_filter
echo 'kvm_unmap_gfn_range' >> /sys/kernel/tracing/set_ftrace_filter
echo 'kvm_mmu_notifier_*' >> /sys/kernel/tracing/set_ftrace_filter
echo 'kvm_arch_mmu_notifier_invalidate_range' >> /sys/kernel/tracing/set_ftrace_filter

# 3. Optionally narrow to PID(s) of the umlctl/UML process
#    (set_ftrace_pid filters by pid; trace only the pid named here)
# echo $UMLCTL_PID > /sys/kernel/tracing/set_ftrace_pid

# 4. Set buffer to 128 MB per CPU
echo 131072 > /sys/kernel/tracing/buffer_size_kb

# 5. Enable
echo 1 > /sys/kernel/tracing/tracing_on
EOF

# Run the soak
UML_KERNEL=... bash tools/testing/selftests/um/soak/run-soak-daemon.sh ...

# Dump trace
sudo cat /sys/kernel/tracing/trace > /home/mjbommar/src/r13-ftrace/ftrace.out
sudo bash -c 'echo 0 > /sys/kernel/tracing/tracing_on; echo nop > /sys/kernel/tracing/current_tracer'
```

### Expected Signal

The trace shows the complete call hierarchy through KVM. Example
of what we're looking for:

```
 5)   kvm_arch_mmu_notifier_invalidate_range_start() {
 5)     kvm_mmu_invalidate_range_start() {
 5)       kvm_mmu_invalidate_zap_pages_in_memslot() {
 5)         tdp_mmu_zap_root() {
 5)           ... (recursive SPTE clears)
 5)         }
 5)       }
 5)     }
 5)   }   [duration: 18 µs]
```

The function-graph output gives timing — slow zaps that overlap
with the cache-abort moment are suspicious.

### Pitfalls

- Massive trace volume. Even with PID filtering, may overflow
  buffer in seconds during a hot Django soak. Use the
  `tracing_off` checkpoint mechanism to stop on a trigger event.
- ftrace function_graph has measurable performance impact
  (~5–15 % slowdown). Cache-abort Heisenbug risk.
- Filter list must be exhaustive — missing one helper hides
  call-chain segments. Audit with `grep` against KVM source.

---

## Technique D — `gdb -p` attached to umlctl with hardware watchpoint on bytecode HVA

### Rationale

UML's kernel runs as a host userspace process. The python child
runs as a host pthread of that process. The bytecode bytes that
read as zero are at a specific guest VA, which maps through
guest pgd → GPA → host VA via UML's physmem mmap.

If I can compute the host VA of the bytecode page at the moment
of the abort, I can set a **hardware watchpoint** in gdb on that
specific byte range. The next write to it (from any host code
path — CPython's interpretation via TDP, host kernel via memslot
update, etc.) breaks gdb. We then dump the full host call stack.

This is the **only technique that can identify the WRITER of
the zero bytes directly**.

### Setup

```bash
# Step 1: patch CPython to ALSO compute and print the bytecode HVA
# (not just GVA) at the moment of the cache abort, then enter a
# pause-forever loop (instead of calling abort()) so gdb has time
# to attach.

# Step 2: run soak with the new patched CPython. When cache abort
# fires, the python process pauses (hung).

# Step 3: From the dump file, extract bytecode GVA (e.g.
# instr_ptr=0x550000818f78). Translate to host VA:
#   - umlctl's uml_physmem mmap base is shown in /proc/<pid>/maps
#     for the kvm-v2 backend
#   - umlctl exposes physmem at HVA hostmap_base + GPA
#   - guest_pgd_walk(GVA) → GPA via /proc/<pid>/pagemap of the UML
#     kernel pthread (or by reading the guest pgd in physmem
#     directly from host)

# Step 4: Attach gdb to the umlctl process:
sudo gdb -p $UMLCTL_PID \
    -ex "set pagination off" \
    -ex "watch *(unsigned long *)0xHHHHHHHHHH" \
    -ex "continue"

# Step 5: When gdb breaks, dump backtrace + registers + memory:
#   (gdb) bt full
#   (gdb) info registers
#   (gdb) info proc mappings
#   (gdb) x/16gx $rsp
```

### Expected Signal

When the watchpoint fires, gdb prints the host code that just
wrote a zero byte to the bytecode HVA. The backtrace will name
the function (e.g., glibc's `__memset_avx2`, or some kernel
syscall return path).

### Pitfalls

- **Hardware watchpoints are limited** — x86_64 has 4 DR
  registers, so up to 4 byte/word watchpoints simultaneously.
  Enough for one bytecode region (4 × 8 bytes = 32 bytes).
  Our typical zero region is 16–40 bytes, so 4 watchpoints
  cover up to 32 bytes — barely enough for one hit.
- **UML's guest VA → host VA translation** is non-trivial. Need
  to compute via guest pgd walk. Easier: patch CPython to print
  the host VA directly. The patched CPython runs inside the
  UML guest, so `&bytecode[0]` IS the guest VA; we need to
  translate to host. Use `os.sched_getcpu()` and `/proc/<pid>/
  pagemap` from inside the UML guest to get the page frame
  number, then `pfn << 12 + offset` is the GPA, then
  `physmem_hostmap_base + gpa` is the host VA.
- **Pause-forever after CACHE abort** in patched CPython risks
  the soak harness killing the process for timeout. Either
  disable soak-side timeout for this round, or use a
  shorter-then-pause window.

---

## Technique E — KVM debugfs counters + per-VM stat sampling

### Rationale

`/sys/kernel/debug/kvm/<vm>/stat/*` exposes per-VM counters for
many MMU operations. Watching these counters at high frequency
during a failing iter shows which counter SPIKES when the cache
abort fires.

Counters of interest:
- `mmu_pte_write` — guest PTE writes detected
- `mmu_unsync_pages` — pages awaiting TLB flush
- `mmu_flooded` — flooding-detection on shadow page (legacy)
- `tlb_flush` — TLB-flush requests
- `nx_lpage_splits` — NX-large-page splits (potentially relevant
  given r/w-x bytecode pages)
- `remote_tlb_flush` — IPI-based remote TLB flushes
- `req_event` — pending VCPU events

### Setup

```bash
# Find the umlctl VM's debugfs path
sudo ls -la /sys/kernel/debug/kvm/
# typically /sys/kernel/debug/kvm/<pid>-<vmfd>/

UMLCTL_VM=$(sudo ls -d /sys/kernel/debug/kvm/*-* | head -1)

# Sample every 100 ms during the soak
sudo bash <<'EOF'
mkdir -p /home/mjbommar/src/r13-debugfs
while true; do
    ts=$(date +%s.%N)
    for stat_file in $UMLCTL_VM/stat/*; do
        v=$(cat $stat_file 2>/dev/null)
        name=$(basename $stat_file)
        echo "$ts $name $v"
    done
    sleep 0.1
done > /home/mjbommar/src/r13-debugfs/stats.log 2>&1 &
EOF

# Run soak. After each iter, also dump dmesg + kvm_debug events.
```

### Post-Processing

```bash
# Plot counter deltas vs time. Look for spikes around cache-abort moments.
awk '
    {
        if (last[$2] != "") {
            delta = $3 - last[$2]
            if (delta > THRESHOLD) print $1, $2, "delta=" delta
        }
        last[$2] = $3
    }
' /home/mjbommar/src/r13-debugfs/stats.log
```

### Expected Signal

A specific counter (e.g., `mmu_unsync_pages` or `nx_lpage_splits`)
spikes anomalously in the seconds leading up to a cache abort.
This narrows the bug class without requiring full call-stack capture.

### Pitfalls

- 100 ms sampling rate is too coarse for sub-millisecond events.
  Use 10 ms or 1 ms if needed (but watch CPU overhead).
- Counters are global per-VM, not per-vCPU. Can't isolate which
  vCPU's MMU is misbehaving.
- Some counters are only present in debug builds (`CONFIG_KVM_DEBUG_FS=y`).

---

## Bonus Technique F — KVM_MMU_AUDIT (host kernel rebuild)

### Rationale

KVM has built-in MMU auditing under `CONFIG_KVM_MMU_AUDIT=y`. When
enabled, every TDP walk verifies the entire SPTE hierarchy for
internal consistency. Bugs in SPTE accounting (missing TLB flush,
stale prev_root, inconsistent role bits, etc.) emit
`KVM: MMU audit: <reason>` warnings.

This is the **gold standard** for catching SPTE-management bugs
in KVM itself.

### Setup

```bash
# Requires host kernel rebuild with CONFIG_KVM_MMU_AUDIT=y.
# That's a significant detour (~30 min build + reboot).

# Once running:
sudo dmesg -w | grep -E "KVM: MMU audit|kvm_audit"
```

### Pitfalls

- Host kernel rebuild required.
- MMU audit has severe performance impact (~10× slower TDP walks).
  Cache-abort Heisenbug risk is HIGH.
- Output is verbose; need to filter for genuinely-bad warnings vs
  expected transient inconsistencies.

---

## Recommended Order

1. **Technique A (perf record -g)** — fastest dispositive next
   step. Single command, ~3 GB output, immediate post-processing.
   Reveals WHO triggers the SPTE invalidation.
2. **Technique B (filtered kvmmmu kprobes)** — if A shows
   ambiguous candidates, filter further and re-run.
3. **Technique C (function_graph)** — if A's tracepoint-level
   resolution is too coarse to see the bug class.
4. **Technique D (gdb watchpoint)** — only if A/B/C don't pin
   down the writer. Requires CPython repatch + HVA translation.
5. **Technique E (debugfs stats)** — cheap parallel monitoring,
   run alongside any of A/B/C/D for counter-spike correlation.
6. **Technique F (MMU_AUDIT rebuild)** — last resort, only if
   the bug really is a KVM-internal SPTE-management bug that
   the audit catches and the call-stack capture missed.

---

## Decision Gates

After each technique, the disposition gate is:

- **Identified the writer of zero bytes (or the originating
  kernel call path)?** → File bug or patch, end of investigation.
- **Identified the originating mm operation but not the writer?**
  → Need Technique D's watchpoint to pin the writer.
- **Got call stacks but they're in a path I expected (mmu_notifier
  from a benign mm op)?** → The bug is in KVM's handling of the
  notifier event (e.g., not invalidating prev_roots correctly).
  Investigate KVM's notifier handler.
- **No correlation found in 3 techniques?** → The bug is in a
  layer I haven't yet imagined. Re-read the data for surprising
  patterns.

---

## Round 13 Update (2026-05-18) — Plan Status

After working through Techniques A–F:

### Executed and analyzed

* **A — `perf record -a -g -e kvmmmu:*`** — DONE. 621k SPTE clears
  in 1130s; 91 % were end-of-iter VM teardown; 9 % were kcompactd
  migration. With `vm.compaction_proactiveness=0` confirmed: aborts
  still fire, so kcompactd is not the cause. See R13 addendum in
  50-doc.

* **B — bpftrace on `__mmu_notifier_invalidate_range_start`** —
  DONE. 27 k events in 240 s with KSM/khugepaged/swappiness/THP all
  off; 100 % from `comm=linux`, zero from external mover threads.

* **B' (T72, R13 follow-up) — bpftrace `os_unmap_memory` bucketed
  by VA prefix** — DONE. Critical refinement: of the ~100/s
  `os_unmap_memory` calls, ALL fire on `0x7ff...` (host kernel /
  stub VA range). ZERO fire on `0x550...` (guest user VA range
  where bytecode lives). UML does NOT host-munmap the bytecode
  page. The R8/R13 "23 k events in 240 s" were on guest-kernel
  VAs, not guest user heap.

* **C — ftrace tracepoints on `kvmmmu:*`** — DONE (lightweight
  variant; not function_graph, which has Heisenbug risk). 2,665
  `kvm_mmu_prepare_zap_page` events captured; **0
  `fast_page_fault`**; 0 `kvm_mmu_zap_all_fast`; 0
  `handle_mmio_page_fault`. **The lockless fast page-fault path
  never fires under this workload** — so the 2024 Tao Su patch
  (fast_pf not saving mmu_invalidate_seq) is not a candidate
  upstream fix.

* **D — per-bytecode-page checksum monitor** — DONE in userspace
  form (`tools/uml/diag/round13-bytecode-monitor/bc-monitor.c`,
  LD_PRELOAD library wrapping `getpid`/`gettid`/`clock_gettime`).
  Iterated v1→v4. v4 catches deterministic Python startup
  arena-zeroing pattern on the first heap page (not the bug), but
  has a seed-race for pages allocated via `brk` AFTER monitor
  init. v5 redesign needed for cache-abort capture; deferred
  pending Arm B result and reopened hypothesis space.

* **E — KVM debugfs counter sampling** — DONE (1 Hz, ~545 samples
  via `~/src/r13-techE/sample-kvm-counters.sh`). Global counters
  stable; per-VM debugfs (`/sys/kernel/debug/kvm/<pid>/`) confirms
  `pf_fast = 0` per VM, ` pf_fixed > pf_taken` slightly (suggests
  slow-path refault retries).

* **F — `CONFIG_KVM_MMU_AUDIT` rebuild** — UNAVAILABLE. Verified
  by `grep KVM_MMU_AUDIT /lib/modules/$(uname -r)/build/arch/x86/kvm/`:
  the option has been removed upstream. F is no longer applicable
  on a modern host kernel.

### Hypothesis status after R13 work

Ruled out by negative evidence:
- External host mover threads (kcompactd, kswapd, khugepaged, KSM)
- Host pagecache reclaim of the python binary
- Fast-page-fault / mmu_invalidate_seq race (path doesn't fire)
- UML's tlb-sync host-munmap of the bytecode page (T72)
- UML's `os_drop_memory(MADV_REMOVE)` on the hot path (not called)
- User-syscall-mediated corruption (Round 11)
- Entry-save block corruption (T60 audit)
- Pool-share / cross-vCPU pinning (R4/R7/R8)
- `MAP_POPULATE` alone (T71 Arm A)

Reopened by R13's T72 negative result:
- The corruption is NOT mmu_notifier-mediated SPTE staleness on
  the bytecode page. Some other path is at work.

Candidates for next-round (Round 14) work, with R13 audit status:

- **H12 (CoW zero-fill)** — Python forks before module init?
  Unlikely (frozen-getpath fires during interpreter bootstrap,
  before any user-level fork — abort pid is the first python,
  no fork yet) but the spawner.c forkserver pattern in kvm-v2
  may cause a relevant CoW. Test: trace `do_wp_page` on the
  bytecode page via kfunc bpftrace. **Status: PROBABLE NEGATIVE
  (captured aborts are in pid=32, the first python; no fork
  has happened).**
- **H13 (FPU/XSAVE residue with zero-fill)** — SMP-T57 enabled
  XSAVE; if a subsequent `XRSTOR` or `FNINIT` zeros AVX state and
  the guest's CPython uses AVX `memset`, a wrong-length write
  could zero into the bytecode page. Test: rebuild CPython with
  `-mno-avx -mno-avx2 -mno-sse4` to disable AVX-based glibc
  memset. **Status: R13 partial audit shows XSAVE save/restore
  is only at snapshot-ioctl time (snapshot.c), NOT on per-dispatch
  hot path. The per-dispatch FPU handling uses KVM_GET_FPU/SET_FPU
  which is well-audited. Likely negative, but not 100% conclusive.**
- **H14 (KVM-internal non-mmu_notifier bug)** — e.g., a
  `vcpu_enter_guest` path that writes to guest memory directly
  via `kvm_write_guest`. **Status: RULED OUT by R13 code audit
  (grep) — kvm-v2 has NO calls to `kvm_write_guest`,
  `kvm_vcpu_write_guest`, `copy_to_user_kvm`, `kvm_vcpu_map`, or
  similar host-side direct-write primitives. The only `copy_to_user`
  reference in kvm-v2 is a COMMENT in region.c (no actual call).
  No `memcpy`/`memset` targets guest VAs. UML's KVM backend never
  writes guest memory directly through any host-side path.**
- **H15 (Microarchitectural artefact / dispatch-elision)** —
  TLB-coherence corner case under deeply-elided dispatch
  boundaries (gadget). The leading candidate. Test: add a forced
  `outb` every N gadget invocations to bring dispatch frequency
  back to seccomp levels. Requires gadget asm change + UML
  rebuild + soak. **Status: NOT YET TESTED; highest-priority
  R14 experiment.**
- **H16 (CPython specialization torn-write race)** — CPython 3.14
  PEP 659 specialization writes cache slots in-place. If the
  specialization machinery zero-fills cache slots BEFORE writing
  the actual cached data, and the thread is interrupted (signal /
  scheduler) between the zero-fill and the write, the slots stay
  zero. Next dispatch reads 0x0000 codeunits and triggers TARGET
  (CACHE). The bug would be CPython's, but only manifests on
  kvm-v2 because the gadget makes syscalls so fast that
  specialization races more often. Test: read CPython
  Python/specialize.c for any pattern matching `memset(cache, 0,
  ...) → ... write specialization data`. **Status: NOT YET
  AUDITED; second-priority R14 experiment.**

### Round 14 priority

If T71 Arm B confirms MAP_LOCKED + mlockall has no rate effect
(as T72 predicts), H15 and H16 become the only remaining live
hypotheses. H15 is more invasive but more KVM-coherence-focused;
H16 is a CPython internal audit and could be done without any
kernel work. **Run H16 audit first** (cheap; could close the bug
via "this is a CPython bug, file upstream"), then if negative,
implement H15 (forced gadget exits).

### Plan

Round 14 will pick from H12–H16 based on Arm B result (currently
running) and rate-vs-baseline comparison.

## What This Plan Does NOT Cover

- **Userspace memory-corruption tools** (Valgrind, ASan,
  AddressSanitizer): not viable inside UML guest with kvm-v2.
- **gVisor / cloud-hypervisor comparison**: would prove the bug
  is kvm-v2-specific (already known from seccomp baseline).
- **Hardware-level KVM emulation tracing** (Intel PT for guest):
  out of scope; would only fire if Intel PT is available on host
  CPU (AMD Zen 4 here, so no Intel PT).
- **Bisecting kvm-v2 commit history**: previous rounds did this
  partially; the bug exists across many commits, so bisection is
  hard.
