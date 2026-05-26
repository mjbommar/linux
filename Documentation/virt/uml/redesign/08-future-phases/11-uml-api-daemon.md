# uml-api — systemd user-service REST API for external orchestrators

**Status:** PROPOSED — future phase. Not in the A/B/C/D plan.

`umlctl` (05) is a single-user local CLI. External
orchestrators (Kubernetes, Nomad, nix-based lab harnesses)
don't exec CLIs — they POST to APIs. uml-api is the daemon
they talk to.

## Ideal end-user flow

Operator installs the systemd user service:

```text
$ systemctl --user enable --now uml-api
● uml-api.service — UML sandbox API
     Active: active (running) since Wed 2026-04-22 09:01:04 UTC
     Main PID: 412033 (uml-api)
```

External orchestrator POSTs:

```text
POST /instances HTTP/1.1
Content-Type: application/json

{
  "name": "ci-job-42",
  "kernel": "sha256:abc123...",
  "profile": "research",
  "rootfs": "sha256:def456...",
  "mem": "1G",
  "workload": {"init": "/bin/run-tests", "timeout_s": 600}
}
```

Response:

```text
201 Created
Location: /instances/ci-job-42

{
  "id": "ci-job-42",
  "status": "starting",
  "console_url": "ws://.../instances/ci-job-42/console",
  "metrics_url": "/instances/ci-job-42/metrics"
}
```

Later:

```text
GET /instances/ci-job-42/metrics
200 OK
# TYPE uml_instance_rss_bytes gauge
uml_instance_rss_bytes{instance="ci-job-42"} 418004992
# TYPE uml_instance_syscall_rate gauge
uml_instance_syscall_rate{instance="ci-job-42"} 1421.3
...
```

## What this builds on

- **`umlctl` (05)** — implements the verbs. uml-api is a
  thin REST wrapper; every endpoint maps to a `umlctl`
  command.
- **`systemd --user`** — the execution substrate.
  uml-api.service runs unprivileged, owns
  `~/.local/state/uml/`, enables via `loginctl enable-
  linger` if the orchestrator wants instances to outlive
  interactive sessions.

## Build shape

- Rust binary under `tools/uml/uml-api/`, sibling to
  uml-launcher / umlctl / uml-mcp.
- HTTP server: `axum` (same choice as uml-probe-web,
  consistent stack). tokio-based.
- Endpoints mirror the REST conventions Kubernetes users
  expect:

  | Method | Path | Maps to |
  |---|---|---|
  | `POST` | `/instances` | `umlctl create` + `umlctl start` |
  | `GET` | `/instances` | `umlctl ps` |
  | `GET` | `/instances/:id` | `umlctl inspect` |
  | `DELETE` | `/instances/:id` | `umlctl stop` + `umlctl rm` |
  | `GET` | `/instances/:id/metrics` | prometheus scrape file |
  | `GET` | `/instances/:id/logs` | `umlctl logs` (supports `?follow=true` via SSE) |
  | `WS` | `/instances/:id/console` | console backend socket proxy |
  | `POST` | `/instances/:id/snapshots` | `umlctl snapshot` |
  | `POST` | `/instances/:id/snapshots/:label/restore` | `umlctl restore` |
  | `POST` | `/instances/:id/exec` | `umlctl exec` with streaming response |
  | `GET` | `/healthz` | liveness |
  | `GET` | `/readyz` | readiness |

- Systemd unit template ships at
  `contrib/systemd/uml-api.service`. Handles
  `Restart=on-failure`, `LockPersonality=yes`,
  `NoNewPrivileges=yes`, `ProtectHome=read-only` except
  `~/.local/state/uml/`.

## Authentication

Two-tier model:

- **Token auth** (default) — `/auth/token` issues
  HMAC-signed bearer tokens scoped to specific instance
  names + verbs. Tokens expire; renewable. Token signing
  key persisted in
  `~/.local/state/uml/uml-api.key` (same pattern as
  uml-probe).
- **mTLS** (optional) — for orchestrators running on a
  shared network. Certificate-based identity, configurable
  CA. Needed for nomad/k8s operator use.
- **Peer-credential auth** (Unix-socket variant) — when
  uml-api listens on a Unix socket (`--listen
  /run/user/1000/uml-api.sock`), accept any client whose
  `SO_PEERCRED` matches the service user. No tokens
  needed for local-machine orchestration.

## Prior art

- **Firecracker API** (`firecracker --api-sock
  /run/firecracker.sock`) — closest match, minimal REST
  surface over a one-instance-per-daemon model. uml-api
  generalizes to multi-instance.
- **Podman API** (`podman system service`) — multi-user,
  per-user daemon via systemd-user. Good precedent for the
  rootless posture + CDI integration patterns.
- **Kubernetes device plugin API** — gRPC pattern for
  "report a custom resource type". Overkill for v1, but if
  someone writes a `k8s.io/device-plugin` for UML later,
  they'd wrap uml-api.
- **libvirt** — the sprawling full-featured
  other-direction. Don't be libvirt. uml-api's surface stays
  small.

## Non-goals

- **OCI-compatible runtime spec.** UML isn't a container;
  spec compatibility (runc / youki) is a different thing.
- **Built-in scheduling across hosts.** This is a
  per-host daemon. Multi-host scheduling is k8s/nomad's
  job; uml-api is the target they schedule onto.
- **Image registry.** Out of scope; rootfs images are
  referenced by path or content-hash on the local host.
  Pulling from registries is the orchestrator's
  responsibility.
- **Web UI.** uml-api is API only. If a browser UI is
  wanted, 08-uml-probe-web.md is the starting point; or a
  separate `uml-ui` that speaks to uml-api.

## Open questions

- **Q1: system-wide (as-root) vs per-user service.** Both
  shapes are defensible. Default: per-user. System-wide
  ships as `contrib/systemd/uml-api.system.service` for
  operators who want it, with socket-activation + sandbox
  options to minimize blast radius.
- **Q2: storage driver abstraction.** Today state lives in
  `~/.local/state/uml/`. If the orchestrator wants to
  offload to object storage (S3-like) for snapshots across
  a fleet, we'd want a pluggable storage driver. Defer;
  local-fs only in v1.
- **Q3: metrics wire format.** Prometheus scrape by
  default. OpenTelemetry OTLP is the alternative / next
  generation; consider adding a second output format later.
- **Q4: what `/instances/:id/exec` looks like on the
  wire.** Long-running, streaming stdio. WebSocket is the
  conventional answer; SSE doesn't do client→server
  streaming. WebSocket it is.

## Effort estimate

3-4 weeks for the MVP (token auth, core endpoints,
prometheus metrics, systemd unit template). Another 2
weeks for WebSocket console + exec, plus mTLS.

## Dependencies

- `umlctl` (05) fully built — uml-api calls it internally
  rather than reimplementing lifecycle.
- C-10 v2 (decomposed backends) for richer `/instances/:id`
  responses (per-backend status).

## Cross-references

- `05-umlctl.md` — the CLI whose verbs this wraps.
- `04-uml-launcher-tui.md` — the human-facing equivalent.
- `06-uml-mcp.md` — the LLM-agent-facing equivalent (MCP
  transport instead of REST).
- `08-uml-probe-web.md` — the live-trace browser UI; can
  sit behind the same mTLS / token layer.
