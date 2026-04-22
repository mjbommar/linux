# uml-probe — web UI for live guest tracefs + debugfs

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

## Ideal end-user flow

Developer A is debugging a kernel issue on their laptop and
wants developer B to see the same running guest. Today: screen
share, ssh tunnels, email dmesg fragments. Proposed:

```text
$ uml-probe serve --instance research-1 --listen 127.0.0.1:7071
Serving http://127.0.0.1:7071 (SSE at /events)

$ uml-probe share --instance research-1 --duration 30m
Token: z83-kfl4-dpra-m9xx
URL:   https://uml.share.localnet:8443/z83-kfl4-dpra-m9xx
      (links expire 2026-04-22T13:32:00Z)
```

Developer B opens the URL in a browser and sees:

- Live dmesg stream (SSE, colored by level).
- Live tracefs event stream (SSE, filtered by the dev's
  pinned filter — kprobes, function_graph, tracepoints).
- Guest status panels: current backend, profile, memory,
  RSS, fault rate.
- An input box to set a kprobe or add a tracepoint, with
  a per-session permission (dev A chooses read-only vs
  read-write when issuing the share link).

The connection is a plain HTTP/SSE session — no Electron,
no WebSocket gymnastics, no browser extension. Works in any
modern browser.

## What this builds on

- **C-04 kprobes** (landed) — lets the web UI install probes
  on demand.
- **C-05 ftrace** (landed) — function tracing + dynamic
  ftrace available at the tracefs surface the web UI exposes.
- **C-04 commit 3b function_graph** (landed) — call-graph
  visualization feeds the browser UI.
- **hostfs** or the C-10 v2 console backend — the channel
  the web server uses to read/write tracefs on behalf of
  the remote user.

## Build shape

- Rust binary under `tools/uml/uml-probe/`.
- HTTP server: `axum` (tokio-based, small) or `actix-web`
  (thread-based). axum is the lighter option and matches the
  rest of the async-free tooling UML uses.
- Server reads tracefs from the guest via hostfs-bound
  directory or by shelling into the guest via the console
  backend. Read-only paths: `/sys/kernel/tracing/trace`,
  `/sys/kernel/tracing/trace_pipe`,
  `/sys/kernel/debug/um/*`. Read-write paths (gated):
  `/sys/kernel/tracing/kprobe_events`,
  `/sys/kernel/tracing/current_tracer`,
  `/sys/kernel/tracing/set_ftrace_filter`.
- SSE stream on `/events`: the server tails
  `trace_pipe` and forwards each event to subscribed
  clients as a typed event (`event: trace\ndata: {...}\n\n`).
  Browser `EventSource` picks it up directly.
- UI: static HTML + small vanilla JS (no framework
  required). Panel = `<table>` with rolling 1000-row
  buffer; filter = `<input>` that re-filters client-side.
  Ships embedded in the binary via `include_str!`.
- Share links: HMAC-signed token + expiry + scoped
  permissions. Stored in memory, no database. The signing
  key is auto-generated on first launch and persisted in
  `~/.local/state/uml/uml-probe.key`.

## Prior art

- **lnav** / **ctop** / **dozzle** — web tails over logs;
  demonstrate the "tiny axum/actix server + SSE" pattern.
- **grafana Explore** — the filter + live-stream UX the
  browser page mimics. Don't import grafana; borrow the
  interaction.
- **jaeger UI** — trace visualization patterns to steal for
  the call-graph panel when function_graph is active.

## Non-goals

- **General-purpose web console for UML.** Not a "run any
  command over the web". Scoped to tracefs + debugfs.
- **Multi-instance.** One instance per running server; use
  umlctl + multiple uml-probe processes for multiple
  guests. The TUI (04) remains the multi-instance view.
- **Authentication + authorization backend.** HMAC link
  tokens + an optional reverse-proxy layer (nginx / caddy)
  for real auth. uml-probe does not ship its own user
  database.
- **Production deployment.** Default listen address is
  `127.0.0.1`; exposing on a public IP requires an explicit
  `--bind` override and is the operator's call.

## Open questions

- **Q1: what "share" means.** Two reasonable shapes: (a)
  the HMAC token + expiry above, where sharing gives a URL
  anyone on the network can load within the window; (b)
  OIDC / OAuth with a real identity provider. (a) is
  sufficient for the "show a colleague my running trace"
  use case; (b) is enterprise. Start with (a); let someone
  else build (b) on top.
- **Q2: per-session scope.** A shared link defaults to
  read-only (no kprobe install, no tracer change). Owner
  upgrades the session if the viewer needs to add a probe.
- **Q3: performance ceiling on the SSE stream.** Under heavy
  `function_graph`, the tracefs pipe pumps MB/s. Need a
  client-side rate-limit + server-side drop policy (the
  browser doesn't need every event; drop oldest on
  backpressure).

## Effort estimate

1 week for the MVP (SSE dmesg + tracefs, no kprobe install).
Another week for the probe-install + share-link + scoped
permissions shape.

## Dependencies

- C-04 kprobes + C-05 ftrace + C-04 3b function_graph (all
  landed).
- C-10 v2 console backend (or hostfs).

## Cross-references

- `04-uml-launcher-tui.md` — TUI is the multi-instance
  overview; uml-probe is the per-instance deep dive.
- `12-uml-perfetto-trace.md` — the offline / download
  sibling of this live-stream tool.
- `11-uml-api-daemon.md` — if the shared-link story is
  insufficient, uml-probe can sit behind the API daemon's
  OIDC layer when that exists.
