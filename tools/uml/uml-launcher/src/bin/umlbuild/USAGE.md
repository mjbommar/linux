# umlbuild — user guide

`umlbuild` is the sibling of `umlctl`: it builds the inputs umlctl
consumes — a UML kernel binary, a ubd-attachable rootfs image, and a
ready-to-boot `Umlfile.toml`.

```
umlbuild instance --profile mvp --out ~/uml-mvp
umlctl up -f ~/uml-mvp/Umlfile.toml
```

That's the whole story. The rest of this document explains what each
piece does, what you can override, and where the cache lives.

## Build + install

```
cd tools/uml/uml-launcher
make            # builds uml-launcher, umlctl, and umlbuild
sudo make install   # installs all three to /usr/local/bin
```

## Subcommands

| verb | what it produces |
|------|------------------|
| `umlbuild kernel`   | a UML kernel binary under `$XDG_CACHE_HOME/uml-build/builds/<profile>/linux` (or `--out`) |
| `umlbuild rootfs`   | a populated rootfs directory under `$XDG_CACHE_HOME/uml-build/rootfs/<profile>/` |
| `umlbuild image`    | an ubd-attachable ext4 image, no sudo required |
| `umlbuild instance` | runs all three + writes a `Umlfile.toml` |
| `umlbuild shell`    | docker-shaped: build (if needed) + drop into an interactive shell or REPL inside the guest |
| `umlbuild profile`  | `list` enumerates profiles; `show NAME` pretty-prints one |

Each lower verb is independently runnable. `instance` is the orchestrator
most users will reach for.

### `umlbuild kernel`

```
umlbuild kernel --profile mvp [--out PATH] [--source PATH] [-j N] [--force]
```

Resolves the profile's `[kernel]` section: starts from the named base
config (`tinyconfig`, `base_defconfig`, `x86_64_defconfig`, or an
explicit defconfig path), applies the listed `--enable` / `--disable`
overlays via `scripts/config`, runs `make olddefconfig && make -j N`,
optionally strips the result.

Idempotent: writes the resolved `.config`'s sha256 into the build
directory; a re-run with the same profile is a no-op unless `--force`.

Builds always use `O=$BUILD_DIR` (never in-tree).

### `umlbuild rootfs`

```
umlbuild rootfs --profile mvp [--out DIR] [--force]
```

Two bases supported at v1:

- **`base = "alpine"`** — downloads + verifies the pinned Alpine
  `minirootfs` tarball into `$XDG_CACHE_HOME/uml-build/dl/`, extracts
  into the output directory, then runs `apk add --no-cache <packages>`
  via the most-portable rootless path available:
    1. host's `apk` if installed (`apk --root $rootfs ...`)
    2. `bwrap` (bubblewrap) bind-mounting the rootfs at `/`, running
       the rootfs's own `/sbin/apk` (works on Ubuntu/Debian with
       AppArmor restrictions enabled)
    3. `unshare -r chroot` as a fallback
- **`base = "debian-slim"`** — runs `mmdebstrap --mode=unshare` (fully
  rootless via user namespaces) with the listed `[rootfs.debian]`
  package set.

In both cases, `umlbuild` installs a generated `/sbin/init` (replacing
Alpine's busybox-symlink default) and bakes the profile's `instance.
sandbox_cmd` into `/etc/sandbox.cmd`.  The init script:

1. mounts the standard pseudo-filesystems defensively;
2. brings `lo` up best-effort;
3. reads the command from `/etc/sandbox.cmd` (or, if you pass
   `sandbox.cmdfile=/path` on the kernel cmdline, from that path);
4. runs it through `/bin/sh -c`, capturing stdout/stderr into
   `/results/`;
5. powers off the guest cleanly.

### `umlbuild image`

```
umlbuild image --rootfs-dir DIR --out PATH --size 200M [--label NAME] [--no-fsck]
```

Packs a populated rootfs directory into an ext4 image using
`mkfs.ext4 -d` — **no sudo, no loop mount, no chroot**.  Runs
`e2fsck -fn` for a self-check unless `--no-fsck`.

Requires `e2fsprogs >= 1.43`.

### `umlbuild instance`

```
umlbuild instance --profile NAME --out DIR [--source PATH] [--force]
```

End-to-end orchestrator.  Produces under `DIR/`:

- `linux` — the built kernel
- `rootfs.d/` — the staged rootfs (kept for inspection / rebuild)
- `rootfs.img` — the ubd-attachable ext4 image
- `Umlfile.toml` — points at the above, ready for `umlctl up -f`

The emitted Umlfile sets `[runtime].root = "ubd"` so umlctl skips its
hostfs synthesis and lets the rootfs's `/sbin/init` run as PID 1.

### `umlbuild shell` — docker-shaped one-shot

```
umlbuild shell [--profile NAME] [--cmd PATH] [--out DIR] [--force]
```

Build (or reuse) an instance, then `execve()` into the kernel so the
host TTY is the guest's console.  Like `docker run -it python bash`
but built on UML.

```
# /bin/sh in a 3 MB minimum-viable guest:
umlbuild shell --profile mvp

# Python REPL in the sandbox profile (with py3-pip + ssl):
umlbuild shell --profile sandbox --cmd /usr/bin/python3

# bash in the dev profile (which ships bash + gcc + gdb):
umlbuild shell --profile dev --cmd /bin/bash

# Outbound networking — TAP + MASQUERADE auto-wired via sudo.
# Inside the guest, `urllib.request.urlopen('https://www.python.org/')` works.
umlbuild shell --profile sandbox-net --network tap --cmd /usr/bin/python3

# Run a one-off command instead of an interactive prompt:
echo 'python3 -c "print(42)"' | umlbuild shell --profile mvp
```

### Networking (`--network tap`)

With `--network tap`, `umlbuild shell`:

1. Creates a host TAP (`umlb-tap0` by default), assigns the profile's
   `[network].host_ip`, brings it up.
2. Enables `net.ipv4.ip_forward=1` and adds `iptables MASQUERADE` from
   the TAP's CIDR to the host's default-route interface (auto-detected
   from `/proc/net/route`; override with `--nat-via <iface>`).
3. Adds `iptables -A FORWARD` rules so packets can traverse the TAP.
4. Boots the kernel with `vec0:transport=tap,ifname=umlb-tap0,depth=128`.
5. The rootfs's `/sbin/init` reads `/etc/sandbox.net` (baked at image
   build time) and runs `ip addr add 10.7.0.2/24 dev vec0; ip link
   set vec0 up; ip route add default via 10.7.0.1` plus writes
   `/etc/resolv.conf`.
6. On shell exit, tears all of the above down (idempotent — leftovers
   from prior crashes are removed first).

All host-side network setup requires `sudo`; if you don't have
NOPASSWD, you'll be prompted once.  Use the `sandbox-net` profile for
a turnkey example.

The default `--cmd /bin/sh` works on any Alpine-based profile.  Type
`exit` or Ctrl-D to leave; the kernel powers down (you'll see a
"Kernel panic — Attempted to kill init" message, which is the normal
UML shutdown path when PID 1 exits).

Differences from `umlctl up`:

|                       | `umlctl up`                          | `umlbuild shell`                |
|-----------------------|--------------------------------------|---------------------------------|
| stdin                 | none (`/dev/null`)                   | host TTY (`fd:0`)               |
| stdout                | log file in `~/.local/state/uml/runs/` | host TTY (`fd:1`)               |
| init                  | umlctl-synthesized init.sh (hostfs)  | user-chosen binary (--cmd)      |
| supervision           | child process, signal-tracked        | execve, foregrounded            |
| use case              | batch / unattended                   | interactive REPL / debugging    |

## Profiles

A profile is a single TOML describing how to build a UML instance.
See `tools/uml/uml-launcher/profiles/mvp.toml` for the canonical
example.

```toml
schema_version = 1

[profile]
name        = "mvp"
description = "Minimum viable UML."

[kernel]
base_config = "tinyconfig"
enable = ["64BIT", "PRINTK", "NULL_CHAN", "HOSTFS", ...]
disable = []
strip = true

[rootfs]
base = "alpine"
sandbox_uid = 0

[rootfs.alpine]
version  = "3.20.3"
sha256   = "d4e6fd67dcf75e40c451560ac7265166c2b72a0f38ddc9aae756a7de3d1efa0c"
mirror   = "https://dl-cdn.alpinelinux.org/alpine"
packages = ["python3", "busybox-extras"]

[image]
size = "200M"
fstype = "ext4"
label = "uml-mvp"

[instance]
mem = "256M"
ncpus = 1
backend = "seccomp"
sandbox_cmd = "python3 -c 'print(\"hi\")'"
```

### Built-in profiles

| name | starting point | est. stripped kernel | image | what it does |
|------|----------------|----------------------|-------|--------------|
| `mvp`         | tinyconfig + 17 Kconfig flips | ~3.3 MB  | 200 MB ext4 (sparse) | runs `python3` from Alpine 3.20; ubd-rooted |
| `sandbox`     | mvp + NET/INET/UNIX/OVERLAY_FS/EVENTFD/EPOLL | ~6 MB    | 500 MB | safe-enough for untrusted Python: loopback, tmpfs overlay, sandbox uid=1000, `py3-pip` + `ca-certificates` |
| `sandbox-net` | sandbox + UML_NET_VECTOR_V2 + IP_PNP | ~7 MB    | 500 MB | outbound networking via TAP+MASQUERADE (set up by `umlbuild shell --network tap`); guest interface is `vec2.0` (v2 driver); iproute2 + curl + ca-certs |
| `dev`         | base_defconfig                | ~90 MB   | 2 GB   | development UML: gcc, git, gdb, strace, vim in Alpine |
| `container`   | base_defconfig + USER_NS/PID_NS/MEMCG/OVERLAY_FS/BRIDGE/NF_TABLES/etc. + runc | ~110 MB | 1 GB | runs OCI containers inside the guest via runc (defense-in-depth over UML's kernel boundary) |

All four are in `tools/uml/uml-launcher/profiles/`.  Build any of them:

```
umlbuild instance --profile sandbox  --out ~/uml-sandbox
umlbuild instance --profile dev      --out ~/uml-dev
umlbuild instance --profile container --out ~/uml-container
umlctl up -f ~/uml-sandbox/Umlfile.toml
```

### Custom profiles

Drop a TOML into `$XDG_CONFIG_HOME/uml-build/profiles/<name>.toml` and
`umlbuild` picks it up automatically.  `umlbuild profile list` shows
both in-tree and user-config profiles.

## Cache + state layout

```
$XDG_CACHE_HOME/uml-build/          # ~/.cache/uml-build by default
├── dl/                             # downloaded base tarballs
├── builds/<profile>/               # kernel O= tree, .config, manifest
├── rootfs/<profile>/               # staged rootfs directories
└── images/<profile>.img            # finished ubd images

$XDG_CONFIG_HOME/uml-build/         # ~/.config/uml-build
└── profiles/                       # user profile TOMLs
```

Wipe `$XDG_CACHE_HOME/uml-build/` to force everything from scratch.

## Manifests + reproducibility

Each stage writes a sidecar `manifest.toml` capturing the inputs that
produced the artifact (config sha, binary sha, kernel source root,
profile name, package count, image size).  Two `umlbuild kernel` runs
of the same profile against the same source HEAD will produce
identical `binary_sha256` (modulo BUILD_TIMESTAMP).

## Privilege model

`umlbuild` is designed to run as a non-root user.

| stage | requires |
|-------|----------|
| `umlbuild kernel` | nothing beyond a working `make`, `gcc`, `bison`, `flex` |
| `umlbuild rootfs` (alpine) | `curl` + `tar` + (one of: host `apk`, `bwrap`, `unshare`) |
| `umlbuild rootfs` (debian-slim) | `mmdebstrap` |
| `umlbuild image` | `e2fsprogs >= 1.43` (for `mkfs.ext4 -d`) |
| `umlbuild instance` | the union of the above |

No `sudo` is invoked or required on the happy path.  On hosts where
AppArmor restricts unprivileged user namespaces (Ubuntu 23.10+), the
bwrap path is used automatically.

## Troubleshooting

- **"no rootless backend available"** — install `bubblewrap`
  (`apt install bubblewrap`) or `apk-tools`.
- **`mkfs.ext4: unknown option -d`** — your `e2fsprogs` is older than
  1.43.  Upgrade.
- **kernel build fails on `bits/libc-header-start.h`** — `tinyconfig`
  defaulted to 32-bit; `umlbuild` adds `CONFIG_64BIT=y` automatically,
  but if you pass `--source` to a tree where this isn't true, set
  `enable = ["64BIT", ...]` in your profile.
- **`apk add` finishes with "ERROR: 110 errors updating directory
  permissions"** — cosmetic; you're not root in the chroot so chown
  syscalls fail.  Files still install correctly.

## What umlbuild does NOT do

- Run kernels (that's `umlctl up`).
- Patch the kernel source tree.
- Cross-compile to other arches.
- Sign images.
- Push to / pull from a registry.

See `SPEC.md` next to this file for the full design rationale, the
open questions list, and the v1 ship sequence.
