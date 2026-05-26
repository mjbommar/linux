# Ideal End-User World and Roadmap Gaps

This note asks a different question from workstreams A-D:

> If UML succeeds from an **end-user** point of view, what should it
> feel like to use?

This is not a replacement for the current roadmap. It is a parking-lot
sketch for future phases, informed by adjacent systems' current user
experience.

## Scope

The comparison below is informed by current official documentation for:

- gVisor runtime monitoring, observability, and performance guidance
- Firecracker's API- and metrics-driven microVM model
- crosvm's process-per-device sandboxing model
- Kata Containers tracing and transparent OCI / Kubernetes integration
- OpenTelemetry's "instrument once" and zero-code instrumentation model
- Perfetto's unified trace + offline UI model
- Parca's continuous profiling / regression-diff model
- Pixie's no-instrumentation service map, request tracing, and
  flamegraph **UX**. Pixie's *implementation* (in-host eBPF probes
  discovering services across containers) does not apply to UML —
  UML is the guest, not a container orchestrator. What we want
  from Pixie is the **property** of "start a workload, get a
  useful service map + request traces + flamegraphs without
  modifying the workload." The implementation path to that
  property under UML is different (guest-internal `uprobes` +
  `user_events` + our Layer 2 gates), but the user-visible shape
  is the same.
- Linux `uprobes` and `user_events` as standard building blocks for
  user-space + kernel trace correlation

The intent is not to copy these projects wholesale. The intent is to
understand what users now reasonably expect from modern sandboxing and
observability tools.

## Ideal world: what a UML user should be able to do

### 1. Run a real workload with minimal ceremony

The end user should be able to take a stock Debian or Alpine workload,
or a convenient image-derived rootfs, and run it without building a lab
setup by hand.

The ideal experience is closer to:

```text
uml run --profile research --rootfs debian-bookworm.img -- python -m uvicorn app:app
```

than to:

```text
./linux ubd0=... mem=... root=... rootfstype=...
```

This does **not** mean UML becomes "a container runtime" in the
market sense rejected in `00-vision.md`. To be concrete about the
boundary — what `uml run` would NOT do, and what a container runtime
DOES do:

- **Image distribution**: a container runtime pulls, stores, and
  layers OCI images from registries. `uml run` consumes an image
  file a user already has locally; fetching + caching stays
  out of scope.
- **Network / policy**: a container runtime configures CNI, network
  namespaces, iptables rules, service meshes. `uml run` exposes
  whatever host-user networking the user configures manually,
  same as today.
- **Orchestration integration**: container runtimes speak
  containerd/CRI/kubelet; they participate in k8s lifecycle, pod
  scheduling, and multi-tenant isolation. `uml run` is a
  single-invocation wrapper around `./linux ...`.
- **Resource accounting**: cgroups hierarchies, OOM handling,
  cadvisor metrics. `uml run` inherits whatever the host
  process inherits.

What `uml run` WOULD do:

- Build or accept a rootfs image.
- Assemble the existing `./linux ubd0=... mem=... root=... ...`
  command line from higher-level flags.
- Optionally mount host paths into the guest.
- Wire stdin/stdout/stderr sensibly.

It is a **launcher**, not a **runtime**. The distinction is the
same one between `qemu-system-x86_64` (launcher) and
`containerd-shim-runc` (runtime) — both valuable, both different
products. Cf. `00-vision.md` §"What UML is not".

### 2. Get immediate baseline visibility

A user dropping a FastAPI app into a guest should get baseline answers
quickly, without rewriting the application first:

- which endpoints are slow,
- which requests are driving CPU,
- which threads are blocked,
- whether latency is scheduler-, syscall-, page-fault-, fs-, or
  network-driven,
- whether errors correlate with host or guest resource pressure.

The modern expectation here is:

- zero- or low-code auto-instrumentation for a useful baseline,
- service maps and endpoint-level latency views,
- flamegraphs or profile diffs,
- correlated traces, metrics, and logs,
- fast drill-down from "service is slow" to "these code paths and kernel
  events explain why".

### 3. Correlate application and kernel behavior in one view

For the "optimize a FastAPI app inside a distro guest" story, kernel-only
visibility is not enough.

The ideal world is a single analysis flow that can correlate:

- request / span identity,
- guest user-space work,
- guest syscalls,
- scheduler transitions,
- page faults and memory pressure,
- network flows and retransmits,
- storage latency,
- optional host-side sandbox/runtime events.

The output should feel like a unified timeline or queryable trace, not a
pile of unrelated logs.

### 4. Export into standard tools instead of inventing a silo

Users should not need to learn a UML-only observability stack.

Ideal outputs:

- OTLP export for traces / metrics / logs
- Perfetto-compatible trace export for unified timeline analysis
- pprof-compatible profiles where practical
- tracefs / perf-friendly events for Linux-native workflows

If a user already runs Jaeger, Grafana, Prometheus, or Perfetto, UML
should meet them there.

### 5. Move from research to production without changing the workload

The same workload should move through a **promotion path**:

1. `research` — heavy visibility, broad debug surfaces
2. `prod-with-hooks` — low-overhead hooks compiled in, mostly off
3. `sandbox` — stronger launcher / isolation constraints
4. `prod-fast` — minimal overhead, best available backend

The guest artifact, launch shape, and runtime assumptions should stay as
stable as possible across those profiles.

### 6. Capture a problem live, then go back to fast mode

The end-user story for `prod-with-hooks` is strong only if it becomes a
real workflow:

- turn on narrow tracing for 10-30 seconds,
- capture a bundle,
- turn it off,
- inspect offline,
- compare with a baseline,
- decide whether to fix app code, guest config, kernel config, or host
  launcher settings.

### 7. Make the secure path and the observable path legible

Users need two different promises, clearly separated:

- **production-debuggable**: operationally safe, low overhead, strong
  incident debugging path
- **hostile-workload-safe**: tighter isolation, smaller debug surface,
  stronger launcher confinement

Those are adjacent, but not identical.

### 8. Benchmark real services, not just microbenchmarks

An end user with a Python web service cares about:

- request latency p50 / p95 / p99
- throughput under load
- cold start / warm start time
- CPU spent in interpreter, TLS, epoll, allocator, kernel, and I/O
- impact of hooks or sanitizers on real service behavior

`getpid()` and `boot-to-/bin/true` are necessary, but not sufficient.

## Gap sketch against the current roadmap

The current roadmap is strong on kernel architecture and weaker on the
end-to-end workload/operator story.

The table below separates three statuses:

- **planned** — already meaningfully covered by A-D
- **partial** — implied or adjacent, but not yet scoped well enough
- **missing** — not meaningfully on the roadmap yet

| Capability | Status vs current roadmap | Notes |
|---|---|---|
| Backend abstraction and low-overhead hook substrate | planned | A and B cover this directly. |
| Research / prod-with-hooks / sandbox profile split | planned | C covers the profile matrix. |
| KVM path toward near-QEMU performance | planned | D covers this, though it is parallel to the critical path. |
| Easy rootfs / distro / image UX for real app workloads | partial | Current docs mention rootfs images and launchers, but there is no first-class workload ingestion UX. |
| Service-level golden workloads (FastAPI, nginx, Redis, PostgreSQL) | missing | Benchmarks are mostly kernel-centric. |
| Unified app + kernel + network trace correlation | partial | Kernel tracing is planned; app-level correlation is not fully scoped. |
| Guest user-space tracing mechanisms (`uprobes`, `user_events`, `perf` counters) *work inside the UML guest* | partial — needs verification spike | These are host-kernel features. The question is "does UML's backend trap-loop route them correctly" — probably yes for uprobes under ptrace, unclear under seccomp, unknown for `user_events` and perf hardware counters. One afternoon of testing resolves this; see PARK.8 spike. |
| Novel **correlation** between guest user-space spans and guest kernel events | missing | This is the actual product work: a story for "trace arrives in guest app → surfaces kernel syscalls / page faults / scheduler transitions it caused → exports as a single trace." No mechanism in the current roadmap produces this. |
| Standard export to OTLP / Perfetto / pprof-like formats | missing | No dedicated task today. |
| Always-on or low-friction continuous profiling | missing | Not clearly represented in B or C. |
| Scriptable debug workflows / query catalog | missing | No Pixie-UX-like or Perfetto-UI-like layer is planned. (Pixie's eBPF-based implementation doesn't port to UML; what's missing is the *"workload in, diagnoses out"* UX property — the mechanism would be different.) |
| Promotion contract across profiles for the same workload artifact | missing | Profiles are documented, but continuity rules are not. |
| Production-safe incident capture bundle | partial | B points in this direction, but no explicit bundle/export workflow exists. |
| OCI / containerd / Kubernetes integration path | partial | C-10 launcher is adjacent, but the plan explicitly avoids becoming "a container runtime." Convenience integration is still underspecified. |
| Snapshotting for service warm-start / debug loops, not just fuzz | partial | C-09 is fuzz-oriented today. |
| Telemetry privacy / redaction / data-governance policy | missing | Important if request payloads or full traces become first-class. |

## The biggest missing pieces

### A. User-space observability and correlation

This is the most important technical gap relative to the "FastAPI inside
Debian/Alpine" vision. It splits cleanly into two sub-problems with
very different costs.

**A.1 — Do Linux's existing userspace-tracing mechanisms work inside
a UML guest today?**

That includes `uprobes`, `user_events`, guest-side `perf` with
hardware counters, `ftrace`, BPF programs attached to userspace
probes. These are host-kernel features that should "just work" in
the guest *if* UML's backend trap-loop routes the relevant signals
correctly. For each mechanism, the answer is one of:

- yes (nothing to do — document that it works + add a selftest),
- yes, but only under one backend (document the asymmetry + track
  it as a backend-parity task),
- no, and the gap is a known deficiency in UML (file a real task).

This is an **afternoon's worth of spike work** per mechanism
(PARK.8). Most of the "missing" row in the gap table above is
expected to collapse into "partial — documented + selftest" here,
not "missing — novel engineering."

**A.2 — Novel correlation: guest userspace spans ↔ guest kernel events.**

This is the actual research-into-product work. Today the roadmap
creates instrumentation surfaces (Layer 2 gates fed by slow paths,
future ftrace, future kcov). It does not produce a single data
stream where a userspace trace span *in the guest process* can be
joined to the syscalls / faults / IRQs / scheduling transitions
that span caused. Landing that requires either:

- a span-ID propagation scheme the gate slow paths honor (write
  current span ID into a per-task field, include it in every
  event), or
- a joinable trace-ID column in an after-the-fact analysis tool
  that correlates on (pid, tid, ts) — cheaper but less rigorous.

Either way: mechanism, schema, and UX. This is the phase-F work.

Without A.2, UML remains an excellent **kernel** observability
platform but not a complete **service** observability platform.
The roadmap should not pretend A.1 and A.2 are the same thing —
solving A.1 is cheap; solving A.2 is a real engineering project.

### B. Standard telemetry outputs

The roadmap currently creates instrumentation surfaces, but not a
clear telemetry contract for users.

The end-user expectation is now:

- export traces to a standard collector,
- view a unified timeline locally or in an existing backend,
- diff profiles between versions / profiles / builds,
- correlate logs with trace IDs.

This suggests future work — **but note that export is downstream of
capture**. Each exporter has a precondition:

- **Perfetto export**: ingests ftrace directly. Once ftrace is
  wired on UML (workstream C), this is near-free UX work.
- **OTLP export**: requires our gate slow paths (or Phase F's
  successor) to emit span-shaped events with start/end timestamps
  and IDs. Today the slow paths increment a counter; span
  production is a separate engineering project.
- **pprof export**: requires guest-side `perf record` sampling,
  which depends on PARK.8's spike outcome and (probably) Phase F's
  perf integration.
- **Offline capture bundles**: a tarball schema for traces +
  profiles + metadata. The schema is trivial once the payloads
  are producible.

Export work in isolation risks being a Potemkin demo — the wrappers
exist but there's nothing interesting for them to carry. Sequence
accordingly: capture first (Phase F), export second (Phase G).

### C. Workload-centric UX

The plan talks about profiles and defconfigs. End users think in terms
of workloads.

There is a gap between:

- "build `uml/research`"

and:

- "run my Python service under UML, inspect its hot paths, compare
  research and prod-with-hooks, and ship"

That gap is mostly tooling, packaging, and validation, not backend
architecture.

### D. Promotion semantics

The current profile docs describe separate targets well. They do
not yet describe a stable promotion workflow for the same workload
artifact.

The contract this phase needs to land, stated as a testable
assertion (the same shape as invariant I6 in
`01-architecture/invariants.md`):

> **Promotion contract (draft):** A workload artifact that passes
> its functional test suite under `research` MUST pass the same
> suite unchanged under `prod-with-hooks`, `sandbox`, and
> `prod-fast`. Only performance metrics (latency, throughput,
> memory, boot time) are permitted to differ. Any functional
> divergence — different return values, different syscall
> outcomes, different signal delivery, different scheduling
> behavior visible to the guest — is a bug in either UML or
> the profile definition, to be fixed before ship.

That single sentence is falsifiable: give it a workload, a test
suite, and two profiles, and you can run the test on both and
declare pass/fail. Future phase-H work fleshes out:

- what "same suite" means (identical binary vs rebuilt per profile),
- how performance deltas are reported (histograms, not point
  estimates),
- which profile is the source of truth for a *performance*
  regression vs a *safety* regression vs a *diagnosis* finding,
- how a failing promotion is communicated back to the workload
  owner.

But the core promise — behavior is identical, only performance
differs — is where the contract lives.

## Candidate future phases

These are not commitments. They are placeholders for future planning.

**Sequencing note — the letters E/F/G/H/I are labels, not a linear
critical path.** Actual dependencies form a DAG:

```
  A–D (current roadmap, kernel architecture)
          │
          ▼
  ┌───────┴────────┐
  │                │
  ▼                ▼
Phase E          Phase F
(workload/      (whole-stack
 image UX)       observability)
  │                │
  │                ▼
  │             Phase G
  │             (export / analysis UX)
  │                │
  ▼                ▼
Phase I          Phase H
(service         (promotion +
 benchmarks)     incident workflow)
```

E and F are parallel. G depends on F. H depends on F + E + G. I
depends on E (needs runnable workloads) and is naturally parallel
to F/G/H. If capacity allows, E and F run simultaneously; G and I
follow; H closes the loop with the promotion story.

### Phase E: Workload and image UX

Goal:

- Make real workloads easy to run and reproduce.

Likely contents:

- rootfs / image builder recipes for Debian and Alpine
- convenience tooling to turn OCI-like inputs into UML-bootable inputs
- golden app workloads: FastAPI, nginx, Redis, PostgreSQL
- documented launch flows for research / prod-with-hooks / sandbox

### Phase F: Whole-stack observability

Goal:

- Move from "kernel observability" to "workload observability".

Likely contents:

- `uprobes` / user-space tracing integration
- `user_events` support for guest-emitted structured events
- request/span correlation from guest user-space to kernel events
- guidance for OpenTelemetry interop
- explicit trace correlation model across app, kernel, and launcher

### Phase G: Export and analysis UX

Goal:

- Make the data usable outside UML-specific tooling.

Phase G is **downstream of Phase F**. Export is the UX on the
data; Phase F produces the data. Each export target has a
specific capture prerequisite that must exist first:

- **OTLP** needs spans (start timestamp, end timestamp, trace ID,
  span ID, parent ID, attributes). The current Layer 2 gates count
  hits; they don't emit spans. Phase F must produce per-span
  start/end events before OTLP export can be more than a token
  demo.
- **Perfetto** can ingest ftrace directly via its trace format;
  this is the lowest-effort target once ftrace lands on UML, and
  doesn't require Phase F to complete. `perf.data` is similar.
- **pprof** needs stack traces sampled periodically. This requires
  guest-side `perf record` to work (PARK.8's spike, and Phase F's
  perf integration) before pprof export is meaningful.

Likely contents once Phase F produces the capture shapes:

- Perfetto export path (achievable once ftrace is wired — could
  land in parallel with F rather than after, if someone picks it up)
- OTLP export path (requires Phase F span-producing work)
- pprof/profile export path (requires guest-side perf integration)
- offline capture bundle format (OS-level tarball containing the
  trace + profile + relevant metadata — Phase F's capture
  artifacts dictate the schema)
- one supported CLI and one supported UI workflow for common
  diagnoses

Capture goes first. Export is thin — once the data shape is right,
each exporter is a few hundred lines of serialization.

### Phase H: Production promotion and incident workflow

Goal:

- Make the research-to-production story legible and safe.

Likely contents:

- promotion contract across profiles
- capture-on-demand bundle workflow for `prod-with-hooks`
- profile diff tooling between research / prod-with-hooks / sandbox /
  prod-fast
- redaction and privacy controls for exported traces

### Phase I: Service-oriented validation

Goal:

- Benchmark what users actually care about.

Likely contents:

- service latency / throughput suites
- p50 / p95 / p99 request benchmarks
- benchmark harnesses for Python, web, DB, and mixed I/O workloads
- workload-level regression gates in CI

## Concrete end-user target workflow

If the long-term experience is good, it should look something like
this:

```text
1. Build or fetch a rootfs:
   uml image build --distro alpine --package python3 --package py3-pip

2. Run the workload in research mode:
   uml run --profile research --rootfs alpine.img -- python -m uvicorn app:app

3. Collect a baseline:
   uml observe capture --duration 30s --export perfetto --export otlp

4. Inspect:
   - endpoint latency
   - slow requests
   - CPU flamegraph
   - syscall / scheduler / page-fault correlation
   - network retransmits and fs hotspots

5. Switch to prod-with-hooks:
   uml run --profile prod-with-hooks --rootfs alpine.img -- python -m uvicorn app:app

6. When production misbehaves:
   uml observe attach --enable trace_syscalls --service api --duration 10s

7. Export a capture bundle, diff it against the research baseline,
   then turn tracing back off.
```

The exact commands are illustrative. The important part is the product
shape:

- same workload
- different profiles
- standard outputs
- one mental model

## Recommended next planning moves

1. Keep A-D focused; do not derail B/C/D with this parking lot.
2. When B stabilizes, scope a **user-space observability** task first.
3. Add at least one **service workload benchmark** before claiming UML
   is a strong app-observability platform.
4. Define a **promotion contract** between `research`,
   `prod-with-hooks`, `sandbox`, and `prod-fast`.
5. Treat standard export formats as a product requirement, not a late
   polish item.

## External signals used for this note

- gVisor runtime monitoring: https://gvisor.dev/docs/user_guide/runtimemonitor/
- gVisor performance guide: https://gvisor.dev/docs/architecture_guide/performance/
- Firecracker overview / API / metrics: https://github.com/firecracker-microvm/firecracker
- crosvm architecture: https://crosvm.dev/book/architecture/overview.html
- crosvm sandboxing: https://crosvm.dev/book/appendix/sandboxing.html
- Kata tracing: https://katacontainers.io/blog/tracing-in-kata-containers/
- OpenTelemetry overview: https://opentelemetry.io/
- OpenTelemetry eBPF instrumentation: https://opentelemetry.io/docs/zero-code/obi/
- Perfetto overview: https://perfetto.dev/docs/
- Parca overview: https://www.parca.dev/
- Pixie overview: https://docs.px.dev/about-pixie/what-is-pixie/
- Pixie service performance tutorial: https://docs.px.dev/tutorials/pixie-101/service-performance/
- Linux `user_events`: https://docs.kernel.org/6.3/trace/user_events.html
- Linux `uprobes`: https://docs.kernel.org/6.4/trace/uprobetracer.html
