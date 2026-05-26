# 09 — Fork-server (template-pause + fork) — UML's fast-spawn story

**Sprint:** post-2026-05-19 fork-server sprint
**Priority:** HIGHEST (the keystone of the 5-week redesign)
**Status:** designing 2026-05-20
**Depends on:** memo 26 snapshot (DONE), memo 52 host controls (DONE),
fast_boot (DONE), kvm-v2 backend (DONE)

This memo replaces the 2026-05-19 draft of the same number.  The
earlier version proposed a Firecracker-shaped "snapshot file →
new process restores from disk" design.  Re-reading the research
(syzkaller's `vm/vmimpl` contract; Firecracker's actual snapshot
flow; Cloud Hypervisor's "snapshot pool") + UML's process model
(the guest IS a host process; `fork(2)` is essentially free
CoW), the right design is simpler: **template-pause + fork**.

---

## 1. Use case driven design — syzkaller

Five weeks of work have a single deliverable that matters more
than any other: **UML v2 must be a credible syzkaller backend.**
Everything else (the 17 audit fixes, the io_uring work, the
transparency commands) is incremental.  This memo's design must
optimize for syzkaller's actual hot loop:

```
syz-manager spawns N worker slots.  Each slot runs forever:
  loop:
    inst, err := Pool.Create(workdir, slot_idx)        # << this
    inst.Copy(syz-executor)                            # ~10 MB
    inst.Forward(manager_rpc_port)                     # one port
    out, errc := inst.Run("syz-executor runner ...")   # streams
                                                       #   for ~1 h
    if crash: inst.Diagnose(rep)
    inst.Close()                                       # << and this
```

Two functions matter: `Create` and `Close`.  Once `Run` is
called the VM serves ~1 hour of fuzzing.  Throughput of
`Create+Close` matters during crash storms when many slots
recycle simultaneously.

**Reference numbers (from the research report):**

  * syzkaller dispatcher: one persistent goroutine per slot;
    `Create → Copy → Forward → Run → Close → Create...`
  * VMs are NOT reused after `Close` — every crash + every
    natural timeout creates a fresh instance.
  * Only QEMU has a "snapshot mode" today; it uses
    `savevm/loadvm` + ivshmem.  No other backend (gce,
    isolated, gvisor, virtualbox, vmware, bhyve, ...) has
    snapshot or fork.

**What syzkaller would do with `umlctl pool take`:**

```go
// hypothetical UML backend in syzkaller/vm/uml/uml.go
func (p *pool) Create(workdir string, idx int) (vmimpl.Instance, error) {
    out, err := exec.Command("umlctl", "pool", "take",
        p.pool_name, "--as", fmt.Sprintf("syz-%d", idx),
        "--workdir", workdir, "--json").Output()
    if err != nil { return nil, err }
    var info struct {
        InstanceName string
        Pid          int
        ConsoleFifo  string
        SSHHost      string  // or unix-socket / vsock
        SSHPort      int
    }
    json.Unmarshal(out, &info)
    return &instance{info: info}, nil
}
```

Per-instance `Run/Copy/Forward/Close` go through the standard
`umlctl` lifecycle commands (`exec`, `cp`, `port-forward` —
to add).  The new piece is `pool take`.

**Latency budget (from syzkaller's perspective):**

  * `Pool.Create` should beat QEMU's ~5 s boot.  Target: 5 ms
    median, 50 ms p99.  Cold-boot equivalent for failure cases
    is 207 ms.
  * `Instance.Close` is best-effort but should be ~10 ms (kill
    + waitpid + cleanup).
  * Pool replenishment must be ≥ Pool.Create rate so the pool
    doesn't drain under sustained load.  Spec: configurable
    `min_warm` (default 4) and `refill_concurrency` (default 2).

---

## 2. Why "template-pause + fork" beats snapshot-file-on-disk

The earlier design serialized physmem to a file and had a new
process mmap + restore.  Three reasons that's the wrong shape
for UML:

  1. **UML doesn't separate VMM and guest.**  In Firecracker
     the VMM is a separate process from the kernel — `loadvm`
     into a fresh VMM is meaningful.  In UML the kernel IS the
     process; "load snapshot into fresh process" means
     re-running `start_kernel` which is what we wanted to skip.

  2. **`fork(2)` already does CoW physmem.**  Linux's fork
     gives us memory-shared-until-write for free.  The
     snapshot file route adds serialization + deserialization
     bytes-on-disk overhead that fork avoids entirely.

  3. **No backwards-compat surface.**  Snapshot files want
     stable format, version-skew handling, sha256
     authentication.  Fork-from-running-process has none of
     that — both ends are the same binary.

The template-pause + fork model:

```
                  +-------------------------+
                  |  umlctl pool serve      |    (supervisor daemon)
                  +-------------------------+
                              |
                  fork() N times CoW
                              |
                  +-----------+-----------+
                  |                       |
            +-----v------+         +------v-----+
            | master UML |         | worker #1  |  ... worker #N
            |  (paused)  |         |  (paused)  |
            +------------+         +------------+
            kernel state:          inherited CoW from master.
            booted + init done.    Re-bound identity on take.
            SIGSTOPped at the      SIGCONTs when "taken."
            template_pause check.
```

The pool supervisor:

  * Boots ONE master UML with `um_template_pause=1` kernel
    cmdline.
  * Master runs all the Umlfile's `[init.phases]` to the
    `READY` marker, calls a new kernel syscall-equivalent
    `um_template_pause()` which raises SIGSTOP on self.
  * Supervisor sees the SIGSTOP via `waitpid(WUNTRACED)`,
    then `fork(2)`s the master N times.  Each child is a CoW
    duplicate at the SIGSTOP point.
  * Children inherit the SIGSTOPped state from their parent.
    They sit ready until a `pool take` arrives.

On `umlctl pool take`:

  1. Pool supervisor picks an idle child.
  2. Supervisor opens a fresh tap fd, generates a new MAC,
     allocates a new IP from a pool range, picks a new instance
     name + bundle path.
  3. Supervisor `write()`s the identity blob to a memfd that
     the child has open.
  4. Supervisor `SIGCONT`s the child.
  5. Child's `um_template_pause()` returns, re-reads the
     identity blob, replumbs (swaps tap fd, rebinds netdev),
     resumes guest execution.
  6. Supervisor returns the instance bundle path + pid to the
     CLI caller (in JSON for `--json`).

The kvm-v2 snapshot API (`kvm_v2_snapshot_capture_full`,
`_restore_full`) is **not used** in this design.  Live fork
captures everything the snapshot API does AND more (every
buffer, every kernel data structure, every cached page) without
serialization overhead.  The kvm_v2 snapshot API remains
relevant for cross-process migration (memo 26's original
target); it's just not the right tool for spawning siblings.

---

## 3. Implementation phases

### Phase 1a — Kernel-side template-pause hook (~250 LoC)

Add a new initcall + a userspace-callable trigger:

```c
/* arch/um/kernel/template_pause.c (new file) */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <os.h>

static bool template_pause_armed __read_mostly;

static int __init template_pause_setup(char *str)
{
    template_pause_armed = true;
    return 1;
}
__setup("um_template_pause", template_pause_setup);

/* Called by an in-guest userspace process (typically umlctl's
 * init phase emits a sentinel that the bootstrap script
 * recognises and then opens /proc/um/template_pause).
 *
 * Behaviour:
 *   - On first open: SIGSTOP the UML host process.
 *   - On SIGCONT: read identity blob from the inherited memfd
 *     pointed to by env var UM_TEMPLATE_IDENTITY_FD.
 *   - Apply identity (new MAC for vec2.0, new mconsole socket
 *     path, new pidfile path).
 *   - Return from /proc/um/template_pause open() so the
 *     bootstrap script continues with the new identity.
 */
int um_template_pause_enter(void);
EXPORT_SYMBOL_GPL(um_template_pause_enter);
```

Expose via `/proc/um/template_pause` (write to enter; the
write blocks in-kernel via `os_stop_self_via_sigstop()` until
SIGCONT lands).  Identity re-application:

```c
struct um_template_identity {
    u32 magic;          /* 'UTID' */
    u32 version;
    char instance_name[64];
    u8  mac_addr[6];
    u8  _pad[2];
    char tap_name[16];      /* new IFF_TAP device name */
    char ipv4_cidr[20];     /* "10.7.0.42/24" */
    char ipv4_gateway[16];
    /* ... */
};
```

### Phase 1b — Pool supervisor (`umlctl pool serve`) (~400 LoC)

A new long-lived umlctl process that:

  * Reads an Umlfile + a pool config (size, replenish policy).
  * Boots the master with `um_template_pause` on the cmdline +
    `UM_TEMPLATE_IDENTITY_FD` env var pointing at a memfd.
  * Waits for master to SIGSTOP itself.
  * `fork(2)` N times.
  * For each child: register in pool state, mark `idle`.
  * Listen on a Unix socket (`$STATE/pools/<name>/api.sock`)
    for `take` / `replenish` / `destroy` / `status` requests.
  * On `take`: pick idle child, allocate identity, write to its
    memfd, SIGCONT, mark `taken`.
  * Track each taken child's pid; reap on death + auto-replenish.

State on disk under `$XDG_STATE_HOME/uml/pools/<name>/`:

```
pools/<name>/
├── config.json          - pool size, refill policy, umlfile path
├── api.sock             - Unix domain socket for CLI
├── supervisor.pid
├── master.pid
├── members/
│   ├── 0/
│   │   ├── pid
│   │   ├── state        (idle / taken)
│   │   └── identity     (last-applied identity blob)
│   ├── 1/...
└── log
```

### Phase 1c — `umlctl pool` subcommand (~200 LoC)

```
umlctl pool create <name> --umlfile <path> [--size N] [--min-warm M]
umlctl pool take   <name> [--as <inst>] [--workdir PATH] [--json]
umlctl pool list   [--json]
umlctl pool status <name> [--json]
umlctl pool replenish <name> [--target N]
umlctl pool destroy <name>
```

The CLI binary connects to the supervisor's Unix socket and
exchanges JSON-shaped messages.  `--json` mode emits a single
JSON object per command for shell / Go consumers.

### Phase 2 — Identity re-plumb (~250 LoC)

The forked child wakes from SIGCONT inside `um_template_pause_
enter()`.  At this point the kernel has the OLD identity (the
master's tap fd, the master's instance name in `/proc`).  The
hook reads the identity blob and:

  1. Calls a new `vec2.0` ethtool-style ioctl to swap the tap
     fd to the new one (passed via SCM_RIGHTS over the memfd
     channel — or a second memfd containing the fd).
  2. Updates the netdev MAC via `dev_set_mac_address`.
  3. Rebinds the netdev's IPv4 via in-kernel `inet_rtm_newaddr`.
  4. Writes the new instance name to a sysfs node that
     mconsole and other facilities read.

Bootstrap script (the init.phases caller of /proc/um/template_
pause) then:

  * Re-execs DHCP / static-ip setup against the new IP.
  * Starts the user payload (e.g. syz-executor) under the new
    identity.

### Phase 3 — Bench + acceptance (~150 LoC)

`tools/testing/selftests/um/pool-bench/`:

  * Boots a pool of 100 warm instances.
  * Measures: `take` latency p50/p99, `take` throughput
    (takes/sec sustained), memory amplification (RSS of
    supervisor + 100 children vs RSS of 1 master).
  * Compares against cold-boot (`umlctl up`) baseline.

**Acceptance gates:**

  * `take` median latency ≤ 5 ms (vs 207 ms cold-boot fast path);
    p99 ≤ 50 ms (allows for slow-path fork + identity setup).
  * 100 forks of a 128 MiB-mem master consume ≤ 200 MiB total
    RSS (CoW sharing of 99 %+ of physmem).
  * 60-second sustained take + close at 50 takes/sec without
    pool drain (assuming `min_warm=4` and `refill_concurrency=2`).
  * No memory leak across 10 000 take/close cycles.

### Phase 4 — syzkaller integration (~150 LoC across Go + docs)

A Go shim VM type at `syzkaller/vm/uml/uml.go` that:

  * Reads `umlctl pool` config from the syz-manager YAML.
  * Implements `vmimpl.Pool.Create` by calling `umlctl pool take`.
  * Implements `vmimpl.Instance.{Run,Copy,Forward,Close,Diagnose}`
    by calling `umlctl exec / cp / port-forward / stop / dmesg`.

Documentation:

  * `Documentation/virt/uml/redesign/06-sequencing/post-2026-05-19-
    next-sprint/09-fork-server-USAGE.md` — operator guide:
    "how to set up a pool, how to wire syzkaller, how to handle
    crashes."
  * Per-command help strings in umlctl with examples.

---

## 4. Why "easy to use AND powerful" (per the goal)

**Easy:**

```
umlctl pool create fuzz --umlfile fuzzer.toml --size 8
umlctl pool take fuzz   # ← returns running instance, ready to use
```

That's it.  Two commands to go from "kernel binary on disk" to
"running guest you can SSH into."  No snapshot files to manage,
no version-skew protocol, no separate VMM process to coordinate.

**Powerful:**

  * `--json` everywhere — every command emits parseable output
    for Go / Python / shell consumers.
  * Pool config in TOML — version-controllable, reviewable.
  * Per-pool seccomp profile (inherits from master Umlfile).
  * Crash callback hook: `--on-crash <script>` runs against the
    dead instance's bundle before reap.
  * Re-templating: `umlctl pool refresh <name>` re-boots the
    master with the same Umlfile, useful when the kernel
    binary changes.
  * Snapshot-to-disk OPTIONAL: `umlctl pool freeze <name>
    --to <file>` writes the master's `kvm_v2_snapshot_capture_
    full` output to disk for cross-host migration (memo 26's
    original use case).

---

## 5. Anti-goals / explicitly out of scope

  * **Cross-host migration of pool members.**  A `pool take`
    instance is local to its supervisor.  Cross-host migration
    needs the snapshot-file path (memo 26), which this memo
    does NOT replace.
  * **Live kernel update.**  If the master's vmlinux changes,
    `umlctl pool refresh <name>` reboots the master (cheap —
    template-pause again).  Pool members from an old master
    are reaped.  We do NOT try to hot-swap the kernel.
  * **Pool member up-time guarantees.**  A pool member that
    receives a `take` becomes a normal `umlctl` instance with
    standard lifecycle; the pool no longer tracks it.
  * **Memory ballooning.**  Each pool member uses the
    master's `mem=` setting.  Per-take memory adjustment is
    out of scope.

---

## 6. Risk notes + rollback

  * **fork-after-mmap surprises.**  The master may have
    file-backed mmaps (kernel binary, vmlinux modules,
    physmem).  These get inherited by children via the same
    inode.  Fine for read-only mappings; writeable ones (e.g.
    `/dev/kvm` fds, `vhost-net` fds) need careful handling.
    Mitigation: explicit `close()` of those fds in the child
    before SIGCONT returns; opened fresh per-identity.
  * **KVM_RUN per-CPU state.**  Each kvm-v2 vCPU has a
    `kvm_run` mmap'd from the host kernel.  Child inherits the
    fd; KVM's per-fd state may or may not be safe to share.
    Mitigation: research + bench.  Worst case: each child
    issues its own `KVM_CREATE_VCPU` after fork, paying ~3 ms
    per-take.
  * **Pool member crashes corrupt sibling state.**  Should not —
    fork's CoW isolates address spaces.  Verify with a fuzzer
    pass that intentionally crashes pool members.
  * **Rollback:** the pool feature is purely additive.
    `umlctl up` / `start` / `gate` / `mission` paths all keep
    working without any pool ever being created.  Setting
    `[runtime].fast_boot = true` (already shipped) is the
    fallback when pools aren't available.

---

## 7. Cross-references

  * Memo 26 — snapshot capture/restore.  Kept for cross-host
    migration; not used in the fork-server data path.
  * Memo 52 — host_resources.  Pool members inherit
    `[host_resources]` from the master Umlfile.
  * `e4ab828f1555` — fast_boot.  Cold-boot fallback path for
    pool create failure.
  * Memo 10 — vhost-net.  Pool members' tap fds open under the
    same handoff shape as standard umlctl (`tapfd::open_tap`).
  * Firecracker's snapshot/restore design:
    https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md
  * Cloud Hypervisor's "instance pool" usage:
    https://www.cloudhypervisor.org/docs/prologue/quick-start/
  * syzkaller `vmimpl.Pool` / `Instance` reference:
    https://github.com/google/syzkaller/blob/master/vm/vmimpl/vmimpl.go
