# Snapshot-to-disk / resume / review (UML v2)

**Status:** design parked in 08-future-phases per `08-future-phases/
README.md`'s rules. Not a commitment. Opens when a trigger from
`04-risks/decisions-log.md` D35 fires, or when a concrete user
request arrives.

**Relationship to C-09 v1:** C-09 v1 delivers an in-memory
fork-server (see `02-workstreams/C-profiles-and-gaps/
09-snapshot-forkserver.md` + D35). v1 is sufficient for fuzz
profile's `RunSnapshot` contract and the M8 milestone. v2 is
everything beyond "while the parent process is alive" — i.e.
persist UML to a file, hand the file to another machine, resume
tomorrow, or just pause-and-inspect.

**Scope boundary (per 08-future-phases/README.md §"Boundary with
workstream C"):** this document begins strictly where C-09 ends.
Nothing here re-negotiates v1's design choices; it builds on
them.

## What this unlocks (user stories)

1. **Survive host reboot.** A long-running UML research session
   hits a syzkaller reproducer at 3 am; user snapshots, reboots
   host the next morning, resumes. Single UML guest, cold host.
2. **Portable reproducer packaging.** A researcher finds a bug;
   wraps the pre-crash state in a single `.umsnap` file; hands
   it to a kernel maintainer over a list. Maintainer loads with
   `uml-restore`, runs the failing syscall, opens gdb.
3. **CI golden boot-state stamp.** CI keeps a
   `post-init.umsnap` alongside the `linux` binary. Every job
   restores from it instead of booting from scratch — cuts
   minutes off every run, deterministic state.
4. **Post-mortem review.** A UML panic produces a `.umsnap`
   instead of (or alongside) a dmesg tail. Reviewer loads it
   read-only in gdb or a `umcrash`-style tool and walks the
   kernel state. Separate from crash restart; both are useful.

These are different users. Scenarios 1 and 3 need full restore;
2 needs restore + portability; 4 needs read-only walk. The
format must be expressive enough for all four without
over-engineering.

## Chosen format: ELF64 core dump + UML `PT_NOTE` types

See D36 in `04-risks/decisions-log.md` for the full rationale.
Short version: UML **is** an ELF64 Linux process; `fs/binfmt_elf.c`
and `fs/proc/vmcore.c` are already the in-kernel precedent for
producing ELF from kernel context; LKML already blesses the
shape for kexec/kdump. We add UML-specific `PT_NOTE` types for
state that doesn't fit in standard ELF.

### Layout

```
ELF64 header
├── PT_LOAD: kernel text + rodata   (from /proc/kallsyms on the running UML;
│                                    usually elided and referenced by build-id
│                                    in a PT_NOTE, see below)
├── PT_LOAD: physmem_fd contents    (guest "RAM"; sparse via FS holes;
│                                    this is the file backing MAP_SHARED
│                                    in arch/um/kernel/physmem.c)
├── PT_LOAD: KASAN shadow           (sparse; only populated regions)
├── PT_LOAD: vmalloc populated runs (each contiguous populated run = 1 LOAD)
├── PT_LOAD: stub pages             (already in physmem_fd but called out
│                                    separately for clarity)
├── PT_NOTE: NT_PRSTATUS × vCPUs    (per-vCPU register state via backend
│                                    ops read_guest_regs; standard ELF note)
├── PT_NOTE: NT_FILE                (open FD table summary, standard note)
├── PT_NOTE: NT_AUXV                (process auxv, standard note)
├── PT_NOTE: UML_VERSION            (schema version; points at D-log entry)
├── PT_NOTE: UML_BACKEND            (backend kind + contract_version +
│                                    backend-specific reinit state)
├── PT_NOTE: UML_MMAP_TABLE         (canonical enumeration of kernel mmap
│                                    regions — what each PT_LOAD above is
│                                    and how it should be mapped on restore)
├── PT_NOTE: UML_FD_TABLE           (per-FD {path, flags, offset,
│                                    disposition: inherited|serialized|
│                                    reconstruct-by-path|skip})
├── PT_NOTE: UML_TIMERS             (timerfd/signalfd state; pending
│                                    signals; RCU/kthread quiesce
│                                    assertions at snapshot time)
├── PT_NOTE: UML_STUB_TABLE         (per-stub-child: guest mm id,
│                                    filter fingerprint, VA layout;
│                                    NOT the live pid — that's restored)
└── PT_NOTE: UML_READY_POINT        (named ready point + origin: boot,
                                    mconsole, debugfs, AFL)
```

All UML-specific note types use vendor name `"UML"` in the
`Elf64_Nhdr` owner field (standard ELF convention). Type
numbers start at `0x554d4c00` (`'U'|'M'|'L'|0`) + ordinal so
they don't collide with anything reserved.

### Why the note types look boring

They look boring because they **are** boring: each one is a
flat, fixed-schema description of a single subsystem. We avoid:

- Nested protobuf-style schemas (complexity; no good in-kernel
  codec).
- Per-device `VMStateDescription` tables (QEMU-shaped; doesn't
  fit our device model).
- Go-reflection serialization (gVisor-shaped; not a C option).

The trade we're making: every new UML subsystem that needs
snapshot/restore adds a new note type. That's an n-per-subsystem
cost, but it's linear, well-bounded (~10-15 subsystems total),
and explicit.

## State enumeration — what's in, what's reconstructed, what's skipped

Three categories for every piece of UML state:

### A. Serialize (must be in the file)

| Item | Where | Why serialize |
|---|---|---|
| physmem_fd contents | `arch/um/kernel/physmem.c` | The whole point |
| KASAN shadow (dirty) | `arch/um/os-Linux/mem.c` | Restore-time reconstruction would lose poisoning state and miss UAF |
| vCPU registers | backend ops read_guest_regs | Needed to resume at the right instruction |
| Pending signals | signalfd state | Drop-on-restore = lost interrupts |
| Timerfd state | timer subsystem | Drop-on-restore = lost wakeups |
| mount points (hostfs paths) | fs layer | Restore-time reconstruction by path |
| Open FD descriptors | `arch/um/os-Linux/file.c` | Paths + offsets + flags |
| Task list + state | generic kernel | `do_exit`-in-flight tasks, blocked tasks |
| Ready-point identity | snapshot.c | Provenance for replay / debug |
| Backend kind + contract_version | backend ops | Which backend impl to restore into |

### B. Reconstruct on restore (not in the file; computed)

| Item | How reconstructed |
|---|---|
| Kernel text + rodata | Mapped from the matching `vmlinux` binary; verified by build-id in NT_NOTE |
| Seccomp filter contents | Compiled into the kernel binary; re-install on restore |
| vCPU host pthreads | `um_vcpus_start_all()` (pull-forward from C-09 v1) |
| Seccomp stub children | Re-spawned by worker_init path; stub VA recomputed from stub table |
| timerfd + signalfd file descriptors | Recreated on restore, re-armed from UML_TIMERS note |
| KASAN shadow structure | mmap'd as before; populated regions copied from PT_LOAD |

### C. Skip (explicit non-goal; documented)

| Item | Why skip |
|---|---|
| Open TCP connections in guest userspace | CRIU's TCP_REPAIR path requires CAP_NET_ADMIN, fails in rootless; not worth reproducing [CRIU issue #2030, #2386] |
| Guest-userspace processes' TLS session state, PRNG-derived caches | Same class of "kernel guarantees vs outside world" bugs that break Firecracker's clones [VMGenID docs] |
| Time-of-day drift across restore | Host clock advances; guest clock is an observable. Document that `wall_clock` may jump forward on restore |
| Open VFIO / virtio-uml backends | These are tied to *host* processes the guest talks to; those processes may be gone. Skip for v1 of v2; revisit with C-10 (crosvm launcher) |

## Restore model — three tiers, pick per-use-case

Same file, three restore paths:

1. **Full preload (simplest, correctness first).** Read the
   whole file into memory, mmap regions, restore state, resume.
   Cost: O(RAM) I/O time. Typical 2 GB UML + 256 MB shadow ≈
   ~2-4 seconds on tmpfs, dominated by `read(2)` throughput.
   Good enough for scenarios 1, 2, 3.

2. **Lazy pages via userfaultfd** (the gVisor model). Map
   `pagesFile`-style region via `userfaultfd`; register a
   handler process that serves demand faults. Resume before
   the file is fully loaded; pages come in as touched. Cost:
   ~10s ms per first-touch page-fault; amortizes to near-zero
   for hot working set. Worth the engineering for interactive
   resume where the first response matters (scenario 1 at 3
   am).

3. **Read-only walk (no restore).** Load the ELF with gdb or
   `crash(8)` + `umcrash` symbol helper; inspect state; don't
   resume. Scenario 4. Zero new code beyond symbol helpers.

Tier 1 is the mandatory starting point. Tiers 2 and 3 are
independent layers on top.

## Known failure modes to design against

From production CRIU / gVisor / Firecracker experience (research
agent's corpus; dates verified 2026-04-20):

1. **External-world coupling is the killer.** Serialization is
   the easy part. What breaks is state that the kernel
   guarantees something about *to* the outside world — TCP
   sequence numbers, PRNG entropy consumers, CPUID consistency
   across hosts. *Mitigation for us:* UML's "outside" is one
   host kernel under our control; our exposure is host-FD
   tables and hostfs. Everything else is in-kernel and
   self-contained.

2. **PRNG / VMGenID equivalent.** Firecracker clones suffer
   duplicated guest PRNG state. *Mitigation:* on restore, bump
   `/dev/urandom`'s reseed-count equivalent and inject fresh
   entropy via `add_random_ready_callback`. Single call site,
   ~10 LOC.

3. **CPU feature drift across hosts.** gVisor bugs land here
   (issue #11486). *Mitigation:* at snapshot time, record host
   CPUID in `UML_VERSION` note; at restore time, refuse to
   resume if CPUID superset decreases, unless user passes
   `--force-cpuid-drift`. Document the failure mode loudly.

4. **Schema evolution landmines.** Firecracker's major-version
   bumps every field add. *Mitigation:* per D36, ELF PT_NOTE
   types with vendor-namespaced type numbers. Adding a field
   = new type number OR extending existing note with backward-
   compatible suffix. Dropping a field = keep reading it, ignore
   value, bump minor in UML_VERSION. Never renumber existing
   types.

5. **Restored process hanging on stdout-write.** gVisor
   issue #11064 — unreproducible in test, lives in prod.
   *Mitigation:* v2 does not restore console/tty state across
   restart; it expects the restore harness to re-open the
   console. Document as a known limitation. (Scenario 4
   read-only walk sidesteps this entirely.)

6. **Pagemap COW storm during dump.** CRIU issue #2386. UML
   sidesteps because it knows its own mmaps and writes
   contiguously.

## Open questions

- **Q1:** Does `userfaultfd` work inside UML's own address
  space for the lazy-pages tier? UML the process can certainly
  open userfaultfd to its *own* kernel regions; whether the
  ergonomics are sane is an open engineering question. Prior
  art: gVisor's pages file is served by FUSE, not userfaultfd;
  Firecracker uses userfaultfd. Either works in principle.
- **Q2:** Hostfs mount trees across restore — if the original
  hostfs path is gone, do we fail-loud or mount an empty
  replacement? Plan: fail-loud with an opt-in override.
- **Q3:** Does the `.umsnap` file survive cross-version
  kernels? I.e., snapshot from vmlinux A, restore into
  vmlinux B where B has a different task_struct layout. Plan:
  **no**, and the build-id check in `UML_VERSION` catches it.
  Cross-version restore is a harder problem than v2 commits
  to; we punt explicitly. This is the same posture as
  QEMU savevm and gVisor.
- **Q4:** Can we share code with `fs/proc/vmcore.c`? Probably
  yes at the ELF-producer layer; investigate during the
  implementation design pass.
- **Q5:** Where does `uml-restore` live? `tools/uml/snapshot/`
  needs user sign-off per AGENT-PROMPT cross-subsystem rule.
  Alternatively, ship `uml-restore` as a Python script under
  `scripts/` that shells out to the UML binary with
  `--restore=<file>` and does setup. TBD.

## Commit plan (when v2 opens)

Three phases, each independently useful:

**Phase 1: Snapshot producer (ELF writer).** Kernel gains
`um_snapshot_write(struct file *, enum um_snapshot_mode)`. Walks
the mmap table (pull-forward from v1), serializes UML state
into ELF PT_LOAD + PT_NOTE form, writes to `struct file`. Can
be triggered from mconsole, debugfs, AFL-fd path. Tier-1
(full-preload) consumers can already use this for scenarios 2
and 4 via standalone ELF tooling. Commit count: 4-6.

**Phase 2: Full-preload restore.** New `uml-restore` tool (in
`tools/uml/snapshot/`, needs sign-off) + kernel-side
`um_snapshot_load(const char *path)` that mirrors
`um_snapshot_write`. Scenarios 1 and 3 light up. Commit count:
3-5.

**Phase 3: Lazy-pages tier.** Userfaultfd handler for the
RAM / shadow PT_LOAD regions. Scenario 1 becomes interactive.
Commit count: 2-3.

Total v2: ~10-14 commits, ~8-12 weeks wall-clock given one
engineer who has C-09 v1 context. Shorter than CRIU's per-
subsystem porting cost because we skip most of it.

## What v2 explicitly does not promise

- Live migration between hosts (we're not QEMU; KVM backend
  may revisit post-D).
- Cross-vmlinux-version restore (D36 discusses why).
- Full TCP state across restore (see §"Skip").
- Restoring VFIO / real hardware passthrough (UML doesn't
  have it; the point is moot).
- Security-grade snapshot integrity (signed files, encrypted
  state at rest) — v3 concern; noted for completeness.

## Relationship to the rest of the plan

- Builds on C-09 v1's kernel seam (same static key, same
  ready-point contract, same worker_init primitives).
- Consumes v1 pull-forward items: mmap enumeration table, FD
  disposition annotations, state_version sysfs node, ready-
  point assertions (see D37).
- Touches no other workstream's scope except to **strengthen**
  C-09's existing invariants.
- Opens when D35's revisit triggers fire, not before.

## Cross-references

- D35: C-09 v1 scope and the fork-vs-CRIU split.
- D36: Format choice (ELF + PT_NOTE) and alternatives.
- D37: v1 pull-forward items (mmap table, ready-point
  contract, FD disposition, state_version, KASAN DONTFORK
  fix).
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`:
  the v1 design this builds on.
- `fs/binfmt_elf.c`, `fs/proc/vmcore.c`: in-tree ELF
  producers whose structure v2's writer should mirror.
- gVisor `pkg/sentry/pgalloc/pgalloc.go`: prior art for the
  demand-fault tier.
