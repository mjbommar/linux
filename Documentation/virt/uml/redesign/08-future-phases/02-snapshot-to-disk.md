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

## Two capture modes: crash-consistent and application-consistent

v2 provides **two capture modes** with the same on-disk format
(ELF + `PT_NOTE`; §"Chosen format" below) but different
coordination stories with the running UML guest. This is the
standard dichotomy every mature snapshot system eventually
lands on: Microsoft VSS calls it "crash-consistent vs
application-consistent", MySQL calls it "hot vs cold", qemu
calls it "external vs internal". See D38 for why we commit
to both.

### Mode A — crash-consistent, external, no guest pause

**Mechanism.** An *outside-the-UML-guest* process (a sidecar,
a CI wrapper, an `uml-snapshot` CLI) captures UML's state
without asking the UML kernel to cooperate. The UML guest
keeps running; it does not know a snapshot is being taken.

Three tricks make this almost-free on UML specifically:

1. **Guest RAM is already a tmpfs file.** `physmem_fd` is an
   unlinked tempfile in `/dev/shm` or similar (see D37 pull-
   forward #5). Capturing RAM is literally
   `cp /proc/$uml_pid/fd/$physmem_fd ram.bin` — no ptrace,
   no pagemap walk, no COW dance. On btrfs/xfs,
   `cp --reflink=always` makes it O(1).
2. **Registers come from `ptrace(PTRACE_GETREGS)`.** A brief
   (~µs) host-level pause per UML thread, nothing the UML
   kernel notices.
3. **FD table comes from `/proc/$uml_pid/fd/`.** Just an
   `ls` + `readlink` of the directory.

UML sees no pause, no quiesce, no signal. The captured state
is "whatever happened to be in RAM at the moment the tool
read it" — some kernel structures may be mid-mutation, some
locks may be held, some RCU callbacks may be partway through.
Restore requires a recovery pass (analogous to filesystem
`fsck` after a crash): drop in-flight state, re-derive
invariants, continue.

**Cost.** A 2 GB guest: ~100 ms of tmpfs read, 0 ms of UML
pause. On btrfs/xfs with reflink: O(1) snapshot, still 0 ms
pause. Recovery pass on restore adds ~10-100 ms depending on
how much state was mid-mutation.

**Use cases.**
- CI "snapshot everything every hour in production" where
  pausing prod is unacceptable.
- Regression reproducers where we want the state that existed
  *before* we knew there was a problem; asking the guest to
  quiesce first would lose the evidence.
- Forensics / debugging; "what was the kernel actually doing
  when the bug happened?"

**Known failure modes.**
- Kernel scheduler queues caught mid-link-update: recovery
  pass rebuilds.
- `rcu_pending()` cbs snapshotted but corresponding data
  already freed in restore: recovery pass drops them.
- Dirty inodes half-written: recovery re-queues or drops.
- Restore-time crashes are real and have to be tolerated by
  the caller. Mode A is "best effort with a recovery pass";
  it is not a guarantee.

### Mode B — application-consistent, cooperative, brief pause

**Mechanism.** The UML kernel is signalled (`SIGRTMIN+N`,
debugfs write, mconsole command, AFL fd handshake — see
§"Trigger channels" below) to hit the cooperative ready
point. From there, the same path v1 uses (§`09-snapshot-
forkserver.md`): park kthreads, drain RCU, stop timer, assert
strict ready-point contract. Then instead of (or in addition
to) forking, the kernel walks the mmap-registry table and
writes the ELF snapshot file. Resumes after the write.

Everything is guaranteed consistent. Restore is deterministic.
No recovery pass needed.

**Cost.** Quiesce takes microseconds; writing the file takes
O(RAM) host-write time. A 2 GB guest: ~50-500 ms depending on
disk, all of it UML-paused. On tmpfs restore-target: ~100 ms.

**Use cases.**
- Survive host reboot — where the guarantee matters and 50 ms
  of pause is nothing.
- Portable reproducer packaging where the recipient will
  restore and expect deterministic behavior.
- Deliberate "checkpoint here" with production-grade
  consistency guarantees.

**Known failure modes.** The same six from
§"Known failure modes to design against" below — they apply
equally to Mode B, because the external-world coupling (PRNG
reseed, CPUID drift, host clock jump) happens on restore
regardless of whether capture was atomic.

### Side-by-side

| Property | Mode A (crash-consistent) | Mode B (application-consistent) |
|---|---|---|
| UML pause on capture | 0 ms (ptrace: ~µs) | ~µs quiesce + O(RAM) file-write |
| UML cooperation required | none | yes (ready-point; signal trigger) |
| Restore semantics | recovery pass needed | deterministic |
| Implementation cost | ~200 LOC host-side tool | kernel-side writer + host restore |
| When it is right | live-running systems, forensics, CI | deliberate checkpoints, handoff |
| When it is wrong | caller can't tolerate restore failures | cost of UML pause matters |

Both modes produce the **same ELF file format** (§"Chosen
format" below). An `ELF_NT_NOTE` of type `UML_CAPTURE_MODE`
records which mode produced the file, and the restore path
branches accordingly: Mode A → run recovery pass first; Mode B
→ resume directly.

### Mode selection

Caller-driven. The host-side CLI offers:

```
uml-snapshot --mode=crash-consistent $PID $FILE   # Mode A
uml-snapshot --mode=app-consistent   $PID $FILE   # Mode B (default)
```

Per-use-case defaults: CI / forensics → A; survive-reboot /
portable-reproducer → B.

### Trigger channels (Mode B)

Four channels, composed through the one
`um_snapshot_ready(point)` entry point that v1 already
introduces:

| Channel | When it fires | Profile availability |
|---|---|---|
| `SIGRTMIN+N` signal | host sends signal to UML pid | any profile (signals are universal) |
| `/sys/kernel/debug/um/snapshot_ready` write | guest userspace or mconsole pokes it | profiles with DEBUGFS=y |
| `mconsole snapshot-ready` | host sends mconsole command | profiles with MCONSOLE=y (not fuzz) |
| AFL fd 198/199 handshake | host pre-opens fds; init checks | profiles with UM_SNAPSHOT_FORKSERVER=y |

The signal channel is new in v2 — v1 (09-snapshot-
forkserver.md) doesn't need it because the fuzz profile only
ever triggers via AFL fds. v2 adds the signal channel for
reach-from-outside scenarios (CI, sidecar) where none of the
other three are guaranteed available.

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
├── PT_NOTE: UML_CAPTURE_MODE       (Mode A crash-consistent or Mode B
│                                    application-consistent; restore branches
│                                    on this — Mode A runs recovery pass first,
│                                    Mode B resumes directly. See D38.)
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

Same file, three restore paths. Mode A (crash-consistent)
captures run through an extra **recovery pass** before any
tier; Mode B (application-consistent) captures skip the
recovery pass.

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

### Recovery pass (Mode A only)

Mode A captures may contain mid-mutation kernel state. Before
any restore tier runs, the recovery pass must:

- Walk the scheduler runqueues and drop any task in
  `TASK_RUNNING` that is also mid-`__schedule()`
  (`prev->on_cpu != 0` when we weren't `prev`). Those tasks
  had already decided to yield; force them to re-enter
  `schedule()`.
- Walk the RCU callback lists and drop any callback whose
  target struct has already been re-allocated (checked via a
  slab generation counter captured at snapshot time).
- Drop inflight writeback — any inode with `I_DIRTY` set and
  a writeback bio partway through gets re-marked dirty with
  bio reset, so the restored kernel re-initiates the write.
- Drop any `wait_queue` entry whose `waker` refers to a
  freed address.
- Re-validate every lock: if any `raw_spinlock_t` is "held"
  at snapshot time (owner != -1), WARN and release — the
  captured state was mid-critical-section; the caller asked
  for crash-consistent, they get fsck semantics.

This is explicitly a best-effort pass. If it cannot recover
cleanly, it logs the violation and fails the restore. Mode A
callers know this is a risk; that's what "crash-consistent"
means.

Mode B captures skip this pass entirely: the ready-point
assertions from D37 pull-forward #2 guarantee none of the
above conditions held at capture.

Tier 1 is the mandatory starting point. Tiers 2 and 3 are
independent layers on top. Recovery pass is mandatory for
Mode A, skipped for Mode B.

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
- **Q6:** For Mode A, does `ptrace(PTRACE_GETREGS)` on UML's
  vCPU host threads give us the actual guest register state,
  or UML-kernel-side state? The backend ops table already
  provides `read_guest_regs`; Mode A would need a kernel-
  cooperative version (e.g. a tiny mconsole command that
  dumps regs to a sidecar fd) OR would need to translate
  from host-thread state to guest state offline. Leaning
  toward the former; Mode A would then ask the kernel for
  regs but not for quiesce — the pause is still only
  ~microseconds.
- **Q7:** Can Mode A + Mode B coexist in one `.umsnap` file
  (for use cases like "snapshot fast and then refine with
  cooperative rewrite in the background")? Plan: no, keep
  the format single-mode. Two files, two separate captures
  is clearer for restore-time reasoning.
- **Q8:** Recovery pass failure modes — if the pass cannot
  recover cleanly, should it print-and-abort, print-and-
  continue-with-warnings, or fall back to read-only walk?
  Plan: print-and-abort for anything that would compromise
  correctness; print-and-continue for drift that is only
  observable externally. Needs per-violation triage in the
  actual implementation.

## Commit plan (when v2 opens)

Four phases. Mode A (crash-consistent) ships first because it
is strictly cheaper — no kernel-side writer, no new ready-point
channel. Mode B (application-consistent) layers on top of v1's
cooperative ready-point infrastructure.

**Phase 1: Mode A host-side capture.** New `tools/uml/snapshot/
uml-snapshot` CLI that takes `--mode=crash-consistent <pid>
<file>` and produces an ELF + `PT_NOTE` file by: `ptrace`'ing
UML briefly to read registers, `cp --reflink=always` (or plain
`cp`) of `physmem_fd` from `/proc/$pid/fd/$n`, enumeration of
open fds via `/proc/$pid/fd/`, write ELF header + notes. No
kernel changes. Host-side only; needs user sign-off per AGENT-
PROMPT cross-subsystem rule. Commit count: 3-5.

**Phase 2: Recovery pass + full-preload restore (both modes).**
Kernel gains `um_snapshot_load(const char *path,
enum um_snapshot_mode)` that mirrors the reader side. Implements
the Mode A recovery pass (§"Recovery pass" above). Scenarios 2
and 4 (read-only review + portable reproducer with recovery)
light up. Scenario 3 (CI golden stamp) lights up for Mode A.
Commit count: 4-6.

**Phase 3: Mode B kernel-side writer.** Kernel gains
`um_snapshot_write(struct file *)` driven by the cooperative
ready-point path. Walks the mmap table (pull-forward from v1),
serializes UML state into ELF `PT_LOAD` + `PT_NOTE` form,
writes to `struct file`. Triggered from the signal channel
(new) + debugfs + mconsole + AFL-fd path (existing from v1).
Scenario 1 (survive reboot with determinism) now has its
guaranteed-clean path. Commit count: 4-6.

**Phase 4: Lazy-pages tier.** `userfaultfd` handler for the
RAM / shadow `PT_LOAD` regions. Scenario 1 becomes interactive.
Commit count: 2-3.

Total v2: ~13-20 commits, ~10-14 weeks wall-clock given one
engineer who has C-09 v1 context. Shorter than CRIU's per-
subsystem porting cost because we skip most of it. The Mode A
/ Mode B split adds ~2 weeks over the original estimate; the
benefit is that Mode A can ship independently and provides the
use cases (forensics, CI) that most users will actually hit.

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
