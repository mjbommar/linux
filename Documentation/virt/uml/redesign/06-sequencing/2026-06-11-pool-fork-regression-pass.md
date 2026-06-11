# UML Pool/Fork Regression Pass

Date: 2026-06-11

Branch: `next`

Commit tested: `59ad334001ea`

UML binary after rebuild: `7.1.0-rc7-00187-g59ad334001ea`

Purpose: refresh the pool, fork-server, template-pause, and syzkaller evidence
on current `next` after the recent KVM v2, selftest, and documentation cleanup
slices. This is a focused regression pass. It is not the final UML v2
completion matrix, and it must be repeated after any later KVM v2 or vector2
changes that can affect pool member startup, networking identity, or
syzkaller-facing command paths.

## Setup

The in-tree UML binary was rebuilt from current `HEAD` before the regression
run:

```sh
make ARCH=um -j$(nproc)
```

The selftests used the rebuilt kernel and the existing debug `umlctl`:

```sh
export UML_BINARY=$PWD/linux
export UM_FORK_KERNEL=$PWD/linux
export UMLCTL=$PWD/tools/uml/uml-launcher/target/debug/umlctl
```

The rebuilt kernel reported:

```text
7.1.0-rc7-00187-g59ad334001ea
```

## Results

| Gate | Result | Evidence |
| --- | --- | --- |
| `template-pause-smoke` | PASS/SKIP | Cases 1-3 passed; case 4 skipped because `vec0` was not visible in the guest. |
| `template-pause-fork-smoke` | PASS | Two identity blobs parsed, two pre-fork teardown lines observed, two master resume cycles completed, and two distinct child PIDs were reported. |
| `template-pause-fork-stress` | PASS | 540 iterations over 10 seconds, 425 distinct child PIDs, 173 identity blob rotations, 540/540 clean identity round-trips, median iteration time 18.5 ms, 0.00% RSS drift, no kernel panics, and no live orphans after teardown. |
| `template-pause-pool-member-smoke` | PASS | The member reached `TPPM_MEMBER_DONE`, produced five timer ticks, parsed the identity fd, and showed no kernel panic or v1 ceiling regression. |
| `template-pause-pool-sustained-smoke` with `UML_POOL_REPLICATE=1` | PASS | Three consecutive replicated members reached `MEMBER_DONE`; `POOL_REPLICATE_OK=3`, `POOL_REPLICATE_FAIL=0`, no kernel panic, and no v1 ceiling regression. |
| `template-pause-pivot-smoke` | PASS | 20 pivots completed, 20 SIGCONT events were sent, and no v1 ceiling panic was observed. |
| `pool-spawn-smoke` | PASS | `umlctl pool spawn/list/destroy` lifecycle completed; the destroyed PID disappeared from the post-destroy list. |
| `pool-serve-smoke` | PASS | The daemon socket came up, `--min-warm=1` prefilled a ready member, ready take and lazy take returned live PIDs, destroy made members non-runnable, daemon shutdown exited cleanly, and the master was killed on shutdown. |
| `pool-exec-smoke` | PASS | `exec/1` NDJSON framing validated for `/bin/true`, stdout/stderr capture, guest exit code 7, timeout exit code 124, late-output suppression, and helper cleanup. |
| `pool-port-forward-smoke` | PASS | TAP-direct `port-forward/1` envelope validated, split host/guest ports parsed, bogus PID failed cleanly, and missing `--host-port` returned the expected clap error. |
| `pool-mconsole-path-probe` | PASS | A replicated member reached `PMCON_MEMBER_DONE`, the per-member mconsole socket existed, and `version` replied with the rebuilt `59ad334001ea` kernel string. |
| `pool-bench` | PASS | All 5 gates passed: p50 1.8 ms, p99 2.4 ms, 100/100 live members at 138.9 MiB PSS and 510.0 MiB summed RSS, 17.5 MiB private dirty, 0.05% drift across 10,000 lifecycle cycles, and 3000/3000 sustained takes. |
| `syzkaller-shim-smoke` | PASS | Source contract check passed; syzkaller-style take, exec output merge, port-forward, status, and destroy all completed. |

## Current Disposition

The current `next` branch has fresh focused evidence that the production
template-pause, fork-on-resume, pool daemon, warm-member, daemon exec,
port-forward, per-member mconsole, pool benchmark, and syzkaller shim paths are
working together on the rebuilt `59ad334001ea` UML binary.

This closes the immediate current-HEAD pool/fork regression audit. It does not
close the broader completion blocker. The final branch still needs:

- a rerun of these gates after any KVM v2 or vector2 changes;
- final Tier 3 and KVM/vector2 networking evidence;
- a disposition for snapshot-backed fork-server behavior; and
- inclusion in the final validation matrix before any 100% completion claim.
