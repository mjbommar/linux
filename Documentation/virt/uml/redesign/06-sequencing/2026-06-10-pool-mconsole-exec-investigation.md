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

## Findings

There are two independent blockers behind successful daemon-routed exec.

First, the per-member mconsole socket is not present for daemon-taken pool
members. `pool_serve.rs` synthesizes a path such as:

```text
<runtime>/pools/<pool>/<instance>.mconsole
```

but the socket file does not appear before `umlctl exec` checks it.

Second, the active kernel mconsole command table does not contain an `exec`
verb. The current commands in `arch/um/drivers/mconsole_user.c` are:

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

Therefore, even a durable per-member mconsole socket would still need either a
new kernel-side command execution primitive or a different daemon transport.

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

Successful daemon-routed guest exec needs at least two implementation steps:

1. Make per-member control sockets durable for live replicated pool members
   without destabilizing the child before it re-enters userspace.
2. Provide an actual guest command primitive. Either add a kernel mconsole
   `exec` command with a bounded ABI, or replace the daemon's current
   `uml_mconsole exec` assumption with another transport that can return
   stdout, stderr, exit status, signal, and timeout state.

Until both are done, `pool-exec-smoke` should continue to be treated as an
error-envelope test, not proof of successful guest exec.

## Next Work

Recommended next sequence:

1. Add a small, bounded diagnostic mode or selftest probe that can arm a
   non-empty `mconsole_path` and preserve the boot log without relying on the
   daemon's null stdout/stderr.
2. Find the first safe point after `POOL_REPLICATE_OK` where the child can bind
   a process-local control socket without corrupting the repaired task, timer,
   IRQ, or SKAS stub state.
3. Once the socket exists reliably, send a harmless existing mconsole command,
   such as `version`, to prove per-member addressability.
4. Design the guest exec primitive. The preferred shape should be explicit and
   bounded: argv, cwd, environment, timeout, stdout/stderr capture limits, exit
   code, signal, and an error path that preserves the current NDJSON contract.
5. Update `pool-exec-smoke` so case A becomes a required pass once the kernel
   advertises the exec primitive, while older kernels continue to produce the
   current clean failure envelope.

