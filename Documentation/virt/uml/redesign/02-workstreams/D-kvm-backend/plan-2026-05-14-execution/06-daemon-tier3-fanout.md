# Daemon-side tier3 per-worker IP carve-out landed (2026-05-14)

## What

`tools/testing/selftests/um/soak/run-soak-daemon.sh` now branches on
the workload name: tier3 workloads (`tier3-*`) use a per-worker
`umlctl gate loop --workers 1` fanout with per-/30 IP allocation
from `192.168.42.0/24`, while pilot workloads + tier1 + tier2 + LTP
keep the original `--workers $WORKERS` shape.

The change resolves task #25 (per-memo
`phase-J-tier3-design-2026-05-14.md` §6.1's spec): each tier3 worker
N gets

  - `host_ip  = 192.168.42.{4N+1}/30`
  - `guest_ip = 192.168.42.{4N+2}/30`
  - `tap_name = soak-tap{N}`

substituted into the template at the daemon's `sed` step. Concurrent
soaks no longer collide on the shared TAP that
`gate_loop.rs:253-277` would otherwise hand out.

The previous TIMEOUT_FOR array also picked up entries for the new
template families (90s tier1, 120s tier2, 180s tier3, 3600s
ltp-runner) so the daemon's per-iter watchdog matches each
workload's expected wall-clock.

## Why the per-worker fanout, again

Memo `phase-J-tier3-design-2026-05-14.md` §5 decision: option (b)
over option (a). The Rust-side fix in option (a) would extend
`umlctl ps --json` with `guest_tap_ip`, threading IPs back through
the manifest schema; it's the right surface in the abstract but
requires a schema-v2 bump and 40-120 LoC across 3-4 Rust files,
which we deferred for Phase J critical-path reasons. Option (b)
gets us a working Tier 3 with ~150 LoC of bash, zero Rust changes,
zero manifest-schema risk.

The trade-off the operator inherits: `umlctl ps` does NOT show the
guest IP. Recovery is `worker_idx → host_ip/guest_ip` via the
`4N+1 / 4N+2` formula, documented inline in the daemon and the
design memo. The soak's `scoreboard.jsonl` rows already carry
`worker_idx` for the tier3 phase, so postmortem reconstruction is
a straightforward arithmetic.

## Verification (dry-run)

Smoke under `--dry-run` with `--workloads tier3-django,iocheck`
`--workers 3 --iters-per-rotation 2`:

  - Tier3 phase prints 3 worker lines, each with a distinct
    `host=192.168.42.{1,5,9}` `guest=192.168.42.{2,6,10}`
    `tap=soak-tap{0,1,2}`.
  - Non-tier3 (iocheck) phase prints a single `umlctl gate loop -W 3`
    invocation, unchanged.
  - Backend rotation (`kvm-v2` then `seccomp`) reuses the same
    tap names within a phase but in serial — taps are torn down
    between phases by umlctl.
  - Per-worker TOMLs at `_tier3-django-{backend}-w{N}.toml` have
    every `{{...}}` placeholder substituted; `grep -F '{{'`
    returns empty.

## What's NOT yet verified

Operator-time work that gates a live tier3 soak:

  1. Kernel rebuild with `CONFIG_UML_NET_VECTOR=y`. Without this
     the TAP setup will fail at `umlctl up`. Cost: ~3 min `make
     ARCH=um O=... -j`.
  2. iptables NAT for tap-mode networking. `umlctl` does this
     automatically when `[network] mode = "tap"` but the host
     must allow `sysctl -w net.ipv4.ip_forward=1` (already set
     by `umlctl compile`).
  3. Python frameworks (`apt-get install python3-django
     python3-fastapi python3-uvicorn`). The current templates
     use Python stdlib `http.server` so this is optional for
     the smoke; operator swaps the `cmd` for `python3 -m django
     runserver` once Django is installed.

These are deferred to the operator pre-flight checklist in the
template headers; the daemon change is correctness-complete for
its bash-side scope.

## LTP scheduling note

The `ltp-runner` workload has TIMEOUT_FOR=3600 (60 min hard kill)
but otherwise uses the standard non-tier3 code path:
`umlctl gate loop -W $WORKERS -M $ITERS`. The memo notes a
"every 8 rotations" frequency preference because each cycle is
45-60 min wall-clock; the daemon doesn't (yet) implement
per-workload rotation modulo. The simplest operator workaround:
exclude `ltp-runner` from `--workloads` for high-frequency
rotations and run it in a separate daemon invocation with a
budget of its own. Filed as a follow-up; not a Phase J blocker.

## Files changed

  - `tools/testing/selftests/um/soak/run-soak-daemon.sh`
    - TIMEOUT_FOR: added tier1/tier2/tier3/ltp-runner entries.
    - +4 helpers: `is_tier3_workload`, `emit_tier3_worker_toml`,
      `process_tier3_phase_results`, `run_one_tier3_phase`.
    - `run_one_phase`: branch on `is_tier3_workload` at entry.

## Acceptance criteria status

Phase J acceptance criterion §5.4 ("Tier 3 wired and one full
rotation pass per framework"):

  - Bash daemon scaffolding: DONE.
  - Template scaffolding: DONE (commit `2026-05-14 — tier3
    templates landed`).
  - Live smoke under a real `CONFIG_UML_NET_VECTOR=y` kernel:
    PENDING operator pre-flight.
  - Headline ratio recorded: PENDING — recorded only after
    operator runs the smoke.

The daemon-side bash scope of #25 is closed by this commit.
The "live smoke under a real kernel" sub-task lives at #28 (or
operator-time follow-up).
