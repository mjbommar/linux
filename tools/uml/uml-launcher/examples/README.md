# uml-launcher / umlctl example configs

Two flavors of example here:

1. **Plain `uml-launcher run` configs** — minimal TOML that maps 1:1
   to CLI flags. The kernel boots straight into `init=/bin/sh` (or
   whatever you set). No init script, no env defaults, no auto-mounts
   — you set everything up by hand once you're at the prompt. Useful
   for low-level kernel work.

   ```
   uml-launcher run --config path/to/<name>.toml [overrides]
   ```

2. **`umlctl up` Umlfile configs** — declarative manifests with full
   schema (`schema_version = 1`, `[instance]`, `[network]`, `[env]`,
   `[[init.phases]]`, etc.). umlctl auto-generates an init script that
   sets up the standard pseudo-filesystems (/proc, /sys, /dev/pts,
   /dev/shm, /tmp), brings loopback up, exports default
   PATH/HOME/TERM/SHELL, applies your `[env]` overrides, mounts
   declared `[[volumes]]`, and runs your `[[init.phases]]` in order.
   The result is a usable Linux environment for actual workloads
   (Python, services, tests) without you having to know which
   filesystems the kernel doesn't auto-mount.

   ```
   umlctl up -f path/to/<name>.toml
   ```

CLI flags > TOML > env vars > defaults.

## Index

### Plain `uml-launcher run` configs

| File | Profile | Use case |
|---|---|---|
| [`fuzz.toml`](fuzz.toml) | fuzz | External AFL++/syzkaller harness binding via `--forkserver` |
| [`research.toml`](research.toml) | research | Interactive debugging / feature exploration |
| [`sandbox.toml`](sandbox.toml) | sandbox | Sandbox-profile launch knobs |
| [`dev.toml`](dev.toml) | any | Fast boot + interactive shell; developer daily driver |

### `umlctl up` Umlfile configs

| File | Use case |
|---|---|
| [`fastapi.toml`](fastapi.toml) | FastAPI server hosted inside UML, with TAP networking + port-forward |
| [`vector2-fd-handoff.toml`](vector2-fd-handoff.toml) | Minimal vector2 TAP smoke using launcher-owned inherited fd handoff |
| [`vector2-fd-multiqueue.toml`](vector2-fd-multiqueue.toml) | Vector2 TAP smoke using launcher-owned inherited fd multiqueue |
| [`vector2-auto-queues.toml`](vector2-auto-queues.toml) | Vector2 TAP smoke using `queues = "auto"` to match runtime.ncpus |
| [`vector2-fastapi-smoke.toml`](vector2-fastapi-smoke.toml) | Vector2 FastAPI/uvicorn smoke using fd handoff and automatic queue sizing |
| [`vector2-lifecycle-stress.toml`](vector2-lifecycle-stress.toml) | Vector2 live `ip link up/down` lifecycle stress with ethtool counter checks |
| [`vector2-failed-open.toml`](vector2-failed-open.toml) | Vector2 validation-only failed-open injection and open-unwind counter check |
| [`cpython-test.toml`](cpython-test.toml) | CPython standard test suite — canonical "is the env real?" check |

The Umlfile configs are not magic either: they're TOML that drives
`tools/uml/uml-launcher/src/bin/umlctl/deploy.rs::render_init_script`
to produce a single bash init script. `umlctl up --dry-run` shows the
exact script, host setup, teardown, kernel arguments, and selected
network plan.

For vector-networking comparisons, keep the TOML stable and switch the
driver from the command line:

```sh
umlctl up -f tools/uml/uml-launcher/examples/fastapi.toml --network-driver vector2 --dry-run
umlctl up -f tools/uml/uml-launcher/examples/fastapi.toml --network-driver vector2 --network-queues auto --dry-run
umlctl gate loop -f tools/uml/uml-launcher/examples/fastapi.toml --sweep network.driver=vector,vector2
```

For the direct vector2 fd-handoff path, start with the focused smoke:

```sh
UML_KERNEL=/path/to/uml/linux \
  umlctl up -f tools/uml/uml-launcher/examples/vector2-fd-handoff.toml --dry-run
UML_KERNEL=/path/to/uml/linux \
  umlctl up -f tools/uml/uml-launcher/examples/vector2-fd-multiqueue.toml --dry-run
UML_KERNEL=/path/to/uml/linux \
  umlctl up -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml --dry-run
UML_KERNEL=/path/to/uml/linux \
  umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-fastapi-smoke.toml \
    -W 1 -M 1 --timeout 240 --pass-marker VECTOR2_FASTAPI_OK
UML_KERNEL=/path/to/uml/linux \
  umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml \
    -W 1 -M 1 --timeout 1800 --pass-marker VECTOR2_LIFECYCLE_STRESS_OK
UML_KERNEL=/path/to/uml/linux \
  umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-lifecycle-stress.toml \
    -W 1 -M 1 --timeout 180 \
    --sweep UML_VECTOR2_LIFECYCLE_CYCLES=25 \
    --pass-marker VECTOR2_LIFECYCLE_STRESS_OK
UML_KERNEL=/path/to/uml/linux \
  umlctl gate loop -f tools/uml/uml-launcher/examples/vector2-failed-open.toml \
    -W 1 -M 1 --timeout 180 \
    --pass-marker VECTOR2_FAILED_OPEN_OK \
    --fail-marker 'VECTOR2_FAILED_OPEN_.*(FAIL|UNEXPECTED|MISSING|BAD)|VERIFY_FAIL|kernel BUG|Kernel panic|BUG:|WARNING:|KCSAN:|data-race'
```

`[network].fail_open_after` is only for vector2 validation Umlfiles such
as `vector2-failed-open.toml`; leave it unset for normal workloads.

For a quick vec0-vs-vector2 guest-to-host TCP baseline through the
same `umlctl` path, use the helper script:

```sh
tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
  --kernel /path/to/uml/linux

UML_VECTOR_PERF_DIRECTION=both \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel /path/to/uml/linux

UML_VECTOR_PERF_DIRECTION=both \
UML_VECTOR_PERF_BYTES_LIST=1048576,33554432 \
UML_VECTOR_PERF_REPEAT=3 \
  tools/uml/uml-launcher/scripts/vector-net-perf-baseline.sh \
    --kernel /path/to/uml/linux
```

For a KCSAN-oriented vector2 concurrent TCP/UDP gate with queue
distribution checks, use the focused helper:

```sh
tools/uml/uml-launcher/scripts/vector2-kcsan-concurrent-traffic.sh \
  --kernel /path/to/kcsan-uml/linux
```
