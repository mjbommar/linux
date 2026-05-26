# 00 — Synthesis: a structural redesign for the UML KVM backend

Author: Synthesis (agents 1-4 → unified plan)
Date:   2026-04-27
Branch: uml-redesign-plan
Mandate: read all four agent proposals, identify convergence and
        divergence, and produce ONE phased redesign plan that the
        team can act on.

The four input memos are:

- `agent-1-gvisor-redesign.md` — model on gVisor's KVM platform
- `agent-2-vmm-redesign.md` — model on production VMMs (QEMU, Firecracker,
  cloud-hypervisor, kvmtool)
- `agent-3-uml-rootcause.md` — first-principles audit of UML, propose
  per-mm worker pthread + TDP/EPT
- `agent-4-radical-redesign.md` — SAUCE: delete KVM entirely, use Intel
  PKU intra-process isolation

---

## 1. What all four agents agree on (the convergence)

Every agent identifies the same root architectural defect:

> **The vCPU is owned by the UML scheduler, not by a host thread.** UML's
> cooperative `switch_threads` (longjmp on the same host thread) puts N
> UML tasks on 1 host thread that owns 1 KVM vCPU. KVM's API contract
> requires the opposite: 1 host thread owns 1 vCPU for life, and the
> per-vCPU `struct kvm_run` mmap is single-writer. Every "snapshot
> exit-state before unblock_signals", every "dirty/synced/needs_full_resync
> cmpxchg dance", every "per-mm IRETQ frame", every "FPU save on
> switch_threads" — they are all symptoms of this single mismatch.

The five race classes documented in memo 19 are five facets of the same
defect. The 50-µs latency that "fixes" them is the IPI / vcpu-kick budget
production VMMs spend on every cross-vCPU synchronization. UML reaches it
only by accident, via SIGALRM jitter.

The four agents also agree on three secondary points:

a. **The shadow PT compounds the problem.** It introduces a multi-writer
   data structure (lazy fill, direct sync, range invalidate, flush-tlb
   sync) where the consumer (KVM_RUN) cannot retry. Every fix attempt
   adds another producer/consumer ordering rule; correctness is asymptotic.
   Agents 1, 3, and 4 all propose deleting the shadow PT; agent 2 proposes
   keeping it short-term but agrees it's not load-bearing for the race.

b. **The signal model is wrong.** UML's `block_signals`/`unblock_signals`
   manipulate UML's *guest-side* software-signal state; KVM checks the
   *host* sigmask. Without `KVM_SET_SIGNAL_MASK` the host SIGALRM lands on
   whichever host thread is in `KVM_RUN`, returns -EINTR, and the
   scheduler races a different task into the same vCPU. Agent 2 argues
   this is the single highest-leverage fix.

c. **The complexity in `struct kvm_shadow_mm` (3 flags + 12 transition
   counters + 4 producers + a seqlock + a mutation ring) is not load-
   bearing for correctness.** It is telemetry to track which producer
   wrote, because the consumer needs that information to know what to
   repair next. The complexity papers over a multi-writer architecture
   that cannot be made correct under mid-KVM_RUN concurrency. Agent 3
   makes this most clearly; agents 1 and 4 implicitly agree by deleting
   the whole apparatus.

## 2. Where the agents diverge

| Question | Agent 1 (gVisor) | Agent 2 (prod VMMs) | Agent 3 (root cause) | Agent 4 (radical) |
|---|---|---|---|---|
| Per-X vCPU? | per-task | per-task | **per-mm** | n/a (no vCPU) |
| Delete shadow PT? | yes | defer | yes | yes |
| Memslot policy | single + memslots-by-host-VA-of-physmem | keep current | per-mm memslot | one slot, no aliasing |
| KVM_SET_SIGNAL_MASK | implicit (per-thread) | **explicit early fix** | implicit (worker thread) | n/a |
| Cooperative threading | keeps it on UML side | keeps it on UML side | keeps it on UML side | rewrites it (PKEY context-switch) |
| Effort | ~5 weeks | 3-4 days minimum, 9-12 days full | 7-10 weeks | ~3 months |
| Risk | medium | low (minimal subset) | high (Phase B memslot churn) | high (PKEY caps, binary compat) |
| Eliminates the bug class? | yes | yes (after step 4) | yes (after Phase B) | yes (by construction) |

The most useful divergence is **per-task vs per-mm vCPU** (agents 1,2 vs
agent 3):

- **Per-task** (agents 1, 2): each UML task has its own `KVM_CREATE_VCPU`
  fd. Cost: ~12 KiB mmap + fpu state per task; for 200 UML tasks ~4-6
  MiB. Risk: KVM_MAX_VCPUS limit (typically 1024 on x86; 32k on modern
  Linux). Excellent fit for SMP UML where N user tasks need to run
  concurrently on N host CPUs.
- **Per-mm** (agent 3): each UML mm has a worker pthread that owns one
  vCPU. Cost: ~12 KiB per mm; UML processes are O(10s), so very cheap.
  Risk: same-mm tasks (CLONE_VM threads) serialize on the worker, so
  pthreads-heavy guest workloads lose parallelism. Matches Linux's "thread
  group" model nicely.

For UML's typical workloads (many short-lived processes, few long-lived
multi-thread programs), **per-task is the better choice**: it scales to
SMP guests naturally, doesn't serialize CLONE_VM threads, and is what
both gVisor and every production VMM does.

The second useful divergence is **agent 4's SAUCE proposal**. It is
genuinely a different shape and would eliminate the bug class by
construction. But it costs:
- Binary compatibility (needs `uml_user_runtime` link)
- Production-sandbox suitability (PKU is not virt-class isolation)
- Significant upstream work (eBPF prog type, PKEY virtualization)
- Reimplementation of record/replay, snapshot/restore against a
  different ABI

For the immediate goal — "make heavy Python work reliably" — SAUCE is
overkill. As a **long-term v2 direction** after the conservative fix
ships and we have data on whether KVM_RUN's ~1500 ns overhead is the
remaining bottleneck, SAUCE deserves serious consideration.

## 3. The recommended path

A staged plan that captures the convergence and front-loads the
highest-leverage low-risk wins:

### Stage A — IMMEDIATE (3-5 days): per-task vCPU + KVM_SET_SIGNAL_MASK

Do agent 2's "minimum viable fix" verbatim, with one upgrade from agent 1
(per-task, not per-thread-with-pool — UML tasks ARE the thread-pool unit).

**Concrete changes**:

1. **`kvm_vcpu_handle` per task** (`arch/x86/include/asm/processor.h` UM
   section adds a pointer to `struct thread_struct`):
   ```c
   struct kvm_vcpu_handle {
       int               fd;             /* KVM_CREATE_VCPU result */
       void             *run;            /* mmap of struct kvm_run */
       size_t            run_size;
       pid_t             host_tid;       /* gettid() of owner */
       /* Per-vCPU caches (move from singleton kvm_um.cached_*) */
       u64               cached_cr3_gpa;
       u64               cached_fs_base;
       u64               cached_gs_base;
       bool              sregs_primed;
       bool              msrs_primed;
       bool              cpuid_done;
   };
   ```
   Allocated in `kvm_thread_create` (`thread.c:227`), freed in
   `kvm_thread_exit`.

2. **`KVM_SET_SIGNAL_MASK` install at vCPU creation**:
   ```c
   sigfillset(&set);
   sigdelset(&set, KVM_UM_KICK_SIGNAL);   /* SIGRTMIN+0 */
   /* push via KVM_SET_SIGNAL_MASK ioctl */
   ```
   Install a no-op handler for `SIGRTMIN+0` at `kvm_init`.

3. **`kvm_vcpu_kick(handle)`** sends `pthread_kill(SIGRTMIN+0)` plus
   `WRITE_ONCE(run->immediate_exit, 1)`. Replaces the cmpxchg dance for
   cross-task vCPU eviction.

4. **`kvm_run_userspace` rewrite**: drop the `block_signals`/
   `unblock_signals` brackets around `KVM_RUN` (the host sigmask is now
   the source of truth). Drop the entire snapshot block — `run->*` is
   per-vCPU, no other thread can write it. This deletes ~80 lines.

5. **Replace every `kvm_backend_vcpu0_fd()` call with
   `current->thread.kvm_vcpu->fd`** and `kvm_backend_ctx()->run0` with
   `current->thread.kvm_vcpu->run`. ~30 call sites.

**What this closes**:
- Race A (singleton vCPU/run mmap aliasing) — structurally impossible, no
  singleton.
- Race E (per-mm IRETQ frame collision) — moot, IRETQ frame can move to
  per-vCPU and never collide.
- The 50-µs SIGALRM-during-KVM_RUN window — `KVM_SET_SIGNAL_MASK` blocks
  SIGALRM on the vCPU thread.
- The post-unblock state-snapshot workaround (commit `b516bee62eb2`) —
  obsolete; delete the snapshot block.

**What this leaves**:
- Race B (shadow PT direct-sync vs full fill mid-KVM_RUN).
- Race C (host-VA aliasing via `os_map_memory`).
- Race D (set_pte_at lock-free vs fill).
- The `dirty/synced/needs_full_resync` state machine (still needed for
  shadow PT, just less racy).

**Validation**: cpython parity gate single-pass median. Today: 15-17/21.
Target after Stage A: ≥18/21 single-pass median, retry-gate becomes
unnecessary for ld-linux startup (Race A is dominant for that class).

**Risk**: low. This is a restructure of ~5 functions and ~30 call sites
behind a clear ownership invariant.

### Stage B — STRUCTURAL (4-6 weeks): kill the shadow PT, use TDP

This is agent 3's Phase B with agent 1's kernel-half-shared upper PT
trick.

**Concrete changes**:

1. **Stop maintaining a shadow PT.** UML's `mm->pgd` becomes the guest
   CR3 root directly. `set_pte_at`, `pte_clear`, `set_ptes`,
   `pmd_clear`, `pud_clear`, `p4d_clear`, `flush_tlb_*` no longer hook
   shadow sync. KVM's TDP MMU walks `mm->pgd` via EPT.

2. **Memslots cover host PFNs that back UML's user VAs.** When
   `kvm_mm_map(virt, len, prot, phys_fd, offset)` runs, register a
   `KVM_SET_USER_MEMORY_REGION` for that range. When `kvm_mm_unmap`
   runs, deregister or split the slot. The shadow's role of "GVA → GPA →
   memslot HVA" collapses into a single hardware EPT walk.

3. **Kernel-half (PGD slots 256..511) is shared across every mm.**
   Allocated once at `kvm_init`. The bootstrap page, IST stack page,
   gadget state page, vvar page, and any other ring-0-side mappings
   live here. Each mm's pgd PML4 entries [256..511] are copies of the
   shared entries. Per-vCPU IRETQ frame slots are addressed by vCPU id.

4. **Delete `arch/um/backend/kvm/shadow_sync.c` entirely.** Delete
   `kvm_shadow_pgd_alloc/free/clear_user/fill_from_uml_pgd/map_page/
   invalidate_va_range/audit_va/audit_content_va/audit_pgd` from
   `lifecycle.c`. Delete the `dirty/synced/needs_full_resync` state
   machine + 12 transition counters from `kvm_backend.h`. Delete
   `kvm_shadow_sync_pte` and the pgtable.h hooks. Delete the F12
   mutation ring. Net: ~3000-4000 lines deleted.

5. **Memslot management** is the new core of `arch/um/backend/kvm/mm.c`.
   The complexity moves from "shadow PT bookkeeping" to "memslot
   bookkeeping" — but memslot semantics are *single-writer in KVM*, so
   the multi-writer race class doesn't exist. Mitigation for the
   `KVM_USER_MEM_SLOTS` cap (~32k on modern Linux): coalesce adjacent
   user-VA ranges into one slot; fall back to a shared overflow slot
   for pathological mm fragmentation.

**What this closes**:
- Race B (shadow PT writers) — no shadow PT, no writers.
- Race C (host-VA aliasing) — memslot's `userspace_addr` IS the host VA;
  the GPA→HVA→PFN chain is single-source.
- Race D (set_pte_at vs fill) — no fill, no race.
- The entire "next entry repairs via needs_full_resync" recovery
  contract — deleted.

**Validation**:
- cpython parity gate single-pass median: target 21/21 on ≥4
  consecutive trials.
- Heavy Python long-running workloads (test_int big-integer
  computation): no wrong-answer corruption across 50 trials.
- Build kernel-under-UML for >30 minutes without divergence.

**Risk**: high. The memslot-per-mm design has not been validated in UML's
context. Per-mm slot churn under heavy `mmap`/`munmap` could be a
performance issue. Two mitigations:

a. Phase Stage B in: keep shadow PT initially, add the TDP walk as a
   *second* path, run them side-by-side with content audits
   (`kvm_shadow_audit_content_va` already added in task #91 is exactly
   this primitive) until TDP is proven. Then delete the shadow path.

b. If memslot churn is too expensive, fall back to a hybrid: register a
   single "physmem" memslot for UML's physical backing memory, and use
   an in-guest user-VA → physmem-GPA mapping that the guest CPU walks
   via its own page tables (which UML maintains in `mm->pgd`). This is
   identical to current behavior but with shadow PT replaced by direct
   walks of `mm->pgd`. Loses the host-VA aliasing benefit but eliminates
   the multi-writer shadow.

### Stage C — POLISH (1 week): cleanup, docs, validation

- Delete dead code from Stage A (per-task migration left vestiges).
- Update `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/`
  memos to mark superseded workstreams.
- KUnit suite update for the new APIs.
- Long-running validation: 24-hour cpython parity gate continuous run;
  no flake should appear.

## 4. What we explicitly DO NOT do (yet)

- **SAUCE / PKU intra-process isolation** (agent 4). Genuinely
  interesting and structurally clean, but breaks binary compat and
  removes production-sandbox suitability. Revisit as a v2 direction
  AFTER Stage B ships and we have data on whether KVM_RUN overhead is
  the remaining bottleneck. Park agent-4-radical-redesign.md in
  `Documentation/virt/uml/redesign/04-future-directions/` with a "pick
  this up after Stage B" note.

- **`bluepill` signal-driven entry** (agent 1). gVisor uses signals as
  VMENTER/VMEXIT triggers because they have a Go runtime that benefits
  from goroutine-transparent context switches. UML's explicit
  `kvm_run_userspace` loop is just as correct and easier to debug.

- **PCID + noflush bit** (agent 1's Phase 5). Worth doing for perf, but
  orthogonal to correctness. After Stage B closes the bug class, measure
  TLB-flush cost on the critical path; if it's >5%, do PCID. Otherwise
  defer.

- **Cross-CPU IPI dirtySet** (agent 1's Phase 4). UML is single-host-
  thread today (or per-task host thread after Stage A). Real cross-CPU
  IPI is needed only for SMP UML guests; defer until SMP UML lands.

- **Per-mm worker pthread** (agent 3's variant). The per-task model from
  agents 1 and 2 dominates this for UML's typical workload. Per-mm
  worker is a fallback if per-task somehow fails the validation.

## 5. Effort and sequencing

| Stage | Effort | Risk | What closes |
|---|---|---|---|
| A — per-task vCPU + KVM_SET_SIGNAL_MASK | 3-5 days | low | Race A, Race E, ld-linux startup race; expected gate jump 17→18/21 single-pass |
| B — kill shadow PT, use TDP + memslots | 4-6 weeks | high | Race B, C, D; expected gate to 21/21 reliable single-pass |
| C — cleanup + long-running validation | 1 week | low | tech debt + confidence |
| **Total** | **5-7 weeks** | — | — |

Stage A is the load-bearing immediate fix. Even if Stage B slips, Stage A
should make the kernel **usable** for heavy Python (no more retry-gate),
and it lands a clean per-task ownership model that simplifies any later
restructure (including SAUCE, if that ever happens).

## 6. Decision points for the user

1. **Approve Stage A unconditionally?** Low risk, ~3-5 days, fixes the
   immediate user-visible flakiness. Recommended: **yes**.

2. **Approve Stage B in principle, schedule for Q2-Q3?** Higher risk,
   4-6 weeks, but the only path to a *structurally* correct kernel that
   doesn't need per-trial retries on any workload. Recommended: **yes
   with a Phase B side-by-side validation gate**.

3. **Park SAUCE for v2 consideration?** Yes — preserve the agent-4 memo
   as a future direction; revisit after Stage B has been in production
   for ≥3 months and we have perf data.

4. **Per-task vs per-mm vCPU?** Per-task. SMP-future-friendly,
   no CLONE_VM serialization, matches gVisor and every production VMM.
   Per-mm worker (agent 3's variant) is the fallback if KVM_MAX_VCPUS
   becomes a problem (it shouldn't on modern Linux).

## 7. Bottom line

Every agent — independently of the others — landed on the same root cause
diagnosis: **KVM's API contract requires "1 host thread = 1 vCPU for
life", and UML's cooperative-threading-on-singleton-vCPU model breaks
that contract**. The 8 fixes from the previous task list (#89-#96) made
the existing shape more correct internally but cannot remove the
contract violation. No incremental change to the current architecture
can make the bug class impossible.

The recommended Stage A (per-task vCPU + KVM_SET_SIGNAL_MASK) is the
minimum viable structural fix and ships in days, not months. Stage B
(eliminate shadow PT, use TDP via memslots) is the complete fix that
makes the bug class structurally impossible — at the cost of 4-6 weeks
of restructure across the memory-management bridge.

Both stages keep UML's value proposition intact (Linux kernel in a
process, KASAN/KMSAN/KCSAN/KCOV/kprobes/ftrace, snapshot/restore). They
do not require new host-kernel features, do not break binary
compatibility, and do not reduce sandbox suitability. They are what
production VMMs do because production VMMs cannot tolerate the
per-trial flakes UML currently has.

The retry-gate stops being needed. That is the goal.
