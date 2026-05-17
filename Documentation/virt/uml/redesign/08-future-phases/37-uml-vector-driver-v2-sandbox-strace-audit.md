# UML vector driver v2 sandbox strace audit

**Status:** partial sandbox evidence - vector2 fd handoff audit.
**Date:** 2026-05-17.

This note records a focused host syscall audit for the launcher-owned
vector2 fd-handoff path.  It also records a small `umlctl --strace`
integration fix needed to make this audit safe to run repeatedly.

The audit is useful evidence for the vector2 sandbox design, but it
does not close the full replacement security gate.  It covers one
short seccomp boot with `queues = "auto"` and distinguishes vector
driver host operations from guest userspace syscalls visible in UML's
host trace.

## umlctl Fix

`umlctl up --strace` now wires the compiled `[debug] strace` request
into the supervised start path.  When strace is enabled, `umlctl`
starts:

```text
strace -f -s 256 -o <strace.log> <uml-kernel> <kernel-args...>
```

The first implementation tracked the strace wrapper PID.  In this UML
mode the tracee becomes the long-lived UML process, so `umlctl down`
could miss the tracee and leave the TAP behind.  The supervised start
path now waits briefly for the first PID at the beginning of the strace
log and records that tracee PID in the pidfile, run bundle, and start
output.  If no trace line appears, it falls back to the wrapper PID so
early failure diagnostics still work.

Unit coverage:

```sh
cargo test --manifest-path tools/uml/uml-launcher/Cargo.toml --bin umlctl
```

Result:

```text
82 passed; 0 failed
```

## Live Audit Command

Kernel:

```text
/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux
```

Command:

```sh
rm -f logs/vector2-auto-queues/strace.log
UML_KERNEL=/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux \
  timeout 300s cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
    --bin umlctl -- up \
    -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
    --strace --wait-for VECTOR2_AUTO_QUEUES_OK --wait-timeout 180

cargo run --manifest-path tools/uml/uml-launcher/Cargo.toml \
  --bin umlctl -- down \
  -f tools/uml/uml-launcher/examples/vector2-auto-queues.toml \
  --force --rm
```

Runtime shape:

```text
[umlctl] network: driver=vector2 guest_dev=vec2.0 tap=v2autoq0 transport=fd host_mode=fd queues=4 queue_spec=auto
[umlctl] network-fd: open tap=v2autoq0 and inherit fds=200..203
started vector2-auto-queues pid=3807670 run_id=01KRV3S353GXS9FQZA082DSADE
[umlctl] wait-for matched /VECTOR2_AUTO_QUEUES_OK/
```

Teardown evidence:

```text
stopped vector2-auto-queues pid=3807670 signal=KILL exit=None run_id=01KRV3S353GXS9FQZA082DSADE
removed vector2-auto-queues
TAP_ABSENT
UML_PROCESS_ABSENT
UP_RC=0 DOWN_RC=0 TAP_RC=0 PROC_RC=0
```

The trace was written to:

```text
logs/vector2-auto-queues/strace.log
```

It contained 170261 lines.  The first trace line confirmed that the
tracked PID was the UML tracee:

```text
3807670 execve("/home/mjbommar/projects/personal/.build/um-vector-r1-v2only/linux", ...)
```

## Forbidden Vector Host Operations

The narrowed scan checks only actual syscall lines, not string data read
from ELF text sections:

```sh
rg -n '^[0-9]+ (open|openat)\([^\n]*"/dev/net/tun"|^[0-9]+ ioctl\([^\n]*TUNSETIFF|^[0-9]+ socket\(AF_PACKET|^[0-9]+ bpf\(|^[0-9]+ execve\("[^"]*(uml_net|uml_switch|helper)' \
  logs/vector2-auto-queues/strace.log -S
```

Result:

```text
no matches
```

For this vector2 fd-handoff boot, the traced UML process did not:

- open `/dev/net/tun`;
- issue `TUNSETIFF`;
- create an `AF_PACKET` socket;
- call `bpf()`;
- execute a UML network helper such as `uml_net` or `uml_switch`.

This is the expected sandbox shape: `umlctl` owns the host TAP setup and
passes already-open TAP fds into the UML process, while vector2 consumes
only the inherited fd range.

## Guest Userspace Boundary

A broad text scan will still find strings such as `/dev/net/tun` and
`TUNSETIFF` because the kernel image or loaded userspace text contains
those strings.  Those are `pread64()` buffer contents, not host opens or
ioctls.

The trace also includes guest userspace activity because UML guest tasks
are host processes.  In this run:

- guest `ip` commands created `AF_NETLINK, SOCK_RAW` sockets;
- guest `ping` created an `AF_INET, SOCK_RAW, IPPROTO_ICMP` socket.

Those entries map to `execveat(..., ["uml-userspace"], ...)` followed by
guest command execs such as `/usr/sbin/ip` and `/usr/bin/ping`.  They
are not vector2 host attach operations, but they are still relevant to
the broader UML secure-execution story.  A future sandbox audit should
decide whether guest raw and netlink socket use is acceptable for a
given workload profile, or whether `umlctl` should provide a stricter
no-raw-socket audit mode that uses non-raw readiness checks.

## What This Closes

This checkpoint closes a practical auditability gap:

- `umlctl up --strace` can now be used on vector2 fd-handoff workloads
  without losing track of the long-lived UML process;
- `umlctl down --force --rm` cleans the traced run without leaving
  `v2autoq0`;
- the vector2 fd path showed no direct host TAP creation, helper exec,
  `AF_PACKET`, or BPF load in the audited boot.

## Remaining Gate

The full sandbox replacement gate remains open until at least:

- the strace scan becomes a repeatable `umlctl gate` or CI check;
- audits cover longer runs and failure paths, not just a successful boot;
- audits cover vector2 FastAPI/Django workloads and kvm-v2 once the
  kvm-v2 readiness blocker is fixed;
- guest userspace raw/netlink socket policy is stated explicitly for
  secure-execution profiles;
- the old trusted in-process TAP path remains impossible in sandbox
  builds except through explicit `INPROC` configurations.
