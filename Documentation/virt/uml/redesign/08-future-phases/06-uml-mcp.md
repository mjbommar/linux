# uml-mcp — Model Context Protocol server exposing UML to LLM agents

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

This is the concrete realization of a motivation the vision
doc already names: "a tighter loop if you wanted to let LLMs
try to iteratively experiment and optimize the kernel (e.g.,
schedulers, network stacks, etc.)". C-09's forkserver gives
the speed; MCP gives the interface.

## Ideal end-user flow

A developer points their LLM agent (Claude, any MCP-aware
client) at a local `uml-mcp` server:

```text
$ uml-mcp --kernel ./linux --profile research --listen stdio &
# agent connects over MCP stdio transport
```

The agent sees these tools:

| Tool | Args | Returns |
|---|---|---|
| `boot_uml` | `profile`, `kernel_config_fragment?` | `instance_id` |
| `run_inside` | `instance_id`, `cmd`, `timeout_s?` | `{stdout, stderr, rc, elapsed_ms}` |
| `attach_kprobe` | `instance_id`, `symbol`, `script?` | `probe_handle` |
| `read_trace` | `instance_id`, `since?`, `limit?` | `{entries[], cursor}` |
| `apply_patch` | `instance_id`, `diff` (unified format) | `{applied, rebuilt, rc}` |
| `bisect` | `good_sha`, `bad_sha`, `test_spec` | `{culprit_sha, boot_log_head, boot_log_bad}` |
| `snapshot` | `instance_id`, `label` | `snapshot_id` |
| `restore` | `instance_id`, `snapshot_id` | `ok` |
| `shutdown` | `instance_id` | `ok` |

Agent then iterates: boot, run a test, read trace, adjust,
patch, rebuild, re-run — at forkserver speed (<50ms per
iteration) when using snapshot/restore, at boot speed
(seconds) for wholesale rebuild loops.

## Prior art

- **MCP (Model Context Protocol)** — Anthropic-authored open
  protocol for LLM ↔ tool communication. Well-defined
  transports (stdio, HTTP + SSE). Spec lives at
  modelcontextprotocol.io; reference servers exist for
  filesystem, git, GitHub, Postgres, SQLite.
- **Claude Code / Cursor** — already consume MCP servers. An
  MCP-compliant uml-mcp plugs straight in.
- **Devcontainers / ephemeral sandboxes** — demonstrate that
  agents benefit from a fully-owned host environment, not
  just "run this shell command"; uml-mcp raises the
  abstraction from the shell to "a running kernel".

## What this builds on

Every tool maps 1:1 onto something that already exists or is
on the current redesign plan:

| Tool | Underlying capability |
|---|---|
| `boot_uml` | C-10 v1 `uml-launcher run` + umlctl (05). |
| `run_inside` | Console backend (C-10 v2 commit 2) + a small guest-side command wrapper. |
| `attach_kprobe` | C-04 kprobes (landed). Writes `/sys/kernel/tracing/kprobe_events`. |
| `read_trace` | C-04 + C-05. Reads `/sys/kernel/tracing/trace`. |
| `apply_patch` | Host-side `git apply` + `make ARCH=um` rebuild. Fastest when restricted to a layered overlay (patch on top of the booted image). |
| `bisect` | 07-uml-bisect.md — the CLI this tool wraps. |
| `snapshot` / `restore` | 08-future-phases/02-snapshot-to-disk.md v2 when that lands; fork-based until then. |
| `shutdown` | umlctl `stop`. |

No new kernel work. All the substrate exists in the landed
or planned C workstreams.

## Build shape

- Rust binary under `tools/uml/uml-mcp/`. Parallel to
  `uml-launcher` and `umlctl`; shares the Cargo workspace
  where sensible.
- MCP implementation: `rmcp` (Anthropic-maintained Rust SDK)
  or an in-tree thin impl if crate vendoring is a concern.
- Transports: stdio (default, for Claude Code / Cursor
  plugging), plus HTTP+SSE for remote agents (same
  semantics, different wire).
- Tool handlers are thin wrappers over umlctl + `uml-launcher
  backend` + the `uml-bisect` CLI. uml-mcp does not
  reimplement; it exposes.
- Security model: every tool invocation is scoped to a single
  `instance_id`. The agent cannot touch other instances on
  the host; cannot touch host files outside
  `~/.local/state/uml/<instance>/`; cannot run host-side code
  (`run_inside` always routes through the guest).

## Speed wedge

The concrete reason this is worth building: the forkserver
gives <50ms per iteration. At that cadence, an LLM agent can
run thousands of experiments per hour. Most prior "LLM +
kernel" setups assumed minutes per iteration (QEMU boot,
physical hardware reflash); the UML redesign fundamentally
changes that economics.

Example agentic loops this unlocks at useful speed:

- **Scheduler tuning** — agent mutates `kernel/sched/fair.c`
  constants, reboots, runs a representative workload,
  compares metrics, iterates.
- **Syscall hardening** — agent adds a kprobe, exercises the
  syscall with fuzzed inputs, reads the trace, proposes a
  check.
- **Bisection under semantic criteria** — "find the commit
  that changed the dmesg signature for event X". The agent
  provides the test function; uml-bisect drives the binary
  search.
- **Reproducer minimization** — agent runs a failing
  workload, progressively simplifies until the minimal
  reproducer is found, each trial a fresh fork.

## Prior art for the protocol surface

- **OpenAI Computer Use / Operator** — tool-shaped
  abstractions over a browser. Maps well onto the "tool per
  capability" pattern; avoids the "free-form shell" antipattern.
- **GitHub Copilot Workspace** — explicit edit/test/run/debug
  phase buttons. Discrete tools beat "run any command"
  because agents respond well to narrow, named verbs.

## Non-goals

- **Not a general code-execution sandbox.** `run_inside`
  routes through the guest and is scoped to the kernel
  experimenting use case. For "please run this Python
  script" LLM tooling, use a container sandbox, not UML.
- **Not an IDE.** uml-mcp exposes verbs; the agent's UI
  (Claude Code, Cursor, bespoke) is outside scope.
- **Not a replacement for umlctl.** uml-mcp is a thin
  MCP-protocol wrapper; all heavy lifting stays in
  uml-launcher + umlctl.
- **Not a cloud service.** Local-process or single-host
  systemd-user service only. Multi-tenant uml-mcp-as-a-
  service is someone else's problem.

## Open questions

- **Q1: per-tool authorization.** Should `apply_patch` be
  gated behind an explicit `--allow-patch` flag, or always
  on? Default off — patch-apply is the most blast-radius
  tool. Agent asks the user; user flips the flag; agent
  re-attempts.
- **Q2: streaming output.** Long-running tools (rebuild,
  bisect) need progress. MCP supports tool streaming via
  notifications; use that. Don't block the agent for
  minutes of silence.
- **Q3: trace shape.** `read_trace` returns what format? Raw
  tracefs lines are large and noisy. Options: (a) structured
  events with `{ts, cpu, task, func, args}`; (b) delegate to
  Perfetto JSON (see 12) and return a file handle. Probably
  (a) for small reads, (b) for "give me everything since the
  last snapshot".
- **Q4: kernel rebuild caching.** Agent-driven patches mean
  1000+ rebuilds per session. `ccache`/`sccache` + `make
  O=/tmp/uml-per-agent-session/` + ramdisk-backed object
  cache. Critical for the speed wedge to actually materialize.

## Effort estimate

2-3 weeks for a minimum-viable MCP server exposing all 9
tools. Most of the time is in `apply_patch` and `bisect`
happy-path correctness (patch rejection, rebuild failure,
partial bisection state).

## Dependencies

- C-10 v2 landed (decomposed backends).
- 05-umlctl.md built (lifecycle CLI that uml-mcp wraps).
- 07-uml-bisect.md built (the bisect tool this exposes).
- MCP protocol stability (the Anthropic spec is young; pin
  to a specific revision at time of build).

## Cross-references

- `00-vision.md` §"end-user vision" — the one-liner this
  doc concretizes.
- `02-workstreams/C-profiles-and-gaps/09-snapshot-forkserver.md`
  — the forkserver that gives the speed wedge.
- `05-umlctl.md` — the CLI uml-mcp wraps.
- `07-uml-bisect.md` — the bisect tool exposed via the
  `bisect` MCP verb.
- `12-uml-perfetto-trace.md` — the trace-export format.
