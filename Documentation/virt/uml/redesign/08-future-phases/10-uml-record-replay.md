# uml-record / uml-replay — deterministic capture + replay on top of time-travel

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

The research profile already enables
`CONFIG_UML_TIME_TRAVEL_SUPPORT=y`. Time-travel mode gives UML
deterministic virtual-time progression: under
`time-travel=inf-cpu` or `time-travel=ext`, the guest advances
its own clock without depending on wall time, and every
deterministic input replays identically. This is the
substrate. uml-record / uml-replay is the thin wrapper.

## Ideal end-user flow

Record:

```text
$ uml-record \
    --instance research-1 \
    --out bug42.umlrec \
    --duration 30s
Recording research-1 for 30s...
Captured 12412 events, 4.1 MB. Saved to bug42.umlrec.
```

Replay:

```text
$ uml-replay bug42.umlrec
Booting UML with recorded inputs...
[   0.000000] Linux version 7.0.0-uml ...
[   0.042118] [replay] applying event 1/12412 (SIGALRM)
[   0.042118] [replay] applying event 2/12412 (fd 3 read 128 bytes)
...
Replay finished. Guest behavior identical to recorded run.

$ uml-replay bug42.umlrec --export-perfetto bug42.perfetto-trace
Replaying and streaming to Perfetto trace format...
Wrote bug42.perfetto-trace. Open at ui.perfetto.dev.
```

## What this builds on

- **`CONFIG_UML_TIME_TRAVEL_SUPPORT`** — landed. Time-travel
  mode is the deterministic substrate. Already enabled in
  the `research` profile.
- **`arch/um/include/shared/timetravel.h` +
  `arch/um/os-Linux/time.c`** — the time-travel scheduler.
  Already the single source of truth for virtual time.
- **C-09 forkserver** — snapshot/fork gives fast "boot to
  point X" so replay can start from a non-boot state
  without re-running the full boot.

## Build shape

- Rust binary under `tools/uml/uml-record/` (single binary
  exposing both `record` and `replay` subcommands — the
  symmetric split is artificial; the shape is one tool with
  two modes).
- Capture sources:
  - Guest-observable inputs: host signals (SIGALRM ticks,
    SIGIO events), fd reads (rootfs, hostfs), vhost-user
    queue arrivals.
  - Deterministic side of time-travel: the mode already
    guarantees CPU scheduling is reproducible given the
    same inputs, so record only the *inputs*, not the
    entire state trajectory.
- Record format: versioned binary blob with:

  ```
  umlrec-v1
  ├── header { kernel_sha, kernel_config_hash, profile, start_time, duration }
  ├── input_events[] { virt_time_ns, source (fd|signal|queue), payload[] }
  └── metadata { boot_cmd_line, rootfs_hash, workload_hash }
  ```
- Replay driver wraps `uml-launcher run` with:
  - Patched `time-travel=replay:<umlrec-path>` mode that
    injects recorded inputs at the recorded virtual-time
    markers instead of reading from the live host.
  - Divergence-detection: if the guest asks for input the
    recording doesn't describe, stop and surface the
    divergence point (useful for bug finding — "the bug
    reproduces until event N, then the guest diverges").

## Prior art

- **rr (Record and Replay)** — the gold standard.
  Single-process deterministic replay via ptrace-driven
  interception. uml-record's "intercept at the input
  boundary" is lighter-weight because UML's boundary is
  narrower (a kernel talking to host via signals + fds vs.
  a whole process's syscall surface).
- **ChronoQemu / Chronon / Replay.io** — session replay in
  different domains. Demonstrates the format-versioning +
  divergence-detection problems are well-trodden.
- **RR4Embedded / OpenReplay** — closer parallels. Good
  precedent for the `rr` model in VM contexts.

## What replay unlocks

- **Deterministic bug repro.** A flaky test records once;
  replays reliably until the bug is understood and fixed.
- **Time-travel debugging.** Paired with a kernel gdb stub
  (exists in UML), replay can be stepped, inspected, paused,
  without needing to re-trigger the live conditions.
- **Shareable repros.** A `.umlrec` file plus a kernel sha
  is a complete bug report. Reviewer replays and sees the
  same behavior byte-for-byte.
- **LLM-agent session history.** 06-uml-mcp.md's agent can
  checkpoint its experiments as recordings; future agents
  (or humans) replay to understand what was tried.

## Non-goals

- **Non-deterministic recording.** If the user runs without
  `time-travel=inf-cpu`, replay cannot guarantee anything.
  uml-record refuses to record on non-time-travel profiles.
- **Replay on a different kernel binary.** `.umlrec` pins a
  kernel sha; replay requires that exact kernel.
- **Distributed recording** (multiple UMLs interacting).
  Single guest per recording.
- **Snapshot/restore.** That's 02-snapshot-to-disk.md; the
  two are composable but not the same thing.

## Open questions

- **Q1: hostfs determinism.** Guest reads from hostfs;
  hostfs files can change between record and replay. Record
  either the file contents on first-access (fat recording)
  or a hash + "restore from this rootfs image" reference
  (slim recording). Start slim; add fat as an explicit
  option.
- **Q2: vhost-user backend interactions.** Net/block
  backends inject data into the guest. Record those, or
  require replay to rerun the same backends? Recording is
  simpler; re-running backends is more flexible. Record by
  default; add a `--replay-backend` flag for workflow that
  needs live backend behavior during replay.
- **Q3: size of recordings.** A 30-second boot + workload
  is O(10-100 MB). Fine for interactive development,
  awkward for CI archives. Add streaming compression (zstd
  inline) to the format.
- **Q4: the time-travel mode's scheduling determinism.**
  Documented in the kernel but not exhaustively tested.
  Early uml-record work should include a "replay-the-same-
  recording-twice, assert identical" test.

## Effort estimate

3-4 weeks for the MVP. Time-travel itself is the hard part
and it's landed; uml-record's job is bookkeeping +
format + divergence detection.

## Dependencies

- `CONFIG_UML_TIME_TRAVEL_SUPPORT` (landed).
- C-10 v2 decomposed backends (interesting for backend
  recording; MVP can do hostfs-rootfs-only).
- 12-uml-perfetto-trace.md (optional; the `--export-
  perfetto` flag depends on it).

## Cross-references

- `arch/um/os-Linux/time.c` — the time-travel
  implementation.
- `03-profiles/research.md` — already enables
  `CONFIG_UML_TIME_TRAVEL_SUPPORT=y`.
- `02-snapshot-to-disk.md` — complementary: snapshot gives
  "boot to point X fast"; record-replay gives "replay
  behavior deterministically from that point".
- `12-uml-perfetto-trace.md` — replay → perfetto export
  for visualization.
- `06-uml-mcp.md` — LLM agents benefit from a
  `record_session` + `replay_session` tool pair on top.
