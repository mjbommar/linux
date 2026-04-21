# uml-launcher example configs

Drop-in TOML configs for common UML workflows. Usage:

```
uml-launcher run --config path/to/<name>.toml [overrides]
```

CLI flags override TOML, TOML overrides env vars, env overrides
defaults.

## Index

| File | Profile | Use case |
|---|---|---|
| [`fuzz.toml`](fuzz.toml) | fuzz | External AFL++/syzkaller harness binding via `--forkserver` |
| [`research.toml`](research.toml) | research | Interactive debugging / feature exploration |
| [`sandbox.toml`](sandbox.toml) | sandbox | Future v2 shape — v1 just runs it unsandboxed |
| [`dev.toml`](dev.toml) | any | Fast boot + interactive shell; developer daily driver |

None of these configs are magic — they're plain TOML that maps
1:1 to CLI flags. Copy, edit, keep in `~/.config/uml-launcher/`
or alongside your kernel trees.
