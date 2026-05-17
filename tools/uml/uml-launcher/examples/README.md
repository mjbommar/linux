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
| [`sandbox.toml`](sandbox.toml) | sandbox | Future v2 shape — v1 just runs it unsandboxed |
| [`dev.toml`](dev.toml) | any | Fast boot + interactive shell; developer daily driver |

### `umlctl up` Umlfile configs

| File | Use case |
|---|---|
| [`fastapi.toml`](fastapi.toml) | FastAPI server hosted inside UML, with TAP networking + port-forward |
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
umlctl gate loop -f tools/uml/uml-launcher/examples/fastapi.toml --sweep network.driver=vector,vector2
```
