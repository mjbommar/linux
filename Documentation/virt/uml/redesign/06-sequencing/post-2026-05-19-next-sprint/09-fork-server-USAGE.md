# Fork-Server And Pool Usage

This guide describes the current UML template-pause and pool interface. It is
for users who want to drive UML instances as reusable pool members through
`umlctl`.

## Build

Build a UML kernel with template-pause support:

```sh
make ARCH=um O=$BUILD_DIR defconfig
./scripts/config --file $BUILD_DIR/.config \
    --enable UM_TEMPLATE_PAUSE \
    --enable UM_TEMPLATE_PAUSE_FORK
make ARCH=um O=$BUILD_DIR -j$(nproc)
```

Optional configuration:

- `CONFIG_UM_TEMPLATE_PAUSE_IDENTITY_KUNIT=y` enables identity parser KUnit.
- `CONFIG_UML_NET_VECTOR_V2=y` enables vector2 TAP reopen for per-member
  networking.

The fork-on-resume path is for seccomp-backed UML. KVM-backed requests are
rejected because KVM fd and vCPU state cannot be inherited safely across the
fork boundary.

## Kernel Command Line

Start a UML master with template pause and pool-member mode enabled:

```sh
$BUILD_DIR/linux \
    backend=force=seccomp \
    mem=128M \
    rootfstype=hostfs rootflags=/ root=/dev/root rw \
    ncpus=1 \
    um_template_pause=fork \
    um_template_pause_pool_member=1 \
    init=/path/to/init.sh
```

Important parameters:

- `um_template_pause=fork` resumes by creating a pool member.
- `um_template_pause_pool_member=1` routes the child into the pool-member
  entry path.
- `ncpus=1` is the expected pool-member configuration.
- `backend=force=seccomp` keeps the backend choice explicit.

The guest workload should write to `/proc/um/template_pause` at the point where
the master is ready to be taken:

```sh
echo ready > /proc/um/template_pause
```

## Direct Spawn

For direct use, let `umlctl` boot a master and resume one member:

```sh
umlctl pool spawn \
    --kernel $BUILD_DIR/linux \
    --instance pool-member-1 \
    --mac 52:54:00:11:22:33 \
    --tap tap-pool-1 \
    --ipv4 10.7.0.42/24 \
    --gateway 10.7.0.1 \
    --json
```

The command writes the identity data, resumes the master, and returns a JSON
description of the member.

## Pool Daemon

For repeated requests, start the daemon:

```sh
umlctl pool serve \
    --name default \
    --kernel $BUILD_DIR/linux \
    --background
```

Then take members through the daemon socket:

```sh
umlctl pool take \
    --name default \
    --instance member-1 \
    --mac 52:54:00:11:22:44 \
    --tap tap-member-1 \
    --ipv4 10.7.0.43/24 \
    --gateway 10.7.0.1 \
    --json
```

Inspect or stop the pool:

```sh
umlctl pool status --name default --json
umlctl pool destroy --name default --pid <member-pid>
```

## Daemon-Routed Commands

When the member has the required in-guest control channel, the daemon can route
commands through the pool API:

```sh
umlctl exec --pool default --pid <member-pid> -- /bin/true
umlctl port-forward --pool default --pid <member-pid> --list
```

If the member does not expose the required channel, commands should return a
structured failure instead of hanging or pretending success.

## Identity Data

The identity blob is fixed-size and written before resume. It contains:

- magic and version fields;
- instance name;
- MAC address;
- host TAP name;
- IPv4 CIDR string;
- IPv4 gateway string;
- reserved space;
- a child-pid return slot.

The kernel parses the blob, applies the MAC and IPv4 settings to the selected
UML netdev, and, when vector2 TAP support is available, reopens the netdev on
the requested host TAP.

## Expected Failures

The interface should fail clearly in these cases:

- fork-on-resume requested under the KVM backend;
- missing template-pause kernel support;
- missing vector2 support when per-member TAP reopen is requested;
- missing mconsole path for daemon-routed exec;
- missing or invalid pool daemon socket;
- invalid identity fields.

Clear failure is part of the contract. A skipped prerequisite is preferable to
a false pass.

## Test Coverage

Relevant coverage lives under `tools/testing/selftests/um/` and the
`tools/uml/uml-launcher` Rust tests. Run the kernel selftests with
`UML_BINARY=$BUILD_DIR/linux` and run launcher tests from
`tools/uml/uml-launcher`.

For publication work, pair functional selftests with a full UML build and a
check that KVM-backed fork-on-resume is refused cleanly.
