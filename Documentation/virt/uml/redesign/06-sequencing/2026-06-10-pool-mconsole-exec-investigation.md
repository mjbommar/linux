# Pool Mconsole Exec Investigation

Date: 2026-06-10
Branch: `next`

## Goal

Close the gap between the current `pool-exec-smoke` contract and the final
goal for daemon-routed guest execution.

Current `pool-exec-smoke` proves the NDJSON wire shape and the clean daemon
failure envelope. It does not prove successful command execution inside a
pool member.

## Current Verified Behavior

After rebuilding the stable source tree:

```sh
make ARCH=um -j16 linux
```

passed.

The focused pool gates also passed:

```sh
UM_FORK_KERNEL=$PWD/linux \
UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
tools/testing/selftests/um/pool-exec-smoke/run-pool-exec-smoke.sh
```

Result:

- daemon socket came up;
- `pool take` returned a live member pid;
- `umlctl exec --json -- /bin/true` emitted valid `start`, `stderr`, and
  `exit` NDJSON frames;
- exit code was `1`, as expected for the current missing-feature path; and
- the daemon error said the synthesized member mconsole socket was not present.

```sh
UM_FORK_KERNEL=$PWD/linux \
UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl \
tools/testing/selftests/um/pool-serve-smoke/run-pool-serve-smoke.sh
```

passed, including warm ready prefill, ready take, replenish, request-specific
take, destroy, shutdown, and master cleanup.

```sh
UML_BINARY=$PWD/linux UML_POOL_REPLICATE=1 \
tools/testing/selftests/um/template-pause-pool-sustained-smoke/run-template-pause-pool-sustained-smoke.sh
```

passed with 3/3 members reaching `MEMBER_DONE`, three `POOL_REPLICATE_OK`
markers, no kernel panic, and no v1 ceiling regression.

The bounded mconsole-path probe now records the first closure in this area:

```sh
UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-mconsole-path-probe/run-pool-mconsole-path-probe.sh
```

Current result: PASS.

- `PMCON_MEMBER_DONE : 1`;
- `POOL_ENTER : 1`;
- `POOL_REPLICATE_OK : 1`;
- `Kernel panic : False`;
- requested mconsole path under `/tmp/pool-mconsole-path-probe.*/`;
- `mconsole exists : True`; and
- `version` returns the member's kernel version string through the per-member
  socket.

## Findings

There were two independent blockers behind successful daemon-routed exec.

The first blocker is now closed. `pool_serve.rs` synthesizes a path such as:

```text
<runtime>/pools/<pool>/<instance>.mconsole
```

That path now exists for live replicated pool members and answers existing
mconsole commands after the child re-arms SIGIO ownership on the inherited
socket.

The remaining blocker is that the active kernel mconsole command table does
not contain an `exec` verb. The current commands in
`arch/um/drivers/mconsole_user.c` are:

- `version`;
- `halt`;
- `reboot`;
- `config`;
- `remove`;
- `sysrq`;
- `help`;
- `cad`;
- `stop`;
- `go`;
- `log`;
- `proc`;
- `stack`; and
- `snapshot_export`.

Therefore, successful daemon-routed guest exec still needs either a new
kernel-side command execution primitive or a different daemon transport.

## Current Fix

The landed fix has two parts:

1. `um_template_identity_apply()` applies `mconsole_path` before looking for a
   target netdev. Hostfs-only guests have no non-loopback target netdev, but
   they still need a control socket.
2. `child_entry_pool_member()` calls `mconsole_rearm_for_pool_member()` after
   timer/task/stub repair. The master binds the socket before fork, but
   `F_SETOWN` still targets the master host process; the child must re-arm
   SIGIO ownership before the socket is addressable.

The fix deliberately does not add an exec command. It only proves the
per-member control socket is durable and addressable.

## Invalidated Approach

A local experiment split template-pause identity application into:

- network identity in the master before fork; and
- mconsole rebinding in the child after fork.

The experiment was not committed. It was reverted before the stable validation
above.

What passed:

- the code built with `make ARCH=um -j16 linux`;
- existing pool smokes still passed in the narrow case where no mconsole socket
  was successfully rebound.

What failed:

- a captured direct pool-member boot with a non-empty `mconsole_path` never
  created the requested socket;
- the member did not reach `TPPM_POST_PAUSE` or `TPPM_MEMBER_DONE`;
- the log showed repeated `Kernel panic - not syncing: Kernel mode fault at
  addr 0x0` after `POOL_REPLICATE_OK`; and
- there was no successful `mconsole_reinit` marker in the log.

The failed probe used the normal pool-member identity shape plus only a
non-empty `mconsole_path`, so the failure is tied to the mconsole path being
armed rather than to missing network fields.

Conclusion: do not land a simple child-side mconsole rebind until the child
repair and control-socket ownership model is better understood.

## Implementation Implications

Successful daemon-routed guest exec still needs an actual guest command
primitive. The daemon now uses a native mconsole client and waits for the
member socket to answer `version` before issuing `exec`, so the remaining work
is either a kernel mconsole `exec` command with a bounded ABI or another
transport that can return stdout, stderr, exit status, signal, and timeout
state.

Until that is done, `pool-exec-smoke` should continue to be treated as an
error-envelope test, not proof of successful guest exec. The daemon now reaches
the later kernel-command failure mode:

```text
mconsole command failed: Unknown command
```

## Next Work

Recommended next sequence:

1. Add a small, bounded diagnostic mode or selftest probe that can arm a
   non-empty `mconsole_path` and preserve the boot log without relying on the
   daemon's null stdout/stderr. Current status: done as
   `tools/testing/selftests/um/pool-mconsole-path-probe/`; it now passes by
   sending `version` through the per-member socket.
2. Design the guest exec primitive. The preferred shape should be explicit and
   bounded: argv, cwd, environment, timeout, stdout/stderr capture limits, exit
   code, signal, and an error path that preserves the current NDJSON contract.
3. Update `pool-exec-smoke` so case A becomes a required pass once the kernel
   advertises the exec primitive, while older kernels continue to produce the
   current clean failure envelope.
