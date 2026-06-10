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

Successful daemon-routed guest exec now has a first bounded kernel primitive.
The daemon uses a native mconsole client, waits for the member socket to answer
`version`, sends `exec <command>`, decodes the kernel's JSON reply, and maps
it into the existing `umlctl exec` NDJSON contract.

The current kernel command is shell-backed and captures up to 2 KiB each of
stdout and stderr through temporary guest files. `pool-exec-smoke` now proves
case A on a freshly built kernel: `/bin/true` exits 0, a shell command returns
captured stdout/stderr, and guest exit code 7 is preserved without a daemon
transport error.

The remaining exec work is timeout/cancellation. The kernel command currently
waits for the helper to finish; the daemon receive timeout can still bound the
client-facing call, but it does not yet cancel a long-running guest helper.

## Next Work

Recommended next sequence:

1. Add a small, bounded diagnostic mode or selftest probe that can arm a
   non-empty `mconsole_path` and preserve the boot log without relying on the
   daemon's null stdout/stderr. Current status: done as
   `tools/testing/selftests/um/pool-mconsole-path-probe/`; it now passes by
   sending `version` through the per-member socket.
2. Add kernel-side timeout/cancellation semantics for `exec` so a daemon
   timeout cannot leave a long-running guest helper behind.
3. Decide whether the shell-backed command string is the final ABI or whether
   it should be replaced with a stricter argv/env/cwd encoding before the
   completion claim.
