# Sustained Pool-Member Physmem Isolation Plan

Date: 2026-06-10
Branch: `next`

## Goal

Make `um_template_pause=fork um_template_pause_pool_member=1` support repeated
live pool-member takes from one paused master:

- each take returns a distinct live UML member;
- member 1 can continue running while the master serves member 2;
- member writes do not mutate the master's scheduler, task, mm, or userspace
  state;
- the existing one-shot pool-member smoke still passes;
- the sustained smoke flips from expected failure to pass; and
- the daemon pool, warm `min_warm`, exec, port-forward, and vector2/TAP paths can
  be validated on real live members.

The result must be production code, not a selftest-only workaround.

## Current Facts

The current committed tree deliberately keeps `um_pool_replicate_physmem()`
unwired from `child_entry_pool_member()`.

With no physmem replication:

- `template-pause-pool-member-smoke` passes for one member;
- `template-pause-pool-sustained-smoke` now keeps the first accepted member
  alive after `MEMBER_DONE`;
- the bounded sustained run reports:
  - iteration 1 reaches `MEMBER_DONE`;
  - iteration 1 reports a nonzero child-pid slot;
  - iteration 2 times out before the second `POOL_ENTER`;
  - iteration 2 leaves the child-pid slot at `0`;
  - no kernel panic is observed;
  - no v1 ceiling regression is observed; and
  - no UML process remains after process-group teardown.

Temporary local markers narrowed the no-replication failure:

- iteration 2 reaches `sched_worker_detach_other_tasks()`;
- it takes the runqueue lock;
- it stalls while walking scheduler state under `rq->cfs_tasks`;
- this is consistent with member 1 mutating kernel memory that is still backed
  by the master's shared `physmem_fd`.

With `um_pool_replicate_physmem()` temporarily re-wired into
`child_entry_pool_member()`:

- iteration 1 reports a child pid;
- iteration 1 reaches `POOL_ENTER`;
- iteration 1 does not reach `MEMBER_DONE`;
- the preserved boot log shows an `init.sh` libc segfault, init-kill panic, and
  repeated master resume cycles.

Older bisect work remains relevant:

- same-inode `mmap(MAP_FIXED)` over the physmem region is benign;
- different-inode runtime remapping is the trigger;
- the first fresh stub after replication can work;
- later guest-mm stub creation after replication can hang before the initial
  seccomp/SIGSYS handshake; and
- a standalone Linux timer/signal repro does not reproduce the failure, so the
  bug is UML-specific.

## Constraints

- Do not add permanent debug prints to hot kernel paths.
- Do not encode internal issue IDs, diaries, or historical shorthand in code
  comments.
- Keep default behavior unchanged until a gated path has passed one-shot and
  sustained tests.
- Keep the existing non-replicating path available while the replacement is
  being proved.
- Do not claim pool completion until live-member tests, daemon tests, and
  benchmark gates all pass against the same production path.

## Recommended Path

Finish the runtime physmem replication path first. It is the least invasive
option because the copy/remap helpers already exist and the current failure is
localized to the post-replication UML userspace/stub path.

The alternative, boot-time per-member physmem setup, should remain the fallback
if runtime remap cannot be made correct. It is a larger architectural change and
does not by itself solve the need to give each forked member independent kernel
memory before it runs.

## Work Plan

### Step 1: Add a Gated Replication Mode

Add a disabled-by-default command-line switch, for example:

```text
um_template_pause_pool_replicate=1
```

When enabled, `child_entry_pool_member()` calls the physmem isolation path before
refreshing SKAS stubs and before returning to guest userspace.

Rules:

- the default path remains today's passing one-shot/no-replication behavior;
- the switch is documented as experimental until all acceptance tests pass;
- failures must abort the child clearly instead of letting the master spin; and
- the switch must be easy to remove if runtime replication is abandoned.

### Step 2: Make Replication Failure Bounded

Before fixing correctness, make the failed replication path diagnosable and
bounded:

- stop repeated master resume cycles after a replication child panic;
- ensure the supervisor sees a nonzero child pid and a final failure reason;
- preserve no leaked UML processes after XFAIL;
- add a focused selftest mode that expects the current replication failure until
  the path is fixed.

This keeps future experiments from producing long panic logs or ambiguous
timeouts.

### Step 3: Fix the Immediate Iteration-1 Regression

The current replication recheck fails before `MEMBER_DONE`. The next local
debug target is the handoff between:

```text
um_pool_replicate_physmem()
template_pause_refresh_child_stubs()
start_userspace_fresh()
userspace()
```

Questions to answer:

- does `um_pool_replicate_physmem()` leave UML local IRQ/preempt state suitable
  for returning to userspace?
- does the child still have a valid `current`, `current->mm`, and `mm_id` after
  the remap?
- does `start_userspace_fresh()` complete after the remap with the expected
  code/data fd pair?
- does the first post-remap syscall from `init.sh` reach the stub signal path?
- if `init.sh` segfaults in libc, is the mapped libc page missing, stale, or
  mapped from the wrong fd?

Expected output for this step:

- a small production fix, or
- a short-lived local marker result documented in this file before the next
  implementation attempt.

### Step 4: Fix New Guest-MM Stub Creation

Once iteration 1 reaches `MEMBER_DONE` under replication, run a member workload
that forks or execs a new guest process. This exercises the older known blocker
where the second new stub can hang before the first seccomp/SIGSYS handshake.

Candidate fixes to evaluate:

- ensure all post-remap physmem fds needed by the trampoline are cleared of
  `FD_CLOEXEC` before `execveat()`;
- validate that `phys_mapping()` returns the replicated fd and offset for both
  stub code and default stub data;
- ensure `mm_id->stack` and its backing fd agree after `start_userspace_fresh()`
  and later `start_userspace()` calls;
- verify the seccomp fd map is reset when a new stub is spawned after remap;
- compare the first failing stub's `stub_init_data` against the pre-remap
  passing path.

Acceptance for this step:

```sh
UML_BINARY=$PWD/linux \
tools/testing/selftests/um/template-pause-pool-member-smoke/run-template-pause-pool-member-smoke.sh
```

with replication enabled must still pass, including timer ticks and
`MEMBER_DONE`.

### Step 5: Flip Sustained Smoke From XFAIL to PASS

After one-shot replication is stable, update the sustained smoke to run the
replication-enabled path and require:

- `SUSTAINED_MEMBER_DONE : 3`;
- three nonzero, distinct child pid reports;
- three distinct identities;
- no kernel panic;
- no v1 ceiling regression;
- no leaked UML process after teardown.

Keep an explicit no-replication XFAIL mode if it remains useful as a regression
sentinel for the shared-physmem failure.

### Step 6: Move Daemon Pool to Real Live Members

Only after Step 5 passes:

- make `umlctl pool serve` launch masters with real pool-member mode;
- ensure `take` returns live members rather than short-lived child pids;
- make `min_warm` maintain ready members;
- update `pool-serve-smoke` to prove member liveness, not just pid write-back;
- update `pool-bench` so the RSS gate samples live children.

### Step 7: Validate User-Facing Pool Functionality

Run and fix, in order:

```sh
UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-serve-smoke/run-pool-serve-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-exec-smoke/run-pool-exec-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-port-forward-smoke/run-pool-port-forward-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-bench/run-pool-bench.sh
```

Completion requires successful daemon-routed guest exec and meaningful live-child
RSS measurements.

### Step 8: Validate Vector2 and Syzkaller Paths

After live daemon members work:

- validate vector2 TAP/fd handoff through a pool member;
- validate the syzkaller-facing command shim against the live daemon path;
- run the launcher unit tests;
- run a bounded soak before declaring the original pool vision complete.

## Acceptance Gates

Minimum gates before calling sustained pool-member lifetime fixed:

```sh
make ARCH=um -j16

bash -n \
tools/testing/selftests/um/template-pause-pool-sustained-smoke/run-template-pause-pool-sustained-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/template-pause-pool-member-smoke/run-template-pause-pool-member-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/template-pause-pool-sustained-smoke/run-template-pause-pool-sustained-smoke.sh
```

Minimum gates before calling pool functionality complete:

```sh
UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-serve-smoke/run-pool-serve-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-exec-smoke/run-pool-exec-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-port-forward-smoke/run-pool-port-forward-smoke.sh

UML_BINARY=$PWD/linux \
tools/testing/selftests/um/pool-bench/run-pool-bench.sh

(cd tools/uml/uml-launcher && cargo fmt --check && cargo test)
```

## Completion Definition

The original pool/fork functionality can be called complete when:

- sustained member lifetime passes in production mode;
- daemon `take` returns live members;
- warm `min_warm` creates and replenishes ready members;
- successful daemon exec works through final member mconsole paths;
- port-forward and vector2/TAP handoff work on live members;
- pool-bench gates measure live children and pass;
- the syzkaller shim can drive the daemon path; and
- status docs no longer list pool-member lifetime, warm pool, exec, TAP/fd
  handoff, or benchmark RSS as pending items.
