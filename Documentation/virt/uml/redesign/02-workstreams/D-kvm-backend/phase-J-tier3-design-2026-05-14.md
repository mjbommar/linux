# Phase J — Tier 3 design (Django / FastAPI HTTP loopback)

> Resolves open question #2 from `phase-J-design-2026-05-07.md` §3.3:
> how does the host-side daemon learn the guest's IP address on the
> umlctl-managed TAP interface for the host-side curl battery?
>
> Scope: design only. Decision + implementation sketch + Tier 3
> template skeleton. No code change in this memo.

## 1. TL;DR

**Choose option (b): fixed per-worker IP allocation policy** —
`192.168.<SOAK_SUBNET>.<2 + 2*worker_idx>/30` for the guest,
`<+1>` for the host-side TAP. The soak daemon generates each
worker's Umlfile with the matching IPs at template-expansion
time. Rationale: (a) requires schema extension to the umlctl
manifest **and** a stateful write from launcher to manifest at
boot, both of which are net-new code paths in a tree where the
manifest is intentionally immutable post-`create`; (b) is six
lines of `sed` and one extra template placeholder.

## 2. umlctl source audit

### 2.1 Where TAP setup actually lives

There are *two* network code paths in `tools/uml/uml-launcher`:

- `src/backend/net.rs` — vhost-user virtio-net backend. **This is
  the data path only.** It opens `/dev/net/tun`, does
  `TUNSETIFF(IFF_TAP | IFF_NO_PI)` against a TAP interface name
  passed in via `BackendNetArgs.tap`, and shuttles Ethernet
  frames between the virtqueue and the TAP fd. It does **not**
  create the TAP, assign IPs, or know what subnet anyone is on.
  The doc-comment is explicit (`backend/net.rs:49-50`):
  > TAP creation or persistence — the operator sets up
  > `ip tuntap add tap0 mode tap ...` before launch.

- `src/bin/umlctl/deploy.rs` — Umlfile compiler and `umlctl up` /
  `umlctl down` host-side setup. **This is where TAP creation,
  IP assignment, iptables NAT and port-forward DNAT all
  happen.** The Phase J memo cites `backend/net.rs` but the
  authoritative place is `deploy.rs` (see §6 of this memo on
  fixing that pointer).

### 2.2 IP allocation policy today

Per-Umlfile, **static, hard-coded defaults**. The `NetworkSection`
in `deploy.rs:96-126`:

```rust
pub struct NetworkSection {
    pub mode: String,           // "none" (default) or "tap"
    pub tap_name: String,       // default "uml-tap0"
    pub guest_ip: String,       // default "10.7.0.2/24"
    pub host_ip: String,        // default "10.7.0.1/24"
    pub gateway: String,        // default "10.7.0.1"
    pub nameservers: Vec<...>,
    pub masquerade_via: String, // "auto" picks default route iface
    pub ports: Vec<String>,     // "8080:8080/tcp", DNAT'd
}
```

No DHCP, no allocator, no pool. Whatever the Umlfile declares is
what the guest gets. `compile()` emits a deterministic list of
`sudo sh -c ip tuntap add ... && ip addr add ... && iptables -t
nat -A ...` lines from these fields.

The defaults assume **one instance per host**: a second instance
booted from the same Umlfile-template will try to
`ip tuntap add dev uml-tap0` (already exists → EEXIST) and
`ip addr add 10.7.0.1/24 dev uml-tap0` (already assigned →
EEXIST). This is exactly the "concurrent soak collision" risk
Phase J risk register §6.4 names as a low-likelihood TAP-name
collision; in practice `gate loop --workers N` *does* duplicate
the network section across workers (see §2.4 below).

### 2.3 Instance state

Two roots, both XDG-derived (`src/bin/umlctl/paths.rs:79-102`):

- **Persistent state** under `$XDG_STATE_HOME/uml/`
  (fallback `~/.local/state/uml/`):
  - `instances/<name>.toml` — the manifest, written once at
    `create`/`up` time.
  - `runs/<run_id>/` — per-run bundles (init.log, kernel.log,
    events.jsonl).
  - `history.jsonl` — verb log.
- **Runtime state** under `$XDG_RUNTIME_DIR/uml/`
  (fallback `/tmp/uml-<uid>/`):
  - `<name>.pid` — pidfile.
  - `<name>.run_id` — current run-id sidecar; removed on stop.

**Crucially, the manifest holds no network fields**
(`manifest.rs:15-46`):

```rust
pub struct Manifest {
    pub schema_version: u32,
    pub instance: InstanceSection,
    pub kernel: KernelSection,
    pub runtime: RuntimeSection,   // mem, ncpus, cmdline, root, forkserver
    pub labels: BTreeMap<String, String>,
}
```

The Umlfile's `[network]` section is consumed by `compile()` into
*shell commands* and a kernel cmdline append
(`vec0:transport=tap,ifname=...`), then thrown away. Nothing
persists the IP after `umlctl up` returns. This matters for §3.

### 2.4 What `umlctl ps` prints today

`src/bin/umlctl/registry.rs` — table or NDJSON depending on
`--json`:

| Column | Source |
|--------|--------|
| `name` | manifest.instance.name |
| `pid` | `$RUNTIME_DIR/<name>.pid` |
| `backend` | manifest.kernel.backend |
| `profile` | manifest.kernel.profile |
| `mem` | manifest.runtime.mem |
| `uptime_secs` | `/proc/<pid>/stat` field 22 |
| `state` | "running" / "stopped" |
| `rss_kb` | `/proc/<pid>/statm` (only with `--size`) |
| `labels` | manifest.labels |

`--json` emits one NDJSON line per row via `serde_json` (see
`registry.rs:194-202`). The Row struct
(`registry.rs:20-32`) has no `tap_name`, no `guest_ip`, no
`host_ip` field. Manifests don't carry them and `ps` doesn't
synthesize them.

### 2.5 Gate-loop worker-fan-out collision

`src/bin/umlctl/gate_loop.rs:253-277` shows exactly what
`umlctl gate loop --workers N` does to the Umlfile:

```rust
for w in 0..lo.workers {
    let mut u = base.clone();
    u.instance.name = format!("{stem}-w{w}");
    u.debug.log_dir = format!("logs/{}", u.instance.name);
    // ... apply sweep KEY=value into u.env ...
    // write u as <stem>-w<w>.toml
}
```

**Only `instance.name` and `debug.log_dir` are rewritten.**
Everything else — including `network.tap_name`, `guest_ip`,
`host_ip`, `ports` — is copied verbatim across all N workers.
With `--workers 2` and `mode = "tap"` in the base Umlfile, both
workers race to `ip tuntap add dev uml-tap0` and one fails. This
is *fine today* because no Phase J template uses `mode = "tap"`,
but it's the table-stakes hazard Tier 3 has to address.

## 3. Option (a) — extend `umlctl ps --json` with `guest_tap_ip`

### 3.1 Path from "compile() knows the IP" to "ps prints it"

Today: `compile()` reads `uml.network.guest_ip`, emits shell
commands, and discards the value. The manifest doesn't carry it.

To surface `guest_tap_ip` from `ps`, the value must be persisted
*somewhere `ps` can find it*. Three sub-options:

- **(a.1) Extend the manifest schema.** Add a
  `NetworkSection` to `Manifest` (manifest.rs). Bump
  `schema_version` from 1 → 2. Write a v1→v2 migration shim,
  since older manifests in `~/.local/state/uml/instances/` are
  guaranteed to exist on developer hosts. `cmd_up` writes the
  resolved network section into the manifest after `compile()`.
  `registry::list` reads it back. `Row` gets a
  `guest_tap_ip: Option<String>` field.
  - Touches: `manifest.rs` (~20 lines), `deploy.rs` cmd_up flow
    in `main.rs` (~10 lines), `registry.rs` (~10 lines),
    `paths.rs` migration helper (~20 lines if we write one),
    plus tests. Maybe 80-120 LOC.

- **(a.2) Write a sidecar `<name>.network.toml`.** Avoids the
  schema bump. `ps` reads it if present; absent =
  `guest_tap_ip = null`. Sidecar parallels the existing
  `<name>.run_id` and `<name>.pid` files but lives under
  `$STATE/instances/` (since it's manifest-y, not runtime-y).
  - Touches: `paths.rs` (+1 helper), `deploy.rs` (cmd_up writes
    it), `registry.rs` (reads it), maybe 40-60 LOC.

- **(a.3) Read the live state from the host kernel.** `ip -j
  addr show dev <tap_name>` returns the assigned IP as JSON.
  `registry::list` runs it per row when state == "running" and
  the manifest has a tap_name. But the manifest *doesn't* have
  a tap_name. So this still needs (a.1) or (a.2) to know which
  TAP to query, and adds a fork/exec per `ps` invocation.
  - Touches: same as (a.1) or (a.2) plus ~40 LOC for the `ip -j`
    parsing path.

Minimum viable (a.2) is the cheapest, but it leaves
`umlctl ps --json` schema bifurcated by mode ("tap" rows have
`guest_tap_ip`, "none" rows don't) and adds another
file-on-disk to keep in sync with manifest lifecycle.

### 3.2 Concurrent-soak correctness

(a) is robust **only after** we also fix the gate-loop worker
fan-out (`gate_loop.rs:253-277`) to rewrite the per-worker
`tap_name` / `guest_ip` / `host_ip` to non-colliding values.
Otherwise both workers run `ip tuntap add dev uml-tap0`, one
fails, and "ps" reports the correct-but-meaningless IP for the
one that succeeded plus a stale or null IP for the one that
didn't. So (a) implicitly requires (b)'s per-worker derivation
logic *somewhere* — either in gate_loop or in the soak daemon.

## 4. Option (b) — fixed per-worker IP policy

### 4.1 Allocation scheme

Reserve a private subnet for the soak rig: `192.168.42.0/24`
(arbitrary; chosen to not collide with `10.7.0.0/24` which is
the umlctl default and may already be in use by hand-launched
fastapi instances). Per-worker /30 carve-out:

```
worker 0:  host_ip = 192.168.42.1/30   guest_ip = 192.168.42.2/30
           tap_name = "soak-tap0"
worker 1:  host_ip = 192.168.42.5/30   guest_ip = 192.168.42.6/30
           tap_name = "soak-tap1"
worker N:  host_ip = 192.168.42.(4N+1) guest_ip = 192.168.42.(4N+2)
           tap_name = "soak-tap{N}"
```

/30 = 4 addresses (net, host, guest, broadcast) per worker; no
DHCP, no overlap, no third party allowed on the link. Worker
count cap is `--workers <= 63` (252 addresses / 4), well above
the rig's W=2 default.

### 4.2 How the daemon wires it

The soak daemon already does template substitution
(`run-pilot.sh:88-89`):

```sh
sed -e "s|{{KERNEL}}|$KERNEL|g" -e "s|{{BACKEND}}|$backend|g" \
    -e "s|{{SOAK_DIR}}|$SOAK_DIR|g" ...
```

Add three placeholders for tier3:
`{{WORKER_IDX}}`, `{{GUEST_IP}}`, `{{HOST_IP}}`, `{{TAP_NAME}}`.
The daemon expands `WORKER_IDX` per worker and derives the other
three from it. The compiled Umlfile then goes through `umlctl up`
unmodified — `umlctl` itself doesn't need to know about the
policy.

Code change: ~10 lines in `run-soak-daemon.sh` to compute
host/guest/tap-name from `WORKER_IDX`, plus the placeholders in
the tier3 template files. Zero Rust changes.

### 4.3 Correctness in the face of concurrent soaks

Each worker gets a unique TAP (`soak-tap0`, `soak-tap1`, ...) and
a unique /30. No iptables collisions because MASQUERADE rules
are `-s <cidr>` and the cidrs don't overlap. No DNAT collision
because each port-forward is `-d <guest_ip>`.

The one global resource — `net.ipv4.ip_forward=1` — is idempotent
under `sysctl -w` and currently set by `compile()` already.

### 4.4 Diagnostic ergonomics

`umlctl ps` does *not* show the IP, but the operator can recover
it from `worker_idx` (in the soak's scoreboard.jsonl
`worker_idx` field — see Phase J memo §2.6) by inverting the
formula. Acceptable trade-off: a soak-specific convention buys
zero changes to umlctl in exchange for a five-line README note.

### 4.5 Forward compatibility

(b) extends naturally to bridged-mode or multi-host soaks: the
allocator just changes shape (different subnet, different
prefix length, plug in a real DHCP later). It does not extend
naturally to *non-soak* fastapi deploys where the operator
hand-writes an Umlfile with `guest_ip = "10.7.0.2/24"`; for
those, `ps` still doesn't know the IP. That's an open question
*for general umlctl*, not for Tier 3.

## 5. Decision and rationale

**Choose (b).** Trade-offs against the rubric in the prompt:

| Axis | (a) `ps --json` extension | (b) per-worker policy | Winner |
|------|---------------------------|------------------------|--------|
| Operator-time setup | Build umlctl from this branch; older umlctl in `/home/mjbommar/bench-bundle/bin/` doesn't have the field. | Soak daemon generates IPs from a fixed formula at template-expand time. | (b) |
| Concurrent soaks | Requires gate-loop fan-out fix anyway (see §3.2); otherwise rows are wrong-or-null for colliding workers. | Each worker has a non-colliding /30 by construction. | (b) |
| Diagnostic value | `umlctl ps --json` is the right surface in the abstract — single source of truth, queryable from anywhere. | Operator derives IP from worker_idx via a documented formula; no live introspection. | (a) |
| Forward compat | Generalises to any umlctl deployment (bridge, multi-NIC, non-soak). | Generalises within the soak rig; doesn't help non-soak users. | (a) |
| LOC | 40-120 LOC across 3-4 Rust files + schema migration concerns. | ~10 lines in one bash script + 4 placeholders in 2 .toml.template files. | (b) |
| Phase J risk | Schema migration is a class of bug we haven't run into yet; doing it for the first time *in the Phase J integration window* is bad sequencing. | Pure additive change in the soak-rig tree; doesn't touch the umlctl manifest discipline. | (b) |

The deciding factors:

1. **Schema discipline.** The manifest's
   `schema_version = 1` is documented (manifest.rs:8) as
   "immutable after create — mutations are rm + create."
   Bumping to v2 to surface a derivable-from-policy value is
   the wrong reason to touch that contract.

2. **Tier 3 is the gate, not umlctl-ps.** Phase J's acceptance
   criterion §5.4 is "Tier 3 wired and one full rotation pass
   per framework." It doesn't say "umlctl introspection
   surfaces the guest IP." Option (b) closes the Phase J
   ticket; option (a) closes a strictly larger ticket
   ("operator can interrogate live umlctl state for network
   info") for which Tier 3 is one consumer among many.

3. **`umlctl ps --json` field is still worth adding later** —
   just not now, and not in the Tier 3 critical path. Filed as
   a follow-up below (§9).

**Caveat the decision implies.** The soak daemon's template
substitution must run *after* gate-loop's per-worker Umlfile
generation, OR replace it. The simplest path: the daemon
expands `{{WORKER_IDX}}` into the per-worker Umlfile *before*
calling `umlctl gate loop --workers 1` per worker. That is, for
Tier 3 specifically, the daemon spawns N
`umlctl gate loop --workers 1` invocations rather than one
`--workers N`. Tiers 1, 2, and the pilot's five workloads keep
the `--workers N` invocation; Tier 3 is the carve-out.

## 6. Implementation sketch

### 6.1 Daemon-side (~15 lines added to `run-soak-daemon.sh`)

```sh
# In the tier3 rotation branch, replace the single
# `umlctl gate loop --workers $W ...` call with:

tier3_template="$1"  # e.g. tier3-django.toml.template
for w in $(seq 0 $((SOAK_WORKERS - 1))); do
    host_ip="192.168.42.$((4*w + 1))"
    guest_ip="192.168.42.$((4*w + 2))"
    tap_name="soak-tap${w}"
    worker_toml="$SOAK_OUT/tier3-w${w}.toml"
    sed -e "s|{{KERNEL}}|$UML_KERNEL|g"     \
        -e "s|{{BACKEND}}|$backend|g"        \
        -e "s|{{WORKER_IDX}}|$w|g"           \
        -e "s|{{HOST_IP}}|${host_ip}/30|g"   \
        -e "s|{{GUEST_IP}}|${guest_ip}/30|g" \
        -e "s|{{HOST_IP_PLAIN}}|${host_ip}|g" \
        -e "s|{{GUEST_IP_PLAIN}}|${guest_ip}|g" \
        -e "s|{{TAP_NAME}}|${tap_name}|g"    \
        "$tier3_template" > "$worker_toml"
    umlctl gate loop --workers 1 --iters "$M" \
        --file "$worker_toml" \
        --pass-marker 'TIER3_OK' \
        --fail-marker 'TIER3_FAIL' \
        --out "$SOAK_OUT/tier3-w${w}-r${rotation_idx}" &
done
wait  # ensure all workers finish before next rotation step
# Host-side curl battery runs *during* the umlctl gate loop's
# wait-for window; see tier3 template phase 2 below for the
# guest-side ready marker.
```

### 6.2 Host-side curl battery — runs in parallel with gate loop

```sh
for w in $(seq 0 $((SOAK_WORKERS - 1))); do
    guest_ip="192.168.42.$((4*w + 2))"
    # Wait for the guest's "SERVER_READY" marker, then probe.
    umlctl_wait_for_marker "tier3-w${w}" 'SERVER_READY' 30
    record_curl_battery "$guest_ip:8080/health" 100 \
        > "$SOAK_OUT/tier3-w${w}-curl.csv" &
done
wait
```

`record_curl_battery` is a ~15-line helper: hits the URL N times,
records HTTP code + RTT per request as CSV, exits non-zero iff
any request fails or returns != 200.

## 7. Tier 3 template skeleton

### 7.1 `tier3-django.toml.template`

```toml
schema_version = 1
volumes = []

[instance]
name = "soak-tier3-django-{{BACKEND}}-w{{WORKER_IDX}}"

[kernel]
path = "{{KERNEL}}"
backend = "{{BACKEND}}"
# Tier 3 requires CONFIG_UML_NET_VECTOR=y; the daemon's
# pre-flight check (run once at soak start) greps this from
# the kernel's adjacent .config file. If missing, daemon
# refuses to start the tier3 rotation and logs WHY.

[runtime]
mem = "768M"   # Django + numpy + cryptography import set
ncpus = 2

[network]
mode = "tap"
tap_name = "{{TAP_NAME}}"
guest_ip = "{{GUEST_IP}}"
host_ip = "{{HOST_IP}}"
gateway = "{{HOST_IP_PLAIN}}"
nameservers = []   # No DNS needed; uv cache primed offline (Tier 2).
masquerade_via = "auto"
ports = []         # No DNAT — we hit guest_ip directly from host.

[env]
TMPDIR = "/tmp"
PYTHONHASHSEED = "0"
# uv cache visible via hostfs for the cryptography/numpy imports
# the Django views exercise.
UV_CACHE_DIR = "/var/lib/uml-soak/uv-cache"
UV_OFFLINE = "1"

# Phase 1: smoke / fail-fast.
[[init.phases]]
name = "smoke"
cmd = "/bin/true && echo SMOKE_OK"
expect = ""
timeout_secs = 5

# Phase 2: bring up Django server in background, wait for socket-up.
# The template's `expect = "SERVER_READY"` makes the gate-loop's
# `--wait-for` mechanism block here until the guest prints the
# marker; the host-side curl battery (above) is launched at that
# point.
[[init.phases]]
name = "django-up"
cmd = """
python3 -m django runserver 0.0.0.0:8080 \
    --noreload --insecure \
    >/tmp/django.log 2>&1 &
echo $! > /tmp/django.pid
# Poll until socket is up (max 30s).
for i in $(seq 1 60); do
    nc -z 127.0.0.1 8080 && break
    sleep 0.5
done
nc -z 127.0.0.1 8080 || { echo SERVER_FAIL; exit 1; }
echo SERVER_READY
"""
expect = "SERVER_READY"
timeout_secs = 45

# Phase 3: in-guest self-test — 100 serial requests to /health.
[[init.phases]]
name = "guest-curl"
cmd = """
ok=0; fail=0
for i in $(seq 1 100); do
    code=$(curl -s -o /dev/null -w '%{http_code}' \
        http://127.0.0.1:8080/health)
    if [ "$code" = "200" ]; then ok=$((ok+1));
    else fail=$((fail+1)); fi
done
echo "GUEST_CURL ok=$ok fail=$fail"
[ "$fail" -eq 0 ] && echo TIER3_OK || (echo TIER3_FAIL; exit 1)
"""
expect = ""
timeout_secs = 30

# Phase 4: shutdown server, drain logs.
[[init.phases]]
name = "shutdown"
cmd = """
kill $(cat /tmp/django.pid) 2>/dev/null || true
sleep 1
echo REPRO_DONE rc=$?
"""
expect = ""
timeout_secs = 10
```

### 7.2 `tier3-fastapi.toml.template`

Same shape with the Django phase replaced by:

```toml
[[init.phases]]
name = "fastapi-up"
cmd = """
uvicorn tier3_app:app --host 0.0.0.0 --port 8080 \
    --no-access-log \
    >/tmp/uvicorn.log 2>&1 &
echo $! > /tmp/uvicorn.pid
for i in $(seq 1 60); do
    nc -z 127.0.0.1 8080 && break
    sleep 0.5
done
nc -z 127.0.0.1 8080 || { echo SERVER_FAIL; exit 1; }
echo SERVER_READY
"""
expect = "SERVER_READY"
timeout_secs = 45
```

`tier3_app` is a ~20-line FastAPI module the daemon drops into
`$SOAK_OUT/tier3-app/` and references via a hostfs-visible path
(env: `PYTHONPATH=/var/soak/tier3-app`). Three endpoints:
`/health` (JSON `{"ok": true}`), `/crypto` (decodes a fixed
PEM-encoded test cert via `cryptography`), `/np` (runs a fixed
4×4 numpy matmul). All three return JSON; non-200 = FAIL.

## 8. Acceptance criteria — Tier 3 end-to-end

Tier 3 is "working" when **all** of:

1. **Pre-flight passes.** Daemon's first action on a Tier 3
   rotation: verify `CONFIG_UML_NET_VECTOR=y` in the kernel's
   adjacent `.config`. If missing, daemon writes a
   `TIER3_PREFLIGHT_FAIL` row to `scoreboard.jsonl` and skips
   the tier3 rotation segment (does not stop the soak).

2. **Per-iteration success markers fire.**
   `umlctl gate loop` records PASS iff the guest's init.log
   contains both `SERVER_READY` (phase 2) and `TIER3_OK`
   (phase 3), in that order, before the iteration timeout.

3. **Host-side curl battery succeeds.** All 100 host-to-guest
   curls return HTTP 200 with the expected JSON body. Daemon
   writes per-worker curl CSV to `$SOAK_OUT/tier3-w<W>-curl.csv`
   and a derived row to scoreboard.jsonl with `host_curl_ok`,
   `host_curl_fail`, `p50_ms`, `p95_ms` fields (extends §2.6
   of the Phase J memo).

4. **Latency budget.** p50 host-to-guest round-trip < 100 ms,
   p95 < 500 ms over the 100-request battery. (Loopback over
   TAP with no traffic competition — these are very loose
   bounds; if we see p50 > 100 ms there is a real
   virtio-net bug to file.)

5. **One full rotation per framework, both backends.**
   `tier3-django` and `tier3-fastapi`, both `kvm-v2` and
   `seccomp`, each with ≥ 40 iterations
   (W=2 × M=20), pass-rate ≥ 99.0% AND Wilson 95% lower bound
   ≥ 95.0%. Matches the Phase J memo §5.4 bar.

6. **Teardown is clean.** After the daemon stops, no leftover
   `soak-tap<W>` interfaces, no leftover iptables rules with
   `192.168.42.0/24` as source/destination. Daemon runs a
   post-soak `ip tuntap show | grep soak-tap` + `iptables-save
   | grep 192.168.42` and writes the (empty) output to
   `$SOAK_OUT/teardown-residue.txt` for the operator to spot-
   check. If non-empty, soak summary surfaces a warning
   (not a fail — leaked TAPs are an `umlctl down` bug, not
   a kernel bug).

## 9. Follow-ups (not gating Phase J)

- **`umlctl ps --json guest_tap_ip` field.** Worth adding for
  the general "umlctl operator wants to know which IP to curl"
  case (non-soak fastapi/django deployments). File as a
  separate umlctl tracking issue post Phase J close; design
  per §3.1(a.2) (sidecar) to avoid the manifest schema bump.

- **gate-loop network-section fan-out.** The current
  `gate_loop.rs:253-277` per-worker Umlfile generator should
  also rewrite `network.tap_name`/`guest_ip`/`host_ip` when
  `network.mode == "tap"` (suffix `-w<N>`, IP from a small
  pool, similar to the daemon's policy here). This would mean
  Tier 3 could use `umlctl gate loop --workers N --file
  tier3.toml` directly without the per-worker spawn loop in
  §6.1. Defer to umlctl phase D-11 (post merge gate widens).

- **CONFIG_UML_NET_VECTOR pre-flight in umlctl up.** Today
  `umlctl up` happily compiles a `mode = "tap"` Umlfile against
  a kernel that wasn't built with vector net; the failure
  surfaces as a confusing "vec0: device not found" inside the
  guest's `ip addr add` phase. Add a `umlctl up --preflight`
  that greps the kernel's `.config` adjacent file and fails
  fast. Same risk register entry as §6.4 of the Phase J memo;
  the Tier 3 daemon's pre-flight (acceptance §1 above) is a
  workaround pending the umlctl fix.

- **Memo cross-reference fix.** `phase-J-design-2026-05-07.md`
  §3.3 cites `tools/uml/uml-launcher/src/backend/net.rs` as
  the place that "provisions tap0 + iptables NAT." That's
  wrong — `backend/net.rs` is the vhost-user data path and
  explicitly declines to do TAP setup (see its doc-comment
  line 49). The actual provisioning is in
  `src/bin/umlctl/deploy.rs::compile()`. Worth a one-line edit
  to the upstream memo so the next reader doesn't chase the
  same false trail.

## 10. References

- `Documentation/virt/uml/redesign/02-workstreams/D-kvm-backend/phase-J-design-2026-05-07.md`
  §3.3 — the open question this memo resolves.
- `tools/uml/uml-launcher/src/bin/umlctl/deploy.rs` — actual
  TAP/iptables/NAT compiler (`compile()` at line 287; defaults
  at `NetworkSection::default` line 113).
- `tools/uml/uml-launcher/src/bin/umlctl/manifest.rs:15-46` —
  manifest schema (no network fields).
- `tools/uml/uml-launcher/src/bin/umlctl/registry.rs:20-32` —
  `ps --json` Row struct.
- `tools/uml/uml-launcher/src/bin/umlctl/gate_loop.rs:253-277` —
  per-worker Umlfile fan-out (rewrites name only).
- `tools/uml/uml-launcher/src/backend/net.rs:25-50` —
  vhost-user data path; explicit non-goal of TAP creation.
- `tools/testing/selftests/um/soak/run-pilot.sh:88-89` —
  current template-substitution pattern the daemon extends.
- `tools/testing/selftests/um/soak/cpython-soak.toml.template`
  — closest existing template; Tier 3 templates extend its
  shape with the new `[network] mode = "tap"` block.
