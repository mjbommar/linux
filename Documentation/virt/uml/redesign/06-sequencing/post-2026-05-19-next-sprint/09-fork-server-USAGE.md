# 09 — Fork-server operator USAGE guide

**Status:** Draft (2026-05-22).  Documents the **current** state of the
template-pause + pool-fork primitive for operators integrating UML as
a fuzzing / dispatch backend (syzkaller, AFL++, custom harnesses).

This guide describes what works **today**, not the eventual sustained-
dispatch target.  See `09-fork-server-STATUS.md` for the design
history and `Documentation/virt/uml/redesign/06-sequencing/
2026-05-22-pool-completion-roadmap.md` for the path to sustained
dispatch.

---

## 1. What works today

  * **Single-iter pool member.** Master UML boots, runs init.sh up to
    the `echo … > /proc/um/template_pause` point, then `SIGSTOP`s
    itself.  On the next `SIGCONT`, master `clone()`s a child UML
    kernel that returns to userspace as a fresh pool member.  The
    child runs the rest of init.sh (the test workload) to natural
    completion; master loops back to `SIGSTOP` and is ready for the
    next take.

  * **Per-iter identity injection.** Before each `SIGCONT`, the
    operator can `pwrite` a `struct um_template_identity` blob into
    the identity memfd (instance name, MAC, host TAP name, IPv4
    CIDR, gateway).  Master applies it to the child before resuming.

  * **Per-pool-member fresh stub.** The child gets a fresh seccomp
    stub child with a private per-mm `stub_data` memfd.  No
    inter-member aliasing for the stub IPC page.

  * **Path A primitive.** The M-fork child runs on a private
    MAP_PRIVATE kernel stack from the moment of clone, so the
    classic kernel-stack race between master and child (commit
    `9c6c4948dc70`) does not apply.

  * **Strict fork-stress gates.** `template-pause-fork-stress`
    passes 100/100 at N=100, 10/10 at N=1000, 10/10 under
    `stress-ng --cpu $(nproc)` background load.  Zero kernel
    panics across 120 runs.

## 2. What does not work yet

  * **Sustained N-member dispatch.** iter 2+ XFAILs at the
    documented MAP_SHARED-physmem ceiling: iter 1's bash userspace
    pages propagate to master via the shared `physmem_fd` backing,
    corrupting iter 2's view of libc text.  Roadmap §3 documents
    the architectural gap and the in-flight investigation.

  * **The pool-bench acceptance gates** (5 ms p50, 50 ms p99,
    ≤200 MiB RSS at N=100, 10 000-cycle no-leak): blocked on
    sustained-smoke first PASSing.

  * **The syzkaller `vm/uml` Go shim:** designed (see
    `11-syzkaller-shim-spec.md`), not implemented.  Lives in the
    syzkaller repository, not this kernel tree.  Until sustained-
    dispatch works, syzkaller users must drive UML in
    single-shot-per-test mode (one UML kernel per syscall sequence,
    equivalent to the `vm/qemu` cold-boot model).

## 3. Build prerequisites

Required Kconfigs (defconfig + flip):

  * `CONFIG_UM_TEMPLATE_PAUSE=y` — the in-kernel
    `/proc/um/template_pause` hook (Phase 1a).
  * `CONFIG_UM_TEMPLATE_PAUSE_FORK=y` — the fork-on-resume loop
    (Phase 2a).  Selects `UM_FUZZ_HOOKS` and
    `UM_SNAPSHOT_FORKSERVER`.
  * (optional) `CONFIG_UM_TEMPLATE_PAUSE_FORK_DIAG=y` — only for
    active fork-bug debugging.  Off in production.
  * (optional) `CONFIG_UM_TEMPLATE_PAUSE_IDENTITY_KUNIT=y` — only
    when running the identity-blob KUnit tests under `kunit.py`.

Build (always use `O=`):

```
cd $LINUX_SRC
make ARCH=um O=$BUILD_DIR defconfig
./scripts/config --file $BUILD_DIR/.config \
    --enable UM_TEMPLATE_PAUSE \
    --enable UM_TEMPLATE_PAUSE_FORK
make ARCH=um O=$BUILD_DIR -j$(nproc)
```

The resulting `$BUILD_DIR/linux` is the UML binary.

## 4. Run-time arming

Master is armed via kernel command line:

```
linux mem=128M rootfstype=hostfs rootflags=/ root=/dev/root rw \
      ncpus=1 \
      um_template_pause=fork \
      um_template_pause_pool_member=1 \
      init=/path/to/init.sh
```

  * `um_template_pause=fork` — enables the fork-on-resume loop.
    Master will `clone()` on each `SIGCONT` instead of returning
    to userspace from the SIGSTOP.
  * `um_template_pause_pool_member=1` — the M-fork child drops
    into the new-pool-member path: `child_entry_pool_member()`
    runs, refreshes per-task state (timer, stubs, mm_id), then
    enters userspace as a fresh long-lived member.

Without `=pool_member`, the child terminates immediately after fork
(production: SIGKILL'd by master in the kvm-aware path).

## 5. Identity blob handoff

Before each `SIGCONT`, write 264 bytes of
`struct um_template_identity` to the identity memfd (file
descriptor passed via the `UM_TEMPLATE_IDENTITY_FD` env var, set
on master's exec).  Layout:

```
offset  field                       type / size
0       magic = 0x44495455 ("TIDU") u32
4       version = 1                 u32
8       instance name               char[64], NUL-padded
72      MAC (6 bytes)               u8[6]
78      pad                         u16
80      tap device name             char[16], NUL-padded
96      ipv4 cidr ("10.7.0.42/24")  char[20], NUL-padded
116     ipv4 gateway                char[16], NUL-padded
132     reserved                    char[96]
228     pad                         char[32]
260     (master writes child's host pid back here, u32)
```

After SIGCONT, master logs:

```
template_pause: identity at "fork-smoke" instance="pool-member-N"
                mac=… tap=… ipv4=… gw=…
template_pause: identity-parsed name=…
```

If the supervisor never updated the blob, master uses the previous
one (the kernel does not enforce per-iter uniqueness).

## 6. Selftests

Run the kernel-side selftests against a built UML binary:

```
UML_BINARY=$BUILD_DIR/linux \
  bash tools/testing/selftests/um/template-pause-pool-member-smoke/\
       run-template-pause-pool-member-smoke.sh
```

Expected: `PASS`.  init.sh in the child reaches `TPPM_MEMBER_DONE`
plus the per-iter identity-parsed line; no kernel panic; no v1
ceiling regression.

```
UML_BINARY=$BUILD_DIR/linux \
  bash tools/testing/selftests/um/template-pause-pool-sustained-smoke/\
       run-template-pause-pool-sustained-smoke.sh
```

Expected today: `XFAIL` (exit 4) with verdict line "iter 1 PASS,
iter 2+ hits MAP_SHARED physmem limit".  iter 1 reaches
`SUSTAINED_MEMBER_DONE`; iter 2 times out at the architectural
ceiling.  This is the documented state until Step A of the pool-
completion roadmap is resolved.

## 7. Supervisor: `umlctl pool spawn`

The Rust supervisor at `tools/uml/uml-launcher/src/bin/umlctl/`
ships a `pool spawn` subcommand (Memo 09 Phase 1c) that handles
the master lifecycle, identity-memfd allocation, and SIGCONT
loop.  See `tools/uml/uml-launcher/README.md` for invocation.

The companion `pool serve` daemon (Memo 09 Phase 1c, commit
`5576cdf21084`) is a long-running supervisor that drives the
master through repeated SIGCONT cycles on demand.  Today it is
single-iter only (each take spawns a new master); sustained
N-member dispatch from one master is the syzkaller integration
target.

## 8. Known limitations & escape hatches

  * **KVM backend refused.** The fork-on-resume path is refused
    under the KVM backend at runtime (fork() aliases `/dev/kvm`
    fds and per-vCPU mmap state).  Use the default seccomp
    backend.

  * **SMP not yet supported.** Master must boot with `ncpus=1`.
    Multi-vCPU fork is deferred (the timer rebuild path in
    `os_timer_worker_rebuild` is UP-only).

  * **No live identity-apply target netdev test.** Identity
    parsing is verified by KUnit and the smoke test, but the
    actual `um_template_identity_apply` to a host netdev/TAP
    requires a live tap device at runtime; the smoke test logs
    `identity apply at "fork-smoke" returned -19 (continuing)`
    when no tap exists — that is normal for hostfs-rootfs tests.

  * **sustained-smoke XFAIL is the current contract.** Do not
    rely on iter 2+ working.  Operators integrating against the
    pool today should drive single-iter per-take dispatch (each
    `pool spawn` launches a new master process; mortality after
    one iter is expected and not a regression).

## 9. Reading further

  * `09-fork-server-STATUS.md` — phase-by-phase landed-commit
    table.
  * `09-fork-server-snapshot-restore.md` — original Memo 09 vision
    (sustained N-member dispatch as the syzkaller backend).
  * `2026-05-22-pool-completion-roadmap.md` — working-backwards
    plan from 100% vision; documents the Step A architectural
    gap blocking sustained-dispatch.
  * `11-syzkaller-shim-spec.md` — syzkaller `vm/uml` Go shim
    design (lives in syzkaller repo; not yet implemented).

---

*This guide is updated as the pool-completion roadmap advances.
The "what works today" / "what does not work yet" split is the
canonical interface between operators and kernel work — when
sustained-dispatch lands, §2 collapses and §1 expands.*
