# Agent 2 — Production-VMM-pattern redesign for the UML KVM backend

Author: Architecture-review agent #2
Date:   2026-04-27
Branch: uml-redesign-plan
Mandate: Structural fix for the timing race documented in
        `02-workstreams/D-kvm-backend/19-next-investigation-playbook/00-playbook.md`.
        Modelled on production VMMs (QEMU, Firecracker, cloud-hypervisor,
        kvmtool/lkvm) — agent #1 covers the gVisor angle; this memo
        deliberately avoids overlap.

----------------------------------------------------------------------

## 1. VMM architecture survey

### 1.1 QEMU — `accel/kvm/kvm-all.c`, `accel/kvm/kvm-accel-ops.c`, `system/cpus.c`

**vCPU threading.** `kvm_vcpu_thread_fn` in `kvm-accel-ops.c:28-56`
spawns one POSIX thread per `CPUState`:

```c
qemu_thread_get_self(cpu->thread);
cpu->thread_id = qemu_get_thread_id();
current_cpu = cpu;                    /* thread-local */
r = kvm_init_vcpu(cpu, &error_fatal);
kvm_init_cpu_signals(cpu);            /* sigmask + KVM_SET_SIGNAL_MASK */
do {
    qemu_process_cpu_events(cpu);
    if (cpu_can_run(cpu))
        r = kvm_cpu_exec(cpu);        /* the KVM_RUN ioctl */
} while (!cpu->unplug || cpu_can_run(cpu));
```

A vCPU fd is opened by exactly one thread and is owned by that
thread for life — `current_cpu` is a thread-local; SMP guests get N
host threads, never multiplexed.

**Signal handling.** QEMU uses `SIG_IPI` (= `SIGUSR1` on Linux) as
the kick signal. `kvm_init_cpu_signals` installs a no-op handler for
`SIG_IPI`, fills a sigset with everything, deletes the signals the
vCPU thread wants to receive (notably `SIG_IPI` itself), and pushes
the result via `KVM_SET_SIGNAL_MASK`. The kick path
(`system/cpus.c:587`) is one line:

```c
int err = pthread_kill(cpu->thread->thread, SIG_IPI);
```

`kvm_cpu_kick` (`accel/kvm/kvm-all.c` ~2700) additionally sets
`run->immediate_exit = 1` when `KVM_CAP_IMMEDIATE_EXIT` is present;
`kvm_eat_signals` (~2730) drains queued kicks before the next
re-entry. ([QEMU mailing list](https://www.mail-archive.com/kvm@vger.kernel.org/msg50331.html),
[qemu/qemu master](https://github.com/qemu/qemu/blob/master/accel/kvm/kvm-all.c))

**Memory.** `KVMMemoryListener` watches the `MemoryRegion` flatview;
`kvm_region_add → kvm_set_phys_mem → KVM_SET_USER_MEMORY_REGION`
allocates slots from a pool. Slots have unique IDs, never overlap in
GPA space, and are bounded by `KVM_CAP_NR_MEMSLOTS` (~509 on x86).
([Ruach](https://ruach.github.io/posts/QEMU-KVM-ADDRESS-SPACE/),
[QEMU memory API](https://www.qemu.org/docs/master/devel/memory.html))

**Guest PT.** Hardware TDP (EPT/NPT). No shadow PT.

### 1.2 Firecracker — `src/vmm/src/vstate/vcpu/mod.rs`

`Vcpu::start_threaded` (lines 151-176) spawns a thread named
`fc_vcpu {index}`, applies seccomp, and runs a state machine over
`paused`/`running`/`exited`. `run_emulation` (lines 331-354) is the
inner loop. The Vcpu struct (lines 94-106) owns its `KvmVcpu` and
event channels — fd is never shared.

`register_kick_signal_handler` (lines 109-120) installs a
fence-only handler for `SIGRTMIN()+0`. The kick
(`VcpuHandle::send_event`, lines 468-481) is the textbook combination:

```rust
self.vcpu_fd.set_kvm_immediate_exit(1);
fence(Ordering::Release);
self.vcpu_thread.as_ref().unwrap()
    .kill(sigrtmin() + VCPU_RTSIG_OFFSET)?;
```

Memory: pre-registers all guest RAM as one or two memslots at boot
(`vmm/src/vstate/memory.rs::Vm::set_kvm_memory_regions`), never resizes.
TDP. ([firecracker GH](https://github.com/firecracker-microvm/firecracker/issues/109))

### 1.3 cloud-hypervisor — `vmm/src/cpu.rs`

`CpuManager::start_vcpu` (~lines 1100-1350): one thread per vCPU,
`sched_setaffinity` for host-CPU pinning, seccomp filter,
`register_signal_handler(SIGRTMIN(), handle_signal)` (no-op handler),
barrier-synchronised start. Pause is `thread::park()`; the kick path
sets a `vcpu_run_interrupted` flag plus `pthread_kill(SIGRTMIN())`.
Same model as Firecracker, with explicit affinity hints for PCID and
TLB locality.

### 1.4 kvmtool / lkvm — `kvm-cpu.c`

`kvm_cpu__start` (~line 140) is the per-CPU thread; `kvm_cpu__run`
(line 152) issues `ioctl(vcpu_fd, KVM_RUN, 0)`. Signal setup
(lines 121-128):

```c
pthread_sigmask(SIG_BLOCK, &sigset, NULL);   /* block SIGALRM */
signal(SIGKVMEXIT,  kvm_cpu_signal_handler);
signal(SIGKVMPAUSE, kvm_cpu_signal_handler);
signal(SIGKVMTASK,  kvm_cpu_signal_handler);
```

`SIGKVM*` are `SIGRTMIN+0/1/2`. SIGALRM is *blocked* on every vCPU
thread so a host-timer tick can never land on a vCPU mid-run.

### 1.5 Cross-cutting invariants

| Invariant | QEMU | FC | CH | lkvm |
|---|---|---|---|---|
| One vCPU fd per host thread, never shared | Y | Y | Y | Y |
| `KVM_SET_SIGNAL_MASK` set once on each vCPU thread | Y | Y | Y | Y |
| Kick = `pthread_kill(SIG_IPI)` + `immediate_exit` | Y | Y | Y | Y (no immediate_exit) |
| Host SIGALRM blocked on vCPU thread | Y | Y | Y | Y |
| Memslots: pre-registered, GPA-disjoint, static after boot | Y | Y | Y | Y |
| Hardware-walked guest PT (TDP) — no shadow | Y | Y | Y | Y |
| `struct kvm_run` mmap owned by one thread only | Y | Y | Y | Y |

UML's KVM backend violates every line of that table.

----------------------------------------------------------------------

## 2. The KVM API contract UML violates

### 2.1 vCPU-fd thread affinity (api.rst §4.10)

> "vcpu ioctls should be issued from the same thread that was used
> to create the vcpu, except for asynchronous vcpu ioctl that are
> marked as such in the documentation. Otherwise, the first ioctl
> after switching threads could see a performance impact."

The wording is "should" — but the impact isn't only performance. The
per-vCPU `struct kvm_run` mmap is single-writer: `run->s.regs.*`,
`run->exit_reason`, `run->io.*`, `run->mmio.*` all race when two host
threads issue `KVM_RUN` against the same fd. UML's `kvm_run_userspace`
(`thread.c:3268`) is called from whichever host thread the UML
scheduler picked, against the singleton `kvm_ctx.vcpu0_fd`
(`thread.c:3270, 3464`). Every UML task that reaches userspace ends
up calling `KVM_RUN` against the same fd from a different host thread.

### 2.2 `KVM_SET_SIGNAL_MASK` covers the `KVM_RUN` window (api.rst §4.21)

> "This ioctl temporarily overrides the threads signal mask … any
> unblocked signal received (except SIGKILL and SIGSTOP) will cause
> KVM_RUN to return with -EINTR."

The contract: install a sigmask that blocks everything but the
dedicated kick signal, KVM atomically swaps it in around `KVM_RUN`.
`grep -rn KVM_SET_SIGNAL_MASK arch/um/` returns zero hits. UML's
`block_signals()` / `unblock_signals()` (`thread.c:3388, 3552`)
manipulate UML's *guest-side* software-signal state — orthogonal to
the *host-thread* sigmask the kernel checks. SIGALRM (UML's timer
tick) is therefore delivered by the host kernel to whichever host
thread it chooses, including the one currently in `KVM_RUN`. The
result: `-EINTR`, signal handler runs, UML schedules a different
task, that task re-enters `KVM_RUN` against the same fd, overwrites
`run->s.regs.*` and the IST stack — exactly the 50µs window the
playbook chases.

### 2.3 Memslot semantics (api.rst §4.35)

> "Slots may not overlap in guest physical address space."

UML registers exactly one memslot covering `[uml_physmem,
uml_physmem + physmem_size)` (`lifecycle.c:594-628`). User VAs (e.g.
`0x40xxxxxx` from playbook traces) lie *outside* the memslot. The
guest reaches user pages only via the shadow PT mapping guest VA → GPA
where GPA is inside the memslot — a translation layer the hardware
walker uses *instead of* EPT. That single-memslot decision is the
root reason UML needs a shadow PT in the first place: the hardware
walker has nothing else to do because there's no GPA mapping to user
VAs.

### 2.4 The vCPU request / kick protocol (`Documentation/virt/kvm/vcpu-requests.rst`)

The kernel guarantees a Dekker barrier between `vcpu->mode` and
`vcpu->requests`. The only legal way to make a vCPU observe a request
(TLB flush, MMU notifier) before its next guest entry is (a) IPI the
host CPU running it, or (b) be the thread that owns the vCPU and is
currently outside guest mode. UML's `um_tlb_sync(current->mm)`
followed by `KVM_RUN` happens on a *different* host thread than the
one that scheduled out, so KVM's notion of "this vCPU's host thread"
is stale and posted TLB-flush requests may not be observed before the
next entry. This is exactly the "asynchronous work that needs ~50µs"
the playbook chases.

----------------------------------------------------------------------

## 3. Why "cooperative threading + singleton vCPU" is structurally racy

In KVM API terms, the UML model has three independent breaks, each
sufficient on its own to produce non-deterministic exit state:

1. **Multi-thread vCPU ownership.** The singleton `vcpu0_fd` is
   reachable from any host thread that runs UML kernel code. The
   per-vCPU `struct kvm_run` mmap is single-writer; two host threads
   issuing `KVM_RUN` race on `run->s.regs.*` / `run->exit_reason` /
   `run->io.*` / `run->mmio.*`. The "snapshot before unblock_signals"
   patch (commit `b516bee62eb2`, `thread.c:3454-3501`) treats one
   symptom — the entire `KVM_RUN` round-trip must be atomic w.r.t.
   other host threads, and on one fd it can't be.

2. **No `KVM_SET_SIGNAL_MASK` discipline.** SIGALRM lands on the
   host thread inside `KVM_RUN`, returns `-EINTR`, the handler runs,
   UML's scheduler ticks, a different task gets picked, that task
   re-enters `KVM_RUN` and clobbers exit state. UML's
   `block_signals` doesn't reach the host kernel.

3. **Cross-mm shadow updates without a TLB-flush kick protocol.**
   When `set_pte_at` from one task mutates an mm whose CR3 is
   currently loaded on the singleton vCPU (running a *different*
   task), nothing kicks the vCPU out. KVM's MMU notifier doesn't
   fire because the user VA is outside the memslot (§2.3). The guest
   TLB keeps the stale translation until the next CR3 reload or
   `INVLPG`. UML's CR4.PGE-toggle dance (`thread.c:2483-2538`) tries
   to flush on entry, gated by the dirty/synced/needs_full_resync
   state machine documented in `kvm_backend.h:438-538`. SIGALRM-
   driven preemption mid-producer breaks the smp_wmb/load_acquire
   ordering invariants silently. Per the playbook, `mb()` and
   `um_tlb_sync()` alone don't close the window — only raw 50µs
   latency does, which is the host-side IPI-and-vcpu-kick budget
   production VMMs spend on every cross-vCPU TLB invalidation.

These three are manifestations of the same mistake: **UML treats the
vCPU as a passive resource shared across UML tasks; KVM's API treats
the vCPU as a thread-bound active object**.

----------------------------------------------------------------------

## 4. Redesign blueprint

### 4.1 Per-task vCPU pool with strict thread-pinning

```c
struct kvm_vcpu_handle {
    int                   fd;             /* KVM_CREATE_VCPU result */
    void                 *run;            /* mmap of struct kvm_run */
    size_t                run_size;
    pid_t                 host_tid;       /* gettid() of owner thread */
    struct task_struct   *owner_thread;
    sigset_t              run_sigmask;    /* installed via KVM_SET_SIGNAL_MASK */
    struct kvm_shadow_mm *active_as;      /* current AS (if shadow kept) */
    bool                  immediate_exit_supported;
    /* Per-vCPU caches replacing the singleton kvm_um.cached_*
     * / *_primed fields so they track THIS vCPU's last
     * programming, not a global one. */
    u64                   cached_cr3_gpa;
    u64                   cached_fs_base;
    u64                   cached_gs_base;
    bool                  sregs_primed;
    bool                  msrs_primed;
    bool                  cpuid_done;
    bool                  kernel_gs_base_primed;
};

struct kvm_vcpu_pool {
    struct mutex      alloc_lock;
    struct hlist_head by_tid[64];   /* hash: host_tid → handle */
    struct list_head  free_list;
    unsigned int      nr_alive, nr_max;
};

struct kvm_um {
    int                   kvm_fd;
    int                   vm_fd;
    refcount_t            mm_refcount;
    struct kvm_vcpu_pool  pool;
    /* DELETED: vcpu0_fd, run0, run_size, cached_cr3_gpa,
     * cached_fs_base, cached_gs_base, sregs_primed, msrs_primed,
     * kernel_gs_base_primed, cpuid_done, gadget_state_*,
     * gadget_vvar_*, all "shadow_pgd_* deprecated" vestiges. */
};
```

`kvm_vcpu_handle_get_for_current()` is the QEMU-equivalent of
`current_cpu`: hash-lookup by host TID, allocate on miss.
**Contract:** caller must not migrate to a different host CPU while
holding the handle (gVisor calls this `LockOSThread`; UML uses the
existing kthread-pinning machinery in `kvm_thread_create`,
`thread.c:227`).

A reasonable initial sizing policy:

- Pre-create `nr_cpus` vCPUs at `kvm_init` (bounded by
  `KVM_CAP_NR_VCPUS`).
- Pin each via `sched_setaffinity` so the host scheduler doesn't
  migrate the thread mid-run.
- Fast path is a TID-hash lookup; on overflow, `alloc_lock`
  governs grow.

### 4.2 Memslot policy — recommend keeping single-memslot for now

UML's Policy A (`03b-memslot-policy.md`) registers one giant memslot
covering UML physmem and routes user-VA references through the shadow
PT. Production VMMs do the opposite (every host VA the guest can
reach is in a memslot, GPA→host-VA is 1:1 within each slot, no shadow
PT). Three options:

| Aspect | A (per-mm slot) | B (one identity slot, UML mm rewrite) | C (keep current; fix threading only) |
|---|---|---|---|
| Need shadow PT? | No | No | Yes |
| Memslot count | O(mms) | 1 | 1 |
| Risk | High (slot churn semantics, ~509 cap) | Very high (UML mm restructure) | Low |
| Closes 50µs race? | Yes | Yes | Yes (via §4.4 sigmask) |

§4.1 (per-task vCPU) is independent of §4.2 and is the load-bearing
fix for the playbook race. **Option C** is the recommended starting
point: defer memslot churn, defer shadow-PT removal, fix the
threading model first. Migrate to A or B later if perf warrants it.

### 4.3 Whether to keep shadow PT — RECOMMENDATION: defer removal

The shadow PT exists only because Policy A leaves user VAs outside
any memslot. With per-task vCPU + sigmask discipline + working kick
protocol, the shadow PT goes from "racy hot path" to "deterministic
hot path that's just more code than necessary". Removing it is a
multi-week restructuring orthogonal to the playbook race. Conditions
that would justify removal: per-entry shadow re-fill perf cost
becomes intolerable; multi-process workloads with many mms blow the
shadow-PT memory budget; SMP guests (`uml_ncpus > 1`) make per-vCPU
shadow coordination worse than just letting EPT handle it.

### 4.4 Signal-mask discipline (the immediate fix for the 50µs race)

The single highest-leverage change. New per-vCPU init step at
handle-creation time:

```c
static int kvm_vcpu_install_sigmask(struct kvm_vcpu_handle *h)
{
    struct {
        u32 len;
        u8  sigset[sizeof(sigset_t)];
    } __packed mask;
    sigset_t set;

    /* Block EVERYTHING — including SIGALRM — during KVM_RUN.
     * KVM_UM_KICK_SIGNAL is the only signal allowed to interrupt
     * the guest. */
    sigfillset(&set);
    sigdelset(&set, KVM_UM_KICK_SIGNAL);
    mask.len = sizeof(sigset_t);
    memcpy(mask.sigset, &set, sizeof(sigset_t));

    /* Issued from h->host_tid — we're on it. */
    return os_ioctl_generic(h->fd, KVM_SET_SIGNAL_MASK,
                            (unsigned long)&mask);
}
```

Pick `KVM_UM_KICK_SIGNAL = SIGRTMIN+0` (UML uses no RT signals
today). Install a no-op handler at boot. The kick path:

```c
void kvm_vcpu_kick(struct kvm_vcpu_handle *h)
{
    if (h->immediate_exit_supported) {
        WRITE_ONCE(((struct kvm_run *)h->run)->immediate_exit, 1);
        smp_wmb();
    }
    kill_pid_info(KVM_UM_KICK_SIGNAL, SEND_SIG_PRIV,
                  task_pid(h->owner_thread));
}
```

After this lands, `block_signals()` / `unblock_signals()` brackets
in `kvm_run_userspace` (`thread.c:3388, 3552`) become obsolete for
the KVM_RUN window — host sigmask is the source of truth, and
`block_signals` is just for UML's own bookkeeping. The "snapshot
exit_reason / kregs / IST 40 bytes" workaround
(`thread.c:3454-3501`) becomes obsolete: the mmap is per-vCPU, the
vCPU is per-host-thread, no other thread can write it.

### 4.5 Memslot-update kick protocol (only needed if shadow PT kept)

For shadow-PT mutations, every change kicks any vCPU currently
running with that shadow as its CR3:

```c
void kvm_shadow_invalidate_va_range_kicked(struct kvm_shadow_mm *as,
                                           u64 va_start, u64 len)
{
    struct kvm_vcpu_handle *h;

    kvm_shadow_invalidate_va_range(as, va_start, len);  /* existing */

    rcu_read_lock();
    list_for_each_entry_rcu(h, &as->active_vcpus, as_link)
        kvm_vcpu_kick(h);
    rcu_read_unlock();
}
```

Each handle records its `active_as` when loading CR3
(`thread.c:2049+`); the AS holds a reverse list of vCPUs walking it.
Kicks land as `-EINTR` from `KVM_RUN`; the next entry sees the
updated shadow. This is what production VMMs spend the "50µs" on —
and only when there's actually a cross-task mutation, not on every
entry.

### 4.6 New `kvm_run_userspace` (pseudocode)

```c
void kvm_run_userspace(struct uml_pt_regs *regs)
{
    struct kvm_vcpu_handle *h = kvm_vcpu_handle_get_for_current();
    struct kvm_run *run = h->run;
    int rc;

    /* No block_signals/unblock_signals around KVM_RUN. The host
     * sigmask was installed at vCPU init; only the kick signal
     * returns -EINTR; SIGALRM is blocked on this thread for life. */

    rc = kvm_vcpu_load_state(h, current->active_mm, regs);
    if (rc)
        panic("um: kvm enter: state load failed (%d)", rc);

    if (READ_ONCE(run->immediate_exit))
        WRITE_ONCE(run->immediate_exit, 0);

    rc = os_ioctl_generic(h->fd, KVM_RUN, 0);

    /* run->* is THIS vCPU's only — no snapshot needed. */
    kvm_dispatch_exit(h, regs, run);
}
```

Compared with today's ~290-line body of `kvm_run_userspace` plus
~780-line `kvm_enter_guest`, the snapshot block, the unblock_signals
race, the SREGS-skip predicate (`thread.c:2196-2538`), the
dirty/synced/needs_full_resync state machine
(`kvm_backend.h:438-538`), and the F12 mutation ring
(`shadow_sync.c:61-121`) all become unnecessary or radically simpler.

### 4.7 Migration path (file-by-file landing order)

1. **`lifecycle.c`** — drop `vcpu0_fd / run0 / run_size` from
   `kvm_um` (lines 71-77, 222-224, 569-571). Add `kvm_vcpu_pool`
   init in `kvm_init`. Don't create vCPUs at init — lazy per host
   thread.

2. **`thread.c`** — new section for `kvm_vcpu_handle_alloc`,
   `kvm_vcpu_handle_get_for_current`, `kvm_vcpu_install_sigmask`,
   `kvm_vcpu_kick`. Register `SIGRTMIN+0` no-op handler at
   `kvm_init`. Convert `kvm_backend_vcpu0_fd()` callers (lines
   1954, 3270, ~30 others per `grep`) to per-thread handles.

3. **`thread.c::kvm_run_userspace`** — rewrite per §4.6. Delete the
   snapshot block and unblock_signals dance.

4. **`thread.c::kvm_enter_guest`** — split into `kvm_vcpu_setup_once`
   (CPUID, bootstrap, gadget state alloc — runs at vCPU creation)
   and `kvm_vcpu_reload_state` (per-entry SREGS/CR3/MSR programming
   keyed on per-handle cached state). Move `cached_*` / `*_primed`
   reads/writes onto the handle.

5. **`shadow_sync.c`** — add the kick protocol per §4.5; mutations
   that today call `kvm_shadow_invalidate_va_range` (`mm.c:180,244`,
   `lifecycle.c:1650`, `shadow_sync.c:153`) gain a follow-on
   `for_each_vcpu_in_active_set(as) kvm_vcpu_kick(h)`.

6. **`mm.c`** — `kvm_mm_attach` adds the per-mm `active_vcpus` list;
   `kvm_mm_detach` walks it and clears handles' `active_as`.

7. **`kvm_backend.h`** — define `KVM_UM_KICK_SIGNAL`,
   `struct kvm_vcpu_handle`, `struct kvm_vcpu_pool`. Delete the
   deprecated `shadow_pgd_*` vestiges (lines 110-117) and the
   singleton-vCPU caches (lines 80-82).

8. **Validation** — playbook reproducers (`import test.test_decimal`
   ×100, cpython parity gate ×4 trials) after step 4 lands.
   Expectation: ld-linux NULL deref class drops from 14% to ~0%.

### 4.8 Estimated effort

| Step | Effort | Risk |
|---|---|---|
| 1. Lifecycle de-singleton | 0.5 day | Low |
| 2. vCPU pool + sigmask install | 1.5 days | Medium (signal semantics) |
| 3. `kvm_run_userspace` rewrite | 1 day | Medium (callers) |
| 4. `kvm_enter_guest` split | 2-3 days | High (state migration) |
| 5. Shadow kick protocol | 1-2 days | Medium |
| 6. mm.c active_vcpus list | 0.5 day | Low |
| 7. Header surgery | 0.5 day | Low |
| 8. Test + diagnose regressions | 2-3 days | Medium |
| **Total** | **9-12 days** | **Medium** |

The minimal viable subset (steps 1-3, ~3-4 days) ships the
threading-model fix alone if shadow-PT and memslot policy are kept.

----------------------------------------------------------------------

## 5. Tradeoffs & risks

**What we lose immediately.**

- Shared per-vCPU caches (`cached_cr3_gpa` / `cached_fs_base` /
  `cached_gs_base` / `sregs_primed`) become per-handle — slightly
  less effective when one host thread serves multiple UML tasks in
  succession; net is small SREGS-reload overhead at task-switch
  boundaries.
- Snapshot/restore (memo 12, `kvm_backend.h:892-895`) becomes
  "iterate over the pool, capture each" — same shape as QEMU's
  per-cpu save.
- Shutdown reaps N handles instead of one — trivial pool walk.

**What gets harder.**

- Record/replay determinism (memo 13). Today UML records `KVM_RUN`
  invocations in arrival order trivially; with per-thread vCPUs, the
  host scheduler decides ordering. Workable: timestamp by
  `ktime_get_ns` and replay by timestamp, as Firecracker /
  cloud-hypervisor do for snapshots.
- Memory pressure: ~12 KiB per vCPU plus one page for the `kvm_run`
  mmap. For `nr_cpus = 8`, this is ~32 KiB — trivial.

**Validation checklist after landing.**

1. `KVM_SET_SIGNAL_MASK` honoured: `pr_info`-once at first install
   confirming ioctl rc == 0 and `/proc/<tid>/status::SigBlk` matches
   expectation.
2. Every vCPU fd touched only by its owning host TID: add a
   `WARN_ON(current->pid != h->host_tid)` debug guard at every fd
   use site for soak.
3. `immediate_exit` works: KVM_CAP_IMMEDIATE_EXIT is widely
   available since Linux 4.16; fall back to "kick is signal-only"
   with a dummy run-loop wakeup if missing.
4. SIGALRM never lands on a vCPU thread: confirm via
   `/proc/<tid>/status::SigBlk`.
5. The two playbook reproducers go quiet:
   `import test.test_decimal` × 100 (currently 14% ldlinux failure)
   and `test_int` long-bignum-string corruption.

**Biggest risk.** Step 4 (per-vCPU state migration) bleeding into a
multi-week project. Mitigation is option-C in §4.2: keep shadow PT
and memslot policy unchanged, ship steps 1-3 (~3-4 days) which alone
close §3.1 + §3.2 race classes.

----------------------------------------------------------------------

## 6. Bottom line

The 50-microsecond timing dependency in the playbook is not a bug to
spot in shadow-PT code. It is the visible artefact of three
independent KVM API contract violations (§3.1-§3.3) that production
VMMs forbid by construction:

1. One vCPU per host thread, owned for life.
2. `KVM_SET_SIGNAL_MASK` blocking everything but the dedicated kick
   signal across the `KVM_RUN` window.
3. Cross-vCPU TLB invalidation via `immediate_exit` + signal kick,
   not via cooperative-thread synchronisation hopes.

Adopt those three patterns — verbatim from QEMU, Firecracker,
cloud-hypervisor, and kvmtool — and the timing race becomes
unreachable structurally. Shadow PT, memslot policy, and the gadget
machinery can stay (option C); they are not implicated in the
playbook race once §4.1 + §4.4 land.

Estimated effort: **9-12 working days end to end**, with the minimal
threading-model fix (§4.1 + §4.4, no shadow-PT or memslot churn)
shippable in **3-4 days**.
