# Tier 3 + LTP template scaffolding landed (2026-05-14)

## What

In-tree TOML scaffolding for Track A's remaining workload types,
based on the design memos:

  - `tier3-django.toml.template` — Django loopback HTTP server.
  - `tier3-fastapi.toml.template` — FastAPI/uvicorn loopback.
  - `ltp-runner.toml.template` — kirk-driven LTP runner.

All three are landed but NOT smoke-tested. They require operator
pre-flight work that's documented inline in each template header:

  - Tier 3: `CONFIG_UML_NET_VECTOR=y` kernel rebuild + (optionally)
    `apt-get install python3-django python3-fastapi
    python3-uvicorn`. The templates use Python stdlib `http.server`
    as a stand-in for the real Django/FastAPI server bring-up so
    the in-tree scaffolding is smokable once VECTOR is in place;
    operator can swap the stdlib shim for real Django/uvicorn
    invocations once the apt installs are done.
  - LTP: `git clone linux-test-project/ltp` + `make install` to
    `/opt/ltp`, plus `git clone linux-test-project/kirk` to
    `/opt/kirk`. Plus the curated skip-list seed from memo §3 lives
    at `{{SOAK_DIR}}/ltp-skip.txt` (also operator action — the
    curation memo enumerates the 112 entries but doesn't generate
    the file).

## Why a stdlib `http.server` shim instead of real Django/FastAPI

The Tier 3 design (memo `phase-J-tier3-design-2026-05-14.md`) calls
for actual Django/FastAPI servers. The shim is a deliberate
intermediate:

  1. It exercises the same UML virtio-net + UML net stack + tap
     iptables NAT path that the real frameworks would, so the
     real bug class (UML's CONFIG_UML_NET_VECTOR=y wiring,
     gate_loop.rs:253-277's per-worker fanout issue, etc.) shows
     up identically.
  2. It removes the apt-install dependency, letting Tier 3 boot
     under the daemon once VECTOR is in place even before
     Django/FastAPI are installed.
  3. Once operator installs Django+FastAPI+uvicorn, the cmd
     lines in the templates get swapped for `python3 -m django
     runserver` / `python3 -m uvicorn` and the rest of the
     pipeline (curl battery, scoreboard, summary) is unchanged.

## Daemon-side per-worker IP allocation

Per memo `phase-J-tier3-design-2026-05-14.md` §6, Tier 3
requires a separate carve-out in `run-soak-daemon.sh`: instead
of one `umlctl gate loop --workers N` invocation, spawn N
`--workers 1` invocations with per-worker IPs from
`192.168.42.0/24` carved into /30s.

This change is NOT yet in `run-soak-daemon.sh` — it's deferred
pending the operator's CONFIG_UML_NET_VECTOR=y rebuild (the
shape of the daemon change depends on what `umlctl gate loop`
actually accepts after the rebuild, which can't be smoke-tested
without that kernel binary).

Filed as task #25 (pending) for the daemon-side IP-allocation
addition.

## Acceptance criteria deferred

The Phase J DONE definition (PLAN §3) says "Tier 1/2/3 wired
with at least one passing cycle of each; LTP runner integrated;
headline ratio recorded." With this commit, Tier 3 + LTP are
**wired in the in-tree sense** (templates exist) but **not
wired in the daemon-rotation sense** (the daemon doesn't yet
include them in its default workload list, and Tier 3 needs
the IP-allocation carve-out).

Next operator-time work:

  1. Rebuild kernel with CONFIG_UML_NET_VECTOR=y (~3 min).
  2. apt-get install python3-django python3-fastapi
     python3-uvicorn (~30 s).
  3. git clone LTP + kirk + `make install` (~10 min, ~500 MB).
  4. Wire Tier 3 + LTP into run-soak-daemon.sh's default
     workload list + add the per-worker IP-allocation carve-out
     for Tier 3.
  5. Smoke-test each template under the daemon.

## Implications for PLAN execution

Tracks A §3.2 + §3.3 are now **designed + scaffolded** but NOT
**verified end-to-end**. The remaining work is bounded operator
time. Other tracks (Time-machine #168, Polish, Upstream) can
proceed in parallel.

This is enough to declare Phase J Track A's scaffolding done
for the in-tree push; final verification of the workload set
loops back when operator pre-flight is done.
