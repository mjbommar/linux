# uml-perfetto-trace — function_graph + kprobes → Perfetto trace

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

The cheapest, highest-leverage of the nine future tools
proposed in this batch: turn existing tracefs output into a
format that renders beautifully in a browser. Zero new
infrastructure; ~200 LOC of Rust or Python.

## Ideal end-user flow

```text
$ uml-perfetto-trace \
    --instance research-1 \
    --duration 10s \
    --out boot.perfetto-trace

Capturing trace from research-1 for 10s...
Wrote boot.perfetto-trace (2.8 MB, 412k events).

$ # drop into ui.perfetto.dev, file → open
```

User sees a timeline view of the boot (or workload) with:

- A swimlane per CPU (UML is UP by default — one lane, rich).
- A swimlane per traced function (function_graph output).
- Kprobe events as pins on the timeline.
- Clickable spans that reveal call-graph depth, durations,
  argument capture.
- Flame graph on any time window.

That's the researcher's complete visual debugger, open in
any modern browser, no install required.

## What this builds on

- **C-04 commit 3b function_graph** (landed) — the entry
  source. `echo function_graph > current_tracer` is the
  only thing the user has to set up.
- **C-04 kprobes + kretprobes** (landed) — the point-event
  source.
- **`/sys/kernel/tracing/trace`** — stable tracefs format.
  Multiple Linux tooling generations have normalized around
  it; Perfetto's `ftrace` importer already handles the
  `trace` format from Android's tracing fork. UML's format
  is the same.

## Perfetto trace format

Perfetto accepts either:

- **`.perfetto-trace`** (protobuf) — the native format,
  smaller, faster to parse. Requires a protobuf schema
  generator.
- **JSON trace format** (chrome://tracing compatible) — text,
  bigger, trivial to write. Well-defined schema at
  `docs.google.com/.../trace_event_format` (now Perfetto
  upstream docs).

Start with JSON. Switch to protobuf only if the browser UI
complains about size on real captures.

## Build shape

- Rust binary under `tools/uml/uml-perfetto-trace/` (tiny
  — one file). Or Python script under
  `scripts/uml-perfetto-trace.py` if the Rust workspace
  overhead feels excessive for a 200-LOC converter.
- Reads tracefs input either:
  - Live (`--instance <name>` + `--duration Ns` — opens the
    guest's `/sys/kernel/tracing/trace_pipe` over hostfs),
    or
  - Offline (`--input trace.txt --output out.perfetto-
    trace` — takes a saved tracefs dump).
- Parses each line of the tracefs format:

  ```
   kworker/0:3-42      [000] d..2  1.234567: funcgraph_entry:                   |  __x64_sys_openat() {
   kworker/0:3-42      [000] d..2  1.234568: funcgraph_exit:       0.001 us   |  }
  ```

  Maps `funcgraph_entry` → Perfetto "Begin" event; matching
  `funcgraph_exit` → "End" event. Kprobe fires → "Instant"
  event. Result: hierarchical spans the UI renders natively.

## Prior art

- **trace-cmd + kernelshark** — existing Linux tracefs
  visualizer. Native and good. Not web-based; the web pivot
  is the "zero-install share with colleagues" wedge.
- **perf report + Flamegraph** — static flame graph.
  Perfetto is richer — time-aware, interactive, with
  depth-first call-graph.
- **Catapult / trace_event_format** — Chrome devtools'
  chrome://tracing. Same format Perfetto ingests.
- **`trace2html.py`** (in-tree, Android) — generates a
  self-contained HTML from a trace for offline sharing.
  Potential last-mile option if a `.perfetto-trace` is
  inconvenient.

## Why this is a low-cost, high-value tool

Every other tool in this batch needs substantial new code
(TUI, umlctl, uml-api, etc.). This one:

- Consumes existing tracefs output — no kernel work.
- Outputs an existing widely-supported format — no new UI.
- Every kernel researcher has had to mentally render
  function_graph output before; Perfetto makes that
  rendering free.

Realistic effort: one focused day for the JSON converter +
docs; maybe two for streaming + protobuf + a command-line
filter vocabulary (`--filter __x64_sys_*`).

## Non-goals

- **New visualization.** Use Perfetto's existing UI. Don't
  build browser rendering.
- **Trace collection from multiple UMLs simultaneously.**
  One trace per invocation; merge manually if you need it.
- **Binary-pb format generation without the `prost` crate
  or similar.** Start with JSON; protobuf is a later
  optimization.
- **Conversion of non-tracefs sources** (dmesg timestamps,
  perf data, syscall strace). Out of scope. This is
  tracefs → Perfetto only.

## Open questions

- **Q1: depth filtering.** A full boot with
  `function_graph` emits millions of events. Perfetto can
  load ~5-10M events comfortably before the browser tab
  dies; above that, filter or sample. Document the ceiling;
  recommend `--duration` and `--filter` for large captures.
- **Q2: thread identity under UML.** UML guest "CPU 0" is
  a single host thread. Map it to one track. Guest kernel
  threads (kworker) are separate virtual tasks; use them
  as separate tracks via the `(task-name, pid)` pair from
  tracefs.
- **Q3: align kprobe events with function_graph spans.**
  Requires correct timestamp ordering. Trivial if the
  trace is read in order; care needed for `trace_pipe`
  concurrent reads.

## Effort estimate

1-2 days for the JSON converter. Scope goes up if we want
protobuf + streaming + sampling.

## Dependencies

- C-04 commit 3b function_graph (landed).
- C-04 kprobes (landed).
- None else.

## Cross-references

- `02-workstreams/C-profiles-and-gaps/04-port-kprobes.md`
  — kprobes surface.
- `02-workstreams/C-profiles-and-gaps/05-port-ftrace.md`
  — function_graph surface.
- `08-uml-probe-web.md` — live browser view; this is the
  offline / shareable sibling.
- `10-uml-record-replay.md` — `uml-replay --export-
  perfetto` composes with this tool (either reuses the
  same converter or builds on it).
